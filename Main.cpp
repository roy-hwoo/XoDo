#include "AppConfig.h"
#include "PinManager.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <array>
#include <string>
#include <unordered_map>

namespace {
constexpr UINT TrayMessage = WM_APP + 1;
constexpr UINT TrayId = 1;
constexpr UINT_PTR PinTimerId = 1;
constexpr int MenuStartup = 10, MenuSettings = 11, MenuExit = 12;
constexpr wchar_t RunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
HINSTANCE instance;
HWND mainWindow;
HICON appIcon;
UINT taskbarCreated;
AppConfig config;
PinManager pins;
std::wstring configPath;
UINT registeredHotkeys = 0;
bool hotkeysSuspended = false;
bool trayVisible = false;
struct Transparency { bool layered; COLORREF key; BYTE alpha; DWORD flags; int preset; };
std::unordered_map<HWND, Transparency> transparentWindows;

bool StartupEnabled();
bool AddTray();

void Notify(const wchar_t* text) {
    TASKDIALOGCONFIG dialog{sizeof(dialog)};
    dialog.hwndParent = mainWindow;
    dialog.hInstance = instance;
    dialog.dwFlags = TDF_USE_HICON_MAIN;
    dialog.dwCommonButtons = TDCBF_OK_BUTTON;
    dialog.pszWindowTitle = L"XoDo";
    dialog.pszMainInstruction = text;
    dialog.hMainIcon = appIcon;
    TaskDialogIndirect(&dialog, nullptr, nullptr, nullptr);
}

void LoadConfig() {
    PWSTR directory = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &directory))) {
        std::wstring folder = std::wstring(directory) + L"\\XoDo";
        CoTaskMemFree(directory);
        CreateDirectoryW(folder.c_str(), nullptr);
        configPath = folder + L"\\settings.ini";
    }
    if (configPath.empty()) return;
    config.alpha01 = GetPrivateProfileIntW(L"Window", L"Alpha01", 20, configPath.c_str());
    config.alpha02 = GetPrivateProfileIntW(L"Window", L"Alpha02", 128, configPath.c_str());
    config.alpha03 = GetPrivateProfileIntW(L"Window", L"Alpha03", 200, configPath.c_str());
    config.pinStyle = GetPrivateProfileIntW(L"Window", L"PinStyle", 0, configPath.c_str());
    if (config.alpha01 < 1 || config.alpha01 > 255) config.alpha01 = 20;
    if (config.alpha02 < 1 || config.alpha02 > 255) config.alpha02 = 128;
    if (config.alpha03 < 1 || config.alpha03 > 255) config.alpha03 = 200;
    if (config.pinStyle < 0 || config.pinStyle > 4) config.pinStyle = 0;

    const auto defaults = DefaultHotkeys();
    // 旧版只有 4 项（01、02、置顶、设置），仅在已存在旧版 ini（有 Key1 且无 Key5）时按旧顺序迁移，
    // 否则全新安装会因缺少 Key5 被误判为旧版配置。
    const bool legacyHotkeys =
        GetFileAttributesW(configPath.c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetPrivateProfileIntW(L"Hotkeys", L"Key5", -1, configPath.c_str()) == -1 &&
        GetPrivateProfileIntW(L"Hotkeys", L"Key1", -1, configPath.c_str()) != -1;
    for (int index = 0; index < 5; ++index) {
        const std::wstring suffix = std::to_wstring(index + 1);
        const int mod = GetPrivateProfileIntW(L"Hotkeys", (L"Mod" + suffix).c_str(),
            static_cast<int>(defaults[index].modifiers), configPath.c_str());
        const int key = GetPrivateProfileIntW(L"Hotkeys", (L"Key" + suffix).c_str(),
            static_cast<int>(defaults[index].key), configPath.c_str());
        config.hotkeys[index].modifiers = static_cast<UINT>(mod) &
            (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
        config.hotkeys[index].key = static_cast<UINT>(key);
    }
    if (legacyHotkeys) {
        config.hotkeys[4] = config.hotkeys[3];
        config.hotkeys[3] = config.hotkeys[2];
        config.hotkeys[2] = defaults[2];
    }
    config.hideOwnTray = GetPrivateProfileIntW(L"Tray", L"HideOwn", 0, configPath.c_str()) != 0;
    config.startup = StartupEnabled();
}
void SaveConfig() {
    if (configPath.empty()) return;
    bool saved = true;
    auto write = [&](const wchar_t* section, const wchar_t* name, const std::wstring& value) {
        saved &= WritePrivateProfileStringW(section, name, value.c_str(), configPath.c_str()) != FALSE;
    };
    write(L"Window", L"Alpha01", std::to_wstring(config.alpha01));
    write(L"Window", L"Alpha02", std::to_wstring(config.alpha02));
    write(L"Window", L"Alpha03", std::to_wstring(config.alpha03));
    write(L"Window", L"PinStyle", std::to_wstring(config.pinStyle));
    for (int index = 0; index < 5; ++index) {
        const std::wstring suffix = std::to_wstring(index + 1);
        write(L"Hotkeys", (L"Mod" + suffix).c_str(), std::to_wstring(config.hotkeys[index].modifiers));
        write(L"Hotkeys", (L"Key" + suffix).c_str(), std::to_wstring(config.hotkeys[index].key));
    }
    write(L"Tray", L"HideOwn", config.hideOwnTray ? L"1" : L"0");
    saved &= WritePrivateProfileStringW(L"Tray", L"Hidden", nullptr, configPath.c_str()) != FALSE;
    saved &= WritePrivateProfileStringW(L"Security", nullptr, nullptr, configPath.c_str()) != FALSE;
    // WritePrivateProfileString 有进程内缓存，进程被强制结束时会丢数据，这里显式落盘。
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, configPath.c_str());
    if (!saved) Notify(L"配置保存失败，请检查用户目录写入权限。");
}
std::wstring ExecutablePath() {
    wchar_t path[32768]{};
    DWORD count = GetModuleFileNameW(nullptr, path, 32768);
    return count && count < 32768 ? std::wstring(path, count) : std::wstring();
}
bool StartupEnabled() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    wchar_t value[32768]{};
    DWORD size = sizeof(value), type = 0;
    const auto status = RegQueryValueExW(key, L"XoDo", nullptr, &type, reinterpret_cast<BYTE*>(value), &size);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_SZ &&
        std::wstring(value) == L"\"" + ExecutablePath() + L"\"";
}
bool SetStartup(bool enabled) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RunKey, 0, nullptr, 0, KEY_SET_VALUE,
        nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    LSTATUS status;
    if (enabled) {
        const auto path = ExecutablePath();
        if (path.empty()) { RegCloseKey(key); return false; }
        const std::wstring command = L"\"" + path + L"\"";
        status = RegSetValueExW(key, L"XoDo", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, L"XoDo");
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}
bool AddTray() {
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = mainWindow;
    data.uID = TrayId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = TrayMessage;
    data.hIcon = appIcon;
    const std::wstring tip = config.hotkeys[4].key
        ? L"XoDo - " + HotkeyDisplay(config.hotkeys[4]) + L" 打开设置"
        : L"XoDo - 打开设置";
    if (tip.size() >= ARRAYSIZE(data.szTip)) wcscpy_s(data.szTip, L"XoDo");
    else wcscpy_s(data.szTip, tip.c_str());
    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        Notify(L"无法添加托盘图标。仍可使用“显示设置界面”快捷键打开设置。");
        return false;
    }
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    return true;
}
void RemoveTray() {
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = mainWindow;
    data.uID = TrayId;
    Shell_NotifyIconW(NIM_DELETE, &data);
}
// 只有“显示设置界面”快捷键确实注册成功，才允许隐藏自己的托盘，否则会彻底失去入口。
void UpdateTrayVisibility() {
    const bool visible = !(config.hideOwnTray && SettingsHotkeyAvailable());
    if (visible == trayVisible) return;
    if (visible) { if (AddTray()) trayVisible = true; }
    else { RemoveTray(); trayVisible = false; }
}
void UnregisterAllHotkeys() {
    for (int id = 1; id <= 5; ++id)
        if (registeredHotkeys & (1u << id)) {
            UnregisterHotKey(mainWindow, id);
            registeredHotkeys &= ~(1u << id);
        }
}
// 返回注册失败项的描述，空串表示全部成功。
std::wstring RegisterHotkeys(const std::array<HotkeyConfig, 5>& hotkeys) {
    static const wchar_t* labels[] = {
        L"窗口透明度 01", L"窗口透明度 02", L"窗口透明度 03",
        L"窗口置顶", L"显示设置界面"};
    std::wstring failures;
    for (int id = 1; id <= 5; ++id) {
        const HotkeyConfig& item = hotkeys[id - 1];
        // 快捷键允许留空，留空表示不注册该项。
        if (item.key == 0) continue;
        if (RegisterHotKey(mainWindow, id, item.modifiers | MOD_NOREPEAT, item.key))
            registeredHotkeys |= 1u << id;
        else
            failures += std::wstring(labels[id - 1]) + L"：" + HotkeyDisplay(item) + L" 已被占用或无效\n";
    }
    return failures;
}
HWND TargetWindow() {
    HWND target = GetAncestor(GetForegroundWindow(), GA_ROOT);
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    wchar_t name[256]{};
    GetClassNameW(target, name, 256);
    if (!target || !IsWindowVisible(target) || IsIconic(target) || pid == GetCurrentProcessId() ||
        target == GetShellWindow() || target == GetDesktopWindow() ||
        wcscmp(name, L"Shell_TrayWnd") == 0 || wcscmp(name, L"Shell_SecondaryTrayWnd") == 0 ||
        wcscmp(name, L"Progman") == 0 || wcscmp(name, L"WorkerW") == 0 ||
        (GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) {
        Notify(L"请先点击要操作的普通应用窗口，再按快捷键。");
        return nullptr;
    }
    return target;
}
bool RestoreTransparency(HWND target, const Transparency& original) {
    if (!IsWindow(target)) return true;
    if (original.layered)
        return SetLayeredWindowAttributes(target, original.key, original.alpha, original.flags) != FALSE;
    SetLastError(0);
    const LONG_PTR style = GetWindowLongPtrW(target, GWL_EXSTYLE);
    if (!SetWindowLongPtrW(target, GWL_EXSTYLE, style & ~WS_EX_LAYERED) && GetLastError()) return false;
    RedrawWindow(target, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    return true;
}
BYTE AlphaForPreset(int preset) {
    if (preset == 1) return static_cast<BYTE>(config.alpha01);
    if (preset == 2) return static_cast<BYTE>(config.alpha02);
    return static_cast<BYTE>(config.alpha03);
}
void ToggleTransparency(HWND target, int preset) {
    auto found = transparentWindows.find(target);
    if (found != transparentWindows.end() && found->second.preset == preset) {
        if (!RestoreTransparency(target, found->second)) Notify(L"无法恢复透明度，可能需要目标程序的访问权限。");
        else transparentWindows.erase(found);
        return;
    }
    Transparency original{};
    bool newlyAdded = found == transparentWindows.end();
    if (newlyAdded) {
        const LONG_PTR exStyle = GetWindowLongPtrW(target, GWL_EXSTYLE);
        original.layered = (exStyle & WS_EX_LAYERED) != 0;
        if (original.layered && !GetLayeredWindowAttributes(target, &original.key, &original.alpha, &original.flags)) {
            Notify(L"目标窗口使用独立分层绘制，无法安全修改并恢复其透明度。");
            return;
        }
        if (!original.layered) {
            SetLastError(0);
            if (!SetWindowLongPtrW(target, GWL_EXSTYLE, exStyle | WS_EX_LAYERED) && GetLastError()) {
                Notify(L"无法修改窗口透明度，可能需要目标程序的访问权限。");
                return;
            }
        }
    } else original = found->second;
    BYTE alpha = AlphaForPreset(preset);
    if (!SetLayeredWindowAttributes(target, original.key, alpha,
        LWA_ALPHA | (original.flags & LWA_COLORKEY))) {
        if (newlyAdded) RestoreTransparency(target, original);
        Notify(L"设置透明度失败。");
        return;
    }
    original.preset = preset;
    transparentWindows[target] = original;
}
void TrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (StartupEnabled() ? MF_CHECKED : MF_UNCHECKED), MenuStartup, L"开机自启");
    AppendMenuW(menu, MF_STRING, MenuSettings, L"设置");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, MenuExit, L"退出");
    POINT cursor;
    GetCursorPos(&cursor);
    SetForegroundWindow(mainWindow);
    UINT selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        cursor.x, cursor.y, 0, mainWindow, nullptr);
    DestroyMenu(menu);
    if (selected) PostMessageW(mainWindow, WM_COMMAND, selected, 0);
    PostMessageW(mainWindow, WM_NULL, 0, 0);
}
LRESULT CALLBACK MainProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (taskbarCreated && message == taskbarCreated) {
        // 资源管理器重启后托盘图标被清空，这里按当前配置重建。
        trayVisible = false;
        UpdateTrayVisibility();
        return 0;
    }
    switch (message) {
    case TrayMessage:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK || LOWORD(lp) == NIN_KEYSELECT)
            ShowSettings(instance, hwnd, config);
        else if (LOWORD(lp) == WM_CONTEXTMENU) TrayMenu();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == MenuSettings) ShowSettings(instance, hwnd, config);
        else if (LOWORD(wp) == MenuExit) DestroyWindow(hwnd);
        else if (LOWORD(wp) == MenuStartup) {
            const bool enable = !AppStartupEnabled();
            if (!SetStartup(enable)) Notify(L"开机自启设置失败。");
            else config.startup = enable;
        }
        return 0;
    case WM_HOTKEY:
        if (wp == 5) {
            // 设置窗口已打开时再次按下该快捷键则关闭，形成开关式切换。
            if (IsSettingsOpen()) CloseSettings();
            else ShowSettings(instance, hwnd, config);
            return 0;
        }
        if (HWND target = TargetWindow()) {
            if (wp >= 1 && wp <= 3) ToggleTransparency(target, static_cast<int>(wp));
            else if (wp == 4) {
                if (!pins.Toggle(target)) Notify(L"置顶操作失败，可能需要目标程序的访问权限。");
                pins.Refresh(config.pinStyle);
            }
        }
        return 0;
    case WM_TIMER:
        [[fallthrough]];
    case WM_XODO_REFRESH_PINS:
        pins.Refresh(config.pinStyle);
        for (auto it = transparentWindows.begin(); it != transparentWindows.end();)
            if (!IsWindow(it->first)) it = transparentWindows.erase(it); else ++it;
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) DestroyWindow(hwnd);
        return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        KillTimer(hwnd, PinTimerId);
        CloseSettings();
        pins.Shutdown();
        for (const auto& entry : transparentWindows) RestoreTransparency(entry.first, entry.second);
        transparentWindows.clear();
        UnregisterAllHotkeys();
        RemoveTray();
        trayVisible = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
}

