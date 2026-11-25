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

    // Offscreen render helper for automated visual verification
    bool RenderPreviewPng(const std::wstring& outPngPath, bool darkPreview);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void RenderFrame();
    void DrawSettingsContent(ID2D1RenderTarget* d2d, UINT w, UINT h, float dpi, const MochiColorScheme& cs);
    float QueryWindowDpiScale() const;
    void StartColorSchemeTransition();
    void SyncControlsFromConfig(bool snap);
    void ApplySwitchToggle(int switchId, bool newState);
    void ApplySegmentSelect(int segId, int newIndex);

    enum class HitKind {
        None,
        TopCaptureBtn,
        WinMinimize,
        WinClose,
        RecordCaptureHotkey,
        RecordFullHotkey,
        ResetHotkeys,
        SwitchRow,        // Clicking anywhere on row toggles switch (index = 0..3)
        SwitchThumbTrack, // Pressing directly on 64x28 switch allows drag OR click (index = 0..3)
        SegmentedControl, // Click or drag across segments (index = 0..2, subIndex = segment)
        AccentSwatch
    };

    struct HitZone {
        HitKind kind = HitKind::None;
        int index = 0;
        int subIndex = 0;
        D2D1_RECT_F rect{};
    };

    // Authentic MochiSwitch state (64x28 track, 38x24 thumb, press expand + drag rubber-band)
    struct SwitchAnim {
        MochiSpring pos{0.0f, 0.34f, 0.72f};
        MochiSpring press{0.0f, 0.20f, 0.78f};
        D2D1_RECT_F trackRect{};
    };

    // Authentic MochiSegmentedControl state (head + lagging tail liquid trail capsule!)
    struct SegmentedAnim {
        MochiSpring head{0.0f, 0.28f, 0.84f};
        MochiSpring tail{0.0f, 0.38f, 0.88f};
        MochiSpring lift{1.0f, 0.20f, 0.80f};
        int itemCount = 2;
        D2D1_RECT_F trackRect{};
    };

    HINSTANCE hInstance_ = nullptr;
    LiquidGlassEngine* engine_ = nullptr;
    AppConfig* config_ = nullptr;
    SettingsCallbacks callbacks_;
    HWND hwnd_ = nullptr;
    WindowSwapChain swapChain_;

    // Smooth scrolling + rubber-band overscroll
    float scrollY_ = 0.0f;
    float maxScrollY_ = 0.0f;
    float scrollVel_ = 0.0f;
    MochiSpring overscrollSpring_{0.0f, 0.32f, 0.88f};

    SwitchAnim switches_[4];     // 0=AutoStart, 1=AutoCopy, 2=WindowSnap, 3=Magnifier
    SegmentedAnim segments_[3];  // 0=SaveFormat(2), 1=GlassMode(3), 2=ThemeMode(3)

    // 420ms Smooth Mochi Color Scheme Interpolation
    MochiColorScheme fromScheme_{};
    MochiColorScheme targetScheme_{};
    float schemeAnimT_ = 1.0f;

    // Hotkey recording: 0 = none, 1 = captureHotkey, 2 = pinClipboardHotkey
    int recordingTarget_ = 0;
    bool captureHkOk_ = true;
    bool fullHkOk_ = true;
    std::wstring statusNotice_;
    float statusTimerSec_ = 0.0f;

    // Interactive drag & pointer tracking
    D2D1_POINT_2F mousePos_{0, 0};
    D2D1_POINT_2F downPos_{0, 0};
    int hoveredZoneIdx_ = -1;
    int pressedZoneIdx_ = -1;
    int draggingSwitchIdx_ = -1;
    float switchDragStartPos_ = 0.0f;
    bool switchDraggedMoved_ = false;
    int draggingSegIdx_ = -1;

    std::vector<HitZone> hitZones_;
    LARGE_INTEGER lastQpc_{};
    LARGE_INTEGER qpcFreq_{};
};

} // namespace mochi
