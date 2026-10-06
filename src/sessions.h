#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <set>
#include <string>
#include <vector>

struct AudioApp {
    std::wstring exe;    // lowercase, e.g. "spotify.exe" (the selection key)
    std::wstring path;   // full image path, empty if not running
    std::wstring title;  // "Spotify", "Google Chrome", ...
    bool running = false;
};

struct DeviceInfo {
    std::wstring id;
    std::wstring name;
};

// Apps that currently have an audio session, plus the exe names in `alsoInclude`
// (so saved selections show up even when silent or closed).
std::vector<AudioApp> ListAudioApps(const std::set<std::wstring>& alsoInclude);

// Top-most PIDs of each running exe in `exeNames` (lowercase): processes whose
// parent is not the same exe. Capturing them with the process tree covers children.
std::vector<DWORD> FindRootPids(const std::set<std::wstring>& exeNames);

std::vector<DeviceInfo> ListDevices(EDataFlow flow);
std::wstring DefaultDeviceId(EDataFlow flow, ERole role);
std::wstring FindDeviceByName(EDataFlow flow, const wchar_t* nameContains);
