#include "bass_loader.h"
#include "resource.h"
#include <shlobj.h>
#include <string>
#include <cstdio>
extern "C" {
#include "MemoryModule.h"
}

static HMEMORYMODULE g_mem = nullptr;   // set when loaded from memory
static HMODULE g_disk = nullptr;        // set when loaded via the disk fallback

static FARPROC Sym(const char* name) {
    if (g_mem) return MemoryGetProcAddress(g_mem, name);
    if (g_disk) return GetProcAddress(g_disk, name);
    return nullptr;
}

// Fallback path: write the embedded DLL to %LOCALAPPDATA%\WowRadio\bass.dll
// and load it the normal way.
static HMODULE LoadFromDisk(const void* data, DWORD size) {
    char base[MAX_PATH];
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))) return nullptr;
    std::string dir = std::string(base) + "\\WowRadio";
    CreateDirectoryA(dir.c_str(), nullptr);
    std::string path = dir + "\\bass.dll";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return nullptr;
    fwrite(data, 1, size, f);
    fclose(f);
    return LoadLibraryA(path.c_str());
}

bool LoadEmbeddedBass(HMODULE self, BassApi& api, const char** how) {
    *how = "";
    HRSRC res = FindResourceA(self, MAKEINTRESOURCEA(IDR_BASS_DLL), (LPCSTR)RT_RCDATA);
    if (!res) { *how = "resource not found"; return false; }
    HGLOBAL h = LoadResource(self, res);
    const void* data = h ? LockResource(h) : nullptr;
    DWORD size = SizeofResource(self, res);
    if (!data || !size) { *how = "resource empty"; return false; }

    g_mem = MemoryLoadLibrary(data, size);
    if (g_mem) {
        *how = "in-memory";
    } else {
        g_disk = LoadFromDisk(data, size);
        if (!g_disk) { *how = "memory load failed and disk fallback failed"; return false; }
        *how = "extracted to %LOCALAPPDATA%\\WowRadio";
    }

    // Resolve each entry point; a missing one means a wrong/mismatched bass.dll.
#define BIND(name) api.name = (decltype(api.name))Sym("BASS_" #name); if (!api.name) { *how = "missing BASS_" #name; return false; }
    BIND(Init) BIND(Free) BIND(SetConfig) BIND(GetVersion) BIND(ErrorGetCode)
    BIND(StreamCreateURL) BIND(StreamFree) BIND(ChannelPlay) BIND(ChannelStop)
    BIND(ChannelSetSync) BIND(ChannelSetAttribute) BIND(ChannelGetTags)
#undef BIND
    return true;
}
