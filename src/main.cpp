#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <cstdio>
#include <filesystem>
#include <memory>
#include "resource.h"
#include "config.h"
#include "mochi_theme.h"
#include "mochi_g2.h"
#include "liquid_glass.h"
#include "screen_capture.h"
#include "capture_overlay.h"
#include "pin_window.h"
#include "settings_window.h"

namespace {

constexpr const wchar_t* kTrayHostClass = L"MochiShot_TrayHost";
constexpr const wchar_t* kMutexName     = L"Local\\MochiShot_SingleInstance_Mutex";
constexpr UINT WM_TRAYICON              = WM_APP + 10;
constexpr UINT WM_MOCHI_ACTIVATE_EXIST  = WM_APP + 11;

constexpr int HOTKEY_ID_CAPTURE   = 1001;
constexpr int HOTKEY_ID_QUICKFULL = 1002;

constexpr UINT IDM_TRAY_CAPTURE   = 3001;
constexpr UINT IDM_TRAY_QUICKFULL = 3002;
constexpr UINT IDM_TRAY_SETTINGS  = 3003;
constexpr UINT IDM_TRAY_AUTOSTART = 3004;
constexpr UINT IDM_TRAY_CLOSEPINS = 3005;
constexpr UINT IDM_TRAY_EXIT      = 3006;

struct AppRuntime {
    HINSTANCE hInstance = nullptr;
    HWND hostHwnd = nullptr;
    NOTIFYICONDATAW nid{};
    bool trayAdded = false;

    mochi::AppConfig config;
    mochi::LiquidGlassEngine engine;
    std::unique_ptr<mochi::CaptureOverlay> overlay;
    std::unique_ptr<mochi::SettingsWindow> settings;

    bool RebindHotkeys(const mochi::HotkeyBinding& capHk, const mochi::HotkeyBinding& fullHk) {
        if (!hostHwnd) return false;
        UnregisterHotKey(hostHwnd, HOTKEY_ID_CAPTURE);
        UnregisterHotKey(hostHwnd, HOTKEY_ID_QUICKFULL);

        bool ok1 = true;
        if (capHk.IsValid()) {
            ok1 = RegisterHotKey(hostHwnd, HOTKEY_ID_CAPTURE, capHk.modifiers | MOD_NOREPEAT, capHk.vk) != FALSE;
        }
        bool ok2 = true;
        if (fullHk.IsValid() && !(fullHk == capHk)) {
            ok2 = RegisterHotKey(hostHwnd, HOTKEY_ID_QUICKFULL, fullHk.modifiers | MOD_NOREPEAT, fullHk.vk) != FALSE;
        }
        if (settings) {
            settings->NotifyHotkeyStatus(ok1, ok2);
        }
        UpdateTrayTooltip();
        return ok1 && ok2;
    }

