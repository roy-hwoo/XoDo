#include "PinManager.h"
#include "AppConfig.h"
#include <dwmapi.h>

namespace {
HWND eventOwner = nullptr;
bool refreshQueued = false;
void CALLBACK WindowEvent(HWINEVENTHOOK, DWORD, HWND, LONG object, LONG, DWORD, DWORD) {
    if (object == OBJID_WINDOW && eventOwner && !refreshQueued) {
        refreshQueued = PostMessageW(eventOwner, WM_XODO_REFRESH_PINS, 0, 0) != FALSE;
    }
}
LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}

bool PinManager::Initialize(HINSTANCE instance, HWND owner) {
    instance_ = instance;
    owner_ = owner;
    WNDCLASSW wc{};
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"XoDo.Pin";
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    eventOwner = owner;
    hook_ = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
        nullptr, WindowEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // The main window's 250 ms timer also covers minimization, destruction and missed events.
    return true;
}

bool PinManager::Toggle(HWND target) {
    auto found = pins_.find(target);
    if (found != pins_.end()) {
        if (!found->second.originallyTopmost && IsWindow(target) &&
            !SetWindowPos(target, HWND_NOTOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) return false;
        DestroyWindow(found->second.overlay);
        pins_.erase(found);
        return true;
    }
    Pin pin;
    pin.originallyTopmost = (GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    pin.overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
        WS_EX_TRANSPARENT | WS_EX_TOPMOST, L"XoDo.Pin", L"", WS_POPUP,
        0, 0, 1, 1, target, nullptr, instance_, nullptr);
    if (!pin.overlay) return false;
    if (!SetWindowPos(target, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
        DestroyWindow(pin.overlay);
        return false;
    }
    pins_.emplace(target, pin);
    return true;
}

bool PinManager::Render(Pin& pin, UINT dpi, int style) {
    const int size = MulDiv(24, dpi, 96);
    auto icon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_PIN_FIRST + style),
        IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    if (!icon) return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool result = false;
    if (memory && bitmap) {
        HGDIOBJ previous = SelectObject(memory, bitmap);
        ZeroMemory(bits, static_cast<SIZE_T>(size) * size * 4);
        if (DrawIconEx(memory, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL)) {
            // Legacy icons have an AND mask rather than an alpha channel.
            auto pixels = static_cast<DWORD*>(bits);
            bool hasAlpha = false;
            for (int i = 0; i < size * size; ++i) hasAlpha |= (pixels[i] & 0xff000000) != 0;
            if (!hasAlpha) {
                HDC maskDC = CreateCompatibleDC(screen);
                HBITMAP maskBitmap = CreateCompatibleBitmap(screen, size, size);
                if (maskDC && maskBitmap) {
                    HGDIOBJ oldMask = SelectObject(maskDC, maskBitmap);
                    PatBlt(maskDC, 0, 0, size, size, WHITENESS);
                    DrawIconEx(maskDC, 0, 0, icon, size, size, 0, nullptr, DI_MASK);
                    for (int y = 0; y < size; ++y)
                        for (int x = 0; x < size; ++x)
                            if (GetPixel(maskDC, x, y) == RGB(0, 0, 0)) pixels[y * size + x] |= 0xff000000;
                    SelectObject(maskDC, oldMask);
                }
                if (maskBitmap) DeleteObject(maskBitmap);
                if (maskDC) DeleteDC(maskDC);
            }
            SIZE dimensions{size, size};
            POINT source{};
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            result = UpdateLayeredWindow(pin.overlay, screen, nullptr, &dimensions,
                memory, &source, 0, &blend, ULW_ALPHA) != FALSE;
        }
        SelectObject(memory, previous);
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    DestroyIcon(icon);
    if (result) { pin.dpi = dpi; pin.style = style; }
    return result;
}

void PinManager::Refresh(int style) {
    refreshQueued = false;
    for (auto it = pins_.begin(); it != pins_.end();) {
        HWND target = it->first;
        auto& pin = it->second;
        if (!IsWindow(target) || !IsWindow(pin.overlay)) {
            if (IsWindow(pin.overlay)) DestroyWindow(pin.overlay);
            it = pins_.erase(it);
            continue;
        }
        ++it;
        DWORD cloaked = 0;
        DwmGetWindowAttribute(target, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (!IsWindowVisible(target) || IsIconic(target) || cloaked) {
            ShowWindow(pin.overlay, SW_HIDE);
            continue;
        }
        RECT rect{};
        if (FAILED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))) &&
            !GetWindowRect(target, &rect)) continue;
        if (IsRectEmpty(&rect)) { ShowWindow(pin.overlay, SW_HIDE); continue; }
        UINT dpi = GetDpiForWindow(target);
        if (!dpi) dpi = 96;
        if ((pin.dpi != dpi || pin.style != style) && !Render(pin, dpi, style)) {
            ShowWindow(pin.overlay, SW_HIDE);
            continue;
        }
        const int size = MulDiv(24, dpi, 96);
        const int gap = MulDiv(6, dpi, 96);
        RECT buttons{};
        TITLEBARINFOEX titleBar{};
        titleBar.cbSize = sizeof(titleBar);
        DWORD_PTR response = 0;
        // 优先读取最小化按钮的屏幕坐标，避免遮挡标题栏按钮。
        if (SendMessageTimeoutW(target, WM_GETTITLEBARINFOEX, 0,
                reinterpret_cast<LPARAM>(&titleBar), SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &response) &&
            !(titleBar.rgstate[2] & STATE_SYSTEM_INVISIBLE) &&
            !IsRectEmpty(&titleBar.rgrect[2])) {
            buttons = titleBar.rgrect[2];
        } else {
            RECT windowRect{};
            if (SUCCEEDED(DwmGetWindowAttribute(target, DWMWA_CAPTION_BUTTON_BOUNDS,
                    &buttons, sizeof(buttons))) && !IsRectEmpty(&buttons) &&
                GetWindowRect(target, &windowRect)) {
                OffsetRect(&buttons, windowRect.left, windowRect.top);
            } else {
                // 自绘标题栏未提供按钮坐标时，按标准标题栏尺寸预留按钮区域。
                const LONG windowStyle = GetWindowLongW(target, GWL_STYLE);
                const int buttonCount = (windowStyle & WS_SYSMENU)
                    ? ((windowStyle & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) ? 3 : 1) : 0;
                buttons = {rect.right - buttonCount * GetSystemMetricsForDpi(SM_CXSIZE, dpi),
                    rect.top, rect.right, rect.top + GetSystemMetricsForDpi(SM_CYCAPTION, dpi)};
            }
        }
        const int x = buttons.left - gap - size;
        const int y = buttons.top + (buttons.bottom - buttons.top - size) / 2;
        SetWindowPos(pin.overlay, HWND_TOPMOST, x, y, size, size,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }
}

void PinManager::Shutdown() {
    eventOwner = nullptr;
    if (hook_) UnhookWinEvent(hook_);
    hook_ = nullptr;
    for (auto& entry : pins_) {
        if (IsWindow(entry.first) && !entry.second.originallyTopmost)
            SetWindowPos(entry.first, HWND_NOTOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (IsWindow(entry.second.overlay)) DestroyWindow(entry.second.overlay);
    }
    pins_.clear();
}
