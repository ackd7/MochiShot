#include "settings_window.h"
#include "mochi_g2.h"
#include "vector_icons.h"
#include "resource.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <cmath>
#include <algorithm>

namespace mochi {

namespace {
constexpr const wchar_t* kSettingsClass = L"MochiShot_SettingsWindow";
constexpr UINT_PTR kSettingsAnimTimer = 2;
constexpr UINT_PTR kDelayedCaptureTimer = 3;
} // namespace

SettingsWindow::SettingsWindow(
    HINSTANCE hInstance,
    LiquidGlassEngine* engine,
    AppConfig* config,
    SettingsCallbacks callbacks
) : hInstance_(hInstance), engine_(engine), config_(config), callbacks_(std::move(callbacks)) {
    QueryPerformanceFrequency(&qpcFreq_);
    QueryPerformanceCounter(&lastQpc_);
    bool dark = ResolveDarkTheme(config_->themeMode);
    fromScheme_ = targetScheme_ = BuildColorScheme(config_->seedAccent, dark);
    schemeAnimT_ = 1.0f;
}

SettingsWindow::~SettingsWindow() {
    CloseAndReleaseMemory();
}

float SettingsWindow::QueryWindowDpiScale() const {
    if (hwnd_) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        static HMODULE user32 = GetModuleHandleW(L"user32.dll");
        static auto fn = user32 ? reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow")) : nullptr;
        if (fn) {
            UINT dpi = fn(hwnd_);
            if (dpi > 0) return static_cast<float>(dpi) / 96.0f;
        }
    }
    HDC screen = GetDC(nullptr);
    int dpiX = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    return (dpiX > 0) ? (static_cast<float>(dpiX) / 96.0f) : 1.0f;
}

void SettingsWindow::StartColorSchemeTransition() {
    bool dark = ResolveDarkTheme(config_->themeMode);
    fromScheme_ = LerpScheme(fromScheme_, targetScheme_, schemeAnimT_);
    targetScheme_ = BuildColorScheme(config_->seedAccent, dark);
    schemeAnimT_ = 0.0f;

    if (hwnd_) {
        BOOL useDark = dark ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &useDark, sizeof(useDark));
    }
}