std::wstring HotkeyDisplay(const HotkeyConfig& hotkey) {
    if (hotkey.key == 0) return L"未设置";
    std::wstring text;
    if (hotkey.modifiers & MOD_CONTROL) text += L"Ctrl+";
    if (hotkey.modifiers & MOD_SHIFT) text += L"Shift+";
    if (hotkey.modifiers & MOD_ALT) text += L"Alt+";
    if (hotkey.modifiers & MOD_WIN) text += L"Win+";
    const UINT key = hotkey.key;
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) text += static_cast<wchar_t>(key);
    else if (key >= VK_F1 && key <= VK_F24) text += L"F" + std::to_wstring(key - VK_F1 + 1);
    else {
        wchar_t name[64]{};
        const UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
        if (scan && GetKeyNameTextW(static_cast<LONG>(scan) << 16, name, 64) > 0) text += name;
        else text += L"键码 " + std::to_wstring(key);
    }
    return text;
}

bool SettingsHotkeyAvailable() {
    return (registeredHotkeys & (1u << 5)) != 0;
}

// 设置窗口处于前台时暂停全局快捷键，否则在快捷键输入框里按下的组合键
// 会被本程序抢先响应，导致无法录入。“显示设置界面”快捷键保留注册，
// 以便再次按下时关闭设置窗口。
void SuspendGlobalHotkeys() {
    hotkeysSuspended = true;
    for (int id = 1; id <= 4; ++id)
        if (registeredHotkeys & (1u << id)) {
            UnregisterHotKey(mainWindow, id);
            registeredHotkeys &= ~(1u << id);
        }
}