    void UpdateTrayTooltip() {
        if (!trayAdded) return;
        std::wstring tip = L"MochiShot 截图 (" + mochi::FormatHotkey(config.captureHotkey) + L")";
        wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void ShowTrayMenu() {
        HMENU menu = CreatePopupMenu();
        std::wstring capLabel = L"区域截图 (" + mochi::FormatHotkey(config.captureHotkey) + L")";
        std::wstring fullLabel = L"全屏快截 (" + mochi::FormatHotkey(config.pinClipboardHotkey) + L")";

        AppendMenuW(menu, MF_STRING, IDM_TRAY_CAPTURE, capLabel.c_str());
        AppendMenuW(menu, MF_STRING, IDM_TRAY_QUICKFULL, fullLabel.c_str());
        if (mochi::GetActivePinWindowCount() > 0) {
            AppendMenuW(menu, MF_STRING, IDM_TRAY_CLOSEPINS, L"关闭全部桌面贴图");
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        config.autoStartOnBoot = mochi::IsAutoStartEnabledInRegistry();
        AppendMenuW(menu, MF_STRING | (config.autoStartOnBoot ? MF_CHECKED : MF_UNCHECKED),
                    IDM_TRAY_AUTOSTART, L"开机自动启动");
        AppendMenuW(menu, MF_STRING, IDM_TRAY_SETTINGS, L"打开 Mochi 设置中心...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"退出 MochiShot");

        POINT pt{};
        GetCursorPos(&pt);
        SetForegroundWindow(hostHwnd);
        UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hostHwnd, nullptr);
        PostMessageW(hostHwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);

        switch (cmd) {
        case IDM_TRAY_CAPTURE:
            if (overlay) overlay->StartCaptureSession();
            break;
        case IDM_TRAY_QUICKFULL:
            if (overlay) overlay->StartInstantFullscreenCopy();
            break;
        case IDM_TRAY_CLOSEPINS:
            mochi::CloseAllPinWindows();
            break;
        case IDM_TRAY_AUTOSTART: {
            bool next = !config.autoStartOnBoot;
            if (mochi::SetAutoStartInRegistry(next)) {
                config.autoStartOnBoot = next;
                mochi::SaveAppConfig(config);
            }
            break;
        }
        case IDM_TRAY_SETTINGS:
            if (settings) settings->ShowOrActivate();
            break;
        case IDM_TRAY_EXIT:
            DestroyWindow(hostHwnd);
            break;
        default:
            break;
        }
    }
};

AppRuntime* g_app = nullptr;

LRESULT CALLBACK TrayHostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static UINT s_taskbarRestartMsg = RegisterWindowMessageW(L"TaskbarCreated");

    if (msg == s_taskbarRestartMsg && g_app && g_app->trayAdded) {
        Shell_NotifyIconW(NIM_ADD, &g_app->nid);
        return 0;
    }

    switch (msg) {
    case WM_HOTKEY:
        if (!g_app) return 0;
        if (wParam == HOTKEY_ID_CAPTURE && g_app->overlay) {
            if (g_app->settings && g_app->settings->IsOpen()) {
                // Keep desktop clean if triggered directly
            }
            g_app->overlay->StartCaptureSession();
        } else if (wParam == HOTKEY_ID_QUICKFULL && g_app->overlay) {
            g_app->overlay->StartInstantFullscreenCopy();
        }
        return 0;

    case WM_TRAYICON:
        if (!g_app) return 0;
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            g_app->ShowTrayMenu();
        } else if (LOWORD(lParam) == WM_LBUTTONUP) {
            if (g_app->settings) g_app->settings->ShowOrActivate();
        } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            if (g_app->overlay) g_app->overlay->StartCaptureSession();
        }
        return 0;

    case WM_MOCHI_ACTIVATE_EXIST:
        if (g_app && g_app->settings) {
            g_app->settings->ShowOrActivate();
        }
        return 0;

    case WM_SETTINGCHANGE:
        // Trim idle memory periodically on system broadcast if idle
        if (g_app && (!g_app->overlay || !g_app->overlay->IsActive()) &&
            (!g_app->settings || !g_app->settings->IsOpen())) {
            g_app->engine.TrimIdleMemory();
        }
        break;

