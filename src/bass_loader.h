// Loads the copy of bass.dll that is embedded in WowRadio.dll as a resource,
// so the radio ships as a single file. Loading is from memory (MemoryModule);
// if that ever fails, the DLL is written to a per-user cache folder and loaded
// with LoadLibrary instead.
//
// Only the BASS entry points the radio actually uses are resolved.
#pragma once
#include <windows.h>
#include "bass.h"

struct BassApi {
    decltype(&BASS_Init)                Init;
    decltype(&BASS_Free)                Free;
    decltype(&BASS_SetConfig)           SetConfig;
    decltype(&BASS_GetVersion)          GetVersion;
    decltype(&BASS_ErrorGetCode)        ErrorGetCode;
    // BASS_StreamCreateURL is overloaded (char/WCHAR) in bass.h, so the char* type is spelled out.
    HSTREAM (WINAPI* StreamCreateURL)(const char*, DWORD, DWORD, DOWNLOADPROC*, void*);
    decltype(&BASS_StreamFree)          StreamFree;
    decltype(&BASS_ChannelPlay)         ChannelPlay;
    decltype(&BASS_ChannelStop)         ChannelStop;
    decltype(&BASS_ChannelSetSync)      ChannelSetSync;
    decltype(&BASS_ChannelSetAttribute) ChannelSetAttribute;
    decltype(&BASS_ChannelGetTags)      ChannelGetTags;
};

// Fills `api` and returns true on success. `how` receives a short note on
// which loading path was used (or what failed), for the log.
bool LoadEmbeddedBass(HMODULE self, BassApi& api, const char** how);
