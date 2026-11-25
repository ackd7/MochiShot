#pragma once
#include <windows.h>
#include <string>
#include <cstdint>
#include "mochi_theme.h"

namespace mochi {

struct HotkeyBinding {
    UINT modifiers = MOD_ALT; // MOD_CONTROL, MOD_ALT, MOD_SHIFT, MOD_WIN
    UINT vk = 'A';            // Default: Alt + A

    bool operator==(const HotkeyBinding& o) const {
        return modifiers == o.modifiers && vk == o.vk;
    }
    bool IsValid() const { return vk != 0; }
};

struct AppConfig {
    HotkeyBinding captureHotkey{ MOD_ALT, 'A' };
    HotkeyBinding pinClipboardHotkey{ MOD_ALT | MOD_SHIFT, 'A' };
    bool autoStartOnBoot = false;
    bool autoCopyOnCapture = true;
    bool showMagnifier = true;
    bool windowAutoSnap = true;
    ThemeMode themeMode = ThemeMode::System;
    SeedAccent seedAccent = SeedAccent::MochiTeal;
    GlassMode glassMode = GlassMode::LiquidGlass;
    int defaultSaveFormat = 0; // 0 = PNG, 1 = JPEG
    int defaultStrokeSize = 4; // px at 96dpi
    int defaultMosaicSize = 14;
    uint32_t defaultColorHex = 0xFF3B30; // iOS / Mochi vibrant red
};

std::wstring GetConfigFilePath();
bool LoadAppConfig(AppConfig& outCfg);
bool SaveAppConfig(const AppConfig& cfg);

// Startup registry management (HKCU\Software\Microsoft\Windows\CurrentVersion\Run)
bool IsAutoStartEnabledInRegistry();
bool SetAutoStartInRegistry(bool enable);

// System theme detection
bool IsWindowsDarkThemeActive();
bool ResolveDarkTheme(ThemeMode mode);

// Human-readable hotkey formatting (e.g. "Alt + A", "Ctrl + Shift + S", "PrintScreen")
std::wstring FormatHotkey(const HotkeyBinding& hk);
std::wstring FormatVirtualKeyName(UINT vk);

// Process memory measurement & trimming
double GetCurrentWorkingSetMB();
void TrimProcessMemory();

} // namespace mochi
