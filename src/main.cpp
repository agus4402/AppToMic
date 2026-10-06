#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <cwchar>
#include <set>
#include <string>
#include <vector>

#include "engine.h"
#include "resource.h"
#include "sessions.h"

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kSyncTimer = 1;
constexpr UINT kSyncIntervalMs = 3000;
constexpr wchar_t kSection[] = L"AppToMic";
constexpr wchar_t kNoMic[] = L"none";

HINSTANCE g_inst;
HWND g_dlg;
HICON g_iconBig;
HICON g_iconSmall;
HIMAGELIST g_images;
UINT g_taskbarCreated;
std::wstring g_iniPath;

Engine g_engine;
std::set<std::wstring> g_selected;    // lowercase exe names
std::vector<std::wstring> g_itemExes;  // exe of each list item
std::vector<DeviceInfo> g_mics;
std::wstring g_micId;  // empty = no mic
std::wstring g_error;
bool g_populating = false;

// ---------------------------------------------------------------- config

std::wstring IniGet(const wchar_t* key, const wchar_t* def) {
    wchar_t buf[4096];
    GetPrivateProfileStringW(kSection, key, def, buf, ARRAYSIZE(buf), g_iniPath.c_str());
    return buf;
}

void IniSet(const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(kSection, key, value.c_str(), g_iniPath.c_str());
}

int IniInt(const wchar_t* key, int def) {
    return GetPrivateProfileIntW(kSection, key, def, g_iniPath.c_str());
}

void SaveApps() {
    std::wstring joined;
    for (const auto& exe : g_selected) joined += (joined.empty() ? L"" : L"|") + exe;
    IniSet(L"Apps", joined);
}

void LoadConfig() {
    std::wstring apps = IniGet(L"Apps", L"");
    for (size_t start = 0; start < apps.size();) {
        size_t end = apps.find(L'|', start);
        if (end == std::wstring::npos) end = apps.size();
        if (end > start) g_selected.insert(apps.substr(start, end - start));
        start = end + 1;
    }
    g_micId = IniGet(L"Mic", L"");
    if (g_micId.empty()) g_micId = DefaultDeviceId(eCapture, eCommunications);
    if (g_micId == kNoMic) g_micId.clear();
}

// ---------------------------------------------------------------- UI helpers

float SliderGain(int id) {
    return static_cast<float>(SendDlgItemMessageW(g_dlg, id, TBM_GETPOS, 0, 0)) / 100.0f;
}

void UpdateSliderText(int sliderId, int textId) {
    wchar_t text[16];
    swprintf_s(text, L"%d%%", static_cast<int>(SendDlgItemMessageW(g_dlg, sliderId, TBM_GETPOS, 0, 0)));
    SetDlgItemTextW(g_dlg, textId, text);
}

void SetTrayTip(const wchar_t* tip) {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_dlg;
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, tip, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void UpdateStatus() {
    std::wstring text;
    if (!g_error.empty()) {
        text = g_error;
    } else if (!g_engine.Running()) {
        text = L"Detenido. En Discord/OBS elegí \"CABLE Output\" como micrófono.";
    } else {
        wchar_t buf[160];
        swprintf_s(buf, L"Enviando a CABLE Input: %zu app(s)%s.", g_engine.ActiveApps(),
                   g_engine.MicActive() ? L" + micrófono" : L"");
        text = buf;
        if (g_engine.DefaultOutputIsCable())
            text += L"\n⚠ Tu salida por defecto es CABLE Input: cambiala o habrá eco.";
    }
    SetDlgItemTextW(g_dlg, IDC_STATUS, text.c_str());
    SetDlgItemTextW(g_dlg, IDC_TOGGLE, g_engine.Running() ? L"Detener" : L"Iniciar");
    SetTrayTip(g_engine.Running() ? L"AppToMic - enviando" : L"AppToMic - detenido");
}

int AddIcon(const std::wstring& path) {
    SHFILEINFOW sfi = {};
    if (path.empty() || !SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON))
        SHGetFileInfoW(L".exe", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                       SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | SHGFI_SMALLICON);
    int index = sfi.hIcon ? ImageList_AddIcon(g_images, sfi.hIcon) : -1;
    if (sfi.hIcon) DestroyIcon(sfi.hIcon);
    return index;
}

