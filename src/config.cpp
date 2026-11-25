#include "config.h"
#include <shlobj.h>
#include <psapi.h>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace mochi {

namespace {
constexpr const wchar_t* kRunRegPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValueName = L"MochiShot";
} // namespace

std::wstring GetConfigFilePath() {
    wchar_t appData[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) {
        std::filesystem::path dir = std::filesystem::path(appData) / L"MochiShot";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return (dir / L"config.ini").wstring();
    }
    return L"MochiShot.ini";
}

bool LoadAppConfig(AppConfig& cfg) {
    const std::wstring path = GetConfigFilePath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        cfg.autoStartOnBoot = IsAutoStartEnabledInRegistry();
        return false;
    }

    auto rdInt = [&](const wchar_t* key, int defVal) -> int {
        return static_cast<int>(GetPrivateProfileIntW(L"MochiShot", key, defVal, path.c_str()));
    };

    cfg.captureHotkey.modifiers = static_cast<UINT>(rdInt(L"CaptureMod", MOD_ALT));
    cfg.captureHotkey.vk        = static_cast<UINT>(rdInt(L"CaptureVk", 'A'));
    if (!cfg.captureHotkey.IsValid()) {
        cfg.captureHotkey = { MOD_ALT, 'A' };
    }

    cfg.pinClipboardHotkey.modifiers = static_cast<UINT>(rdInt(L"QuickFullMod", MOD_ALT | MOD_SHIFT));
    cfg.pinClipboardHotkey.vk        = static_cast<UINT>(rdInt(L"QuickFullVk", 'A'));

    cfg.autoCopyOnCapture = rdInt(L"AutoCopy", 1) != 0;
    cfg.showMagnifier     = rdInt(L"ShowMagnifier", 1) != 0;
    cfg.windowAutoSnap    = rdInt(L"WindowAutoSnap", 1) != 0;
    cfg.themeMode         = static_cast<ThemeMode>(std::clamp(rdInt(L"ThemeMode", 0), 0, 2));
    cfg.seedAccent        = static_cast<SeedAccent>(std::clamp(rdInt(L"SeedAccent", 0), 0, 4));
    cfg.glassMode         = static_cast<GlassMode>(std::clamp(rdInt(L"GlassMode", 0), 0, 2));
    cfg.defaultSaveFormat = std::clamp(rdInt(L"SaveFormat", 0), 0, 1);
    cfg.defaultStrokeSize = std::clamp(rdInt(L"StrokeSize", 4), 1, 32);
    cfg.defaultMosaicSize = std::clamp(rdInt(L"MosaicSize", 14), 4, 48);
    cfg.defaultColorHex   = static_cast<uint32_t>(rdInt(L"ColorHex", 0xFF3B30));
    cfg.autoStartOnBoot   = IsAutoStartEnabledInRegistry();
    return true;
}

bool SaveAppConfig(const AppConfig& cfg) {
    const std::wstring path = GetConfigFilePath();
    auto wrInt = [&](const wchar_t* key, int val) -> bool {
        wchar_t buf[32]{};
        swprintf_s(buf, L"%d", val);
        return WritePrivateProfileStringW(L"MochiShot", key, buf, path.c_str()) != FALSE;
    };

    bool ok = true;
    ok = wrInt(L"CaptureMod", static_cast<int>(cfg.captureHotkey.modifiers)) && ok;
    ok = wrInt(L"CaptureVk", static_cast<int>(cfg.captureHotkey.vk)) && ok;
    ok = wrInt(L"QuickFullMod", static_cast<int>(cfg.pinClipboardHotkey.modifiers)) && ok;
    ok = wrInt(L"QuickFullVk", static_cast<int>(cfg.pinClipboardHotkey.vk)) && ok;
    ok = wrInt(L"AutoCopy", cfg.autoCopyOnCapture ? 1 : 0) && ok;
    ok = wrInt(L"ShowMagnifier", cfg.showMagnifier ? 1 : 0) && ok;
    ok = wrInt(L"WindowAutoSnap", cfg.windowAutoSnap ? 1 : 0) && ok;
    ok = wrInt(L"ThemeMode", static_cast<int>(cfg.themeMode)) && ok;
    ok = wrInt(L"SeedAccent", static_cast<int>(cfg.seedAccent)) && ok;
    ok = wrInt(L"GlassMode", static_cast<int>(cfg.glassMode)) && ok;
    ok = wrInt(L"SaveFormat", cfg.defaultSaveFormat) && ok;
    ok = wrInt(L"StrokeSize", cfg.defaultStrokeSize) && ok;
    ok = wrInt(L"MosaicSize", cfg.defaultMosaicSize) && ok;
    ok = wrInt(L"ColorHex", static_cast<int>(cfg.defaultColorHex)) && ok;
    return ok;
}

