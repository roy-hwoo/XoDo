#include "AppConfig.h"

#include <windowsx.h>
#include <commctrl.h>

#include <array>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "Comctl32.lib")

namespace {
constexpr wchar_t kWindowClass[] = L"XoDo.SettingsWindow";
constexpr int kPageCount = 5;

// 设置窗口按客户区 650x450 设计，实际整窗尺寸由 DPI 反推非客户区得到。
constexpr int kClientWidth = 650;
constexpr int kClientHeight = 450;
constexpr DWORD kWindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
constexpr DWORD kWindowExStyle = WS_EX_CONTROLPARENT;

constexpr int kTab = 1000;
constexpr int kSave = IDOK;
constexpr int kClose = IDCANCEL;
constexpr int kTrack01 = 1101;
constexpr int kTrack02 = 1102;
constexpr int kTrack03 = 1103;
constexpr int kOwnTray = 1203;
constexpr int kPinRadioFirst = 1301;
constexpr int kHotkeyFirst = 1401;
constexpr int kResetHotkeys = 1410;
constexpr int kStartup = 1505;

constexpr UINT_PTR kPageBrushTimerId = 71;
constexpr int kPageBrushMaxAttempts = 10;

struct SettingsState {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    AppConfig* config = nullptr;
    HWND tab = nullptr;
    HWND save = nullptr;
    HWND close = nullptr;
    HFONT font = nullptr;
    UINT dpi = 96;
    HBRUSH pageBrush = nullptr;
    int pageBrushAttempts = 0;
    std::array<std::vector<HWND>, kPageCount> pages;

    // 页 0：窗口透明度
    HWND track01 = nullptr, track02 = nullptr, track03 = nullptr;
    HWND track01Value = nullptr, track02Value = nullptr, track03Value = nullptr;

    // 页 1：窗口置顶
    std::array<HWND, 5> pinPreviews{};
    std::array<HWND, 5> pinRadios{};
    std::array<HICON, 5> pinIcons{};

    // 页 2：快捷键
    std::array<HWND, 5> hotkeyLabels{};
    std::array<HWND, 5> hotkeys{};
    HWND resetHotkeys = nullptr;
    bool dialogOpen = false;

    // 页 3：配置
    HWND ownTray = nullptr;
    HWND startupCheck = nullptr;

