#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <tlhelp32.h>
#include <cwchar>
#include <map>

#include "sessions.h"

namespace {

struct ProcInfo {
    std::wstring exe;  // lowercase
    DWORD parent;
};

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], static_cast<DWORD>(s.size()));
    return s;
}

std::map<DWORD, ProcInfo> SnapshotProcesses() {
    std::map<DWORD, ProcInfo> table;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return table;
    PROCESSENTRY32W pe = {sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        table[pe.th32ProcessID] = {Lower(pe.szExeFile), pe.th32ParentProcessID};
    CloseHandle(snap);
    return table;
}

bool IsRoot(DWORD pid, const std::map<DWORD, ProcInfo>& table) {
    auto it = table.find(pid);
    if (it == table.end()) return true;
    auto parent = table.find(it->second.parent);
    return parent == table.end() || parent->first == pid || parent->second.exe != it->second.exe;
}

DWORD RootOf(DWORD pid, const std::map<DWORD, ProcInfo>& table) {
    for (int guard = 0; guard < 64 && !IsRoot(pid, table); ++guard)
        pid = table.at(pid).parent;
    return pid;
}

std::wstring ProcessPath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD len = ARRAYSIZE(buf);
    std::wstring path = QueryFullProcessImageNameW(h, 0, buf, &len) ? buf : L"";
    CloseHandle(h);
    return path;
}

std::wstring FileDescription(const std::wstring& path) {
    DWORD ignored;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    struct Translation { WORD lang, codepage; }* tr;
    UINT len;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &len) ||
        len < sizeof(Translation))
        return {};
    wchar_t key[64];
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->lang, tr->codepage);
    wchar_t* value;
    if (VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&value), &len) && len > 1) return value;
    return {};
}

AudioApp DescribeApp(const std::wstring& exe, DWORD pid) {
    AudioApp app;
    app.exe = exe;
    app.running = pid != 0;
    if (pid) app.path = ProcessPath(pid);
    if (!app.path.empty()) app.title = FileDescription(app.path);
    if (app.title.empty()) {
        app.title = exe;
        if (app.title.size() > 4 && app.title.compare(app.title.size() - 4, 4, L".exe") == 0)
            app.title.resize(app.title.size() - 4);
    }
    return app;
}

IMMDeviceEnumerator* CreateEnumerator() {
    IMMDeviceEnumerator* e = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e));
    return e;
}

std::wstring FriendlyName(IMMDevice* device) {
    std::wstring name;
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
            name = pv.pwszVal;
        PropVariantClear(&pv);
        props->Release();
    }
    return name;
}

std::wstring DeviceId(IMMDevice* device) {
    std::wstring id;
    LPWSTR raw = nullptr;
    if (SUCCEEDED(device->GetId(&raw))) {
        id = raw;
        CoTaskMemFree(raw);
    }
    return id;
}

// PIDs that own an audio session on any active output device.
std::set<DWORD> SessionPids() {
    std::set<DWORD> pids;
    IMMDeviceEnumerator* enumerator = CreateEnumerator();
    if (!enumerator) return pids;
    IMMDeviceCollection* devices = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) {
        UINT count = 0;
        devices->GetCount(&count);
        for (UINT d = 0; d < count; ++d) {
            IMMDevice* device = nullptr;
            IAudioSessionManager2* manager = nullptr;
            IAudioSessionEnumerator* sessions = nullptr;
            if (SUCCEEDED(devices->Item(d, &device)) &&
                SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(&manager))) &&
                SUCCEEDED(manager->GetSessionEnumerator(&sessions))) {
                int n = 0;
                sessions->GetCount(&n);
                for (int i = 0; i < n; ++i) {
                    IAudioSessionControl* control = nullptr;
                    IAudioSessionControl2* control2 = nullptr;
                    if (SUCCEEDED(sessions->GetSession(i, &control)) &&
                        SUCCEEDED(control->QueryInterface(IID_PPV_ARGS(&control2))) &&
                        control2->IsSystemSoundsSession() != S_OK) {
                        DWORD pid = 0;
                        control2->GetProcessId(&pid);
                        if (pid) pids.insert(pid);
                    }
                    if (control2) control2->Release();
                    if (control) control->Release();
                }
            }
            if (sessions) sessions->Release();
            if (manager) manager->Release();
            if (device) device->Release();
        }
        devices->Release();
    }
    enumerator->Release();
    return pids;
}

}  // namespace

std::vector<AudioApp> ListAudioApps(const std::set<std::wstring>& alsoInclude) {
    auto table = SnapshotProcesses();
    std::map<std::wstring, DWORD> found;  // exe -> root pid (0 = not running)

    for (DWORD pid : SessionPids()) {
        if (pid == GetCurrentProcessId()) continue;
        DWORD root = RootOf(pid, table);
        auto it = table.find(root);
        if (it == table.end() || it->second.exe == L"audiodg.exe") continue;
        found.emplace(it->second.exe, root);
    }
    for (const auto& exe : alsoInclude) {
        if (found.count(exe)) continue;
        DWORD pid = 0;
        for (const auto& [p, info] : table)
            if (info.exe == exe && IsRoot(p, table)) { pid = p; break; }
        found.emplace(exe, pid);
    }

    std::vector<AudioApp> apps;
    for (const auto& [exe, pid] : found) apps.push_back(DescribeApp(exe, pid));
    return apps;
}

std::vector<DWORD> FindRootPids(const std::set<std::wstring>& exeNames) {
    std::vector<DWORD> pids;
    if (exeNames.empty()) return pids;
    auto table = SnapshotProcesses();
    for (const auto& [pid, info] : table)
        if (exeNames.count(info.exe) && pid != GetCurrentProcessId() && IsRoot(pid, table))
            pids.push_back(pid);
    return pids;
}

std::vector<DeviceInfo> ListDevices(EDataFlow flow) {
    std::vector<DeviceInfo> list;
    IMMDeviceEnumerator* enumerator = CreateEnumerator();
    if (!enumerator) return list;
    IMMDeviceCollection* devices = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &devices))) {
        UINT count = 0;
        devices->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (SUCCEEDED(devices->Item(i, &device))) {
                list.push_back({DeviceId(device), FriendlyName(device)});
                device->Release();
            }
        }
        devices->Release();
    }
    enumerator->Release();
    return list;
}

std::wstring DefaultDeviceId(EDataFlow flow, ERole role) {
    std::wstring id;
    IMMDeviceEnumerator* enumerator = CreateEnumerator();
    if (!enumerator) return id;
    IMMDevice* device = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, role, &device))) {
        id = DeviceId(device);
        device->Release();
    }
    enumerator->Release();
    return id;
}

std::wstring FindDeviceByName(EDataFlow flow, const wchar_t* nameContains) {
    for (const auto& d : ListDevices(flow))
        if (wcsstr(d.name.c_str(), nameContains)) return d.id;
    return {};
}