bool IsAutoStartEnabledInRegistry() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t val[MAX_PATH * 2]{};
    DWORD cb = sizeof(val);
    DWORD type = 0;
    LSTATUS st = RegQueryValueExW(hKey, kRunValueName, nullptr, &type, reinterpret_cast<LPBYTE>(val), &cb);
    RegCloseKey(hKey);
    return (st == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && val[0] != L'\0');
}

bool SetAutoStartInRegistry(bool enable) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = false;
    if (enable) {
        wchar_t exePath[MAX_PATH]{};
        DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::wstring cmd = L"\"";
            cmd += exePath;
            cmd += L"\" --silent";
            DWORD bytes = static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t));
            ok = (RegSetValueExW(hKey, kRunValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()), bytes) == ERROR_SUCCESS);
        }
    } else {
        LSTATUS st = RegDeleteValueW(hKey, kRunValueName);
        ok = (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(hKey);
    return ok;
}

bool IsWindowsDarkThemeActive() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    DWORD appsUseLight = 1;
    DWORD cb = sizeof(appsUseLight);
    RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr, reinterpret_cast<LPBYTE>(&appsUseLight), &cb);
    RegCloseKey(hKey);
    return appsUseLight == 0;
}

bool ResolveDarkTheme(ThemeMode mode) {
    if (mode == ThemeMode::Dark) return true;
    if (mode == ThemeMode::Light) return false;
    return IsWindowsDarkThemeActive();
}

std::wstring FormatVirtualKeyName(UINT vk) {
    if (vk >= 'A' && vk <= 'Z') {
        return std::wstring(1, static_cast<wchar_t>(vk));
    }
    if (vk >= '0' && vk <= '9') {
        return std::wstring(1, static_cast<wchar_t>(vk));
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return L"F" + std::to_wstring(vk - VK_F1 + 1);
    }
    switch (vk) {
    case VK_SNAPSHOT: return L"PrintScreen";
    case VK_SPACE:    return L"Space";
    case VK_TAB:      return L"Tab";
    case VK_INSERT:   return L"Insert";
    case VK_DELETE:   return L"Delete";
    case VK_HOME:     return L"Home";
    case VK_END:      return L"End";
    case VK_PRIOR:    return L"PageUp";
    case VK_NEXT:     return L"PageDown";
    case VK_OEM_3:    return L"`";
    case VK_OEM_MINUS:return L"-";
    case VK_OEM_PLUS: return L"=";
    case VK_OEM_4:    return L"[";
    case VK_OEM_6:    return L"]";
    case VK_OEM_5:    return L"\\";
    case VK_OEM_1:    return L";";
    case VK_OEM_7:    return L"'";
    case VK_OEM_COMMA:return L",";
    case VK_OEM_PERIOD:return L".";
    case VK_OEM_2:    return L"/";
    default: break;
    }
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    wchar_t name[64]{};
    if (scan != 0 && GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64) > 0) {
        return name;
    }
    wchar_t fallback[32]{};
    swprintf_s(fallback, L"VK_%02X", vk);
    return fallback;
}

std::wstring FormatHotkey(const HotkeyBinding& hk) {
    if (!hk.IsValid()) return L"未设置";
    std::wstring res;
    if (hk.modifiers & MOD_CONTROL) res += L"Ctrl + ";
    if (hk.modifiers & MOD_ALT)     res += L"Alt + ";
    if (hk.modifiers & MOD_SHIFT)   res += L"Shift + ";
    if (hk.modifiers & MOD_WIN)     res += L"Win + ";
    res += FormatVirtualKeyName(hk.vk);
    return res;
}

double GetCurrentWorkingSetMB() {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    }
    return 0.0;
}

void TrimProcessMemory() {
    EmptyWorkingSet(GetCurrentProcess());
}

} // namespace mochi
