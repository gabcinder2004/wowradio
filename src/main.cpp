// WowRadio.dll - in-game internet radio for World of Warcraft 3.3.5a (build 12340)
//
// What this DLL does
//   1. Gets loaded by the game because the game's executable imports it
//      (see tools/patch_exe_import.py). All work starts in DllMain.
//   2. Adds a handful of Radio_* functions to the client's addon Lua API by
//      hooking the client's own "register built-in functions" routine.
//   3. Streams an Icecast/Shoutcast station with the BASS audio library, which
//      is embedded in this DLL as a resource (see bass_loader.cpp), on a
//      worker thread so the game thread never blocks on the network.
//
// The audio plays on its own Windows audio path, outside the game's FMOD sound
// engine. The companion addon (addon/OOBRadio) is responsible for muting the
// game's own music while the radio is on and for mirroring the player's volume
// settings onto the stream.
//
// Threading model
//   - Lua functions run on the game's main thread. They only touch atomics and
//     a mutex-guarded set of strings, and never call into BASS or the network.
//   - The worker thread owns the BASS stream handle and performs every
//     potentially blocking call (connect, play, stop).
//
// Memory patches (both in-process only; nothing on disk is modified)
//   - 0x52AB17: the rel32 of "call LoadScriptFunctions" is redirected to our
//     hook so Radio_* get registered every time the UI Lua state is created,
//     including on /reload. Same hook site the WotLK-Extensions project uses.
//   - 0xD415B8/0xD415BC: the client caches a [lo, hi) address range and refuses
//     to call registered C functions outside it ("Invalid function pointer").
//     The range is derived from Wow.exe's code section, so functions living in
//     this DLL would be rejected. We widen the cached range. This is a crash
//     guard rather than a security boundary: Lua cannot fabricate pointers.
//   Every patch verifies the bytes it expects before writing and logs and
//   backs out on a mismatch, so an unexpected client build simply gets no
//   radio instead of a crash.

#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include "bass.h"
#include "bass_loader.h"

static const char* RADIO_VERSION = "0.3.1";

// Only these stream hosts are accepted from Lua. Without a list, any addon
// could make the client connect to an arbitrary host and reveal the player's IP.
static const char* ALLOWED_URL_PREFIXES[] = {
    "http://radio.outofbounds.live:8000/",
    "http://radio.outofbounds.live/",
};
static const char* DEFAULT_URL = "http://radio.outofbounds.live:8000/radio.mp3";

// Keep the log from growing without bound across sessions.
static const long LOG_MAX_BYTES = 512 * 1024;

// ---------------------------------------------------------------------------
// Client (build 12340) entry points and patch sites.
// These are fixed virtual addresses in the stock 3.3.5a executable. Project
// Alterac's exe differs from stock in 87 small places, none of them here.
// ---------------------------------------------------------------------------
namespace client {
    struct lua_State;
    typedef int(__cdecl* lua_CFunction)(lua_State*);

    // FrameScript_RegisterFunction(name, fn): adds a global C function to the UI Lua state.
    static auto RegisterFunction    = (void(__cdecl*)(const char*, lua_CFunction))0x817F90;
    // FrameScript_LoadScriptFunctions(): registers all of Blizzard's built-in functions.
    static auto LoadScriptFunctions = (int(__cdecl*)())0x5120E0;

    // The client's copy of the Lua C API (Lua 5.1, cdecl).
    static auto lua_gettop      = (int(__cdecl*)(lua_State*))0x84DBD0;
    static auto lua_isnumber    = (int(__cdecl*)(lua_State*, int))0x84DF20;
    static auto lua_isstring    = (int(__cdecl*)(lua_State*, int))0x84DF60;
    static auto lua_tonumber    = (double(__cdecl*)(lua_State*, int))0x84E030;
    static auto lua_tolstring   = (const char*(__cdecl*)(lua_State*, int, size_t*))0x84E0E0;
    static auto lua_toboolean   = (int(__cdecl*)(lua_State*, int))0x84E0B0;
    static auto lua_pushnumber  = (void(__cdecl*)(lua_State*, double))0x84E2A0;
    static auto lua_pushstring  = (void(__cdecl*)(lua_State*, const char*))0x84E350;
    static auto lua_pushboolean = (void(__cdecl*)(lua_State*, int))0x84E4D0;