    case WM_DESTROY:
        if (g_app) {
            UnregisterHotKey(hwnd, HOTKEY_ID_CAPTURE);
            UnregisterHotKey(hwnd, HOTKEY_ID_QUICKFULL);
            if (g_app->trayAdded) {
                Shell_NotifyIconW(NIM_DELETE, &g_app->nid);
                g_app->trayAdded = false;
            }
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Automated headless self-test suite (--self-test) for local & GitHub Actions verification!
int RunAutomatedSelfTests(HINSTANCE hInstance) {
    // 1. Test Mochi Spring physics convergence & velocity continuity
    {
        mochi::MochiSpring sp(0.0f, 0.30f, 0.85f);
        sp.target = 1.0f;
        for (int i = 0; i < 180; ++i) {
            sp.Step(1.0f / 60.0f);
        }
        if (!sp.IsSettled() || std::fabs(sp.value - 1.0f) > 1e-3f) return 10;
    }

    // 2. Test Mochi G2 Supercircle SDF accuracy (inside < 0, boundary ~ 0, outside > 0)
    {
        float dCenter = mochi::SupercircleSdf(0.0f, 0.0f, 100.0f, 50.0f, 26.0f, 1.0f);
        float dEdge   = mochi::SupercircleSdf(100.0f, 0.0f, 100.0f, 50.0f, 26.0f, 1.0f);
        float dOut    = mochi::SupercircleSdf(120.0f, 60.0f, 100.0f, 50.0f, 26.0f, 1.0f);
        if (!(dCenter < -40.0f && std::fabs(dEdge) < 1e-3f && dOut > 15.0f)) return 11;
    }

    // 3. Test Hotkey formatting & Color scheme interpolation
    {
        mochi::HotkeyBinding hk{ MOD_CONTROL | MOD_SHIFT, 'S' };
        if (mochi::FormatHotkey(hk) != L"Ctrl + Shift + S") return 12;
        auto lightCs = mochi::BuildColorScheme(mochi::SeedAccent::MochiJade, false);
        auto darkCs  = mochi::BuildColorScheme(mochi::SeedAccent::MochiJade, true);
        auto midCs   = mochi::LerpScheme(lightCs, darkCs, 0.5f);
        if (midCs.groupedBackground.r <= darkCs.groupedBackground.r ||
            midCs.groupedBackground.r >= lightCs.groupedBackground.r) return 13;
    }

    // 4. Test Direct3D11 + Direct2D1.1 + HLSL Liquid Glass Shader compilation!
    mochi::LiquidGlassEngine engine;
    if (!engine.Initialize()) return 20;
    if (!engine.SupportsShaderGlass()) return 21;

    // 5. Test G2 Path Geometry & End-to-End D3D11/D2D/HLSL Liquid Glass SwapChain Render!
    {
        auto geom = mochi::CreateG2RoundRectGeometry(engine.D2DFactory(), D2D1::RectF(0, 0, 200, 54), 27.0f, 1.0f);
        if (!geom) return 22;

        HWND testHwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"MochiTest", WS_POPUP, 0, 0, 256, 128, nullptr, nullptr, hInstance, nullptr);
        if (testHwnd) {
            mochi::WindowSwapChain sc;
            if (!engine.EnsureWindowSwapChain(testHwnd, 256, 128, sc)) { DestroyWindow(testHwnd); return 26; }
            if (!engine.BeginFrame(sc, D2D1::ColorF(0.95f, 0.96f, 0.96f, 1.0f))) { DestroyWindow(testHwnd); return 27; }
            if (!engine.CaptureBackdrop(sc)) { DestroyWindow(testHwnd); return 28; }
            mochi::GlassSurfaceSpec gs{};
            gs.rect = D2D1::RectF(24, 24, 232, 80);
            gs.cornerRadiusPx = 28.0f;
            gs.dpiScale = 1.0f;
            gs.mode = mochi::GlassMode::LiquidGlass;
            engine.DrawGlassSurface(sc, gs);
            if (!engine.EndFrame(sc, 0)) { DestroyWindow(testHwnd); return 29; }
            sc.Reset();
            DestroyWindow(testHwnd);
        }
    }

    // 6. Test Mosaic, Blur, WIC PNG/JPEG encoding, and full Annotation Export pipeline
    {
        mochi::AppConfig cfg{};
        mochi::CaptureOverlay overlay(hInstance, &engine, &cfg);
        if (!overlay.SelfTestRenderPipeline()) return 23;

        wchar_t tmpDir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmpDir);
        std::wstring tmpPng = (std::filesystem::path(tmpDir) / L"mochishot_selftest.png").wstring();
        std::vector<uint8_t> dummy(64 * 64 * 4, 200);
        for (size_t i = 3; i < dummy.size(); i += 4) dummy[i] = 255;
        if (!mochi::SaveBgraToFile(engine.WIC(), tmpPng, dummy.data(), 64, 64, false)) return 24;
        std::error_code ec;
        auto fsz = std::filesystem::file_size(tmpPng, ec);
        std::filesystem::remove(tmpPng, ec);
        if (ec || fsz < 64) return 25;
    }