void PopulateApps() {
    HWND list = GetDlgItem(g_dlg, IDC_LIST);
    g_populating = true;
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    ImageList_RemoveAll(g_images);
    g_itemExes.clear();

    auto apps = ListAudioApps(g_selected);
    std::sort(apps.begin(), apps.end(), [](const AudioApp& a, const AudioApp& b) {
        return _wcsicmp(a.title.c_str(), b.title.c_str()) < 0;
    });

    for (const auto& app : apps) {
        std::wstring text = app.title;
        if (!app.running) text += L"  (cerrada)";
        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = static_cast<int>(g_itemExes.size());
        item.pszText = &text[0];
        item.iImage = AddIcon(app.path);
        int index = ListView_InsertItem(list, &item);
        ListView_SetCheckState(list, index, g_selected.count(app.exe) ? TRUE : FALSE);
        g_itemExes.push_back(app.exe);
    }

    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
    g_populating = false;
}

void PopulateMics() {
    HWND combo = GetDlgItem(g_dlg, IDC_MIC);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    g_mics.clear();
    for (auto& d : ListDevices(eCapture))
        if (!wcsstr(d.name.c_str(), L"CABLE Output")) g_mics.push_back(std::move(d));

    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(Sin micrófono)"));
    int selected = 0;
    for (size_t i = 0; i < g_mics.size(); ++i) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(g_mics[i].name.c_str()));
        if (g_mics[i].id == g_micId) selected = static_cast<int>(i) + 1;
    }
    if (selected == 0) g_micId.clear();  // saved mic missing or was the cable itself
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
}

// ---------------------------------------------------------------- actions

void StartEngine() {
    g_error.clear();
    g_engine.SetAppGain(SliderGain(IDC_VOLAPP));
    g_engine.SetMicGain(SliderGain(IDC_VOLMIC));
    g_engine.SetMic(g_micId);
    if (g_engine.Start(g_error)) {
        g_engine.SyncApps(g_selected);
        SetTimer(g_dlg, kSyncTimer, kSyncIntervalMs, nullptr);
    }
    UpdateStatus();
}

void StopEngine() {
    KillTimer(g_dlg, kSyncTimer);
    g_engine.Stop();
    g_error.clear();
    UpdateStatus();
}

void ToggleEngine() {
    if (g_engine.Running()) StopEngine();
    else StartEngine();
}

void ShowMain() {
    ShowWindow(g_dlg, SW_SHOW);
    ShowWindow(g_dlg, SW_RESTORE);
    SetForegroundWindow(g_dlg);
}

void AddTrayIcon() {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_dlg;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_iconSmall;
    wcscpy_s(nid.szTip, L"AppToMic");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void Quit() {
    KillTimer(g_dlg, kSyncTimer);
    g_engine.Stop();
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_dlg;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    DestroyWindow(g_dlg);
}

void ShowTrayMenu() {
    enum { kOpen = 1, kToggle, kExit };
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kOpen, L"Abrir");
    AppendMenuW(menu, MF_STRING, kToggle, g_engine.Running() ? L"Detener" : L"Iniciar");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kExit, L"Salir");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_dlg);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_dlg, nullptr);
    DestroyMenu(menu);
    if (cmd == kOpen) ShowMain();
    else if (cmd == kToggle) ToggleEngine();
    else if (cmd == kExit) Quit();
}

void InitDialog(HWND dlg) {
    g_dlg = dlg;
    SendMessageW(dlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_iconBig));
    SendMessageW(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_iconSmall));

    HWND list = GetDlgItem(dlg, IDC_LIST);
    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    g_images = ImageList_Create(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                ILC_COLOR32 | ILC_MASK, 8, 8);
    ListView_SetImageList(list, g_images, LVSIL_SMALL);
    RECT rc;
    GetClientRect(list, &rc);
    LVCOLUMNW col = {LVCF_WIDTH};
    col.cx = rc.right - GetSystemMetrics(SM_CXVSCROLL);
    ListView_InsertColumn(list, 0, &col);

    for (int id : {IDC_VOLAPP, IDC_VOLMIC}) {
        SendDlgItemMessageW(dlg, id, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
        SendDlgItemMessageW(dlg, id, TBM_SETPAGESIZE, 0, 10);
    }

    LoadConfig();
    SendDlgItemMessageW(dlg, IDC_VOLAPP, TBM_SETPOS, TRUE, IniInt(L"AppVolume", 100));
    SendDlgItemMessageW(dlg, IDC_VOLMIC, TBM_SETPOS, TRUE, IniInt(L"MicVolume", 100));
    UpdateSliderText(IDC_VOLAPP, IDC_VOLAPP_TXT);
    UpdateSliderText(IDC_VOLMIC, IDC_VOLMIC_TXT);
    CheckDlgButton(dlg, IDC_AUTOSTART, IniInt(L"AutoStart", 0) ? BST_CHECKED : BST_UNCHECKED);

    PopulateMics();
    PopulateApps();
    AddTrayIcon();
    UpdateStatus();

    if (IsDlgButtonChecked(dlg, IDC_AUTOSTART)) StartEngine();
}