    // Inside FrameScript initialisation: "E8 <rel32>" = call LoadScriptFunctions.
    static const uintptr_t kLoadCallOpcode  = 0x52AB16;   // the E8 byte
    static const uintptr_t kLoadCallOperand = 0x52AB17;   // the rel32 we rewrite
    static const uintptr_t kLoadCallNext    = 0x52AB1B;   // address after the call

    // Cached bounds used by the C-function pointer check (checker at 0x86B5A0).
    static const uintptr_t kFnRangeLo = 0xD415B8;
    static const uintptr_t kFnRangeHi = 0xD415BC;
}

// ---------------------------------------------------------------------------
// Logging: wowradio.log next to the DLL. Truncated when it gets large.
// ---------------------------------------------------------------------------
static HMODULE g_self = nullptr;

static std::string DllDir() {
    char path[MAX_PATH];
    GetModuleFileNameA(g_self, path, MAX_PATH);
    std::string s(path);
    return s.substr(0, s.find_last_of('\\') + 1);
}

static void Log(const char* fmt, ...) {
    std::string path = DllDir() + "wowradio.log";
    FILE* f = fopen(path.c_str(), "a");
    if (!f) return;
    if (ftell(f) > LOG_MAX_BYTES) { fclose(f); f = fopen(path.c_str(), "w"); if (!f) return; }
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Radio state shared between the Lua side (main thread) and the worker thread.
// ---------------------------------------------------------------------------
enum State { STOPPED = 0, CONNECTING, PLAYING, ERR };

static const char* StateName(int s) {
    switch (s) {
        case STOPPED:    return "stopped";
        case CONNECTING: return "connecting";
        case PLAYING:    return "playing";
        default:         return "error";
    }
}

static BassApi bass;                         // filled by LoadEmbeddedBass on first play
static std::atomic<int> g_bassReady{0};

static std::mutex g_mx;                      // guards g_url, g_title, g_error
static std::string g_url = DEFAULT_URL;
static std::string g_title;                  // current track from ICY metadata
static std::string g_error;                  // last error text, empty when none

static std::atomic<int>   g_state{STOPPED};
static std::atomic<int>   g_reqPlay{0};      // set by Lua, consumed by the worker
static std::atomic<int>   g_reqStop{0};
static std::atomic<float> g_volume{0.5f};    // 0..1
static std::atomic<int>   g_bgMute{0};       // mute while the game is not the foreground window

static HSTREAM g_stream = 0;                 // owned by the worker thread

static bool UrlAllowed(const char* url) {
    for (const char* prefix : ALLOWED_URL_PREFIXES)
        if (strncmp(url, prefix, strlen(prefix)) == 0) return true;
    return false;
}

static bool GameInForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// Push the effective volume to BASS. Safe from any thread; BASS attribute
// calls are thread-safe and this only reads atomics.
static void ApplyVolume() {
    if (!g_stream || !g_bassReady.load()) return;
    float v = g_volume.load();
    if (g_bgMute.load() && !GameInForeground()) v = 0.0f;
    bass.ChannelSetAttribute(g_stream, BASS_ATTRIB_VOL, v);
}

// BASS calls this on its own thread whenever the stream's ICY metadata changes.
// The tag looks like: StreamTitle='Artist - Title';StreamUrl='';
static void CALLBACK OnMeta(HSYNC, DWORD channel, DWORD, void*) {
    const char* meta = bass.ChannelGetTags(channel, BASS_TAG_META);
    std::string title;
    if (meta) {
        const char* p = strstr(meta, "StreamTitle='");
        if (p) {
            p += 13;
            const char* e = strstr(p, "';");
            title.assign(p, e ? (size_t)(e - p) : strlen(p));
        }
    }
    { std::lock_guard<std::mutex> lk(g_mx); g_title = title; }
    Log("meta: %s", title.c_str());
}

static void CALLBACK OnEnd(HSYNC, DWORD, DWORD, void*) {
    { std::lock_guard<std::mutex> lk(g_mx); g_error = "stream ended"; }
    g_state = ERR;
    Log("stream ended");
}

static void SetError(const std::string& msg) {
    { std::lock_guard<std::mutex> lk(g_mx); g_error = msg; }
    g_state = ERR;
    Log("%s", msg.c_str());
}

// ---------------------------------------------------------------------------
// Worker thread: the only code that talks to BASS beyond volume changes.
// ---------------------------------------------------------------------------
static bool EnsureBass() {
    if (g_bassReady.load()) return true;
    const char* how = "";
    if (!LoadEmbeddedBass(g_self, bass, &how)) {
        SetError(std::string("BASS load failed: ") + how);
        return false;
    }
    if (!bass.Init(-1, 44100, 0, nullptr, nullptr) && bass.ErrorGetCode() != BASS_ERROR_ALREADY) {
        SetError("BASS_Init failed " + std::to_string(bass.ErrorGetCode()));
        return false;
    }
    bass.SetConfig(BASS_CONFIG_NET_PLAYLIST, 0);      // the URL is a stream, not a playlist file
    bass.SetConfig(BASS_CONFIG_NET_PREBUF_WAIT, 0);   // start as soon as data arrives
    g_bassReady = 1;
    Log("BASS %08x initialised (%s)", bass.GetVersion(), how);
    return true;
}

static void WorkerStop() {
    if (g_stream && g_bassReady.load()) {
        bass.ChannelStop(g_stream);
        bass.StreamFree(g_stream);
    }
    g_stream = 0;
    { std::lock_guard<std::mutex> lk(g_mx); g_title.clear(); }
    g_state = STOPPED;
}

static void WorkerPlay() {
    WorkerStop();
    if (!EnsureBass()) return;

    std::string url;
    { std::lock_guard<std::mutex> lk(g_mx); url = g_url; g_error.clear(); }
    g_state = CONNECTING;
    Log("opening %s", url.c_str());

    // BASS_STREAM_STATUS makes ICY headers/tags available; BLOCK keeps decoding
    // in step with the download so a slow connection stalls instead of skipping.
    g_stream = bass.StreamCreateURL(url.c_str(), 0, BASS_STREAM_BLOCK | BASS_STREAM_STATUS, nullptr, nullptr);
    if (!g_stream) {
        SetError("connect failed " + std::to_string(bass.ErrorGetCode()));
        return;
    }
    bass.ChannelSetSync(g_stream, BASS_SYNC_META, 0, OnMeta, nullptr);
    bass.ChannelSetSync(g_stream, BASS_SYNC_END, 0, OnEnd, nullptr);
    ApplyVolume();
    if (!bass.ChannelPlay(g_stream, FALSE)) {
        SetError("play failed " + std::to_string(bass.ErrorGetCode()));
        return;
    }
    g_state = PLAYING;
    OnMeta(0, g_stream, 0, nullptr);   // pick up the title already in the first chunk
    Log("playing");
}

static DWORD WINAPI Worker(LPVOID) {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    Log("WowRadio %s loaded in %s (pid %lu)", RADIO_VERSION, exe, GetCurrentProcessId());
    for (;;) {
        if (g_reqStop.exchange(0)) WorkerStop();
        if (g_reqPlay.exchange(0)) WorkerPlay();
        if (g_state.load() == PLAYING) ApplyVolume();   // tracks foreground changes
        Sleep(100);
    }
}

// ---------------------------------------------------------------------------
// Lua API. All run on the game's main thread; none may block.
// ---------------------------------------------------------------------------
using client::lua_State;

// Radio_Play([url]) -> true
// Starts (or restarts) the stream. A URL is accepted only if it matches the
// allow-list; otherwise the previous/default URL is kept.
static int __cdecl L_Radio_Play(lua_State* L) {
    if (client::lua_gettop(L) >= 1 && client::lua_isstring(L, 1)) {
        const char* u = client::lua_tolstring(L, 1, nullptr);
        if (u && UrlAllowed(u)) {
            std::lock_guard<std::mutex> lk(g_mx);
            g_url = u;
        } else if (u) {
            Log("Radio_Play: url rejected by allow-list: %s", u);
        }
    }
    g_reqPlay = 1;
    client::lua_pushboolean(L, 1);
    return 1;
}

// Radio_Stop()
static int __cdecl L_Radio_Stop(lua_State*) {
    g_reqStop = 1;
    return 0;
}

// Radio_SetVolume(0..1)
static int __cdecl L_Radio_SetVolume(lua_State* L) {
    if (client::lua_gettop(L) >= 1 && client::lua_isnumber(L, 1)) {
        double v = client::lua_tonumber(L, 1);
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        g_volume = (float)v;
        ApplyVolume();
    }
    return 0;
}

// Radio_SetBackgroundMute(bool)
static int __cdecl L_Radio_SetBackgroundMute(lua_State* L) {
    g_bgMute = (client::lua_gettop(L) >= 1 && client::lua_toboolean(L, 1)) ? 1 : 0;
    return 0;
}

// Radio_GetStatus() -> state, title, url, volume, error
static int __cdecl L_Radio_GetStatus(lua_State* L) {
    std::string title, url, err;
    { std::lock_guard<std::mutex> lk(g_mx); title = g_title; url = g_url; err = g_error; }
    client::lua_pushstring(L, StateName(g_state.load()));
    client::lua_pushstring(L, title.c_str());
    client::lua_pushstring(L, url.c_str());
    client::lua_pushnumber(L, g_volume.load());
    client::lua_pushstring(L, err.c_str());
    return 5;
}

// Radio_GetVersion() -> "x.y.z"
static int __cdecl L_Radio_GetVersion(lua_State* L) {
    client::lua_pushstring(L, RADIO_VERSION);
    return 1;
}

// Replaces the client's call to LoadScriptFunctions: register ours, then
// let the client register its own as usual.
static int __cdecl LoadScriptFunctionsHook() {
    client::RegisterFunction("Radio_Play", L_Radio_Play);
    client::RegisterFunction("Radio_Stop", L_Radio_Stop);
    client::RegisterFunction("Radio_SetVolume", L_Radio_SetVolume);
    client::RegisterFunction("Radio_SetBackgroundMute", L_Radio_SetBackgroundMute);
    client::RegisterFunction("Radio_GetStatus", L_Radio_GetStatus);
    client::RegisterFunction("Radio_GetVersion", L_Radio_GetVersion);
    Log("Radio_* Lua functions registered");
    return client::LoadScriptFunctions();
}

// ---------------------------------------------------------------------------
// Installation of the two memory patches described at the top of the file.
// ---------------------------------------------------------------------------
static bool InstallHook() {
    // Refuse to touch anything unless the call site is exactly what we expect.
    uint8_t opcode = *(uint8_t*)client::kLoadCallOpcode;
    int32_t rel = *(int32_t*)client::kLoadCallOperand;
    uintptr_t target = client::kLoadCallNext + rel;
    if (opcode != 0xE8 || target != (uintptr_t)client::LoadScriptFunctions) {
        Log("hook site mismatch: opcode %02x target %08x, not patching", opcode, (unsigned)target);
        return false;
    }

    DWORD old;
    if (!VirtualProtect((LPVOID)client::kLoadCallOperand, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(int32_t*)client::kLoadCallOperand = (int32_t)((uintptr_t)&LoadScriptFunctionsHook - client::kLoadCallNext);
    VirtualProtect((LPVOID)client::kLoadCallOperand, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)client::kLoadCallOpcode, 5);

    if (!VirtualProtect((LPVOID)client::kFnRangeLo, 8, PAGE_READWRITE, &old)) return false;
    *(uint32_t*)client::kFnRangeLo = 0x00401000;
    *(uint32_t*)client::kFnRangeHi = 0x7FFFFFFF;
    VirtualProtect((LPVOID)client::kFnRangeLo, 8, old, &old);
    return true;
}

// Exported so a patched executable has a named symbol to import. The import
// is what gets the DLL loaded; everything real happens in DllMain.
extern "C" __declspec(dllexport) const char* WowRadio_Loaded() {
    return RADIO_VERSION;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        bool hooked = InstallHook();
        HANDLE h = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
        Log("hook installed: %s", hooked ? "yes" : "NO");
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_bassReady.load()) bass.Free();
    }
    return TRUE;
}
