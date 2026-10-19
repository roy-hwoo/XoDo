#pragma once
#include <windows.h>
#include <unordered_map>

class PinManager {
public:
    bool Initialize(HINSTANCE instance, HWND owner);
    bool Toggle(HWND target);
    void Refresh(int style);
    void Shutdown();
private:
    struct Pin { HWND overlay = nullptr; bool originallyTopmost = false; UINT dpi = 0; int style = -1; };
    HINSTANCE instance_ = nullptr;
    HWND owner_ = nullptr;
    HWINEVENTHOOK hook_ = nullptr;
    std::unordered_map<HWND, Pin> pins_;
    bool Render(Pin& pin, UINT dpi, int style);
};