void SuspendSettingsHotkey() {
    if (!(registeredHotkeys & (1u << 5))) return;
    UnregisterHotKey(mainWindow, 5);
    registeredHotkeys &= ~(1u << 5);
}

void ResumeSettingsHotkey() {
    if (registeredHotkeys & (1u << 5)) return;
    const HotkeyConfig& item = config.hotkeys[4];
    if (item.key != 0 &&
        RegisterHotKey(mainWindow, 5, item.modifiers | MOD_NOREPEAT, item.key))
        registeredHotkeys |= 1u << 5;
}

void ResumeGlobalHotkeys() {
    if (!hotkeysSuspended) return;
    hotkeysSuspended = false;
    UnregisterAllHotkeys();
    RegisterHotkeys(config.hotkeys);
    UpdateTrayVisibility();
}

bool AppStartupEnabled() {
    return StartupEnabled();
}

bool ApplyAppConfig(const AppConfig& candidate, std::wstring& error) {
    if (candidate.alpha01 < 1 || candidate.alpha01 > 255 ||
        candidate.alpha02 < 1 || candidate.alpha02 > 255 ||
        candidate.alpha03 < 1 || candidate.alpha03 > 255) {
        error = L"窗口透明度取值必须在 1 到 255 之间。";
        return false;
    }
    if (candidate.pinStyle < 0 || candidate.pinStyle > 4) {
        error = L"图钉样式无效，请在“窗口置顶”页重新选择。";
        return false;
    }
    for (size_t index = 0; index < candidate.hotkeys.size(); ++index) {
        // 快捷键允许留空，只对已设置的快捷键检查重复。
        if (candidate.hotkeys[index].key == 0) continue;
        for (size_t other = index + 1; other < candidate.hotkeys.size(); ++other)
            if (candidate.hotkeys[index].key == candidate.hotkeys[other].key &&
                candidate.hotkeys[index].modifiers == candidate.hotkeys[other].modifiers) {
                error = L"快捷键 " + HotkeyDisplay(candidate.hotkeys[index]) +
                        L" 被重复使用，请为每一项分配不同的组合键。";
                return false;
            }
    }

    // 先试注册新热键，任何一项失败都回滚到旧配置，避免出现半可用状态。
    const std::array<HotkeyConfig, 5> previous = config.hotkeys;
    UnregisterAllHotkeys();
    const std::wstring failures = RegisterHotkeys(candidate.hotkeys);
    // 隐藏自身托盘的前提是“显示设置界面”快捷键可用，否则用户会失去打开设置的入口。
    const bool trayBlocked = candidate.hideOwnTray && !SettingsHotkeyAvailable();
    if (!failures.empty() || trayBlocked) {
        UnregisterAllHotkeys();
        RegisterHotkeys(previous);
        if (!failures.empty())
            error = L"以下快捷键无法注册，配置未生效：\n" + failures +
                    (trayBlocked ? L"因此本程序托盘图标也不能隐藏。" : L"");
        else
            error = L"“显示设置界面”快捷键未注册成功，为防止无法打开设置，不能隐藏本程序托盘图标。";
        return false;
    }
    if (!SetStartup(candidate.startup)) {
        UnregisterAllHotkeys();
        RegisterHotkeys(previous);
        error = L"开机自启写入注册表失败，请检查权限。";
        return false;
    }

    config = candidate;
    SaveConfig();
    UpdateTrayVisibility();
    for (const auto& entry : transparentWindows)
        if (IsWindow(entry.first))
            SetLayeredWindowAttributes(entry.first, entry.second.key,
                AlphaForPreset(entry.second.preset),
                LWA_ALPHA | (entry.second.flags & LWA_COLORKEY));
    pins.Refresh(config.pinStyle);
    return true;
}

