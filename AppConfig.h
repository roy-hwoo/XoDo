#pragma once
#include <windows.h>
#include <array>
#include <string>

constexpr UINT WM_XODO_SETTINGS_CHANGED = WM_APP + 40;
constexpr UINT WM_XODO_REFRESH_PINS = WM_APP + 41;
constexpr int IDI_XODO = 101;
constexpr int IDI_PIN_FIRST = 201;

struct HotkeyConfig {
    UINT modifiers = MOD_CONTROL;
    UINT key = 0;
};
inline std::array<HotkeyConfig, 5> DefaultHotkeys() {
    return {{{MOD_CONTROL, '1'}, {MOD_CONTROL, '2'}, {MOD_CONTROL, '3'},
             {MOD_CONTROL, VK_F12}, {MOD_CONTROL, VK_F11}}};
}
struct AppConfig {
    int alpha01 = 180;
    int alpha02 = 110;
    int alpha03 = 60;
    int pinStyle = 0;
    std::array<HotkeyConfig, 5> hotkeys = DefaultHotkeys();
    bool hideOwnTray = false;
    bool startup = true;
};

// 在主线程同步校验、注册热键并保存；失败时保留原配置并返回原因。
bool ApplyAppConfig(const AppConfig& candidate, std::wstring& error);
bool SettingsHotkeyAvailable();
// 设置窗口打开期间暂停全局快捷键，关闭后恢复注册。
void SuspendGlobalHotkeys();
void ResumeGlobalHotkeys();
// 焦点进入快捷键输入框时临时注销“显示设置界面”快捷键，避免按键被直接拦截。
void SuspendSettingsHotkey();
void ResumeSettingsHotkey();
bool AppStartupEnabled();
std::wstring HotkeyDisplay(const HotkeyConfig& hotkey);
void ShowSettings(HINSTANCE instance, HWND owner, AppConfig& config);
bool IsSettingsOpen();
void CloseSettings();
bool HandleSettingsMessage(MSG& message);