INT_PTR CALLBACK DialogProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG:
        InitDialog(dlg);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_REFRESH:
            PopulateMics();
            PopulateApps();
            return TRUE;
        case IDC_TOGGLE:
            ToggleEngine();
            return TRUE;
        case IDC_AUTOSTART:
            IniSet(L"AutoStart", IsDlgButtonChecked(dlg, IDC_AUTOSTART) ? L"1" : L"0");
            return TRUE;
        case IDC_MIC:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int sel = static_cast<int>(SendDlgItemMessageW(dlg, IDC_MIC, CB_GETCURSEL, 0, 0));
                g_micId = sel > 0 ? g_mics[sel - 1].id : L"";
                IniSet(L"Mic", g_micId.empty() ? kNoMic : g_micId);
                g_engine.SetMic(g_micId);
                UpdateStatus();
            }
            return TRUE;
        case IDCANCEL:  // Esc / close button: hide to tray
            ShowWindow(dlg, SW_HIDE);
            return TRUE;
        }
        break;

    case WM_HSCROLL: {
        HWND slider = reinterpret_cast<HWND>(lParam);
        if (slider == GetDlgItem(dlg, IDC_VOLAPP)) {
            g_engine.SetAppGain(SliderGain(IDC_VOLAPP));
            UpdateSliderText(IDC_VOLAPP, IDC_VOLAPP_TXT);
            IniSet(L"AppVolume", std::to_wstring(SendMessageW(slider, TBM_GETPOS, 0, 0)));
        } else if (slider == GetDlgItem(dlg, IDC_VOLMIC)) {
            g_engine.SetMicGain(SliderGain(IDC_VOLMIC));
            UpdateSliderText(IDC_VOLMIC, IDC_VOLMIC_TXT);
            IniSet(L"MicVolume", std::to_wstring(SendMessageW(slider, TBM_GETPOS, 0, 0)));
        }
        return TRUE;
    }

    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMLISTVIEW*>(lParam);
        if (nm->hdr.idFrom == IDC_LIST && nm->hdr.code == LVN_ITEMCHANGED && !g_populating &&
            (nm->uChanged & LVIF_STATE) && ((nm->uOldState ^ nm->uNewState) & LVIS_STATEIMAGEMASK) &&
            nm->iItem >= 0 && nm->iItem < static_cast<int>(g_itemExes.size())) {
            const std::wstring& exe = g_itemExes[nm->iItem];
            if (ListView_GetCheckState(nm->hdr.hwndFrom, nm->iItem)) g_selected.insert(exe);
            else g_selected.erase(exe);
            SaveApps();
            g_engine.SyncApps(g_selected);
            UpdateStatus();
        }
        break;
    }

    case WM_TIMER:
        if (wParam == kSyncTimer) {
            if (g_engine.RenderFailed()) {
                StopEngine();
                g_error = L"Se perdió el dispositivo CABLE Input. Tocá Iniciar para reintentar.";
                UpdateStatus();
            } else {
                g_engine.SyncApps(g_selected);
                UpdateStatus();
            }
        }
        return TRUE;

    case WM_TRAY:
        if (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_LBUTTONDBLCLK) ShowMain();
        else if (LOWORD(lParam) == WM_RBUTTONUP) ShowTrayMenu();
        return TRUE;

    case WM_DESTROY:
        PostQuitMessage(0);
        return TRUE;

    default:
        if (msg == g_taskbarCreated && msg != 0) {  // Explorer restarted
            AddTrayIcon();
            UpdateStatus();
            return TRUE;
        }
    }
    return FALSE;
}

void LoadAppIcons() {
    wchar_t path[MAX_PATH];
    ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\SndVol.exe", path, MAX_PATH);
    ExtractIconExW(path, 0, &g_iconBig, &g_iconSmall, 1);
    if (!g_iconBig) g_iconBig = LoadIconW(nullptr, IDI_APPLICATION);
    if (!g_iconSmall) g_iconSmall = LoadIconW(nullptr, IDI_APPLICATION);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int) {
    // Single instance: bring the existing window to front instead.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"AppToMic.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"#32770", L"AppToMic")) {
            ShowWindow(existing, SW_SHOW);
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    g_inst = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    g_iniPath = exePath;
    g_iniPath = g_iniPath.substr(0, g_iniPath.find_last_of(L'.')) + L".ini";

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    LoadAppIcons();

    HWND dlg = CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), nullptr, DialogProc, 0);
    if (!dlg) return 1;
    ShowWindow(dlg, wcsstr(cmdLine, L"/tray") ? SW_HIDE : SW_SHOW);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