void SettingsWindow::NotifyHotkeyStatus(bool captureOk, bool fullOk) {
    captureHkOk_ = captureOk;
    fullHkOk_ = fullOk;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::ShowOrActivate() {
    if (IsOpen()) {
        ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
        return;
    }

    if (!engine_->Initialize()) {
        return;
    }

    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInstance_;
        wc.hIcon = LoadIconW(hInstance_, MAKEINTRESOURCEW(IDI_APP_ICON));
        wc.hIconSm = wc.hIcon;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kSettingsClass;
        RegisterClassExW(&wc);
        reg = true;
    }

    // Sync config switches
    config_->autoStartOnBoot = IsAutoStartEnabledInRegistry();
    swAutoStart_.SnapTo(config_->autoStartOnBoot ? 1.0f : 0.0f);
    swAutoCopy_.SnapTo(config_->autoCopyOnCapture ? 1.0f : 0.0f);
    swWinSnap_.SnapTo(config_->windowAutoSnap ? 1.0f : 0.0f);
    swMagnifier_.SnapTo(config_->showMagnifier ? 1.0f : 0.0f);
    segFormatSpring_.SnapTo(static_cast<float>(config_->defaultSaveFormat));
    segGlassSpring_.SnapTo(static_cast<float>(config_->glassMode));
    segThemeSpring_.SnapTo(static_cast<float>(config_->themeMode));

    bool dark = ResolveDarkTheme(config_->themeMode);
    fromScheme_ = targetScheme_ = BuildColorScheme(config_->seedAccent, dark);
    schemeAnimT_ = 1.0f;
    recordingTarget_ = 0;

    float initDpi = QueryWindowDpiScale();
    int winW = static_cast<int>(500.0f * initDpi);
    int winH = static_cast<int>(690.0f * initDpi);

    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (scrW - winW) / 2;
    int posY = std::max(32, (scrH - winH) / 2);

    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW,
        kSettingsClass,
        L"MochiShot 设置",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_SYSMENU,
        posX, posY, winW, winH,
        nullptr, nullptr, hInstance_, this
    );
    if (!hwnd_) return;

    // Enable Windows 11 DWM native rounded corners + shadow
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &cornerPref, sizeof(cornerPref));
    BOOL useDark = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &useDark, sizeof(useDark));
    MARGINS margins = { 0, 0, 0, 1 };
    DwmExtendFrameIntoClientArea(hwnd_, &margins);

    // Re-adjust size if target monitor has different DPI
    float actualDpi = QueryWindowDpiScale();
    if (std::fabs(actualDpi - initDpi) > 0.05f) {
        winW = static_cast<int>(500.0f * actualDpi);
        winH = static_cast<int>(690.0f * actualDpi);
        SetWindowPos(hwnd_, nullptr, 0, 0, winW, winH, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    cachedRamMB_ = GetCurrentWorkingSetMB();
    lastRamTick_ = GetTickCount();
    QueryPerformanceCounter(&lastQpc_);

    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetTimer(hwnd_, kSettingsAnimTimer, 10, nullptr);
    RenderFrame();
}

void SettingsWindow::CloseAndReleaseMemory() {
    recordingTarget_ = 0;
    if (hwnd_) {
        KillTimer(hwnd_, kSettingsAnimTimer);
        KillTimer(hwnd_, kDelayedCaptureTimer);
        HWND h = hwnd_;
        hwnd_ = nullptr;
        if (IsWindow(h)) DestroyWindow(h);
    }
    hitZones_.clear();
    swapChain_.Reset();
    if (engine_) {
        engine_->NotifyWindowClosed();
    }
}

void SettingsWindow::RenderFrame() {
    if (!hwnd_ || IsIconic(hwnd_)) return;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    UINT w = static_cast<UINT>( std::max(1L, rc.right - rc.left) );
    UINT h = static_cast<UINT>( std::max(1L, rc.bottom - rc.top) );

    if (!engine_->EnsureWindowSwapChain(hwnd_, w, h, swapChain_)) return;

    const float dpi = QueryWindowDpiScale();
    const MochiColorScheme cs = LerpScheme(fromScheme_, targetScheme_, schemeAnimT_);

    if (!engine_->BeginFrame(swapChain_, cs.groupedBackground)) return;
    auto* d2d = engine_->D2D();

    hitZones_.clear();
    auto addZone = [&](HitKind kind, const D2D1_RECT_F& r, int index = 0) -> int {
        int id = static_cast<int>(hitZones_.size());
        hitZones_.push_back({ kind, index, r });
        return id;
    };

    ComPtr<ID2D1SolidColorBrush> cardBrush, textBrush, subBrush, divBrush, trackBrush, primaryBrush, onPrimaryBrush, secBrush, onSecBrush, errBrush, hiBrush;
    d2d->CreateSolidColorBrush(cs.groupedContainer, &cardBrush);
    d2d->CreateSolidColorBrush(cs.onSurface, &textBrush);
    d2d->CreateSolidColorBrush(cs.onSurfaceVariant, &subBrush);
    d2d->CreateSolidColorBrush(cs.divider, &divBrush);
    d2d->CreateSolidColorBrush(cs.controlTrack, &trackBrush);
    d2d->CreateSolidColorBrush(cs.primary, &primaryBrush);
    d2d->CreateSolidColorBrush(cs.onPrimary, &onPrimaryBrush);
    d2d->CreateSolidColorBrush(cs.secondaryContainer, &secBrush);
    d2d->CreateSolidColorBrush(cs.onSecondaryContainer, &onSecBrush);
    d2d->CreateSolidColorBrush(cs.error, &errBrush);
    d2d->CreateSolidColorBrush(cs.rowHighlight, &hiBrush);

    auto titleFmt   = engine_->CreateTextFormat(17.5f * dpi, DWRITE_FONT_WEIGHT_BOLD);
    auto secHdrFmt  = engine_->CreateTextFormat(13.0f * dpi, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    auto rowMainFmt = engine_->CreateTextFormat(14.5f * dpi, DWRITE_FONT_WEIGHT_MEDIUM);
    auto rowSubFmt  = engine_->CreateTextFormat(12.0f * dpi, DWRITE_FONT_WEIGHT_NORMAL);
    auto segFmt     = engine_->CreateTextFormat(12.5f * dpi, DWRITE_FONT_WEIGHT_SEMI_BOLD);

    const float headerH = 52.0f * dpi;
    const float railPad = 18.0f * dpi; // Mochi 16-18dp horizontal alignment rail
    const float cardRadius = 26.0f * dpi; // MochiListDefaults.SectionCorner = 26.dp!
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);

    // ========================================================================
    // 1. Scrollable Body Content (clipped below compact title bar, extends under
    //    bottom floating Liquid Glass bar so glass refracts scrolled cards!)
    // ========================================================================
    D2D1_RECT_F scrollViewport = D2D1::RectF(0.0f, headerH, fw, fh);
    d2d->PushAxisAlignedClip(scrollViewport, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    float curY = headerH + 8.0f * dpi - scrollY_ + overscrollSpring_.value;
    const float cardLeft = railPad;
    const float cardRight = fw - railPad;
    const float rowPadH = 16.0f * dpi;

    // Helper: Draw Mochi iOS-26 64x28 Capsule Switch (38x24 pill thumb + jelly stretch!)
    auto drawMochiSwitch = [&](float rightEdge, float rowCenterY, const MochiSpring& sp, HitKind hk) {
        const float swW = 60.0f * dpi;
        const float swH = 28.0f * dpi;
        const float inset = 2.0f * dpi;
        const float thumbBaseW = 34.0f * dpi;
        const float thumbBaseH = 24.0f * dpi;
        const float travel = swW - inset * 2.0f - thumbBaseW;

        D2D1_RECT_F trackR = D2D1::RectF(rightEdge - swW, rowCenterY - swH * 0.5f, rightEdge, rowCenterY + swH * 0.5f);
        int zid = addZone(hk, trackR);

        float p = std::clamp(sp.value, 0.0f, 1.0f);
        D2D1_COLOR_F tCol = LerpColor(cs.controlTrack, cs.primary, p);
        ComPtr<ID2D1SolidColorBrush> swTrackB, thumbB;
        d2d->CreateSolidColorBrush(tCol, &swTrackB);
        d2d->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &thumbB);
        FillG2RoundRect(d2d, engine_->D2DFactory(), trackR, swH * 0.5f, swTrackB.Get());

        float stretch = JellyStretch(sp.velocity * 0.45f, 0.65f);
        float pressGrow = (pressedZoneIdx_ == zid) ? 1.06f : 1.0f;
        float tw = thumbBaseW * (1.0f + std::fabs(stretch)) * pressGrow;
        float th = thumbBaseH * JellyCrossScale(std::fabs(stretch) * 0.5f) * pressGrow;
        float tx = trackR.left + inset + sp.value * travel - (tw - thumbBaseW) * 0.5f;
        float ty = rowCenterY - th * 0.5f;
        D2D1_RECT_F thumbR = D2D1::RectF(tx, ty, tx + tw, ty + th);

        DrawMochiShadow(d2d, engine_->D2DFactory(), thumbR, th * 0.5f, 3.0f, dpi, cs.isDark, 0.9f);
        FillG2RoundRect(d2d, engine_->D2DFactory(), thumbR, th * 0.5f, thumbB.Get());
    };

    // Helper: Draw Mochi Segmented Control (capsule track + spring-sliding elevated pill)
    auto drawSegmented = [&](float rightEdge, float rowCenterY, float totalW,
                             const std::vector<const wchar_t*>& labels, const MochiSpring& sp, HitKind hk) {
        const float segH = 32.0f * dpi;
        const float pad = 3.0f * dpi;
        D2D1_RECT_F trackR = D2D1::RectF(rightEdge - totalW, rowCenterY - segH * 0.5f, rightEdge, rowCenterY + segH * 0.5f);
        FillG2RoundRect(d2d, engine_->D2DFactory(), trackR, segH * 0.5f, trackBrush.Get());

        const int count = static_cast<int>(labels.size());
        const float itemW = (totalW - pad * 2.0f) / static_cast<float>(count);
        const float pillH = segH - pad * 2.0f;

        float pillX = trackR.left + pad + std::clamp(sp.value, 0.0f, static_cast<float>(count - 1)) * itemW;
        float stretch = JellyStretch(sp.velocity * 0.35f, 0.5f);
        float pw = itemW * (1.0f + std::fabs(stretch) * 0.35f);
        D2D1_RECT_F pillR = D2D1::RectF(pillX - (pw - itemW) * 0.5f, trackR.top + pad,
                                        pillX + itemW + (pw - itemW) * 0.5f, trackR.bottom - pad);

        ComPtr<ID2D1SolidColorBrush> pillB;
        d2d->CreateSolidColorBrush(cs.surfaceElevated, &pillB);
        DrawMochiShadow(d2d, engine_->D2DFactory(), pillR, pillH * 0.5f, 2.5f, dpi, cs.isDark, 0.75f);
        FillG2RoundRect(d2d, engine_->D2DFactory(), pillR, pillH * 0.5f, pillB.Get());

        if (segFmt) {
            segFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            segFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            for (int i = 0; i < count; ++i) {
                D2D1_RECT_F ir = D2D1::RectF(trackR.left + pad + i * itemW, trackR.top,
                                             trackR.left + pad + (i + 1) * itemW, trackR.bottom);
                addZone(hk, ir, i);
                bool active = (std::round(sp.target) == i);
                d2d->DrawText(labels[i], static_cast<UINT32>(wcslen(labels[i])), segFmt.Get(), ir,
                              active ? textBrush.Get() : subBrush.Get());
            }
        }
    };

    auto drawSectionHeader = [&](const wchar_t* title) {
        if (secHdrFmt) {
            d2d->DrawText(title, static_cast<UINT32>(wcslen(title)), secHdrFmt.Get(),
                          D2D1::RectF(cardLeft + 14.0f * dpi, curY, cardRight, curY + 20.0f * dpi), subBrush.Get());
        }
        curY += 26.0f * dpi;
    };

    auto drawRowLabels = [&](float topY, float rowH, const wchar_t* title, const wchar_t* sub, float maxRight) {
        if (!sub || sub[0] == L'\0') {
            if (rowMainFmt) {
                rowMainFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                d2d->DrawText(title, static_cast<UINT32>(wcslen(title)), rowMainFmt.Get(),
                              D2D1::RectF(cardLeft + rowPadH, topY, maxRight, topY + rowH), textBrush.Get());
            }
        } else {
            if (rowMainFmt) {
                rowMainFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                d2d->DrawText(title, static_cast<UINT32>(wcslen(title)), rowMainFmt.Get(),
                              D2D1::RectF(cardLeft + rowPadH, topY + 11.0f * dpi, maxRight, topY + 32.0f * dpi), textBrush.Get());
            }
            if (rowSubFmt) {
                rowSubFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                d2d->DrawText(sub, static_cast<UINT32>(wcslen(sub)), rowSubFmt.Get(),
                              D2D1::RectF(cardLeft + rowPadH, topY + 33.0f * dpi, maxRight, topY + rowH - 8.0f * dpi), subBrush.Get());
            }
        }
    };

    // ------------------------------------------------------------------------
    // SECTION 1: 快捷键 (Hotkeys)
    // ------------------------------------------------------------------------
    drawSectionHeader(L"快捷键");
    {
        const float r1H = 60.0f * dpi;
        const float r2H = 60.0f * dpi;
        const float r3H = 46.0f * dpi;
        D2D1_RECT_F secRect = D2D1::RectF(cardLeft, curY, cardRight, curY + r1H + r2H + r3H);
        FillG2RoundRect(d2d, engine_->D2DFactory(), secRect, cardRadius, cardBrush.Get());

        auto drawHkPill = [&](float rowTop, float rowH, const HotkeyBinding& hk, bool isRec, bool isOk, HitKind kind) {
            std::wstring label = isRec ? L"请按下组合键..." : FormatHotkey(hk);
            float pillW = std::max(118.0f * dpi, static_cast<float>(label.size()) * 9.2f * dpi + 28.0f * dpi);
            float pillH = 32.0f * dpi;
            float cy = rowTop + rowH * 0.5f;
            D2D1_RECT_F pr = D2D1::RectF(cardRight - rowPadH - pillW, cy - pillH * 0.5f, cardRight - rowPadH, cy + pillH * 0.5f);
            int zid = addZone(kind, pr);

            ID2D1Brush* bg = isRec ? primaryBrush.Get() : secBrush.Get();
            ID2D1Brush* fg = isRec ? onPrimaryBrush.Get() : onSecBrush.Get();
            if (!isOk && !isRec) {
                bg = errBrush.Get();
                fg = onPrimaryBrush.Get();
            }
            if (hoveredZoneIdx_ == zid && !isRec) {
                FillG2RoundRect(d2d, engine_->D2DFactory(), pr, pillH * 0.5f, bg);
                FillG2RoundRect(d2d, engine_->D2DFactory(), pr, pillH * 0.5f, hiBrush.Get());
            } else {
                FillG2RoundRect(d2d, engine_->D2DFactory(), pr, pillH * 0.5f, bg);
            }
            if (segFmt) {
                segFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                segFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                d2d->DrawText(label.c_str(), static_cast<UINT32>(label.size()), segFmt.Get(), pr, fg);
            }
            return pr.left - 12.0f * dpi;
        };

        // Row 1: Capture Hotkey
        float maxR1 = drawHkPill(curY, r1H, config_->captureHotkey, recordingTarget_ == 1, captureHkOk_, HitKind::RecordCaptureHotkey);
        drawRowLabels(curY, r1H, L"区域截图与批注",
                      captureHkOk_ ? L"点击右侧胶囊可自定义全局热键" : L"当前热键被其他软件占用，请点击更换", maxR1);
        d2d->DrawLine(D2D1::Point2F(cardLeft + rowPadH, curY + r1H), D2D1::Point2F(cardRight, curY + r1H), divBrush.Get(), 1.0f);

        // Row 2: Quick Fullscreen Capture Hotkey
        float y2 = curY + r1H;
        float maxR2 = drawHkPill(y2, r2H, config_->pinClipboardHotkey, recordingTarget_ == 2, fullHkOk_, HitKind::RecordFullHotkey);
        drawRowLabels(y2, r2H, L"快速全屏截图",
                      fullHkOk_ ? L"一键截取整个桌面并复制到剪贴板" : L"当前热键被其他软件占用，请点击更换", maxR2);
        d2d->DrawLine(D2D1::Point2F(cardLeft + rowPadH, y2 + r2H), D2D1::Point2F(cardRight, y2 + r2H), divBrush.Get(), 1.0f);

        // Row 3: Reset default hotkeys
        float y3 = y2 + r2H;
        D2D1_RECT_F resetRow = D2D1::RectF(cardLeft, y3, cardRight, y3 + r3H);
        int rz = addZone(HitKind::ResetHotkeys, resetRow);
        if (hoveredZoneIdx_ == rz) {
            FillG2RoundRect(d2d, engine_->D2DFactory(),
                            D2D1::RectF(cardLeft + 6.0f * dpi, y3 + 4.0f * dpi, cardRight - 6.0f * dpi, y3 + r3H - 4.0f * dpi),
                            (r3H - 8.0f * dpi) * 0.5f, hiBrush.Get());
        }
        if (rowMainFmt) {
            rowMainFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            d2d->DrawText(L"恢复默认快捷键 (Alt + A / Alt + Shift + A)", 37, rowMainFmt.Get(),
                          D2D1::RectF(cardLeft + rowPadH, y3, cardRight - rowPadH, y3 + r3H), primaryBrush.Get());
        }

        curY = secRect.bottom + 24.0f * dpi;
    }

    // ------------------------------------------------------------------------
    // SECTION 2: 启动与捕获 (Startup & Capture)
    // ------------------------------------------------------------------------
    drawSectionHeader(L"启动与捕获");
    {
        const float rowH = 58.0f * dpi;
        const float fmtRowH = 54.0f * dpi;
        const float totalSecH = rowH * 4.0f + fmtRowH;
        D2D1_RECT_F secRect = D2D1::RectF(cardLeft, curY, cardRight, curY + totalSecH);
        FillG2RoundRect(d2d, engine_->D2DFactory(), secRect, cardRadius, cardBrush.Get());

        float ry = curY;
        drawMochiSwitch(cardRight - rowPadH, ry + rowH * 0.5f, swAutoStart_, HitKind::ToggleAutoStart);
        drawRowLabels(ry, rowH, L"开机自动启动", L"登录系统时静默驻留托盘，无需管理员权限", cardRight - 92.0f * dpi);
        d2d->DrawLine(D2D1::Point2F(cardLeft + rowPadH, ry + rowH), D2D1::Point2F(cardRight, ry + rowH), divBrush.Get(), 1.0f);

        ry += rowH;
        drawMochiSwitch(cardRight - rowPadH, ry + rowH * 0.5f, swAutoCopy_, HitKind::ToggleAutoCopy);
        drawRowLabels(ry, rowH, L"自动复制到剪贴板", L"保存文件或贴图时同步写入系统剪贴板", cardRight - 92.0f * dpi);
        d2d->DrawLine(D2D1::Point2F(cardLeft + rowPadH, ry + rowH), D2D1::Point2F(cardRight, ry + rowH), divBrush.Get(), 1.0f);

        ry += rowH;
        drawMochiSwitch(cardRight - rowPadH, ry + rowH * 0.5f, swWinSnap_, HitKind::ToggleWindowSnap);
        drawRowLabels(ry, rowH, L"智能窗口吸附", L"框选前自动识别光标下的窗口边界", cardRight - 92.0f * dpi);
        d2d->DrawLine(D2D1::Point2F(cardLeft + rowPadH, ry + rowH), D2D1::Point2F(cardRight, ry + rowH), divBrush.Get(), 1.0f);

        ry += rowH;
        drawMochiSwitch(cardRight - rowPadH, ry + rowH * 0.5f, swMagnifier_, HitKind::ToggleMagnifier);
        drawRowLabels(ry, rowH, L"放大镜与实时取色", L"框选时显示像素网格，按 C 复制 HEX 色值", cardRight - 92.0f * dpi);
        d2d->DrawLine(D2D1::Point2F(cardLeft + rowPadH, ry + rowH), D2D1::Point2F(cardRight, ry + rowH), divBrush.Get(), 1.0f);

        ry += rowH;
        drawSegmented(cardRight - rowPadH, ry + fmtRowH * 0.5f, 138.0f * dpi,
                      { L"PNG", L"JPEG" }, segFormatSpring_, HitKind::SegSaveFormat);
        drawRowLabels(ry, fmtRowH, L"默认保存格式", nullptr, cardRight - 165.0f * dpi);

        curY = secRect.bottom + 24.0f * dpi;
    }

    // ------------------------------------------------------------------------
    // SECTION 3: 外观与材质 (Mochi Compose Theme & Liquid Glass)
    // ------------------------------------------------------------------------
    drawSectionHeader(L"外观与材质");
    {
        const float r1H = 56.0f * dpi;
        const float r2H = 56.0f * dpi;
        const float r3H = 66.0f * dpi;
        D2D1_RECT_F secRect = D2D1::RectF(cardLeft, curY, cardRight, curY + r1H + r2H + r3H);
        FillG2RoundRect(d2d, engine_->D2DFactory(), secRect, cardRadius, cardBrush.Get());

        float ry = curY;
        drawSegmented(cardRight - rowPadH, ry + r1H * 0.5f, 214.0f * dpi,
                      { L"液态玻璃", L"毛玻璃", L"纯色" }, segGlassSpring_, HitKind::SegGlassMode);
        drawRowLabels(ry, r1H, L"浮层材质", nullptr, cardRight - 236.0f * dpi);
        d2d->DrawLine(D2D1::Point2F(cardLeft + rowPadH, ry + r1H), D2D1::Point2F(cardRight, ry + r1H), divBrush.Get(), 1.0f);

        ry += r1H;
        drawSegmented(cardRight - rowPadH, ry + r2H * 0.5f, 214.0f * dpi,
                      { L"跟随系统", L"浅色", L"深色" }, segThemeSpring_, HitKind::SegThemeMode);
        drawRowLabels(ry, r2H, L"深浅色模式", nullptr, cardRight - 236.0f * dpi);
        d2d->DrawLine(D2D1::Point2F(cardLeft + rowPadH, ry + r2H), D2D1::Point2F(cardRight, ry + r2H), divBrush.Get(), 1.0f);

        // Seed Color Swatches (5 Mochi dual-tone circles like glass.png!)
        ry += r2H;
        drawRowLabels(ry, r3H, L"主题种子色", nullptr, cardLeft + 140.0f * dpi);
        const float swR = 14.0f * dpi;
        const float swGap = 12.0f * dpi;
        float swStartX = cardRight - rowPadH - (5 * swR * 2.0f + 4 * swGap);
        float swCy = ry + r3H * 0.5f;

        for (int i = 0; i < 5; ++i) {
            auto acc = static_cast<SeedAccent>(i);
            auto pal = BuildColorScheme(acc, false);
            float cx = swStartX + i * (swR * 2.0f + swGap) + swR;
            D2D1_RECT_F hitR = D2D1::RectF(cx - swR - 3 * dpi, swCy - swR - 3 * dpi, cx + swR + 3 * dpi, swCy + swR + 3 * dpi);
            addZone(HitKind::AccentSwatch, hitR, i);

            // Split pastel/primary circle inspired by Mochi Theme Picker!
            ComPtr<ID2D1SolidColorBrush> topB, botB;
            d2d->CreateSolidColorBrush(pal.keyPreview, &topB);
            d2d->CreateSolidColorBrush(pal.secondaryContainer, &botB);

            d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, swCy), swR, swR), botB.Get());
            d2d->PushAxisAlignedClip(D2D1::RectF(cx - swR, swCy - swR, cx + swR, swCy), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, swCy), swR, swR), topB.Get());
            d2d->PopAxisAlignedClip();

            if (config_->seedAccent == acc) {
                d2d->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, swCy), swR + 3.2f * dpi, swR + 3.2f * dpi),
                                 primaryBrush.Get(), 2.0f * dpi);
            }
        }

        curY = secRect.bottom + 18.0f * dpi;
    }

    // Shortcut cheatsheet footer
    if (rowSubFmt) {
        const wchar_t* footer = L"编辑内快捷键：Enter / 双击完成复制 · Ctrl+S 保存 · T 贴图到桌面 · Ctrl+Z/Y 撤销重做 · 右键重新框选";
        d2d->DrawText(footer, static_cast<UINT32>(wcslen(footer)), rowSubFmt.Get(),
                      D2D1::RectF(cardLeft + 10.0f * dpi, curY, cardRight - 10.0f * dpi, curY + 44.0f * dpi), subBrush.Get());
    }
    curY += 108.0f * dpi; // Extra clearance for bottom floating Mochi Liquid Glass bar!

    float contentHeight = (curY + scrollY_ - overscrollSpring_.value) - headerH;
    maxScrollY_ = std::max(0.0f, contentHeight - (fh - headerH));
    scrollY_ = std::clamp(scrollY_, 0.0f, maxScrollY_);

    // Subtle Mochi capsule scrollbar when content overflows
    if (maxScrollY_ > 4.0f) {
        float viewH = fh - headerH - 80.0f * dpi;
        float thumbH = std::max(32.0f * dpi, viewH * ((fh - headerH) / contentHeight));
        float ratio = scrollY_ / maxScrollY_;
        float sbY = headerH + 8.0f * dpi + ratio * std::max(0.0f, viewH - thumbH);
        D2D1_RECT_F sbRect = D2D1::RectF(fw - 6.5f * dpi, sbY, fw - 3.0f * dpi, sbY + thumbH);
        FillG2RoundRect(d2d, engine_->D2DFactory(), sbRect, 1.75f * dpi, divBrush.Get());
    }

    d2d->PopAxisAlignedClip();

    // ========================================================================
    // 2. Capture Backdrop so Top Bar & Bottom Floating Mochi Liquid Glass Bar
    //    refract the scrolled settings cards in real time!
    // ========================================================================
    if (config_->glassMode != GlassMode::Solid) {
        engine_->CaptureBackdrop(swapChain_);
    }

    // ========================================================================
    // 3. Compact Title Bar (MochiCompactTitleScaffold)
    // ========================================================================
    {
        ComPtr<ID2D1SolidColorBrush> hdrBg;
        d2d->CreateSolidColorBrush(WithAlpha(cs.groupedBackground, 0.90f), &hdrBg);
        d2d->FillRectangle(D2D1::RectF(0, 0, fw, headerH), hdrBg.Get());
        if (scrollY_ > 2.0f * dpi) {
            d2d->DrawLine(D2D1::Point2F(0, headerH), D2D1::Point2F(fw, headerH), divBrush.Get(), 1.0f);
        }

        // App Badge + Title
        D2D1_RECT_F badgeR = D2D1::RectF(railPad, 13.0f * dpi, railPad + 26.0f * dpi, 39.0f * dpi);
        FillG2RoundRect(d2d, engine_->D2DFactory(), badgeR, 7.0f * dpi, primaryBrush.Get());
        DrawMochiIcon(d2d, engine_->D2DFactory(), MochiIcon::CameraCrop,
                      (badgeR.left + badgeR.right) * 0.5f, (badgeR.top + badgeR.bottom) * 0.5f,
                      15.0f * dpi, onPrimaryBrush.Get(), engine_->RoundStrokeStyle().Get(), 1.8f * dpi);

        if (titleFmt) {
            titleFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            d2d->DrawText(L"MochiShot", 9, titleFmt.Get(),
                          D2D1::RectF(badgeR.right + 10.0f * dpi, 0, fw - 110.0f * dpi, headerH), textBrush.Get());
        }

        // Minimize & Close circular capsule buttons
        const float winBtnS = 30.0f * dpi;
        float closeX = fw - railPad - winBtnS;
        float minX   = closeX - 8.0f * dpi - winBtnS;
        float btnY   = (headerH - winBtnS) * 0.5f;

        D2D1_RECT_F minR   = D2D1::RectF(minX, btnY, minX + winBtnS, btnY + winBtnS);
        D2D1_RECT_F closeR = D2D1::RectF(closeX, btnY, closeX + winBtnS, btnY + winBtnS);
        int minZ   = addZone(HitKind::WinMinimize, minR);
        int closeZ = addZone(HitKind::WinClose, closeR);

        FillG2RoundRect(d2d, engine_->D2DFactory(), minR, winBtnS * 0.5f,
                        (hoveredZoneIdx_ == minZ) ? secBrush.Get() : trackBrush.Get());
        FillG2RoundRect(d2d, engine_->D2DFactory(), closeR, winBtnS * 0.5f,
                        (hoveredZoneIdx_ == closeZ) ? errBrush.Get() : trackBrush.Get());

        d2d->DrawLine(D2D1::Point2F(minX + 9.5f * dpi, headerH * 0.5f),
                      D2D1::Point2F(minX + winBtnS - 9.5f * dpi, headerH * 0.5f),
                      textBrush.Get(), 1.7f * dpi, engine_->RoundStrokeStyle().Get());
        DrawMochiIcon(d2d, engine_->D2DFactory(), MochiIcon::CloseCross,
                      closeX + winBtnS * 0.5f, headerH * 0.5f, 13.0f * dpi,
                      (hoveredZoneIdx_ == closeZ) ? onPrimaryBrush.Get() : textBrush.Get(),
                      engine_->RoundStrokeStyle().Get(), 1.7f * dpi);
    }

    // ========================================================================
    // 4. Floating Mochi Liquid Glass Bottom Toolbar (MochiFloatingToolbar)
    // ========================================================================
    {
        const float barH = 54.0f * dpi;
        const float barMarginH = 22.0f * dpi;
        const float barBottom = fh - 16.0f * dpi;
        D2D1_RECT_F floatBar = D2D1::RectF(barMarginH, barBottom - barH, fw - barMarginH, barBottom);

        GlassSurfaceSpec gs{};
        gs.rect = floatBar;
        gs.cornerRadiusPx = barH * 0.5f; // Full G2 MochiCapsule!
        gs.dpiScale = dpi;
        gs.isDark = cs.isDark;
        gs.thick = false;
        gs.mode = config_->glassMode;
        gs.solidFallback = WithAlpha(cs.groupedContainer, 0.96f);
        gs.hasPressGlow = (glowSpring_.value > 0.01f);
        gs.glowCenterPx = glowCenter_;
        gs.glowRadiusPx = 80.0f * dpi;
        gs.glowIntensity = glowSpring_.value * 0.32f;
        engine_->DrawGlassSurface(swapChain_, gs);

        // Live Memory Footprint indicator on left
        if (GetTickCount() - lastRamTick_ > 1200) {
            cachedRamMB_ = GetCurrentWorkingSetMB();
            lastRamTick_ = GetTickCount();
        }
        wchar_t memBuf[96]{};
        if (toastTimerSec_ > 0.0f && !toastText_.empty()) {
            swprintf_s(memBuf, L"%s", toastText_.c_str());
        } else {
            swprintf_s(memBuf, L"托盘常驻 ~2MB · 渲染中 %.1f MB", cachedRamMB_);
        }
        if (rowSubFmt) {
            rowSubFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            d2d->DrawText(memBuf, static_cast<UINT32>(wcslen(memBuf)), rowSubFmt.Get(),
                          D2D1::RectF(floatBar.left + 18.0f * dpi, floatBar.top, floatBar.right - 150.0f * dpi, floatBar.bottom),
                          (toastTimerSec_ > 0.0f) ? primaryBrush.Get() : subBrush.Get());
        }

        // Primary "立即截图" Mochi Capsule Button on right (ConcentricInner!)
        const float inset = 6.0f * dpi;
        const float btnH = barH - inset * 2.0f;
        const float btnW = 136.0f * dpi;
        D2D1_RECT_F capBtn = D2D1::RectF(floatBar.right - inset - btnW, floatBar.top + inset,
                                         floatBar.right - inset, floatBar.bottom - inset);
        int capZ = addZone(HitKind::PrimaryCaptureBtn, capBtn);
        if (pressedZoneIdx_ == capZ) {
            float sc = pressSpring_.value;
            float cx = (capBtn.left + capBtn.right) * 0.5f;
            float cy = (capBtn.top + capBtn.bottom) * 0.5f;
            capBtn = D2D1::RectF(cx - btnW * 0.5f * sc, cy - btnH * 0.5f * sc, cx + btnW * 0.5f * sc, cy + btnH * 0.5f * sc);
        }
        FillG2RoundRect(d2d, engine_->D2DFactory(), capBtn, btnH * 0.5f, primaryBrush.Get());

        DrawMochiIcon(d2d, engine_->D2DFactory(), MochiIcon::CameraCrop,
                      capBtn.left + 24.0f * dpi, (capBtn.top + capBtn.bottom) * 0.5f,
                      16.0f * dpi, onPrimaryBrush.Get(), engine_->RoundStrokeStyle().Get(), 1.8f * dpi);
        if (segFmt) {
            segFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            segFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            d2d->DrawText(L"立即截图", 4, segFmt.Get(),
                          D2D1::RectF(capBtn.left + 40.0f * dpi, capBtn.top, capBtn.right - 10.0f * dpi, capBtn.bottom),
                          onPrimaryBrush.Get());
        }
    }

    // Crisp subtle window border for Win10 fallback
    ComPtr<ID2D1SolidColorBrush> borderB;
    d2d->CreateSolidColorBrush(cs.outline, &borderB);
    d2d->DrawRectangle(D2D1::RectF(0.5f, 0.5f, fw - 0.5f, fh - 0.5f), borderB.Get(), 1.0f);

    engine_->EndFrame(swapChain_, 1);
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<SettingsWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT SettingsWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    auto ptIn = [](const D2D1_RECT_F& r, float x, float y) {
        return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    };

    switch (msg) {
    case WM_NCCALCSIZE: {
        if (wParam == TRUE) {
            // Borderless client area covering full window while preserving DWM shadow & Win11 snap/round corners
            return 0;
        }
        break;
    }

    case WM_NCHITTEST: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd_, &pt);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        float dpi = QueryWindowDpiScale();
        float x = static_cast<float>(pt.x);
        float y = static_cast<float>(pt.y);

        // Allow dragging window by header bar (except over Minimize/Close buttons)
        if (y >= 0 && y <= 52.0f * dpi && x < (rc.right - 96.0f * dpi)) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            RenderFrame();
        }
        return 0;

    case WM_DPICHANGED: {
        auto* suggested = reinterpret_cast<RECT*>(lParam);
        if (suggested) {
            SetWindowPos(hwnd_, nullptr,
                         suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        RenderFrame();
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        RenderFrame();
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_TIMER: {
        if (wParam == kDelayedCaptureTimer) {
            KillTimer(hwnd_, kDelayedCaptureTimer);
            auto cb = callbacks_.onTriggerCapture;
            CloseAndReleaseMemory();
            if (cb) {
                cb();
            }
            return 0;
        }
        if (wParam == kSettingsAnimTimer) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            float dt = static_cast<float>(now.QuadPart - lastQpc_.QuadPart) / static_cast<float>(qpcFreq_.QuadPart);
            lastQpc_ = now;
            dt = std::min(dt, 0.05f);

            bool dirty = false;
            dirty = swAutoStart_.Step(dt) || dirty;
            dirty = swAutoCopy_.Step(dt) || dirty;
            dirty = swWinSnap_.Step(dt) || dirty;
            dirty = swMagnifier_.Step(dt) || dirty;
            dirty = segFormatSpring_.Step(dt) || dirty;
            dirty = segGlassSpring_.Step(dt) || dirty;
            dirty = segThemeSpring_.Step(dt) || dirty;
            dirty = overscrollSpring_.Step(dt) || dirty;
            dirty = pressSpring_.Step(dt) || dirty;
            dirty = glowSpring_.Step(dt) || dirty;

            if (schemeAnimT_ < 1.0f) {
                schemeAnimT_ = std::min(1.0f, schemeAnimT_ + dt / 0.42f); // 420ms Mochi color transition!
                dirty = true;
            }
            if (std::fabs(scrollVel_) > 1.0f) {
                float nextY = scrollY_ + scrollVel_ * dt;
                if (nextY < 0.0f) {
                    overscrollSpring_.value = RubberBand(-nextY, 300.0f, 48.0f);
                    scrollY_ = 0.0f;
                    scrollVel_ *= 0.65f;
                } else if (nextY > maxScrollY_) {
                    overscrollSpring_.value = -RubberBand(nextY - maxScrollY_, 300.0f, 48.0f);
                    scrollY_ = maxScrollY_;
                    scrollVel_ *= 0.65f;
                } else {
                    scrollY_ = nextY;
                    scrollVel_ *= std::pow(0.002f, dt);
                }
                dirty = true;
            } else {
                scrollVel_ = 0.0f;
            }
            if (toastTimerSec_ > 0.0f) {
                toastTimerSec_ = std::max(0.0f, toastTimerSec_ - dt);
                if (toastTimerSec_ == 0.0f) dirty = true;
            }
            if (dirty) {
                RenderFrame();
            }
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        float dpi = QueryWindowDpiScale();
        scrollVel_ -= static_cast<float>(delta) * 3.2f * dpi;
        overscrollSpring_.target = 0.0f;
        RenderFrame();
        return 0;
    }

    case WM_MOUSEMOVE: {
        float x = static_cast<float>(GET_X_LPARAM(lParam));
        float y = static_cast<float>(GET_Y_LPARAM(lParam));
        mousePos_ = D2D1::Point2F(x, y);
        glowCenter_ = mousePos_;

        int prevHover = hoveredZoneIdx_;
        hoveredZoneIdx_ = -1;
        for (int i = static_cast<int>(hitZones_.size()) - 1; i >= 0; --i) {
            if (ptIn(hitZones_[i].rect, x, y)) {
                hoveredZoneIdx_ = i;
                break;
            }
        }
        SetCursor(LoadCursor(nullptr, hoveredZoneIdx_ >= 0 ? IDC_HAND : IDC_ARROW));
        if (prevHover != hoveredZoneIdx_) {
            RenderFrame();
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        float x = static_cast<float>(GET_X_LPARAM(lParam));
        float y = static_cast<float>(GET_Y_LPARAM(lParam));
        mousePos_ = D2D1::Point2F(x, y);
        SetFocus(hwnd_);
        SetCapture(hwnd_);

        pressedZoneIdx_ = -1;
        for (int i = static_cast<int>(hitZones_.size()) - 1; i >= 0; --i) {
            if (ptIn(hitZones_[i].rect, x, y)) {
                pressedZoneIdx_ = i;
                pressSpring_.target = 0.95f;
                glowCenter_ = mousePos_;
                glowSpring_.target = 1.0f;
                break;
            }
        }
        if (pressedZoneIdx_ < 0 && recordingTarget_ != 0) {
            recordingTarget_ = 0;
        }
        RenderFrame();
        return 0;
    }

    case WM_LBUTTONUP: {
        float x = static_cast<float>(GET_X_LPARAM(lParam));
        float y = static_cast<float>(GET_Y_LPARAM(lParam));
        ReleaseCapture();

        int clicked = pressedZoneIdx_;
        pressedZoneIdx_ = -1;
        pressSpring_.target = 1.0f;
        glowSpring_.target = 0.0f;

        if (clicked >= 0 && clicked < static_cast<int>(hitZones_.size()) && ptIn(hitZones_[clicked].rect, x, y)) {
            const auto zone = hitZones_[clicked];
            switch (zone.kind) {
            case HitKind::WinMinimize:
                ShowWindow(hwnd_, SW_MINIMIZE);
                return 0;
            case HitKind::WinClose:
                CloseAndReleaseMemory();
                return 0;
            case HitKind::RecordCaptureHotkey:
                recordingTarget_ = (recordingTarget_ == 1) ? 0 : 1;
                break;
            case HitKind::RecordFullHotkey:
                recordingTarget_ = (recordingTarget_ == 2) ? 0 : 2;
                break;
            case HitKind::ResetHotkeys: {
                recordingTarget_ = 0;
                HotkeyBinding cDef{ MOD_ALT, 'A' };
                HotkeyBinding fDef{ MOD_ALT | MOD_SHIFT, 'A' };
                config_->captureHotkey = cDef;
                config_->pinClipboardHotkey = fDef;
                SaveAppConfig(*config_);
                if (callbacks_.onRebindHotkeys) callbacks_.onRebindHotkeys(cDef, fDef);
                toastText_ = L"已恢复默认快捷键";
                toastTimerSec_ = 2.5f;
                break;
            }
            case HitKind::ToggleAutoStart: {
                bool next = !config_->autoStartOnBoot;
                if (SetAutoStartInRegistry(next)) {
                    config_->autoStartOnBoot = next;
                    swAutoStart_.target = next ? 1.0f : 0.0f;
                    swAutoStart_.velocity += next ? 5.5f : -5.5f;
                    SaveAppConfig(*config_);
                    toastText_ = next ? L"已开启开机自动启动" : L"已关闭开机自动启动";
                    toastTimerSec_ = 2.2f;
                }
                break;
            }
            case HitKind::ToggleAutoCopy: {
                config_->autoCopyOnCapture = !config_->autoCopyOnCapture;
                swAutoCopy_.target = config_->autoCopyOnCapture ? 1.0f : 0.0f;
                swAutoCopy_.velocity += config_->autoCopyOnCapture ? 5.5f : -5.5f;
                SaveAppConfig(*config_);
                break;
            }
            case HitKind::ToggleWindowSnap: {
                config_->windowAutoSnap = !config_->windowAutoSnap;
                swWinSnap_.target = config_->windowAutoSnap ? 1.0f : 0.0f;
                swWinSnap_.velocity += config_->windowAutoSnap ? 5.5f : -5.5f;
                SaveAppConfig(*config_);
                break;
            }
            case HitKind::ToggleMagnifier: {
                config_->showMagnifier = !config_->showMagnifier;
                swMagnifier_.target = config_->showMagnifier ? 1.0f : 0.0f;
                swMagnifier_.velocity += config_->showMagnifier ? 5.5f : -5.5f;
                SaveAppConfig(*config_);
                break;
            }
            case HitKind::SegSaveFormat:
                config_->defaultSaveFormat = zone.index;
                segFormatSpring_.target = static_cast<float>(zone.index);
                SaveAppConfig(*config_);
                break;
            case HitKind::SegGlassMode:
                config_->glassMode = static_cast<GlassMode>(zone.index);
                segGlassSpring_.target = static_cast<float>(zone.index);
                SaveAppConfig(*config_);
                break;
            case HitKind::SegThemeMode:
                config_->themeMode = static_cast<ThemeMode>(zone.index);
                segThemeSpring_.target = static_cast<float>(zone.index);
                StartColorSchemeTransition();
                SaveAppConfig(*config_);
                break;
            case HitKind::AccentSwatch:
                config_->seedAccent = static_cast<SeedAccent>(zone.index);
                StartColorSchemeTransition();
                SaveAppConfig(*config_);
                break;
            case HitKind::PrimaryCaptureBtn:
                // Hide settings window first so we capture the clean desktop underneath!
                ShowWindow(hwnd_, SW_HIDE);
                SetTimer(hwnd_, kDelayedCaptureTimer, 140, nullptr);
                return 0;
            default:
                break;
            }
        }
        RenderFrame();
        return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        if (recordingTarget_ != 0) {
            UINT vk = static_cast<UINT>(wParam);
            if (vk == VK_ESCAPE) {
                recordingTarget_ = 0;
                RenderFrame();
                return 0;
            }
            // Ignore standalone modifier key presses while waiting for the main key
            if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
                vk == VK_SHIFT   || vk == VK_LSHIFT   || vk == VK_RSHIFT   ||
                vk == VK_MENU    || vk == VK_LMENU    || vk == VK_RMENU    ||
                vk == VK_LWIN    || vk == VK_RWIN) {
                return 0;
            }

            UINT mods = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
            if (GetKeyState(VK_MENU)    & 0x8000) mods |= MOD_ALT;
            if (GetKeyState(VK_SHIFT)   & 0x8000) mods |= MOD_SHIFT;
            if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) mods |= MOD_WIN;

            // Require at least one modifier unless it's a function key F1-F24 or PrintScreen
            bool allowBare = (vk == VK_SNAPSHOT || (vk >= VK_F1 && vk <= VK_F24));
            if (mods == 0 && !allowBare) {
                toastText_ = L"普通按键请搭配 Ctrl / Alt / Shift";
                toastTimerSec_ = 2.5f;
                RenderFrame();
                return 0;
            }

            HotkeyBinding newHk{ mods, vk };
            if (recordingTarget_ == 1) {
                config_->captureHotkey = newHk;
            } else {
                config_->pinClipboardHotkey = newHk;
            }
            recordingTarget_ = 0;
            SaveAppConfig(*config_);

            bool ok = true;
            if (callbacks_.onRebindHotkeys) {
                ok = callbacks_.onRebindHotkeys(config_->captureHotkey, config_->pinClipboardHotkey);
            }
            toastText_ = ok ? (L"已绑定快捷键：" + FormatHotkey(newHk)) : L"热键被其他软件占用，请换一组";
            toastTimerSec_ = 3.0f;
            RenderFrame();
            return 0;
        }

        if (wParam == VK_ESCAPE) {
            CloseAndReleaseMemory();
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        CloseAndReleaseMemory();
        return 0;
    }

    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

} // namespace mochi
