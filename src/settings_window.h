#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>
#include "liquid_glass.h"
#include "config.h"

namespace mochi {

struct SettingsCallbacks {
    std::function<bool(const HotkeyBinding& captureHk, const HotkeyBinding& fullHk)> onRebindHotkeys;
    std::function<void()> onTriggerCapture;
    std::function<void()> onQuitApp;
};

class SettingsWindow {
public:
    SettingsWindow(HINSTANCE hInstance, LiquidGlassEngine* engine, AppConfig* config, SettingsCallbacks callbacks);
    ~SettingsWindow();

    void ShowOrActivate();
    void CloseAndReleaseMemory();
    bool IsOpen() const { return hwnd_ != nullptr && IsWindow(hwnd_); }
    void NotifyHotkeyStatus(bool captureOk, bool fullOk);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void RenderFrame();
    float QueryWindowDpiScale() const;
    void StartColorSchemeTransition();

    // Hit-testable interactive regions rebuilt during layout
    enum class HitKind {
        None,
        WinMinimize,
        WinClose,
        RecordCaptureHotkey,
        RecordFullHotkey,
        ResetHotkeys,
        ToggleAutoStart,
        ToggleAutoCopy,
        ToggleWindowSnap,
        ToggleMagnifier,
        SegSaveFormat,
        SegGlassMode,
        SegThemeMode,
        AccentSwatch,
        PrimaryCaptureBtn
    };

    struct HitZone {
        HitKind kind = HitKind::None;
        int index = 0;
        D2D1_RECT_F rect{};
    };

    HINSTANCE hInstance_ = nullptr;
    LiquidGlassEngine* engine_ = nullptr;
    AppConfig* config_ = nullptr;
    SettingsCallbacks callbacks_;
    HWND hwnd_ = nullptr;
    WindowSwapChain swapChain_;

    // Scroll & rubber-band overscroll
    float scrollY_ = 0.0f;
    float maxScrollY_ = 0.0f;
    float scrollVel_ = 0.0f;
    MochiSpring overscrollSpring_{0.0f, 0.34f, 0.86f};

    // Animated Mochi Switches (pos 0..1 with jelly stretch)
    MochiSpring swAutoStart_{0.0f, 0.28f, 0.80f};
    MochiSpring swAutoCopy_{1.0f, 0.28f, 0.80f};
    MochiSpring swWinSnap_{1.0f, 0.28f, 0.80f};
    MochiSpring swMagnifier_{1.0f, 0.28f, 0.80f};

    // Animated Mochi Segmented Controls (selected index float)
    MochiSpring segFormatSpring_{0.0f, 0.28f, 0.84f};
    MochiSpring segGlassSpring_{0.0f, 0.28f, 0.84f};
    MochiSpring segThemeSpring_{0.0f, 0.28f, 0.84f};

    // 420ms Smooth Mochi Color Scheme Interpolation
    MochiColorScheme fromScheme_{};
    MochiColorScheme targetScheme_{};
    float schemeAnimT_ = 1.0f;

    // Hotkey recording state: 0 = none, 1 = captureHotkey, 2 = pinClipboardHotkey
    int recordingTarget_ = 0;
    bool captureHkOk_ = true;
    bool fullHkOk_ = true;
    std::wstring toastText_;
    float toastTimerSec_ = 0.0f;

    // Pointer & press feedback
    D2D1_POINT_2F mousePos_{0, 0};
    int hoveredZoneIdx_ = -1;
    int pressedZoneIdx_ = -1;
    MochiSpring pressSpring_{1.0f, 0.20f, 0.82f};
    MochiSpring glowSpring_{0.0f, 0.28f, 0.88f};
    D2D1_POINT_2F glowCenter_{0, 0};
    std::vector<HitZone> hitZones_;

    double cachedRamMB_ = 0.0;
    DWORD lastRamTick_ = 0;
    LARGE_INTEGER lastQpc_{};
    LARGE_INTEGER qpcFreq_{};
};

} // namespace mochi