    // 7. Test idle memory trim
    engine.TrimIdleMemory();
    engine.Shutdown();
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::wstring cmdLine = lpCmdLine ? lpCmdLine : L"";
    if (cmdLine.find(L"--self-test") != std::wstring::npos) {
        int code = RunAutomatedSelfTests(hInstance);
        if (SUCCEEDED(coHr)) CoUninitialize();
        return code;
    }

    const bool silentBoot = (cmdLine.find(L"--silent") != std::wstring::npos);
    const bool immediateCapture = (cmdLine.find(L"--capture") != std::wstring::npos);

    // Single-instance check: if already running, wake existing Settings window
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(kTrayHostClass, nullptr);
        if (existing) {
            PostMessageW(existing, WM_MOCHI_ACTIVATE_EXIST, 0, 0);
        }
        CloseHandle(hMutex);
        if (SUCCEEDED(coHr)) CoUninitialize();
        return 0;
    }

    AppRuntime app;
    app.hInstance = hInstance;
    g_app = &app;

    mochi::LoadAppConfig(app.config);
    app.engine.EnsureWIC();
    app.engine.SetIdleCheckCallback([&app]() {
        const bool overlayOpen  = app.overlay && app.overlay->IsActive();
        const bool settingsOpen = app.settings && app.settings->IsOpen();
        if (!overlayOpen && !settingsOpen) {
            app.engine.SuspendGpu();
        }
    });

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayHostWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.lpszClassName = kTrayHostClass;
    RegisterClassExW(&wc);

    app.hostHwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW, kTrayHostClass, L"MochiShot Tray Host", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr
    );

    app.overlay = std::make_unique<mochi::CaptureOverlay>(hInstance, &app.engine, &app.config);

    mochi::SettingsCallbacks cbs{};
    cbs.onRebindHotkeys = [&app](const mochi::HotkeyBinding& c, const mochi::HotkeyBinding& f) {
        return app.RebindHotkeys(c, f);
    };
    cbs.onTriggerCapture = [&app]() {
        if (app.overlay) app.overlay->StartCaptureSession();
    };
    cbs.onQuitApp = [&app]() {
        if (app.hostHwnd) DestroyWindow(app.hostHwnd);
    };
    app.settings = std::make_unique<mochi::SettingsWindow>(hInstance, &app.engine, &app.config, cbs);

    // Register Tray Icon
    app.nid.cbSize = sizeof(app.nid);
    app.nid.hWnd = app.hostHwnd;
    app.nid.uID = 1;
    app.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    app.nid.uCallbackMessage = WM_TRAYICON;
    app.nid.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    std::wstring tip = L"MochiShot 截图 (" + mochi::FormatHotkey(app.config.captureHotkey) + L")";
    wcsncpy_s(app.nid.szTip, tip.c_str(), _TRUNCATE);
    app.trayAdded = (Shell_NotifyIconW(NIM_ADD, &app.nid) != FALSE);

    // Bind Global Hotkeys
    app.RebindHotkeys(app.config.captureHotkey, app.config.pinClipboardHotkey);

    if (immediateCapture) {
        app.overlay->StartCaptureSession();
    } else if (!silentBoot) {
        // Show Mochi Settings Dashboard on manual launch; on boot (--silent), trim memory immediately!
        app.settings->ShowOrActivate();
    } else {
        app.engine.TrimIdleMemory();
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    mochi::CloseAllPinWindows();
    app.settings.reset();
    app.overlay.reset();
    app.engine.Shutdown();
    g_app = nullptr;

    if (hMutex) CloseHandle(hMutex);
    if (SUCCEEDED(coHr)) CoUninitialize();
    return static_cast<int>(msg.wParam);
}