int WINAPI wWinMain(HINSTANCE app, HINSTANCE, PWSTR, int) {
    instance = app;
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\Roy.XoDo.SingleInstance");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"XoDo.Main", L"XoDo");
        if (existing) {
            DWORD processId = 0;
            GetWindowThreadProcessId(existing, &processId);
            AllowSetForegroundWindow(processId);
            PostMessageW(existing, WM_COMMAND, MenuSettings, 0);
        }
        CloseHandle(mutex);
        return 0;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls),
        ICC_TAB_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES |
        ICC_HOTKEY_CLASS};
    InitCommonControlsEx(&controls);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    appIcon = static_cast<HICON>(LoadImageW(app, MAKEINTRESOURCEW(IDI_XODO), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    WNDCLASSW wc{};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = app;
    wc.hIcon = appIcon;
    wc.lpszClassName = L"XoDo.Main";
    RegisterClassW(&wc);
    // A hidden top-level window receives Explorer restart broadcasts; message-only windows do not.
    mainWindow = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"XoDo", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, app, nullptr);
    if (!mainWindow || !pins.Initialize(app, mainWindow)) {
        Notify(L"XoDo 初始化失败。");
        if (mainWindow) DestroyWindow(mainWindow);
        DestroyIcon(appIcon);
        CoUninitialize();
        CloseHandle(mutex);
        return 1;
    }
    LoadConfig();
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (!configPath.empty() && GetPrivateProfileIntW(L"App", L"Initialized", 0, configPath.c_str()) == 0) {
        // 仅首次运行套用默认值，之后每次启动都以用户的显式设置为准。
        if (!SetStartup(true)) Notify(L"首次运行默认启用开机自启失败，可稍后通过托盘菜单设置。");
        else config.startup = true;
        WritePrivateProfileStringW(L"App", L"Initialized", L"1", configPath.c_str());
        SaveConfig();
    }
    // 启动时保持宽松：个别快捷键被占用不影响其余功能，只提示一次。
    if (const std::wstring failures = RegisterHotkeys(config.hotkeys); !failures.empty()) {
        const std::wstring warning =
            L"以下快捷键注册失败（可能被其他软件占用）：\n" + failures + L"仍可通过托盘打开设置或退出。";
        Notify(warning.c_str());
    }
    UpdateTrayVisibility();
    SetTimer(mainWindow, PinTimerId, 250, nullptr);
    MSG message{};
    int result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (!HandleSettingsMessage(message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    DestroyIcon(appIcon);
    CoUninitialize();
    CloseHandle(mutex);
    return result == -1 ? 1 : static_cast<int>(message.wParam);
}
