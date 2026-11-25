#include "settings_window.h"
#include "mochi_g2.h"
#include "screen_capture.h"
#include "vector_icons.h"
#include "resource.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <cmath>
#include <algorithm>
#include <cstring>

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
    segments_[0].itemCount = 2;
    segments_[1].itemCount = 3;
    segments_[2].itemCount = 3;

    bool dark = ResolveDarkTheme(config_->themeMode);
    fromScheme_ = targetScheme_ = BuildColorScheme(config_->seedAccent, dark);
    schemeAnimT_ = 1.0f;
    SyncControlsFromConfig(true);
}

SettingsWindow::~SettingsWindow() {
    CloseAndReleaseMemory();
}

void SettingsWindow::SyncControlsFromConfig(bool snap) {
    const bool vals[4] = {
        config_->autoStartOnBoot,
        config_->autoCopyOnCapture,
        config_->windowAutoSnap,
        config_->showMagnifier
    };
    for (int i = 0; i < 4; ++i) {
        float t = vals[i] ? 1.0f : 0.0f;
        if (snap) {
            switches_[i].pos.SnapTo(t);
            switches_[i].press.SnapTo(0.0f);
        } else {
            switches_[i].pos.target = t;
        }
    }

    const float segVals[3] = {
        static_cast<float>(config_->defaultSaveFormat),
        static_cast<float>(config_->glassMode),
        static_cast<float>(config_->themeMode)
    };
    for (int i = 0; i < 3; ++i) {
        if (snap) {
            segments_[i].head.SnapTo(segVals[i]);
            segments_[i].tail.SnapTo(segVals[i]);
            segments_[i].lift.SnapTo(1.0f);
        } else {
            segments_[i].head.target = segVals[i];
            segments_[i].tail.target = segVals[i];
        }
    }
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

void SettingsWindow::ApplySwitchToggle(int idx, bool next) {
    switch (idx) {
    case 0:
        if (SetAutoStartInRegistry(next)) {
            config_->autoStartOnBoot = next;
        } else {
            next = config_->autoStartOnBoot;
        }
        break;
    case 1:
        config_->autoCopyOnCapture = next;
        break;
    case 2:
        config_->windowAutoSnap = next;
        break;
    case 3:
        config_->showMagnifier = next;
        break;
    default:
        return;
    }
    float t = next ? 1.0f : 0.0f;
    if (std::fabs(switches_[idx].pos.target - t) > 0.01f) {
        switches_[idx].pos.velocity += next ? 4.2f : -4.2f;
    }
    switches_[idx].pos.target = t;
    SaveAppConfig(*config_);
}

void SettingsWindow::ApplySegmentSelect(int segId, int newIndex) {
    if (segId < 0 || segId >= 3) return;
    newIndex = std::clamp(newIndex, 0, segments_[segId].itemCount - 1);
    segments_[segId].head.target = static_cast<float>(newIndex);
    segments_[segId].tail.target = static_cast<float>(newIndex);

    if (segId == 0 && config_->defaultSaveFormat != newIndex) {
        config_->defaultSaveFormat = newIndex;
        SaveAppConfig(*config_);
    } else if (segId == 1 && static_cast<int>(config_->glassMode) != newIndex) {
        config_->glassMode = static_cast<GlassMode>(newIndex);
        SaveAppConfig(*config_);
    } else if (segId == 2 && static_cast<int>(config_->themeMode) != newIndex) {
        config_->themeMode = static_cast<ThemeMode>(newIndex);
        StartColorSchemeTransition();
        SaveAppConfig(*config_);
    }
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

    config_->autoStartOnBoot = IsAutoStartEnabledInRegistry();
    SyncControlsFromConfig(true);

    bool dark = ResolveDarkTheme(config_->themeMode);
    fromScheme_ = targetScheme_ = BuildColorScheme(config_->seedAccent, dark);
    schemeAnimT_ = 1.0f;
    recordingTarget_ = 0;
    scrollY_ = 0.0f;
    scrollVel_ = 0.0f;

    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    int workW = (workArea.right > workArea.left) ? (workArea.right - workArea.left) : GetSystemMetrics(SM_CXSCREEN);
    int workH = (workArea.bottom > workArea.top) ? (workArea.bottom - workArea.top) : GetSystemMetrics(SM_CYSCREEN);

    float initDpi = QueryWindowDpiScale();
    int winW = static_cast<int>(476.0f * initDpi);
    int winH = std::min(static_cast<int>(808.0f * initDpi), workH - 36);

    int posX = workArea.left + (workW - winW) / 2;
    int posY = workArea.top + std::max(16, (workH - winH) / 2);

    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW,
        kSettingsClass,
        L"MochiShot 设置",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_SYSMENU,
        posX, posY, winW, winH,
        nullptr, nullptr, hInstance_, this
    );
    if (!hwnd_) return;

    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &cornerPref, sizeof(cornerPref));
    BOOL useDark = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &useDark, sizeof(useDark));
    MARGINS margins = { 0, 0, 0, 1 };
    DwmExtendFrameIntoClientArea(hwnd_, &margins);

    float actualDpi = QueryWindowDpiScale();
    if (std::fabs(actualDpi - initDpi) > 0.05f) {
        winW = static_cast<int>(476.0f * actualDpi);
        winH = std::min(static_cast<int>(808.0f * actualDpi), workH - 36);
        SetWindowPos(hwnd_, nullptr, 0, 0, winW, winH, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    QueryPerformanceCounter(&lastQpc_);
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetTimer(hwnd_, kSettingsAnimTimer, 8, nullptr); // Smooth ~120Hz spring loop
    RenderFrame();
}

void SettingsWindow::CloseAndReleaseMemory() {
    recordingTarget_ = 0;
    draggingSwitchIdx_ = -1;
    draggingSegIdx_ = -1;
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

void SettingsWindow::DrawSettingsContent(
    ID2D1RenderTarget* d2d,
    UINT w,
    UINT h,
    float dpi,
    const MochiColorScheme& cs
) {
    hitZones_.clear();
    auto addZone = [&](HitKind kind, const D2D1_RECT_F& r, int index = 0, int subIndex = 0) -> int {
        int id = static_cast<int>(hitZones_.size());
        hitZones_.push_back({ kind, index, subIndex, r });
        return id;
    };

    ComPtr<ID2D1SolidColorBrush> cardBrush, textBrush, subBrush, divBrush, primaryBrush, onPrimaryBrush, secBrush, onSecBrush, errBrush, hiBrush, borderBrush;
    d2d->CreateSolidColorBrush(cs.groupedContainer, &cardBrush);
    d2d->CreateSolidColorBrush(cs.onSurface, &textBrush);
    d2d->CreateSolidColorBrush(cs.onSurfaceVariant, &subBrush);
    d2d->CreateSolidColorBrush(cs.divider, &divBrush);
    d2d->CreateSolidColorBrush(cs.primary, &primaryBrush);
    d2d->CreateSolidColorBrush(cs.onPrimary, &onPrimaryBrush);
    d2d->CreateSolidColorBrush(cs.secondaryContainer, &secBrush);
    d2d->CreateSolidColorBrush(cs.onSecondaryContainer, &onSecBrush);
    d2d->CreateSolidColorBrush(cs.error, &errBrush);
    d2d->CreateSolidColorBrush(cs.rowHighlight, &hiBrush);
    d2d->CreateSolidColorBrush(cs.outline, &borderBrush);

    auto navTitleFmt  = engine_->CreateTextFormat(17.0f * dpi, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    auto secHdrFmt    = engine_->CreateTextFormat(14.0f * dpi, DWRITE_FONT_WEIGHT_MEDIUM);
    auto rowMainFmt   = engine_->CreateTextFormat(15.0f * dpi, DWRITE_FONT_WEIGHT_NORMAL);
    auto rowSubFmt    = engine_->CreateTextFormat(12.5f * dpi, DWRITE_FONT_WEIGHT_NORMAL);
    auto segBoldFmt   = engine_->CreateTextFormat(13.0f * dpi, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    auto segNormalFmt = engine_->CreateTextFormat(13.0f * dpi, DWRITE_FONT_WEIGHT_NORMAL);

    const float headerH    = 52.0f * dpi;
    const float railPad    = 16.0f * dpi; // Mochi 16dp vertical alignment rail
    const float cardRadius = 26.0f * dpi; // MochiListDefaults.SectionCorner = 26dp
    const float rowPadH    = 16.0f * dpi;
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);
    const float cardLeft  = railPad;
    const float cardRight = fw - railPad;

    // ========================================================================
    // 1. Scrollable Body (Inset Grouped Cards modeled directly after glass.png)
    // ========================================================================
    D2D1_RECT_F scrollViewport = D2D1::RectF(0.0f, headerH, fw, fh);
    d2d->PushAxisAlignedClip(scrollViewport, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    float curY = headerH + 4.0f * dpi - scrollY_ + overscrollSpring_.value;

    auto drawSectionHeader = [&](const wchar_t* title) {
        if (secHdrFmt) {
            secHdrFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            d2d->DrawText(title, static_cast<UINT32>(wcslen(title)), secHdrFmt.Get(),
                          D2D1::RectF(cardLeft + 10.0f * dpi, curY, cardRight, curY + 20.0f * dpi), subBrush.Get());
        }
        curY += 26.0f * dpi;
    };

    auto drawDivider = [&](float y) {
        d2d->DrawLine(
            D2D1::Point2F(cardLeft + rowPadH, y),
            D2D1::Point2F(cardRight - rowPadH, y),
            divBrush.Get(),
            1.0f
        );
    };

    auto drawRowText = [&](float topY, float rowH, const wchar_t* title, const wchar_t* sub, float maxRight) {
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
                              D2D1::RectF(cardLeft + rowPadH, topY + 33.5f * dpi, maxRight, topY + rowH - 8.0f * dpi), subBrush.Get());
            }
        }
    };

    // Authentic iOS-26 / Mochi 64x28 Capsule Switch (38x24 white pill thumb, 2dp inset)
    auto drawMochiSwitchRow = [&](float topY, float rowH, int swIdx,
                                  const wchar_t* title, const wchar_t* sub,
                                  bool isFirst, bool isLast) {
        D2D1_RECT_F fullRow = D2D1::RectF(cardLeft, topY, cardRight, topY + rowH);
        int rowZid = addZone(HitKind::SwitchRow, fullRow, swIdx);

        const float swW = 64.0f * dpi;
        const float swH = 28.0f * dpi;
        const float inset = 2.0f * dpi;
        const float thumbW = 38.0f * dpi;
        const float thumbH = 24.0f * dpi;
        const float travel = swW - inset * 2.0f - thumbW; // 22dp travel!

        const float cy = topY + rowH * 0.5f;
        D2D1_RECT_F trackR = D2D1::RectF(cardRight - rowPadH - swW, cy - swH * 0.5f,
                                         cardRight - rowPadH, cy + swH * 0.5f);
        switches_[swIdx].trackRect = trackR;
        int swZid = addZone(HitKind::SwitchThumbTrack, trackR, swIdx);

        // Subtle row hover/press highlight (clipped cleanly to card G2 corners on first/last row)
        if (hoveredZoneIdx_ == rowZid || hoveredZoneIdx_ == swZid || pressedZoneIdx_ == rowZid) {
            float tlR = isFirst ? cardRadius : 6.0f * dpi;
            float trR = isFirst ? cardRadius : 6.0f * dpi;
            float brR = isLast  ? cardRadius : 6.0f * dpi;
            float blR = isLast  ? cardRadius : 6.0f * dpi;
            D2D1_RECT_F hlR = isFirst || isLast ? fullRow
                : D2D1::RectF(cardLeft + 6.0f * dpi, topY + 2.0f * dpi, cardRight - 6.0f * dpi, topY + rowH - 2.0f * dpi);
            auto hlGeom = CreateG2RoundRectGeometry4(engine_->D2DFactory(), hlR, tlR, trR, brR, blR);
            if (hlGeom) d2d->FillGeometry(hlGeom.Get(), hiBrush.Get());
        }

        drawRowText(topY, rowH, title, sub, trackR.left - 14.0f * dpi);

        // Switch Track: unmistakable high-contrast interpolation between OFF gray and ON deep accent!
        auto& st = switches_[swIdx];
        float pClamp = std::clamp(st.pos.value, 0.0f, 1.0f);
        D2D1_COLOR_F trackCol = LerpColor(cs.switchUncheckedTrack, cs.switchCheckedTrack, pClamp);

        ComPtr<ID2D1SolidColorBrush> trackB, trackRimB, thumbB;
        d2d->CreateSolidColorBrush(trackCol, &trackB);
        FillG2RoundRect(d2d, engine_->D2DFactory(), trackR, swH * 0.5f, trackB.Get());

        // Crisp subtle inner rim on unchecked side so gray slot stands out sharply on white card
        if (pClamp < 0.95f) {
            D2D1_COLOR_F rimCol = cs.isDark
                ? D2D1::ColorF(1, 1, 1, 0.10f * (1.0f - pClamp))
                : D2D1::ColorF(0, 0, 0, 0.08f * (1.0f - pClamp));
            d2d->CreateSolidColorBrush(rimCol, &trackRimB);
            D2D1_RECT_F insetTrack = D2D1::RectF(trackR.left + 0.5f, trackR.top + 0.5f, trackR.right - 0.5f, trackR.bottom - 0.5f);
            StrokeG2RoundRect(d2d, engine_->D2DFactory(), insetTrack, (swH - 1.0f) * 0.5f, trackRimB.Get(), 1.0f);
        }

        // Thumb geometry: MochiSwitch.kt press overflow (1.16x W, 1.24x H) + velocity jelly stretch!
        float kPress = std::clamp(st.press.value, 0.0f, 1.0f);
        float stretch = JellyStretch(st.pos.velocity * 0.55f, 0.60f);
        float wCur = thumbW * (1.0f + 0.16f * kPress) * (1.0f + std::fabs(stretch));
        float hCur = thumbH * (1.0f + 0.24f * kPress) * JellyCrossScale(std::fabs(stretch) * 0.45f);
        float thumbCx = trackR.left + inset + thumbW * 0.5f + st.pos.value * travel;
        D2D1_RECT_F thumbR = D2D1::RectF(thumbCx - wCur * 0.5f, cy - hCur * 0.5f,
                                         thumbCx + wCur * 0.5f, cy + hCur * 0.5f);

        DrawThumbContactShadow(d2d, engine_->D2DFactory(), thumbR, hCur * 0.5f, dpi, cs.isDark, kPress);
        d2d->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &thumbB);
        FillG2RoundRect(d2d, engine_->D2DFactory(), thumbR, hCur * 0.5f, thumbB.Get());
    };

    // Authentic MochiSegmentedControl (opaque capsule slot + trail-stretching raised thumb!)
    auto drawMochiSegmented = [&](float rightEdge, float rowCenterY, float totalW,
                                  const std::vector<const wchar_t*>& labels, int segIdx) {
        auto& seg = segments_[segIdx];
        const int count = static_cast<int>(labels.size());
        seg.itemCount = count;

        const float segH = 34.0f * dpi;
        const float pad  = 2.5f * dpi;
        D2D1_RECT_F trackR = D2D1::RectF(rightEdge - totalW, rowCenterY - segH * 0.5f, rightEdge, rowCenterY + segH * 0.5f);
        seg.trackRect = trackR;

        ComPtr<ID2D1SolidColorBrush> slotB, thumbB;
        d2d->CreateSolidColorBrush(cs.segmentedTrack, &slotB);
        d2d->CreateSolidColorBrush(cs.surfaceElevated, &thumbB);
        FillG2RoundRect(d2d, engine_->D2DFactory(), trackR, segH * 0.5f, slotB.Get());

        const float itemW = (totalW - pad * 2.0f) / static_cast<float>(count);
        const float baseThumbH = segH - pad * 2.0f;

        // MochiSegmentedControl.kt liquid trail-stretch formula (head + lagging tail!)
        float backPos = seg.head.value + (seg.tail.value - seg.head.value) * 0.55f;
        float minIdx = std::min(seg.head.value, backPos);
        float maxIdx = std::max(seg.head.value, backPos);
        float span = std::fabs(seg.head.value - backPos);
        float thin = 1.0f - 0.12f * std::clamp(span, 0.0f, 1.2f);

        float lv = seg.lift.value;
        float rawLeft  = trackR.left + pad + minIdx * itemW;
        float rawRight = trackR.left + pad + (maxIdx + 1.0f) * itemW;
        float rawMidX  = (rawLeft + rawRight) * 0.5f;
        float pillW    = (rawRight - rawLeft) * lv;
        float pillH    = baseThumbH * thin * (1.0f + (lv - 1.0f) * 0.55f);

        D2D1_RECT_F pillR = D2D1::RectF(
            rawMidX - pillW * 0.5f,
            rowCenterY - pillH * 0.5f,
            rawMidX + pillW * 0.5f,
            rowCenterY + pillH * 0.5f
        );

        DrawThumbContactShadow(d2d, engine_->D2DFactory(), pillR, pillH * 0.5f, dpi, cs.isDark, (lv - 1.0f) * 4.0f);
        FillG2RoundRect(d2d, engine_->D2DFactory(), pillR, pillH * 0.5f, thumbB.Get());

        if (segBoldFmt && segNormalFmt) {
            segBoldFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            segBoldFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            segNormalFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            segNormalFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            for (int i = 0; i < count; ++i) {
                D2D1_RECT_F ir = D2D1::RectF(trackR.left + pad + i * itemW, trackR.top,
                                             trackR.left + pad + (i + 1) * itemW, trackR.bottom);
                int zid = addZone(HitKind::SegmentedControl, ir, segIdx, i);

                // Smooth weight crossfade just like MochiSegmentedControl.kt lines 380-399!
                float selWeight = std::clamp(1.0f - std::fabs(seg.head.value - static_cast<float>(i)), 0.0f, 1.0f);
                float dimPress = (pressedZoneIdx_ == zid && selWeight < 0.5f) ? 0.55f : 1.0f;

                if (selWeight > 0.02f) {
                    textBrush->SetOpacity(selWeight * dimPress);
                    d2d->DrawText(labels[i], static_cast<UINT32>(wcslen(labels[i])), segBoldFmt.Get(), ir, textBrush.Get());
                }
                if (selWeight < 0.98f) {
                    textBrush->SetOpacity((1.0f - selWeight) * 0.80f * dimPress);
                    d2d->DrawText(labels[i], static_cast<UINT32>(wcslen(labels[i])), segNormalFmt.Get(), ir, textBrush.Get());
                }
                textBrush->SetOpacity(1.0f);
            }
        }
    };

    // ------------------------------------------------------------------------
    // SECTION 1: 快捷键
    // ------------------------------------------------------------------------
    drawSectionHeader(L"快捷键");
    {
        const float r1H = 60.0f * dpi;
        const float r2H = 60.0f * dpi;
        const float r3H = 48.0f * dpi;
        D2D1_RECT_F secRect = D2D1::RectF(cardLeft, curY, cardRight, curY + r1H + r2H + r3H);
        FillG2RoundRect(d2d, engine_->D2DFactory(), secRect, cardRadius, cardBrush.Get());

        auto drawHkRow = [&](float rowTop, float rowH, const HotkeyBinding& hk,
                             int recId, bool isOk, HitKind kind,
                             const wchar_t* title, const wchar_t* normalSub) {
            bool isRec = (recordingTarget_ == recId);
            std::wstring pillText = isRec ? L"按下新按键..." : FormatHotkey(hk);
            float pillW = std::max(114.0f * dpi, static_cast<float>(pillText.size()) * 8.8f * dpi + 28.0f * dpi);
            float pillH = 34.0f * dpi;
            float cy = rowTop + rowH * 0.5f;

            D2D1_RECT_F rowR = D2D1::RectF(cardLeft, rowTop, cardRight, rowTop + rowH);
            D2D1_RECT_F pr = D2D1::RectF(cardRight - rowPadH - pillW, cy - pillH * 0.5f, cardRight - rowPadH, cy + pillH * 0.5f);
            int zid = addZone(kind, rowR); // Whole row clickable to record hotkey!

            if (hoveredZoneIdx_ == zid && !isRec) {
                D2D1_RECT_F hlR = (recId == 1)
                    ? rowR
                    : D2D1::RectF(cardLeft + 6.0f * dpi, rowTop + 2.0f * dpi, cardRight - 6.0f * dpi, rowTop + rowH - 2.0f * dpi);
                auto hlG = CreateG2RoundRectGeometry4(engine_->D2DFactory(), hlR,
                    recId == 1 ? cardRadius : 6.0f * dpi, recId == 1 ? cardRadius : 6.0f * dpi, 6.0f * dpi, 6.0f * dpi);
                if (hlG) d2d->FillGeometry(hlG.Get(), hiBrush.Get());
            }

            ID2D1Brush* bg = isRec ? primaryBrush.Get() : secBrush.Get();
            ID2D1Brush* fg = isRec ? onPrimaryBrush.Get() : onSecBrush.Get();
            if (!isOk && !isRec) {
                bg = errBrush.Get();
                fg = onPrimaryBrush.Get();
            }
            FillG2RoundRect(d2d, engine_->D2DFactory(), pr, pillH * 0.5f, bg);
            if (isRec) {
                D2D1_RECT_F outerRing = D2D1::RectF(pr.left - 2.5f * dpi, pr.top - 2.5f * dpi, pr.right + 2.5f * dpi, pr.bottom + 2.5f * dpi);
                StrokeG2RoundRect(d2d, engine_->D2DFactory(), outerRing, (pillH + 5.0f * dpi) * 0.5f, primaryBrush.Get(), 1.5f * dpi);
            }
            if (segBoldFmt) {
                segBoldFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                segBoldFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                d2d->DrawText(pillText.c_str(), static_cast<UINT32>(pillText.size()), segBoldFmt.Get(), pr, fg);
            }

            const wchar_t* subToShow = isRec
                ? L"请按组合键 (如 Ctrl+Shift+A)，按 Esc 取消"
                : (isOk ? normalSub : L"该快捷键已被其他程序占用，请点击更换");
            drawRowText(rowTop, rowH, title, subToShow, pr.left - 12.0f * dpi);
        };

        drawHkRow(curY, r1H, config_->captureHotkey, 1, captureHkOk_,
                  HitKind::RecordCaptureHotkey, L"区域截图与批注", L"点击此行自定义截图热键");
        drawDivider(curY + r1H);

        float y2 = curY + r1H;
        drawHkRow(y2, r2H, config_->pinClipboardHotkey, 2, fullHkOk_,
                  HitKind::RecordFullHotkey, L"全屏快速截图", L"直接截取全屏并写入剪贴板");
        drawDivider(y2 + r2H);

        float y3 = y2 + r2H;
        D2D1_RECT_F resetRow = D2D1::RectF(cardLeft, y3, cardRight, y3 + r3H);
        int rz = addZone(HitKind::ResetHotkeys, resetRow);
        if (hoveredZoneIdx_ == rz || pressedZoneIdx_ == rz) {
            auto hlG = CreateG2RoundRectGeometry4(engine_->D2DFactory(), resetRow, 4.0f * dpi, 4.0f * dpi, cardRadius, cardRadius);
            if (hlG) d2d->FillGeometry(hlG.Get(), hiBrush.Get());
        }
        if (rowMainFmt) {
            rowMainFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            std::wstring resetLabel = (statusTimerSec_ > 0.0f && !statusNotice_.empty())
                ? statusNotice_
                : L"恢复默认快捷键";
            d2d->DrawText(resetLabel.c_str(), static_cast<UINT32>(resetLabel.size()), rowMainFmt.Get(),
                          D2D1::RectF(cardLeft + rowPadH, y3, cardRight - rowPadH, y3 + r3H), primaryBrush.Get());
        }

        curY = secRect.bottom + 18.0f * dpi;
    }

    // ------------------------------------------------------------------------
    // SECTION 2: 通用与捕获 (Switches + Default Format)
    // ------------------------------------------------------------------------
    drawSectionHeader(L"通用与捕获");
    {
        const float swRowH  = 56.0f * dpi;
        const float segRowH = 52.0f * dpi;
        const float totalSecH = swRowH * 4.0f + segRowH;
        D2D1_RECT_F secRect = D2D1::RectF(cardLeft, curY, cardRight, curY + totalSecH);
        FillG2RoundRect(d2d, engine_->D2DFactory(), secRect, cardRadius, cardBrush.Get());

        float ry = curY;
        drawMochiSwitchRow(ry, swRowH, 0, L"开机自动启动", L"登录 Windows 时静默驻留系统托盘", true, false);
        drawDivider(ry + swRowH);

        ry += swRowH;
        drawMochiSwitchRow(ry, swRowH, 1, L"自动复制到剪贴板", L"保存图像或贴图时同步写入剪贴板", false, false);
        drawDivider(ry + swRowH);

        ry += swRowH;
        drawMochiSwitchRow(ry, swRowH, 2, L"智能窗口吸附", L"框选前自动贴合光标下的窗口边界", false, false);
        drawDivider(ry + swRowH);

        ry += swRowH;
        drawMochiSwitchRow(ry, swRowH, 3, L"放大镜与像素取色", L"框选时显示网格，按 C 复制色值", false, false);
        drawDivider(ry + swRowH);

        ry += swRowH;
        drawMochiSegmented(cardRight - rowPadH, ry + segRowH * 0.5f, 146.0f * dpi, { L"PNG", L"JPEG" }, 0);
        drawRowText(ry, segRowH, L"默认保存格式", nullptr, cardRight - 170.0f * dpi);

        curY = secRect.bottom + 18.0f * dpi;
    }

    // ------------------------------------------------------------------------
    // SECTION 3: 外观与主题色 (Modeled directly after glass.png!)
    // ------------------------------------------------------------------------
    drawSectionHeader(L"外观与主题");
    {
        const float r1H = 52.0f * dpi;
        const float r2H = 52.0f * dpi;
        const float r3H = 68.0f * dpi;
        D2D1_RECT_F secRect = D2D1::RectF(cardLeft, curY, cardRight, curY + r1H + r2H + r3H);
        FillG2RoundRect(d2d, engine_->D2DFactory(), secRect, cardRadius, cardBrush.Get());

        float ry = curY;
        drawMochiSegmented(cardRight - rowPadH, ry + r1H * 0.5f, 224.0f * dpi,
                           { L"液态玻璃", L"毛玻璃", L"纯色" }, 1);
        drawRowText(ry, r1H, L"浮层材质", nullptr, cardRight - 246.0f * dpi);
        drawDivider(ry + r1H);

        ry += r1H;
        drawMochiSegmented(cardRight - rowPadH, ry + r2H * 0.5f, 224.0f * dpi,
                           { L"跟随系统", L"浅色", L"深色" }, 2);
        drawRowText(ry, r2H, L"深浅色", nullptr, cardRight - 246.0f * dpi);
        drawDivider(ry + r2H);

        // 3-Quadrant Mochi Tonal Pie Swatches (exact match to glass.png "主题色"!)
        ry += r2H;
        drawRowText(ry, r3H, L"主题色", nullptr, cardLeft + 100.0f * dpi);

        const float swR   = 18.0f * dpi;
        const float swGap = 14.0f * dpi;
        float swStartX = cardRight - rowPadH - (5.0f * swR * 2.0f + 4.0f * swGap);
        float swCy     = ry + r3H * 0.5f;

        for (int i = 0; i < 5; ++i) {
            auto acc = static_cast<SeedAccent>(i);
            auto pal = BuildColorScheme(acc, false);
            float cx = swStartX + i * (swR * 2.0f + swGap) + swR;

            D2D1_RECT_F hitR = D2D1::RectF(cx - swR - 4 * dpi, swCy - swR - 4 * dpi, cx + swR + 4 * dpi, swCy + swR + 4 * dpi);
            addZone(HitKind::AccentSwatch, hitR, i);

            float drawR = (config_->seedAccent == acc) ? (swR - 2.5f * dpi) : swR;

            ComPtr<ID2D1SolidColorBrush> bTop, bBL, bBR;
            d2d->CreateSolidColorBrush(pal.pieTop, &bTop);
            d2d->CreateSolidColorBrush(pal.pieBottomLeft, &bBL);
            d2d->CreateSolidColorBrush(pal.pieBottomRight, &bBR);

            // Top semicircle
            d2d->PushAxisAlignedClip(D2D1::RectF(cx - drawR, swCy - drawR, cx + drawR, swCy + 0.5f), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, swCy), drawR, drawR), bTop.Get());
            d2d->PopAxisAlignedClip();

            // Bottom-left quadrant
            d2d->PushAxisAlignedClip(D2D1::RectF(cx - drawR, swCy, cx + 0.5f, swCy + drawR), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, swCy), drawR, drawR), bBL.Get());
            d2d->PopAxisAlignedClip();

            // Bottom-right quadrant
            d2d->PushAxisAlignedClip(D2D1::RectF(cx, swCy, cx + drawR, swCy + drawR), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, swCy), drawR, drawR), bBR.Get());
            d2d->PopAxisAlignedClip();

            // Selected gapped outer ring (just like glass.png!)
            if (config_->seedAccent == acc) {
                ComPtr<ID2D1SolidColorBrush> ringB;
                d2d->CreateSolidColorBrush(pal.primary, &ringB);
                d2d->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, swCy), swR + 1.5f * dpi, swR + 1.5f * dpi),
                                 ringB.Get(), 2.2f * dpi);
            }
        }

        curY = secRect.bottom + 22.0f * dpi;
    }

    float contentHeight = (curY + scrollY_ - overscrollSpring_.value) - headerH;
    maxScrollY_ = std::max(0.0f, contentHeight - (fh - headerH));
    scrollY_ = std::clamp(scrollY_, 0.0f, maxScrollY_);

    if (maxScrollY_ > 4.0f) {
        float viewH = fh - headerH - 16.0f * dpi;
        float thumbH = std::max(36.0f * dpi, viewH * ((fh - headerH) / contentHeight));
        float ratio = scrollY_ / maxScrollY_;
        float sbY = headerH + 8.0f * dpi + ratio * std::max(0.0f, viewH - thumbH);
        D2D1_RECT_F sbRect = D2D1::RectF(fw - 6.0f * dpi, sbY, fw - 2.8f * dpi, sbY + thumbH);
        FillG2RoundRect(d2d, engine_->D2DFactory(), sbRect, 1.6f * dpi, divBrush.Get());
    }

    d2d->PopAxisAlignedClip();

    // ========================================================================
    // 2. Centered Compact Title Bar (MochiCompactTitleScaffold — like glass.png!)
    // ========================================================================
    {
        ComPtr<ID2D1SolidColorBrush> hdrBg, circleBtnBg;
        d2d->CreateSolidColorBrush(WithAlpha(cs.groupedBackground, 0.94f), &hdrBg);
        d2d->CreateSolidColorBrush(cs.groupedContainer, &circleBtnBg);
        d2d->FillRectangle(D2D1::RectF(0, 0, fw, headerH), hdrBg.Get());
        if (scrollY_ > 2.0f * dpi) {
            d2d->DrawLine(D2D1::Point2F(0, headerH), D2D1::Point2F(fw, headerH), divBrush.Get(), 1.0f);
        }

        // Centered Title ("设置")
        if (navTitleFmt) {
            navTitleFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            navTitleFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            d2d->DrawText(L"设置", 2, navTitleFmt.Get(), D2D1::RectF(80.0f * dpi, 0, fw - 80.0f * dpi, headerH), textBrush.Get());
        }

        // Top-Left Circular Mochi Action Button (Quick Capture — styled like top-right circle in glass.png!)
        const float topBtnS = 34.0f * dpi;
        const float topBtnY = (headerH - topBtnS) * 0.5f;
        D2D1_RECT_F capR = D2D1::RectF(railPad, topBtnY, railPad + topBtnS, topBtnY + topBtnS);
        int capZ = addZone(HitKind::TopCaptureBtn, capR);
        FillG2RoundRect(d2d, engine_->D2DFactory(), capR, topBtnS * 0.5f,
                        (hoveredZoneIdx_ == capZ) ? secBrush.Get() : circleBtnBg.Get());
        StrokeG2RoundRect(d2d, engine_->D2DFactory(), capR, topBtnS * 0.5f, borderBrush.Get(), 1.0f);
        DrawMochiIcon(d2d, engine_->D2DFactory(), MochiIcon::CameraCrop,
                      railPad + topBtnS * 0.5f, headerH * 0.5f, 16.5f * dpi,
                      primaryBrush.Get(), engine_->RoundStrokeStyle().Get(), 1.8f * dpi);

        // Top-Right Minimize & Close Circular Buttons
        const float winBtnS = 32.0f * dpi;
        const float winBtnY = (headerH - winBtnS) * 0.5f;
        float closeX = fw - railPad - winBtnS;
        float minX   = closeX - 8.0f * dpi - winBtnS;

        D2D1_RECT_F minR   = D2D1::RectF(minX, winBtnY, minX + winBtnS, winBtnY + winBtnS);
        D2D1_RECT_F closeR = D2D1::RectF(closeX, winBtnY, closeX + winBtnS, winBtnY + winBtnS);
        int minZ   = addZone(HitKind::WinMinimize, minR);
        int closeZ = addZone(HitKind::WinClose, closeR);

        FillG2RoundRect(d2d, engine_->D2DFactory(), minR, winBtnS * 0.5f,
                        (hoveredZoneIdx_ == minZ) ? secBrush.Get() : circleBtnBg.Get());
        StrokeG2RoundRect(d2d, engine_->D2DFactory(), minR, winBtnS * 0.5f, borderBrush.Get(), 1.0f);

        FillG2RoundRect(d2d, engine_->D2DFactory(), closeR, winBtnS * 0.5f,
                        (hoveredZoneIdx_ == closeZ) ? errBrush.Get() : circleBtnBg.Get());
        if (hoveredZoneIdx_ != closeZ) {
            StrokeG2RoundRect(d2d, engine_->D2DFactory(), closeR, winBtnS * 0.5f, borderBrush.Get(), 1.0f);
        }

        d2d->DrawLine(D2D1::Point2F(minX + 10.5f * dpi, headerH * 0.5f),
                      D2D1::Point2F(minX + winBtnS - 10.5f * dpi, headerH * 0.5f),
                      textBrush.Get(), 1.6f * dpi, engine_->RoundStrokeStyle().Get());
        DrawMochiIcon(d2d, engine_->D2DFactory(), MochiIcon::CloseCross,
                      closeX + winBtnS * 0.5f, headerH * 0.5f, 12.5f * dpi,
                      (hoveredZoneIdx_ == closeZ) ? onPrimaryBrush.Get() : textBrush.Get(),
                      engine_->RoundStrokeStyle().Get(), 1.6f * dpi);
    }

    d2d->DrawRectangle(D2D1::RectF(0.5f, 0.5f, fw - 0.5f, fh - 0.5f), borderBrush.Get(), 1.0f);
}