    // 页 4：关于
    HWND aboutIcon = nullptr;
    HICON aboutIconHandle = nullptr;
};

SettingsState g_settings;

int Scale(int value) {
    return MulDiv(value, static_cast<int>(g_settings.dpi), 96);
}

UINT WindowDpi(HWND window) {
    const UINT dpi = GetDpiForWindow(window);
    return dpi ? dpi : 96;
}

// 按目标 DPI 换算出整窗尺寸，保证客户区始终为设计尺寸的等比缩放。
SIZE SettingsWindowSize(UINT dpi) {
    const int scaleDpi = dpi ? static_cast<int>(dpi) : 96;
    RECT rect{0, 0, MulDiv(kClientWidth, scaleDpi, 96), MulDiv(kClientHeight, scaleDpi, 96)};
    AdjustWindowRectExForDpi(&rect, kWindowStyle, FALSE, kWindowExStyle, scaleDpi);
    return SIZE{rect.right - rect.left, rect.bottom - rect.top};
}

HWND AddControl(const wchar_t* className, const wchar_t* text, DWORD style,
                int id = 0, int page = -1, DWORD extendedStyle = 0) {
    HWND control = CreateWindowExW(extendedStyle, className, text,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, 0, 0, 0, 0, g_settings.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_settings.instance, nullptr);
    if (control && page >= 0 && page < kPageCount) g_settings.pages[page].push_back(control);
    return control;
}

void UpdateFont() {
    HFONT font = CreateFontW(-Scale(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HFONT oldFont = g_settings.font;
    g_settings.font = font;
    HFONT controlFont = font ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    for (HWND child = GetWindow(g_settings.window, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(controlFont), TRUE);
    }
    if (oldFont) DeleteObject(oldFont);
}

void Place(HWND window, int x, int y, int width, int height) {
    if (window) MoveWindow(window, x, y, width, height, TRUE);
}

// 标签页正文是主题的半透明白叠加在窗口背景上，主题 API 取不到最终颜色，
// 只能在窗口真正绘制完成后用 PrintWindow 取样；过早取样会得到全黑图。
bool SampleTabBodyColor(COLORREF& color) {
    RECT rect{};
    if (!g_settings.tab || !GetClientRect(g_settings.tab, &rect) ||
        rect.right <= 32 || rect.bottom <= 32) return false;

    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, rect.right, rect.bottom);
    ReleaseDC(nullptr, screen);
    if (!memory || !bitmap) {
        if (memory) DeleteDC(memory);
        if (bitmap) DeleteObject(bitmap);
        return false;
    }

    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
    COLORREF sample = CLR_INVALID;
    const BOOL printed = PrintWindow(g_settings.tab, memory, PW_RENDERFULLCONTENT);
    if (printed) {
        // 取正文右下角的空白处，不会被子控件覆盖。
        sample = GetPixel(memory, rect.right - 30, rect.bottom - 30);
    }
    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    if (!printed || sample == CLR_INVALID || sample == RGB(0, 0, 0)) return false;
    color = sample;
    return true;
}

void ApplyPageBrush(COLORREF color) {
    if (g_settings.pageBrush) DeleteObject(g_settings.pageBrush);
    g_settings.pageBrush = CreateSolidBrush(color);
    // 滑轨不会在重绘时取父窗口的画刷，必须显式设置背景色，否则一直是灰色块。
    for (HWND track : {g_settings.track01, g_settings.track02, g_settings.track03}) {
        if (!track) continue;
        SendMessageW(track, 0x0413 /* TBM_SETBKCOLOR */, 0, static_cast<LPARAM>(color));
        InvalidateRect(track, nullptr, TRUE);
    }
    if (g_settings.window) {
        RedrawWindow(g_settings.window, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
}

// ---------------------------------------------------------------- 布局

void LayoutTransparency(int x, int y, int width) {
    auto& page = g_settings.pages[0];
    Place(page[0], x, y, width, Scale(24));
    for (int row = 0; row < 3; ++row) {
        const int rowY = y + Scale(44 + row * 54);
        const int index = 1 + row * 3;
        Place(page[index], x, rowY + Scale(6), Scale(110), Scale(24));
        Place(page[index + 1], x + Scale(120), rowY, width - Scale(120 + 190), Scale(34));
        Place(page[index + 2], x + width - Scale(180), rowY + Scale(6), Scale(180), Scale(24));
    }
}

void LayoutPins(int x, int y, int width) {
    auto& page = g_settings.pages[1];
    Place(page[0], x, y, width, Scale(24));
    const int gap = Scale(10);
    const int cellWidth = (width - gap * 4) / 5;
    const int iconSize = Scale(32);
    const int cellTop = y + Scale(42);
    for (int index = 0; index < 5; ++index) {
        const int cellX = x + index * (cellWidth + gap);
        // 图钉图标在单元格内水平居中，和下方单选框对齐。
        Place(g_settings.pinPreviews[index], cellX + (cellWidth - iconSize) / 2,
              cellTop, iconSize, iconSize);
        Place(g_settings.pinRadios[index], cellX, cellTop + iconSize + Scale(8), cellWidth, Scale(26));
    }
}

void LayoutHotkeys(int x, int y, int width) {
    const int gapX = Scale(20);
    const int columnWidth = (width - gapX * 2) / 3;
    const int labelHeight = Scale(24);
    const int boxHeight = Scale(30);
    const int blockGap = Scale(18);
    const int blockHeight = labelHeight + Scale(6) + boxHeight;
    for (int index = 0; index < static_cast<int>(g_settings.hotkeys.size()); ++index) {
        const int row = index / 3;
        const int column = index % 3;
        const int cellX = x + column * (columnWidth + gapX);
        const int cellY = y + row * (blockHeight + blockGap);
        Place(g_settings.hotkeyLabels[index], cellX, cellY, columnWidth, labelHeight);
        Place(g_settings.hotkeys[index], cellX, cellY + labelHeight + Scale(6),
              columnWidth, boxHeight);
    }
    // “恢复默认快捷键”单独占一行，与网格保持左对齐。
    const int resetY = y + blockHeight * 2 + blockGap + Scale(12);
    Place(g_settings.resetHotkeys, x, resetY, columnWidth, boxHeight);
}

void LayoutConfig(int x, int y, int width) {
    Place(g_settings.startupCheck, x, y, width, Scale(26));
    Place(g_settings.ownTray, x, y + Scale(40), width, Scale(26));
}

void LayoutAbout(int x, int y, int width, int height) {
    auto& page = g_settings.pages[4];
    const int iconSize = Scale(96);
    Place(g_settings.aboutIcon, x + Scale(10), y + (height - iconSize) / 2, iconSize, iconSize);
    const int textX = x + Scale(10) + iconSize + Scale(28);
    Place(page[1], textX, y + (height - iconSize) / 2 + Scale(16), width - (textX - x), Scale(32));
    Place(page[2], textX, y + (height - iconSize) / 2 + Scale(56), width - (textX - x), Scale(26));
}

void Layout() {
    if (!g_settings.tab) return;
    RECT client{};
    GetClientRect(g_settings.window, &client);
    const int margin = Scale(16);
    const int buttonWidth = Scale(90);
    const int buttonHeight = Scale(32);
    const int bottom = client.bottom - margin - buttonHeight;
    Place(g_settings.save, client.right - margin - buttonWidth * 2 - Scale(12),
          bottom, buttonWidth, buttonHeight);
    Place(g_settings.close, client.right - margin - buttonWidth, bottom, buttonWidth, buttonHeight);
    Place(g_settings.tab, margin, margin, client.right - margin * 2, bottom - margin - Scale(12));

    RECT page{};
    GetClientRect(g_settings.tab, &page);
    TabCtrl_AdjustRect(g_settings.tab, FALSE, &page);
    MapWindowPoints(g_settings.tab, g_settings.window, reinterpret_cast<POINT*>(&page), 2);
    const int x = page.left + Scale(20);
    const int y = page.top + Scale(20);
    const int width = page.right - x - Scale(20);
    const int height = page.bottom - y - Scale(20);
    if (width <= 0 || height <= 0) return;
    LayoutTransparency(x, y, width);
    LayoutPins(x, y, width);
    LayoutHotkeys(x, y, width);
    LayoutConfig(x, y, width);
    LayoutAbout(x, y, width, height);
}

void ShowPage(int index) {
    if (index < 0 || index >= kPageCount) return;
    // 让标签控件位于底部，避免同层兄弟窗口重叠绘制时盖住页面控件。
    SetWindowPos(g_settings.tab, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    for (int page = 0; page < kPageCount; ++page)
        for (HWND control : g_settings.pages[page])
            ShowWindow(control, page == index ? SW_SHOW : SW_HIDE);
}

// ---------------------------------------------------------------- 页面刷新

void UpdateTrackLabel(HWND track, HWND label) {
    const int alpha = static_cast<int>(SendMessageW(track, TBM_GETPOS, 0, 0));
    wchar_t text[64]{};
    swprintf_s(text, 64, L"%d/255 %d%%", alpha, (alpha * 100 + 127) / 255);
    SetWindowTextW(label, text);
}

void UpdatePinIcons() {
    for (int index = 0; index < 5; ++index) {
        if (!g_settings.pinPreviews[index]) continue;
        if (g_settings.pinIcons[index]) { DestroyIcon(g_settings.pinIcons[index]); g_settings.pinIcons[index] = nullptr; }
        g_settings.pinIcons[index] = static_cast<HICON>(LoadImageW(g_settings.instance,
            MAKEINTRESOURCEW(IDI_PIN_FIRST + index), IMAGE_ICON,
            Scale(32), Scale(32), LR_DEFAULTCOLOR));
        SendMessageW(g_settings.pinPreviews[index], STM_SETICON,
                     reinterpret_cast<WPARAM>(g_settings.pinIcons[index]), 0);
    }
}

void UpdateAboutIcon() {
    if (!g_settings.aboutIcon) return;
    if (g_settings.aboutIconHandle) DestroyIcon(g_settings.aboutIconHandle);
    g_settings.aboutIconHandle = static_cast<HICON>(LoadImageW(g_settings.instance,
        MAKEINTRESOURCEW(IDI_XODO), IMAGE_ICON, Scale(96), Scale(96), LR_DEFAULTCOLOR));
    SendMessageW(g_settings.aboutIcon, STM_SETICON,
                 reinterpret_cast<WPARAM>(g_settings.aboutIconHandle), 0);
}

BYTE ModifiersToHotkey(UINT modifiers) {
    BYTE result = 0;
    if (modifiers & MOD_SHIFT) result |= HOTKEYF_SHIFT;
    if (modifiers & MOD_CONTROL) result |= HOTKEYF_CONTROL;
    if (modifiers & MOD_ALT) result |= HOTKEYF_ALT;
    return result;
}

UINT HotkeyToModifiers(BYTE hotkey) {
    UINT result = 0;
    if (hotkey & HOTKEYF_SHIFT) result |= MOD_SHIFT;
    if (hotkey & HOTKEYF_CONTROL) result |= MOD_CONTROL;
    if (hotkey & HOTKEYF_ALT) result |= MOD_ALT;
    return result;
}

void SetHotkeyControl(HWND control, const HotkeyConfig& value) {
    SendMessageW(control, HKM_SETHOTKEY,
        MAKEWORD(static_cast<BYTE>(value.key), ModifiersToHotkey(value.modifiers)), 0);
}

// 快捷键允许留空：key 为 0 表示该项不注册，modifiers 一并清零。
HotkeyConfig ReadHotkeyControl(HWND control) {
    const DWORD packed = static_cast<DWORD>(SendMessageW(control, HKM_GETHOTKEY, 0, 0));
    HotkeyConfig value{};
    value.key = LOBYTE(packed);
    if (value.key) value.modifiers = HotkeyToModifiers(HIBYTE(packed));
    return value;
}

void LoadHotkeys(const std::array<HotkeyConfig, 5>& hotkeys) {
    for (int index = 0; index < 5; ++index) SetHotkeyControl(g_settings.hotkeys[index], hotkeys[index]);
}

// 隐藏托盘的前提是“显示设置界面”快捷键不为空，否则用户会失去打开设置的入口。
void UpdateOwnTray() {
    if (!g_settings.ownTray || !g_settings.hotkeys[4]) return;
    const bool available = ReadHotkeyControl(g_settings.hotkeys[4]).key != 0;
    if (!available) Button_SetCheck(g_settings.ownTray, BST_UNCHECKED);
    EnableWindow(g_settings.ownTray, available);
}

void Report(int page, const std::wstring& message) {
    TabCtrl_SetCurSel(g_settings.tab, page);
    ShowPage(page);
    TASKDIALOGCONFIG dialog{sizeof(dialog)};
    dialog.hwndParent = g_settings.window;
    dialog.hInstance = g_settings.instance;
    dialog.dwFlags = TDF_USE_HICON_MAIN;
    dialog.dwCommonButtons = TDCBF_OK_BUTTON;
    dialog.pszWindowTitle = L"XoDo - 设置";
    dialog.pszMainInstruction = message.c_str();
    HICON icon = LoadIconW(g_settings.instance, MAKEINTRESOURCEW(IDI_XODO));
    dialog.hMainIcon = icon;
    // 弹窗期间设置窗口会短暂失去前台焦点，这里避免据此恢复全局快捷键。
    g_settings.dialogOpen = true;
    TaskDialogIndirect(&dialog, nullptr, nullptr, nullptr);
    g_settings.dialogOpen = false;
    if (icon) DestroyIcon(icon);
}

// ---------------------------------------------------------------- 创建控件

bool CreateControls() {
    g_settings.tab = AddControl(WC_TABCONTROLW, L"", WS_TABSTOP, kTab);
    g_settings.save = AddControl(L"BUTTON", L"保存", BS_DEFPUSHBUTTON | WS_TABSTOP, kSave);
    g_settings.close = AddControl(L"BUTTON", L"关闭", BS_PUSHBUTTON | WS_TABSTOP, kClose);
    if (!g_settings.tab || !g_settings.save || !g_settings.close) return false;
    const wchar_t* titles[] = {L"窗口透明度", L"窗口置顶", L"快捷键", L"配置", L"关于"};
    for (int index = 0; index < kPageCount; ++index) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(titles[index]);
        if (SendMessageW(g_settings.tab, TCM_INSERTITEMW, index,
                         reinterpret_cast<LPARAM>(&item)) == -1) return false;
    }

    // 页 0：窗口透明度（滑块）
    AddControl(L"STATIC", L"窗口透明度配置：数值越小越透明。", SS_LEFT, 0, 0);
    AddControl(L"STATIC", L"透明度 01", SS_LEFT, 0, 0);
    g_settings.track01 = AddControl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, kTrack01, 0);
    g_settings.track01Value = AddControl(L"STATIC", L"", SS_LEFT, 0, 0);
    AddControl(L"STATIC", L"透明度 02", SS_LEFT, 0, 0);
    g_settings.track02 = AddControl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, kTrack02, 0);
    g_settings.track02Value = AddControl(L"STATIC", L"", SS_LEFT, 0, 0);
    AddControl(L"STATIC", L"透明度 03", SS_LEFT, 0, 0);
    g_settings.track03 = AddControl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, kTrack03, 0);
    g_settings.track03Value = AddControl(L"STATIC", L"", SS_LEFT, 0, 0);

    // 页 1：窗口置顶（单选框铺开）
    AddControl(L"STATIC", L"选择置顶时显示在窗口右上角样式：", SS_LEFT, 0, 1);
    for (int index = 0; index < 5; ++index) {
        g_settings.pinPreviews[index] = AddControl(L"STATIC", L"", SS_ICON | SS_CENTERIMAGE, 0, 1);
        wchar_t label[32]{};
        swprintf_s(label, 32, L"图钉 %d", index + 1);
        g_settings.pinRadios[index] = AddControl(L"BUTTON", label,
            BS_AUTORADIOBUTTON | BS_CENTER | WS_TABSTOP | (index == 0 ? WS_GROUP : 0),
            kPinRadioFirst + index, 1);
    }

    // 页 2：快捷键
    const wchar_t* hotkeyLabels[] = {
        L"窗口透明 01", L"窗口透明 02", L"窗口透明 03",
        L"窗口置顶", L"显示设置界面"};
    for (int index = 0; index < 5; ++index) {
        g_settings.hotkeyLabels[index] = AddControl(L"STATIC", hotkeyLabels[index], SS_LEFT, 0, 2);
        g_settings.hotkeys[index] = AddControl(HOTKEY_CLASSW, L"", WS_TABSTOP, kHotkeyFirst + index, 2);
    }
    g_settings.resetHotkeys = AddControl(L"BUTTON", L"恢复默认快捷键",
        BS_PUSHBUTTON | WS_TABSTOP, kResetHotkeys, 2);

    // 页 3：配置
    g_settings.startupCheck = AddControl(L"BUTTON", L"开机启动",
        BS_AUTOCHECKBOX | WS_TABSTOP, kStartup, 3);
    g_settings.ownTray = AddControl(L"BUTTON", L"隐藏托盘图标",
        BS_AUTOCHECKBOX | WS_TABSTOP, kOwnTray, 3);

    // 页 4：关于
    g_settings.aboutIcon = AddControl(L"STATIC", L"", SS_ICON, 0, 4);
    AddControl(L"STATIC", L"XoDo 1.0 版本", SS_LEFT, 0, 4);
    AddControl(L"STATIC", L"本程序版权由Roy.Xu所有", SS_LEFT, 0, 4);

    const size_t expected[] = {10, 11, 11, 2, 3};
    for (size_t page = 0; page < static_cast<size_t>(kPageCount); ++page)
        if (g_settings.pages[page].size() != expected[page]) return false;
    if (!g_settings.ownTray || !g_settings.track01 || !g_settings.track02 ||
        !g_settings.track03 || !g_settings.startupCheck) return false;

    // 透明度滑块
    for (HWND track : {g_settings.track01, g_settings.track02, g_settings.track03}) {
        SendMessageW(track, TBM_SETRANGE, TRUE, MAKELONG(1, 255));
        SendMessageW(track, TBM_SETTICFREQ, 16, 0);
        SendMessageW(track, TBM_SETPAGESIZE, 0, 16);
        SendMessageW(track, TBM_SETLINESIZE, 0, 1);
    }
    SendMessageW(g_settings.track01, TBM_SETPOS, TRUE, g_settings.config->alpha01);
    SendMessageW(g_settings.track02, TBM_SETPOS, TRUE, g_settings.config->alpha02);
    SendMessageW(g_settings.track03, TBM_SETPOS, TRUE, g_settings.config->alpha03);

    // 图钉单选框
    const int pinStyle = g_settings.config->pinStyle;
    Button_SetCheck(g_settings.pinRadios[pinStyle >= 0 && pinStyle < 5 ? pinStyle : 0], BST_CHECKED);

    // 快捷键
    LoadHotkeys(g_settings.config->hotkeys);

    // 配置页
    Button_SetCheck(g_settings.startupCheck,
        AppStartupEnabled() ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(g_settings.ownTray,
        g_settings.config->hideOwnTray ? BST_CHECKED : BST_UNCHECKED);

    UpdateFont();
    UpdatePinIcons();
    UpdateAboutIcon();
    UpdateTrackLabel(g_settings.track01, g_settings.track01Value);
    UpdateTrackLabel(g_settings.track02, g_settings.track02Value);
    UpdateTrackLabel(g_settings.track03, g_settings.track03Value);
    UpdateOwnTray();
    ShowPage(0);
    Layout();
    return true;
}

// ---------------------------------------------------------------- 保存

bool SaveSettings() {
    AppConfig candidate = *g_settings.config;
    candidate.alpha01 = static_cast<int>(SendMessageW(g_settings.track01, TBM_GETPOS, 0, 0));
    candidate.alpha02 = static_cast<int>(SendMessageW(g_settings.track02, TBM_GETPOS, 0, 0));
    candidate.alpha03 = static_cast<int>(SendMessageW(g_settings.track03, TBM_GETPOS, 0, 0));
    candidate.pinStyle = 0;
    for (int index = 0; index < 5; ++index)
        if (Button_GetCheck(g_settings.pinRadios[index]) == BST_CHECKED) candidate.pinStyle = index;

    for (int index = 0; index < 5; ++index) {
        const HotkeyConfig value = ReadHotkeyControl(g_settings.hotkeys[index]);
        // 允许留空；只对已设置的快捷键做重复检查。
        if (value.key != 0) {
            for (int other = 0; other < index; ++other) {
                if (candidate.hotkeys[other].key == value.key &&
                    candidate.hotkeys[other].modifiers == value.modifiers) {
                    Report(2, L"快捷键 " + HotkeyDisplay(value) + L" 已经被设置，请重新配置。");
                    SetFocus(g_settings.hotkeys[index]);
                    return false;
                }
            }
        }
        candidate.hotkeys[index] = value;
    }

    candidate.hideOwnTray = Button_GetCheck(g_settings.ownTray) == BST_CHECKED &&
                            IsWindowEnabled(g_settings.ownTray);
    candidate.startup = Button_GetCheck(g_settings.startupCheck) == BST_CHECKED;

    std::wstring error;
    if (!ApplyAppConfig(candidate, error)) {
        Report(2, error.empty() ? L"保存失败。" : error);
        return false;
    }
    UpdateOwnTray();
    return true;
}

// ---------------------------------------------------------------- 窗口过程

LRESULT CALLBACK SettingsWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCCREATE:
        // 必须交给 DefWindowProcW：只有默认处理才会把 CREATESTRUCT 的标题写入窗口。
        g_settings.window = window;
        break;
    case WM_CREATE:
        g_settings.dpi = WindowDpi(window);
        return CreateControls() ? 0 : -1;
    case WM_ACTIVATE:
        // 设置窗口在前台时暂停全局快捷键，保证快捷键输入框能录入任何组合键。
        if (LOWORD(wParam) == WA_INACTIVE) {
            if (!g_settings.dialogOpen) ResumeGlobalHotkeys();
        } else {
            SuspendGlobalHotkeys();
        }
        break;
    case WM_SIZE:
        Layout();
        return 0;
    case WM_DPICHANGED: {
        g_settings.dpi = HIWORD(wParam);
        UpdateFont();
        UpdatePinIcons();
        UpdateAboutIcon();
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        // 位置采用系统建议值，尺寸按新 DPI 重新反推，保持客户区比例。
        const SIZE size = SettingsWindowSize(g_settings.dpi);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
    }
    case WM_TIMER:
        if (wParam == kPageBrushTimerId) {
            COLORREF color = 0;
            if (SampleTabBodyColor(color)) {
                KillTimer(window, kPageBrushTimerId);
                ApplyPageBrush(color);
            } else if (++g_settings.pageBrushAttempts >= kPageBrushMaxAttempts) {
                KillTimer(window, kPageBrushTimerId);
                ApplyPageBrush(GetSysColor(COLOR_BTNFACE));
            }
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        // 文字背景取标签页正文色，避免出现比正文更深的独立色块。
        SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
        return reinterpret_cast<LRESULT>(g_settings.pageBrush
            ? g_settings.pageBrush : GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_HSCROLL: {
        const HWND control = reinterpret_cast<HWND>(lParam);
        if (control == g_settings.track01) UpdateTrackLabel(control, g_settings.track01Value);
        else if (control == g_settings.track02) UpdateTrackLabel(control, g_settings.track02Value);
        else if (control == g_settings.track03) UpdateTrackLabel(control, g_settings.track03Value);
        return 0;
    }
    case WM_NOTIFY: {
        const auto* notification = reinterpret_cast<const NMHDR*>(lParam);
        if (notification->hwndFrom == g_settings.tab && notification->code == TCN_SELCHANGE) {
            ShowPage(TabCtrl_GetCurSel(g_settings.tab));
            return 0;
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kSave:
            if (HIWORD(wParam) == BN_CLICKED) {
                // 保存失败时 ApplyAppConfig 会恢复旧快捷键，需要重新暂停。
                if (SaveSettings()) DestroyWindow(window);
                else SuspendGlobalHotkeys();
            }
            return 0;
        case kClose:
            DestroyWindow(window);
            return 0;
        case kResetHotkeys:
            if (HIWORD(wParam) == BN_CLICKED) {
                LoadHotkeys(DefaultHotkeys());
                UpdateOwnTray();
            }
            return 0;
        case kHotkeyFirst + 4:
            // “显示设置界面”快捷键变化时，同步“隐藏托盘图标”的可用状态。
            if (HIWORD(wParam) == EN_CHANGE) UpdateOwnTray();
            return 0;
        default:
            // 焦点进入快捷键输入框时临时注销该全局快捷键，保证任何组合都能录入。
            if (LOWORD(wParam) >= kHotkeyFirst && LOWORD(wParam) <= kHotkeyFirst + 4) {
                if (HIWORD(wParam) == EN_SETFOCUS) SuspendSettingsHotkey();
                else if (HIWORD(wParam) == EN_KILLFOCUS) ResumeSettingsHotkey();
                return 0;
            }
            break;
        }
        break;
    case DM_GETDEFID:
        return MAKELRESULT(kSave, DC_HASDEFID);
    case DM_SETDEFID:
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_NCDESTROY: {
        // 设置窗口关闭后恢复全局快捷键（使用最新配置）。
        ResumeGlobalHotkeys();
        const LRESULT result = DefWindowProcW(window, message, wParam, lParam);
        if (g_settings.font) DeleteObject(g_settings.font);
        if (g_settings.pageBrush) DeleteObject(g_settings.pageBrush);
        for (HICON icon : g_settings.pinIcons) if (icon) DestroyIcon(icon);
        if (g_settings.aboutIconHandle) DestroyIcon(g_settings.aboutIconHandle);
        g_settings = SettingsState{};
        return result;
    }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace

void ShowSettings(HINSTANCE instance, HWND owner, AppConfig& config) {
    if (IsWindow(g_settings.window)) {
        ShowWindow(g_settings.window, IsIconic(g_settings.window) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(g_settings.window);
        SetActiveWindow(g_settings.window);
        return;
    }

    INITCOMMONCONTROLSEX controls{sizeof(controls),
        ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_HOTKEY_CLASS | ICC_STANDARD_CLASSES};
    if (!InitCommonControlsEx(&controls)) return;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = SettingsWindowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_XODO));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;

    g_settings.instance = instance;
    g_settings.config = &config;
    g_settings.dpi = WindowDpi(owner);
    // 设置窗口为固定尺寸：只保留标题栏与关闭按钮，不提供最小化/最大化/缩放边框。
    const SIZE initialSize = SettingsWindowSize(g_settings.dpi);
    HWND window = CreateWindowExW(kWindowExStyle, kWindowClass, L"XoDo - 设置",
        kWindowStyle, CW_USEDEFAULT, CW_USEDEFAULT,
        initialSize.cx, initialSize.cy, owner, nullptr, instance, nullptr);
    if (!window) {
        g_settings = SettingsState{};
        return;
    }
    // 创建尺寸按 owner 的 DPI 计算，若窗口实际落在不同 DPI 的显示器上，按实际 DPI 校正。
    g_settings.dpi = WindowDpi(window);
    const SIZE actualSize = SettingsWindowSize(g_settings.dpi);
    RECT windowRect{};
    GetWindowRect(window, &windowRect);
    if (windowRect.right - windowRect.left != actualSize.cx ||
        windowRect.bottom - windowRect.top != actualSize.cy) {
        SetWindowPos(window, nullptr, 0, 0, actualSize.cx, actualSize.cy,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // 在触发操作所在显示器的可用区域内居中显示。
    HMONITOR monitor = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (!monitor) monitor = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo)) {
        GetWindowRect(window, &windowRect);
        const int width = windowRect.right - windowRect.left;
        const int height = windowRect.bottom - windowRect.top;
        const RECT& work = monitorInfo.rcWork;
        const int x = work.left + (work.right - work.left - width) / 2;
        const int y = work.top + (work.bottom - work.top - height) / 2;
        SetWindowPos(window, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    // 等窗口完成首次绘制后再取样正文色，否则 PrintWindow 会得到全黑位图。
    SetTimer(window, kPageBrushTimerId, 40, nullptr);
    SetForegroundWindow(window);
    SetFocus(g_settings.tab);
}

bool IsSettingsOpen() {
    return IsWindow(g_settings.window) != FALSE;
}

void CloseSettings() {
    if (IsWindow(g_settings.window)) DestroyWindow(g_settings.window);
}

bool HandleSettingsMessage(MSG& message) {
    return IsWindow(g_settings.window) && IsDialogMessageW(g_settings.window, &message) != FALSE;
}