void SettingsWindow::RenderFrame() {
    if (!hwnd_ || IsIconic(hwnd_)) return;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    UINT w = static_cast<UINT>(std::max(1L, rc.right - rc.left));
    UINT h = static_cast<UINT>(std::max(1L, rc.bottom - rc.top));

    if (!engine_->EnsureWindowSwapChain(hwnd_, w, h, swapChain_)) return;

    const float dpi = QueryWindowDpiScale();
    const MochiColorScheme cs = LerpScheme(fromScheme_, targetScheme_, schemeAnimT_);

    if (!engine_->BeginFrame(swapChain_, cs.groupedBackground)) return;
    DrawSettingsContent(engine_->D2D(), w, h, dpi, cs);
    engine_->EndFrame(swapChain_, 1);
}

bool SettingsWindow::RenderPreviewPng(const std::wstring& outPngPath, bool darkPreview) {
    if (!engine_->Initialize()) return false;

    const float dpi = 1.5f; // Crisp 144 DPI retina preview
    const int w = static_cast<int>(476.0f * dpi);
    const int h = static_cast<int>(808.0f * dpi);

    ComPtr<IWICBitmap> wicBmp;
    if (FAILED(engine_->EnsureWIC()->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &wicBmp))) {
        return false;
    }
    D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f
    );
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(engine_->D2DFactory()->CreateWicBitmapRenderTarget(wicBmp.Get(), rtp, &rt))) {
        return false;
    }

    SyncControlsFromConfig(true);
    // Show 2 switches ON and 2 switches OFF in preview so ON vs OFF contrast is clearly demonstrated!
    switches_[0].pos.SnapTo(0.0f);
    switches_[1].pos.SnapTo(1.0f);
    switches_[2].pos.SnapTo(1.0f);
    switches_[3].pos.SnapTo(0.0f);
    segments_[2].head.SnapTo(darkPreview ? 2.0f : 1.0f);
    segments_[2].tail.SnapTo(darkPreview ? 2.0f : 1.0f);

    MochiColorScheme cs = BuildColorScheme(SeedAccent::MochiTeal, darkPreview);
    rt->BeginDraw();
    rt->Clear(cs.groupedBackground);
    DrawSettingsContent(rt.Get(), static_cast<UINT>(w), static_cast<UINT>(h), dpi, cs);
    if (FAILED(rt->EndDraw())) return false;

    WICRect lockR{ 0, 0, w, h };
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(wicBmp->Lock(&lockR, WICBitmapLockRead, &lock))) return false;
    UINT stride = 0, cb = 0;
    BYTE* ptr = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&cb, &ptr)) || !ptr) return false;

    std::vector<uint8_t> bgra(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int y = 0; y < h; ++y) {
        std::memcpy(bgra.data() + static_cast<size_t>(y) * w * 4, ptr + static_cast<size_t>(y) * stride, static_cast<size_t>(w) * 4);
    }
    return SaveBgraToFile(engine_->EnsureWIC(), outPngPath, bgra.data(), w, h, false);
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
    case WM_NCCALCSIZE:
        if (wParam == TRUE) return 0;
        break;

    case WM_NCHITTEST: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd_, &pt);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        float dpi = QueryWindowDpiScale();
        float x = static_cast<float>(pt.x);
        float y = static_cast<float>(pt.y);

        // Drag window by top bar center (between left Capture button and right Min/Close buttons)
        if (y >= 0 && y <= 54.0f * dpi && x > 58.0f * dpi && x < (rc.right - 92.0f * dpi)) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) RenderFrame();
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
            if (cb) cb();
            return 0;
        }
        if (wParam == kSettingsAnimTimer) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            float dt = static_cast<float>(now.QuadPart - lastQpc_.QuadPart) / static_cast<float>(qpcFreq_.QuadPart);
            lastQpc_ = now;
            dt = std::min(dt, 0.05f);

            bool dirty = false;
            for (auto& sw : switches_) {
                dirty = sw.pos.Step(dt) || dirty;
                dirty = sw.press.Step(dt) || dirty;
            }
            for (auto& sg : segments_) {
                dirty = sg.head.Step(dt) || dirty;
                dirty = sg.tail.Step(dt) || dirty;
                dirty = sg.lift.Step(dt) || dirty;
            }
            dirty = overscrollSpring_.Step(dt) || dirty;

            if (schemeAnimT_ < 1.0f) {
                schemeAnimT_ = std::min(1.0f, schemeAnimT_ + dt / 0.42f);
                dirty = true;
            }
            if (std::fabs(scrollVel_) > 1.0f) {
                float nextY = scrollY_ + scrollVel_ * dt;
                if (nextY < 0.0f) {
                    overscrollSpring_.value = RubberBand(-nextY, 300.0f, 44.0f);
                    scrollY_ = 0.0f;
                    scrollVel_ *= 0.60f;
                } else if (nextY > maxScrollY_) {
                    overscrollSpring_.value = -RubberBand(nextY - maxScrollY_, 300.0f, 44.0f);
                    scrollY_ = maxScrollY_;
                    scrollVel_ *= 0.60f;
                } else {
                    scrollY_ = nextY;
                    scrollVel_ *= std::pow(0.0018f, dt);
                }
                dirty = true;
            } else {
                scrollVel_ = 0.0f;
            }
            if (statusTimerSec_ > 0.0f) {
                statusTimerSec_ = std::max(0.0f, statusTimerSec_ - dt);
                if (statusTimerSec_ == 0.0f) dirty = true;
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
        scrollVel_ -= static_cast<float>(delta) * 3.0f * dpi;
        overscrollSpring_.target = 0.0f;
        RenderFrame();
        return 0;
    }

    case WM_MOUSEMOVE: {
        float x = static_cast<float>(GET_X_LPARAM(lParam));
        float y = static_cast<float>(GET_Y_LPARAM(lParam));
        mousePos_ = D2D1::Point2F(x, y);

        // Handle live MochiSwitch thumb dragging with rubber-band overshoot!
        if (draggingSwitchIdx_ >= 0 && draggingSwitchIdx_ < 4) {
            auto& sw = switches_[draggingSwitchIdx_];
            float dx = x - downPos_.x;
            if (std::fabs(dx) > 3.0f) {
                switchDraggedMoved_ = true;
            }
            if (switchDraggedMoved_) {
                float dpi = QueryWindowDpiScale();
                float travel = std::max(1.0f, 22.0f * dpi);
                float raw = switchDragStartPos_ + dx / travel;
                float clamped = std::clamp(raw, 0.0f, 1.0f);
                float over = RubberBand(raw - clamped, 1.0f, 0.12f);
                sw.pos.response = 0.10f;
                sw.pos.dampingRatio = 1.0f;
                sw.pos.target = clamped + over;
                RenderFrame();
            }
            return 0;
        }

        // Handle live MochiSegmentedControl thumb scrubbing!
        if (draggingSegIdx_ >= 0 && draggingSegIdx_ < 3) {
            auto& seg = segments_[draggingSegIdx_];
            float trackW = seg.trackRect.right - seg.trackRect.left;
            if (trackW > 10.0f) {
                float rel = (x - seg.trackRect.left) / trackW * static_cast<float>(seg.itemCount) - 0.5f;
                float maxIdx = static_cast<float>(seg.itemCount - 1);
                float clamped = std::clamp(rel, 0.0f, maxIdx);
                float over = RubberBand(rel - clamped, 1.0f, 0.14f);
                seg.head.response = 0.10f;
                seg.head.dampingRatio = 1.0f;
                seg.head.target = clamped + over;
                seg.tail.target = clamped + over;
                RenderFrame();
            }
            return 0;
        }

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
        mousePos_ = downPos_ = D2D1::Point2F(x, y);
        SetFocus(hwnd_);
        SetCapture(hwnd_);

        pressedZoneIdx_ = -1;
        for (int i = static_cast<int>(hitZones_.size()) - 1; i >= 0; --i) {
            if (ptIn(hitZones_[i].rect, x, y)) {
                pressedZoneIdx_ = i;
                break;
            }
        }

        if (pressedZoneIdx_ >= 0) {
            const auto& z = hitZones_[pressedZoneIdx_];
            if (z.kind == HitKind::SwitchThumbTrack || z.kind == HitKind::SwitchRow) {
                switches_[z.index].press.target = 1.0f; // Expand thumb on press!
                if (z.kind == HitKind::SwitchThumbTrack) {
                    draggingSwitchIdx_ = z.index;
                    switchDragStartPos_ = std::clamp(switches_[z.index].pos.value, 0.0f, 1.0f);
                    switchDraggedMoved_ = false;
                }
            } else if (z.kind == HitKind::SegmentedControl) {
                auto& seg = segments_[z.index];
                // Pressing on the active thumb lifts it for scrubbing; pressing another segment dims text
                if (std::fabs(seg.head.value - static_cast<float>(z.subIndex)) < 0.6f) {
                    draggingSegIdx_ = z.index;
                    seg.lift.target = 1.10f;
                }
            }
        }

        if (recordingTarget_ != 0) {
            bool clickedHkRow = (pressedZoneIdx_ >= 0 &&
                (hitZones_[pressedZoneIdx_].kind == HitKind::RecordCaptureHotkey ||
                 hitZones_[pressedZoneIdx_].kind == HitKind::RecordFullHotkey));
            if (!clickedHkRow) {
                recordingTarget_ = 0;
            }
        }
        RenderFrame();
        return 0;
    }

    case WM_LBUTTONUP: {
        float x = static_cast<float>(GET_X_LPARAM(lParam));
        float y = static_cast<float>(GET_Y_LPARAM(lParam));
        ReleaseCapture();

        for (auto& sw : switches_) {
            sw.press.target = 0.0f;
            sw.pos.response = 0.34f;
            sw.pos.dampingRatio = 0.72f;
        }
        for (auto& sg : segments_) {
            sg.lift.target = 1.0f;
            sg.head.response = 0.28f;
            sg.head.dampingRatio = 0.84f;
        }

        if (draggingSwitchIdx_ >= 0) {
            int idx = draggingSwitchIdx_;
            bool dragged = switchDraggedMoved_;
            draggingSwitchIdx_ = -1;
            switchDraggedMoved_ = false;

            const bool curVal = (idx == 0 ? config_->autoStartOnBoot :
                                 idx == 1 ? config_->autoCopyOnCapture :
                                 idx == 2 ? config_->windowAutoSnap : config_->showMagnifier);
            bool nextVal = dragged ? (switches_[idx].pos.value >= 0.5f) : !curVal;
            ApplySwitchToggle(idx, nextVal);
            pressedZoneIdx_ = -1;
            RenderFrame();
            return 0;
        }

        if (draggingSegIdx_ >= 0) {
            int segId = draggingSegIdx_;
            draggingSegIdx_ = -1;
            int targetIdx = static_cast<int>(std::round(std::clamp(segments_[segId].head.value, 0.0f, static_cast<float>(segments_[segId].itemCount - 1))));
            ApplySegmentSelect(segId, targetIdx);
            pressedZoneIdx_ = -1;
            RenderFrame();
            return 0;
        }

        int clicked = pressedZoneIdx_;
        pressedZoneIdx_ = -1;

        if (clicked >= 0 && clicked < static_cast<int>(hitZones_.size()) && ptIn(hitZones_[clicked].rect, x, y)) {
            const auto zone = hitZones_[clicked];
            switch (zone.kind) {
            case HitKind::TopCaptureBtn:
                ShowWindow(hwnd_, SW_HIDE);
                SetTimer(hwnd_, kDelayedCaptureTimer, 120, nullptr);
                return 0;
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
                statusNotice_ = L"已恢复默认快捷键 (Alt+A)";
                statusTimerSec_ = 2.2f;
                break;
            }
            case HitKind::SwitchRow: {
                int idx = zone.index;
                const bool curVal = (idx == 0 ? config_->autoStartOnBoot :
                                     idx == 1 ? config_->autoCopyOnCapture :
                                     idx == 2 ? config_->windowAutoSnap : config_->showMagnifier);
                ApplySwitchToggle(idx, !curVal);
                break;
            }
            case HitKind::SegmentedControl:
                ApplySegmentSelect(zone.index, zone.subIndex);
                break;
            case HitKind::AccentSwatch:
                config_->seedAccent = static_cast<SeedAccent>(zone.index);
                StartColorSchemeTransition();
                SaveAppConfig(*config_);
                break;
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

            bool allowBare = (vk == VK_SNAPSHOT || (vk >= VK_F1 && vk <= VK_F24));
            if (mods == 0 && !allowBare) {
                statusNotice_ = L"普通按键请搭配 Ctrl / Alt / Shift";
                statusTimerSec_ = 2.5f;
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
            statusNotice_ = ok ? (L"已更新快捷键：" + FormatHotkey(newHk)) : L"快捷键被其他软件占用";
            statusTimerSec_ = 2.5f;
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
