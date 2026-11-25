#include "capture_overlay.h"
#include "pin_window.h"
#include "mochi_g2.h"
#include <windowsx.h>
#include <imm.h>
#include <shellscalingapi.h>
#include <cmath>
#include <algorithm>

namespace mochi {

namespace {

constexpr const wchar_t* kOverlayClass = L"MochiShot_CaptureOverlay";
constexpr UINT_PTR kAnimTimerId = 1;

enum BtnActionId {
    BTN_TOOL_BASE = 100, // + EditTool
    BTN_ACT_UNDO  = 201,
    BTN_ACT_REDO  = 202,
    BTN_ACT_CLEAR = 203,
    BTN_ACT_PIN   = 204,
    BTN_ACT_SAVE  = 205,
    BTN_ACT_CLOSE = 206,
    BTN_ACT_COPY  = 207
};

static constexpr uint32_t kAnnotationColors[] = {
    0xFF3B30, // Vibrant Red
    0xFF9500, // Warm Orange
    0xFFCC00, // Sun Yellow
    0x34C759, // iOS Green
    0x007AFF, // Ocean Blue
    0xAF52DE, // Iris Purple
    0xFFFFFF  // Pure White
};

} // namespace

CaptureOverlay::CaptureOverlay(HINSTANCE hInstance, LiquidGlassEngine* engine, AppConfig* config)
    : hInstance_(hInstance), engine_(engine), config_(config) {
    QueryPerformanceFrequency(&qpcFreq_);
    QueryPerformanceCounter(&lastQpc_);
}

CaptureOverlay::~CaptureOverlay() {
    CloseSession();
}

bool CaptureOverlay::StartInstantFullscreenCopy() {
    CapturedImage img;
    if (!CaptureVirtualDesktop(img)) return false;
    bool ok = CopyBgraToClipboard(nullptr, img.bgra.data(), img.width, img.height);
    img.Clear();
    TrimProcessMemory();
    return ok;
}

bool CaptureOverlay::StartCaptureSession() {
    if (IsActive()) {
        SetForegroundWindow(hwnd_);
        return true;
    }

    if (!engine_->Initialize()) {
        return false;
    }

    // Enumerate visible windows BEFORE showing our overlay
    auto rawRects = EnumerateVisibleWindowRects(nullptr);

    if (!CaptureVirtualDesktop(captured_)) {
        return false;
    }

    windowSnapRects_.clear();
    for (const auto& r : rawRects) {
        RECT lr{
            r.left - captured_.originX,
            r.top - captured_.originY,
            r.right - captured_.originX,
            r.bottom - captured_.originY
        };
        windowSnapRects_.push_back(lr);
    }

    static bool classReg = false;
    if (!classReg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInstance_;
        wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
        wc.lpszClassName = kOverlayClass;
        RegisterClassExW(&wc);
        classReg = true;
    }

    // Reset state
    stage_ = Stage::Selecting;
    dragMode_ = DragMode::None;
    activeTool_ = EditTool::Move;
    annotations_.clear();
    redoStack_.clear();
    hasActiveDraft_ = false;
    editingText_ = false;
    nextStepNumber_ = 1;
    hoveredBtnId_ = -1;
    pressedBtnId_ = -1;
    pillInitialized_ = false;
    cachedMosaicBlockPx_ = 0;
    cachedBlurRadiusPx_ = 0;
    cachedFilterSelRect_ = {};

    currentColorHex_ = config_->defaultColorHex;
    currentStrokeDp_ = static_cast<float>(config_->defaultStrokeSize);
    currentMosaicDp_ = static_cast<float>(config_->defaultMosaicSize);

    pressScaleSpring_.SnapTo(1.0f);
    glowAlphaSpring_.SnapTo(0.0f);

    POINT cur{};
    GetCursorPos(&cur);
    mousePos_ = D2D1::Point2F(
        static_cast<float>(cur.x - captured_.originX),
        static_cast<float>(cur.y - captured_.originY)
    );

    hasHoveredWin_ = false;
    if (config_->windowAutoSnap) {
        POINT ptLocal{ static_cast<LONG>(mousePos_.x), static_cast<LONG>(mousePos_.y) };
        for (const auto& r : windowSnapRects_) {
            if (PtInRect(&r, ptLocal)) {
                hoveredWinRect_ = r;
                hasHoveredWin_ = true;
                break;
            }
        }
    }

    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kOverlayClass,
        L"MochiShot Capture",
        WS_POPUP,
        captured_.originX,
        captured_.originY,
        captured_.width,
        captured_.height,
        nullptr,
        nullptr,
        hInstance_,
        this
    );
    if (!hwnd_) {
        captured_.Clear();
        return false;
    }

    if (!engine_->EnsureWindowSwapChain(
            hwnd_,
            static_cast<UINT>(captured_.width),
            static_cast<UINT>(captured_.height),
            swapChain_)) {
        CloseSession();
        return false;
    }

    // Upload captured desktop BGRA into D2D GPU bitmap
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        96.0f, 96.0f
    );
    HRESULT hr = engine_->D2D()->CreateBitmap(
        D2D1::SizeU(static_cast<UINT>(captured_.width), static_cast<UINT>(captured_.height)),
        captured_.bgra.data(),
        static_cast<UINT>(captured_.width * 4),
        &bp,
        &desktopBmp_
    );
    if (FAILED(hr) || !desktopBmp_) {
        CloseSession();
        return false;
    }

    QueryPerformanceCounter(&lastQpc_);
    RenderFrame();

    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    SetTimer(hwnd_, kAnimTimerId, 8, nullptr); // ~120Hz spring animation tick when active
    return true;
}

void CaptureOverlay::CloseSession() {
    if (hwnd_) {
        KillTimer(hwnd_, kAnimTimerId);
        HWND h = hwnd_;
        hwnd_ = nullptr;
        if (IsWindow(h)) {
            DestroyWindow(h);
        }
    }
    editingText_ = false;
    hasActiveDraft_ = false;
    annotations_.clear();
    annotations_.shrink_to_fit();
    redoStack_.clear();
    redoStack_.shrink_to_fit();
    windowSnapRects_.clear();
    windowSnapRects_.shrink_to_fit();

    blurBmp_.Reset();
    mosaicBmp_.Reset();
    desktopBmp_.Reset();
    swapChain_.Reset();
    captured_.Clear();

    if (engine_) {
        engine_->NotifyWindowClosed();
    }
}

float CaptureOverlay::GetMonitorDpiScaleAt(float localX, float localY) const {
    POINT screenPt{
        static_cast<LONG>(localX) + captured_.originX,
        static_cast<LONG>(localY) + captured_.originY
    };
    HMONITOR mon = MonitorFromPoint(screenPt, MONITOR_DEFAULTTONEAREST);
    if (mon) {
        UINT dpiX = 96, dpiY = 96;
        using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        static HMODULE shcore = LoadLibraryW(L"Shcore.dll");
        static auto fn = shcore ? reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor")) : nullptr;
        if (fn && SUCCEEDED(fn(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dpiX, &dpiY)) && dpiX > 0) {
            return static_cast<float>(dpiX) / 96.0f;
        }
    }
    return 1.0f;
}

RECT CaptureOverlay::GetMonitorLocalRectAt(float localX, float localY) const {
    POINT screenPt{
        static_cast<LONG>(localX) + captured_.originX,
        static_cast<LONG>(localY) + captured_.originY
    };
    HMONITOR mon = MonitorFromPoint(screenPt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        return RECT{
            mi.rcMonitor.left - captured_.originX,
            mi.rcMonitor.top - captured_.originY,
            mi.rcMonitor.right - captured_.originX,
            mi.rcMonitor.bottom - captured_.originY
        };
    }
    return RECT{ 0, 0, captured_.width, captured_.height };
}

D2D1_RECT_F CaptureOverlay::NormalizedSelection() const {
    float l = std::min(selection_.left, selection_.right);
    float t = std::min(selection_.top, selection_.bottom);
    float r = std::max(selection_.left, selection_.right);
    float b = std::max(selection_.top, selection_.bottom);
    l = std::clamp(l, 0.0f, static_cast<float>(captured_.width));
    t = std::clamp(t, 0.0f, static_cast<float>(captured_.height));
    r = std::clamp(r, 0.0f, static_cast<float>(captured_.width));
    b = std::clamp(b, 0.0f, static_cast<float>(captured_.height));
    return D2D1::RectF(std::floor(l), std::floor(t), std::ceil(r), std::ceil(b));
}

CaptureOverlay::DragMode CaptureOverlay::HitTestSelectionHandles(float x, float y, float dpi) const {
    D2D1_RECT_F s = NormalizedSelection();
    const float hit = 9.0f * dpi;
    const float mx = (s.left + s.right) * 0.5f;
    const float my = (s.top + s.bottom) * 0.5f;

    auto nearPt = [&](float px, float py) {
        return std::fabs(x - px) <= hit && std::fabs(y - py) <= hit;
    };

    if (nearPt(s.left, s.top))     return DragMode::ResizeNW;
    if (nearPt(s.right, s.top))    return DragMode::ResizeNE;
    if (nearPt(s.right, s.bottom)) return DragMode::ResizeSE;
    if (nearPt(s.left, s.bottom))  return DragMode::ResizeSW;
    if (nearPt(mx, s.top))         return DragMode::ResizeN;
    if (nearPt(s.right, my))       return DragMode::ResizeE;
    if (nearPt(mx, s.bottom))      return DragMode::ResizeS;
    if (nearPt(s.left, my))        return DragMode::ResizeW;

    // Edge bands
    if (y >= s.top && y <= s.bottom) {
        if (std::fabs(x - s.left) <= hit * 0.7f)  return DragMode::ResizeW;
        if (std::fabs(x - s.right) <= hit * 0.7f) return DragMode::ResizeE;
    }
    if (x >= s.left && x <= s.right) {
        if (std::fabs(y - s.top) <= hit * 0.7f)    return DragMode::ResizeN;
        if (std::fabs(y - s.bottom) <= hit * 0.7f) return DragMode::ResizeS;
    }
    return DragMode::None;
}

void CaptureOverlay::UpdateCursorForPos(float x, float y) {
    auto ptIn = [](const D2D1_RECT_F& r, float px, float py) {
        return px >= r.left && px <= r.right && py >= r.top && py <= r.bottom;
    };

    if (stage_ == Stage::Editing) {
        const bool showProps = (activeTool_ != EditTool::Move);
        if (ptIn(mainBarRect_, x, y) || (showProps && ptIn(propBarRect_, x, y))) {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            return;
        }
        float dpi = GetMonitorDpiScaleAt(x, y);
        DragMode hm = HitTestSelectionHandles(x, y, dpi);
        switch (hm) {
        case DragMode::ResizeNW:
        case DragMode::ResizeSE: SetCursor(LoadCursor(nullptr, IDC_SIZENWSE)); return;
        case DragMode::ResizeNE:
        case DragMode::ResizeSW: SetCursor(LoadCursor(nullptr, IDC_SIZENESW)); return;
        case DragMode::ResizeN:
        case DragMode::ResizeS:  SetCursor(LoadCursor(nullptr, IDC_SIZENS));   return;
        case DragMode::ResizeE:
        case DragMode::ResizeW:  SetCursor(LoadCursor(nullptr, IDC_SIZEWE));   return;
        default: break;
        }

        D2D1_RECT_F s = NormalizedSelection();
        if (ptIn(s, x, y)) {
            if (activeTool_ == EditTool::Move) {
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            } else if (activeTool_ == EditTool::Text) {
                SetCursor(LoadCursor(nullptr, IDC_IBEAM));
            } else {
                SetCursor(LoadCursor(nullptr, IDC_CROSS));
            }
            return;
        }
    }
    SetCursor(LoadCursor(nullptr, IDC_CROSS));
}

void CaptureOverlay::UpdateImmCaretPosition() {
    if (!hwnd_ || !editingText_) return;
    HIMC himc = ImmGetContext(hwnd_);
    if (himc) {
        COMPOSITIONFORM cf{};
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos.x = static_cast<LONG>(activeDraft_.startPt.x);
        cf.ptCurrentPos.y = static_cast<LONG>(activeDraft_.startPt.y);
        ImmSetCompositionWindow(himc, &cf);
        ImmReleaseContext(hwnd_, himc);
    }
}

void CaptureOverlay::RebuildFilterBitmapsIfNeeded() {
    if (!captured_.IsValid() || !engine_ || !engine_->D2D()) return;

    bool needsMosaic = (activeTool_ == EditTool::Mosaic);
    bool needsBlur   = (activeTool_ == EditTool::Blur);
    for (const auto& a : annotations_) {
        if (a.type == EditTool::Mosaic) needsMosaic = true;
        if (a.type == EditTool::Blur)   needsBlur = true;
    }
    if (!needsMosaic && !needsBlur) return;

    D2D1_RECT_F sel = NormalizedSelection();
    RECT sr{
        std::clamp(static_cast<LONG>(sel.left), 0L, static_cast<LONG>(captured_.width)),
        std::clamp(static_cast<LONG>(sel.top), 0L, static_cast<LONG>(captured_.height)),
        std::clamp(static_cast<LONG>(sel.right), 0L, static_cast<LONG>(captured_.width)),
        std::clamp(static_cast<LONG>(sel.bottom), 0L, static_cast<LONG>(captured_.height))
    };
    int sw = sr.right - sr.left;
    int sh = sr.bottom - sr.top;
    if (sw <= 0 || sh <= 0) return;

    float dpi = GetMonitorDpiScaleAt((sel.left + sel.right) * 0.5f, (sel.top + sel.bottom) * 0.5f);
    int mosaicBlockPx = std::max(4, static_cast<int>(std::round(currentMosaicDp_ * dpi)));
    int blurRadPx     = std::max(4, static_cast<int>(std::round(currentMosaicDp_ * 0.9f * dpi)));

    const bool rectChanged = (sr.left != cachedFilterSelRect_.left || sr.top != cachedFilterSelRect_.top ||
                              sr.right != cachedFilterSelRect_.right || sr.bottom != cachedFilterSelRect_.bottom);

    if (rectChanged || (needsMosaic && (!mosaicBmp_ || cachedMosaicBlockPx_ != mosaicBlockPx)) ||
                       (needsBlur && (!blurBmp_ || cachedBlurRadiusPx_ != blurRadPx))) {
        // Extract selection sub-rect pixels
        std::vector<uint8_t> sub(static_cast<size_t>(sw) * static_cast<size_t>(sh) * 4);
        for (int y = 0; y < sh; ++y) {
            const uint8_t* srcRow = captured_.bgra.data() +
                (static_cast<size_t>(sr.top + y) * captured_.width + sr.left) * 4;
            std::memcpy(sub.data() + static_cast<size_t>(y) * sw * 4, srcRow, static_cast<size_t>(sw) * 4);
        }

        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            96.0f, 96.0f
        );

        if (needsMosaic && (rectChanged || !mosaicBmp_ || cachedMosaicBlockPx_ != mosaicBlockPx)) {
            std::vector<uint8_t> mos;
            GenerateMosaicBuffer(sub.data(), sw, sh, mosaicBlockPx, mos);
            mosaicBmp_.Reset();
            engine_->D2D()->CreateBitmap(
                D2D1::SizeU(static_cast<UINT>(sw), static_cast<UINT>(sh)),
                mos.data(), static_cast<UINT>(sw * 4), &bp, &mosaicBmp_
            );
            cachedMosaicBlockPx_ = mosaicBlockPx;
        }

        if (needsBlur && (rectChanged || !blurBmp_ || cachedBlurRadiusPx_ != blurRadPx)) {
            std::vector<uint8_t> blr;
            GenerateBlurredBuffer(sub.data(), sw, sh, blurRadPx, blr);
            blurBmp_.Reset();
            engine_->D2D()->CreateBitmap(
                D2D1::SizeU(static_cast<UINT>(sw), static_cast<UINT>(sh)),
                blr.data(), static_cast<UINT>(sw * 4), &bp, &blurBmp_
            );
            cachedBlurRadiusPx_ = blurRadPx;
        }

        cachedFilterSelRect_ = sr;
    }
}

void CaptureOverlay::DrawSingleAnnotation(
    ID2D1RenderTarget* rt,
    const AnnotationItem& item,
    ID2D1Brush* mosaicBrush,
    ID2D1Brush* blurBrush,
    float offsetX,
    float offsetY,
    bool showCaret
) {
    if (!rt) return;

    auto pt = [&](D2D1_POINT_2F p) {
        return D2D1::Point2F(p.x + offsetX, p.y + offsetY);
    };

    if (item.type == EditTool::Mosaic || item.type == EditTool::Blur) {
        ID2D1Brush* filterBrush = (item.type == EditTool::Mosaic) ? mosaicBrush : blurBrush;
        if (!filterBrush) return;

        if (item.isBoxRegion) {
            D2D1_POINT_2F a = pt(item.startPt);
            D2D1_POINT_2F b = pt(item.endPt);
            D2D1_RECT_F r = D2D1::RectF(std::min(a.x, b.x), std::min(a.y, b.y),
                                        std::max(a.x, b.x), std::max(a.y, b.y));
            rt->FillRectangle(r, filterBrush);
        } else if (!item.points.empty()) {
            if (item.points.size() == 1) {
                D2D1_POINT_2F c = pt(item.points[0]);
                rt->FillEllipse(D2D1::Ellipse(c, item.strokeWidth * 0.5f, item.strokeWidth * 0.5f), filterBrush);
            } else {
                ComPtr<ID2D1PathGeometry> pg;
                if (SUCCEEDED(engine_->D2DFactory()->CreatePathGeometry(&pg))) {
                    ComPtr<ID2D1GeometrySink> s;
                    if (SUCCEEDED(pg->Open(&s))) {
                        s->BeginFigure(pt(item.points[0]), D2D1_FIGURE_BEGIN_HOLLOW);
                        for (size_t i = 1; i + 1 < item.points.size(); ++i) {
                            D2D1_POINT_2F p1 = pt(item.points[i]);
                            D2D1_POINT_2F p2 = pt(item.points[i + 1]);
                            D2D1_POINT_2F mid = D2D1::Point2F((p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f);
                            s->AddQuadraticBezier(D2D1::QuadraticBezierSegment(p1, mid));
                        }
                        s->AddLine(pt(item.points.back()));
                        s->EndFigure(D2D1_FIGURE_END_OPEN);
                        s->Close();
                        rt->DrawGeometry(pg.Get(), filterBrush, item.strokeWidth, engine_->RoundStrokeStyle().Get());
                    }
                }
            }
        }
        return;
    }

    const float alpha = (item.type == EditTool::Highlighter) ? 0.38f : 1.0f;
    ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush(Rgba(item.colorHex, alpha), &brush);
    if (!brush) return;

    switch (item.type) {
    case EditTool::Pen:
    case EditTool::Highlighter: {
        if (item.points.empty()) break;
        // For highlighter, push an opacity layer so overlapping loops within one stroke stay uniform alpha!
        ComPtr<ID2D1Layer> layer;
        bool pushedLayer = false;
        if (item.type == EditTool::Highlighter) {
            brush->SetColor(Rgba(item.colorHex, 1.0f));
            if (SUCCEEDED(rt->CreateLayer(&layer)) && layer) {
                rt->PushLayer(
                    D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                          D2D1::IdentityMatrix(), 0.38f),
                    layer.Get()
                );
                pushedLayer = true;
            }
        }

        const float sw = (item.type == EditTool::Highlighter) ? (item.strokeWidth * 3.2f) : item.strokeWidth;
        if (item.points.size() == 1) {
            D2D1_POINT_2F c = pt(item.points[0]);
            rt->FillEllipse(D2D1::Ellipse(c, sw * 0.5f, sw * 0.5f), brush.Get());
        } else {
            ComPtr<ID2D1PathGeometry> pg;
            if (SUCCEEDED(engine_->D2DFactory()->CreatePathGeometry(&pg))) {
                ComPtr<ID2D1GeometrySink> s;
                if (SUCCEEDED(pg->Open(&s))) {
                    s->BeginFigure(pt(item.points[0]), D2D1_FIGURE_BEGIN_HOLLOW);
                    for (size_t i = 1; i + 1 < item.points.size(); ++i) {
                        D2D1_POINT_2F p1 = pt(item.points[i]);
                        D2D1_POINT_2F p2 = pt(item.points[i + 1]);
                        D2D1_POINT_2F mid = D2D1::Point2F((p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f);
                        s->AddQuadraticBezier(D2D1::QuadraticBezierSegment(p1, mid));
                    }
                    s->AddLine(pt(item.points.back()));
                    s->EndFigure(D2D1_FIGURE_END_OPEN);
                    s->Close();
                    rt->DrawGeometry(pg.Get(), brush.Get(), sw, engine_->RoundStrokeStyle().Get());
                }
            }
        }

        if (pushedLayer) {
            rt->PopLayer();
        }
        break;
    }

    case EditTool::Rect: {
        D2D1_POINT_2F a = pt(item.startPt);
        D2D1_POINT_2F b = pt(item.endPt);
        D2D1_RECT_F r = D2D1::RectF(std::min(a.x, b.x), std::min(a.y, b.y),
                                    std::max(a.x, b.x), std::max(a.y, b.y));
        float g2Rad = std::min(10.0f + item.strokeWidth * 1.2f, std::min(r.right - r.left, r.bottom - r.top) * 0.25f);
        StrokeG2RoundRect(rt, engine_->D2DFactory(), r, g2Rad, brush.Get(), item.strokeWidth, engine_->RoundStrokeStyle().Get());
        break;
    }

    case EditTool::Ellipse: {
        D2D1_POINT_2F a = pt(item.startPt);
        D2D1_POINT_2F b = pt(item.endPt);
        D2D1_POINT_2F c = D2D1::Point2F((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        float rx = std::fabs(b.x - a.x) * 0.5f;
        float ry = std::fabs(b.y - a.y) * 0.5f;
        if (rx > 0.5f && ry > 0.5f) {
            rt->DrawEllipse(D2D1::Ellipse(c, rx, ry), brush.Get(), item.strokeWidth, engine_->RoundStrokeStyle().Get());
        }
        break;
    }

    case EditTool::Arrow: {
        // Sleek Snipaste / iOS style tapered geometric arrow
        D2D1_POINT_2F p0 = pt(item.startPt);
        D2D1_POINT_2F p1 = pt(item.endPt);
        float dx = p1.x - p0.x;
        float dy = p1.y - p0.y;
        float len = std::hypot(dx, dy);
        if (len < 2.0f) break;

        float ux = dx / len;
        float uy = dy / len;
        float nx = -uy;
        float ny = ux;

        float headLen = std::min(len * 0.55f, std::max(16.0f, item.strokeWidth * 4.8f));
        float headHalfW = headLen * 0.52f;
        float neckHalfW = std::min(headHalfW * 0.42f, std::max(2.2f, item.strokeWidth * 0.85f));
        float tailHalfW = std::max(1.0f, item.strokeWidth * 0.24f);

        D2D1_POINT_2F neckCenter = D2D1::Point2F(p1.x - ux * (headLen * 0.78f), p1.y - uy * (headLen * 0.78f));
        D2D1_POINT_2F wingBase   = D2D1::Point2F(p1.x - ux * headLen, p1.y - uy * headLen);

        ComPtr<ID2D1PathGeometry> pg;
        if (SUCCEEDED(engine_->D2DFactory()->CreatePathGeometry(&pg))) {
            ComPtr<ID2D1GeometrySink> s;
            if (SUCCEEDED(pg->Open(&s))) {
                s->BeginFigure(D2D1::Point2F(p0.x + nx * tailHalfW, p0.y + ny * tailHalfW), D2D1_FIGURE_BEGIN_FILLED);
                s->AddLine(D2D1::Point2F(neckCenter.x + nx * neckHalfW, neckCenter.y + ny * neckHalfW));
                s->AddLine(D2D1::Point2F(wingBase.x + nx * headHalfW, wingBase.y + ny * headHalfW));
                s->AddLine(p1);
                s->AddLine(D2D1::Point2F(wingBase.x - nx * headHalfW, wingBase.y - ny * headHalfW));
                s->AddLine(D2D1::Point2F(neckCenter.x - nx * neckHalfW, neckCenter.y - ny * neckHalfW));
                s->AddLine(D2D1::Point2F(p0.x - nx * tailHalfW, p0.y - ny * tailHalfW));
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                s->Close();
                rt->FillGeometry(pg.Get(), brush.Get());
            }
        }
        break;
    }

    case EditTool::StepCounter: {
        D2D1_POINT_2F c = pt(item.startPt);
        float rad = std::max(13.0f, 10.0f + item.strokeWidth * 1.4f);

        // Soft contact shadow + crisp white ring + vibrant badge
        ComPtr<ID2D1SolidColorBrush> shBrush, whiteBrush;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.25f), &shBrush);
        rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1.0f), &whiteBrush);
        if (shBrush) rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x, c.y + 2.0f), rad + 1.5f, rad + 1.5f), shBrush.Get());
        if (whiteBrush) rt->FillEllipse(D2D1::Ellipse(c, rad + 1.8f, rad + 1.8f), whiteBrush.Get());
        rt->FillEllipse(D2D1::Ellipse(c, rad, rad), brush.Get());

        // Contrast text color
        bool lightBg = (item.colorHex == 0xFFFFFF || item.colorHex == 0xFFCC00);
        ComPtr<ID2D1SolidColorBrush> fgBrush;
        rt->CreateSolidColorBrush(lightBg ? D2D1::ColorF(0.1f, 0.12f, 0.12f, 1.0f) : D2D1::ColorF(1, 1, 1, 1.0f), &fgBrush);

        auto fmt = engine_->CreateTextFormat(rad * 1.12f, DWRITE_FONT_WEIGHT_BOLD);
        if (fmt && fgBrush) {
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            std::wstring numStr = std::to_wstring(item.stepIndex);
            D2D1_RECT_F tr = D2D1::RectF(c.x - rad, c.y - rad, c.x + rad, c.y + rad);
            rt->DrawText(numStr.c_str(), static_cast<UINT32>(numStr.size()), fmt.Get(), tr, fgBrush.Get());
        }
        break;
    }

    case EditTool::Text: {
        D2D1_POINT_2F p = pt(item.startPt);
        float fontPx = std::max(14.0f, 12.0f + item.strokeWidth * 3.0f);
        auto fmt = engine_->CreateTextFormat(fontPx, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        if (!fmt) break;

        std::wstring display = item.text;
        if (showCaret && ((GetTickCount() - caretBlinkTick_) % 1000 < 530)) {
            display += L"|";
        }
        if (display.empty() && showCaret) {
            display = L" ";
        }
        if (display.empty()) break;

        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(engine_->DWrite()->CreateTextLayout(
                display.c_str(),
                static_cast<UINT32>(display.size()),
                fmt.Get(),
                2400.0f,
                1600.0f,
                &layout))) {
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);

            if (showCaret) {
                D2D1_RECT_F box = D2D1::RectF(
                    p.x - 6.0f, p.y - 4.0f,
                    p.x + std::max(64.0f, tm.widthIncludingTrailingWhitespace + 12.0f),
                    p.y + std::max(fontPx * 1.35f, tm.height + 8.0f)
                );
                ComPtr<ID2D1SolidColorBrush> bgBrush, dashBrush;
                rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.28f), &bgBrush);
                rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.72f), &dashBrush);
                FillG2RoundRect(rt, engine_->D2DFactory(), box, 8.0f, bgBrush.Get());
                StrokeG2RoundRect(rt, engine_->D2DFactory(), box, 8.0f, dashBrush.Get(), 1.2f);
            }

            // Subtle drop shadow for legibility on both white and black screenshots
            ComPtr<ID2D1SolidColorBrush> shadowBrush;
            bool isWhiteText = (item.colorHex == 0xFFFFFF);
            rt->CreateSolidColorBrush(isWhiteText ? D2D1::ColorF(0, 0, 0, 0.65f) : D2D1::ColorF(0, 0, 0, 0.35f), &shadowBrush);
            if (shadowBrush) {
                rt->DrawTextLayout(D2D1::Point2F(p.x + 1.0f, p.y + 1.2f), layout.Get(), shadowBrush.Get());
            }
            rt->DrawTextLayout(p, layout.Get(), brush.Get());
        }
        break;
    }

    default:
        break;
    }
}

void CaptureOverlay::DrawAnnotations(
    ID2D1RenderTarget* rt,
    ID2D1Bitmap* mosaicBmp,
    ID2D1Bitmap* blurBmp,
    float offsetX,
    float offsetY
) {
    ComPtr<ID2D1BitmapBrush> mosaicBrush, blurBrush;
    D2D1_BITMAP_BRUSH_PROPERTIES bbp = D2D1::BitmapBrushProperties(
        D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
    );
    D2D1_BITMAP_BRUSH_PROPERTIES bbpLinear = D2D1::BitmapBrushProperties(
        D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
    );

    const float brushOriginX = static_cast<float>(cachedFilterSelRect_.left) + offsetX;
    const float brushOriginY = static_cast<float>(cachedFilterSelRect_.top) + offsetY;

    if (mosaicBmp && SUCCEEDED(rt->CreateBitmapBrush(mosaicBmp, bbp, &mosaicBrush))) {
        mosaicBrush->SetTransform(D2D1::Matrix3x2F::Translation(brushOriginX, brushOriginY));
    }
    if (blurBmp && SUCCEEDED(rt->CreateBitmapBrush(blurBmp, bbpLinear, &blurBrush))) {
        blurBrush->SetTransform(D2D1::Matrix3x2F::Translation(brushOriginX, brushOriginY));
    }

    for (const auto& item : annotations_) {
        DrawSingleAnnotation(rt, item, mosaicBrush.Get(), blurBrush.Get(), offsetX, offsetY, false);
    }
    if (hasActiveDraft_) {
        DrawSingleAnnotation(rt, activeDraft_, mosaicBrush.Get(), blurBrush.Get(), offsetX, offsetY, editingText_);
    }
}

void CaptureOverlay::DrawMagnifierLoupe() {
    if (!config_->showMagnifier) return;
    if (stage_ == Stage::Editing && dragMode_ == DragMode::None) return;
    if (dragMode_ == DragMode::DrawingAnnotation || dragMode_ == DragMode::DraggingPropertySlider) return;

    auto* d2d = engine_->D2D();
    float dpi = GetMonitorDpiScaleAt(mousePos_.x, mousePos_.y);
    RECT mon = GetMonitorLocalRectAt(mousePos_.x, mousePos_.y);
    const bool isDark = ResolveDarkTheme(config_->themeMode);
    const auto cs = BuildColorScheme(config_->seedAccent, isDark);

    const float cardW = 144.0f * dpi;
    const float cardH = 178.0f * dpi;
    const float margin = 22.0f * dpi;

    float lx = mousePos_.x + margin;
    float ly = mousePos_.y + margin;
    if (lx + cardW > mon.right - 8.0f * dpi)  lx = mousePos_.x - margin - cardW;
    if (ly + cardH > mon.bottom - 8.0f * dpi) ly = mousePos_.y - margin - cardH;
    lx = std::max(static_cast<float>(mon.left) + 8.0f * dpi, lx);
    ly = std::max(static_cast<float>(mon.top) + 8.0f * dpi, ly);

    D2D1_RECT_F cardRect = D2D1::RectF(lx, ly, lx + cardW, ly + cardH);

    GlassSurfaceSpec gs{};
    gs.rect = cardRect;
    gs.cornerRadiusPx = 20.0f * dpi;
    gs.dpiScale = dpi;
    gs.isDark = true; // Dark glass loupe gives crisp contrast over any screen
    gs.thick = true;
    gs.mode = config_->glassMode;
    gs.solidFallback = D2D1::ColorF(0.09f, 0.12f, 0.12f, 0.94f);
    engine_->DrawGlassSurface(swapChain_, gs);

    // 11x11 zoomed pixel grid inside concentric G2 window
    const float pad = 10.0f * dpi;
    const float gridSize = cardW - pad * 2.0f;
    D2D1_RECT_F gridRect = D2D1::RectF(lx + pad, ly + pad, lx + pad + gridSize, ly + pad + gridSize);
    const float innerRad = ConcentricInner(20.0f * dpi, pad, 6.0f * dpi);

    auto clipGeom = CreateG2RoundRectGeometry(engine_->D2DFactory(), gridRect, innerRad, 1.0f);
    ComPtr<ID2D1Layer> clipLayer;
    if (clipGeom && SUCCEEDED(d2d->CreateLayer(&clipLayer))) {
        d2d->PushLayer(D2D1::LayerParameters(gridRect, clipGeom.Get()), clipLayer.Get());

        constexpr int kCells = 11;
        const float cellPx = gridSize / static_cast<float>(kCells);
        const int cx = static_cast<int>(mousePos_.x);
        const int cy = static_cast<int>(mousePos_.y);

        ComPtr<ID2D1SolidColorBrush> pxBrush, lineBrush, crossBrush;
        d2d->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &pxBrush);
        d2d->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.10f), &lineBrush);
        d2d->CreateSolidColorBrush(cs.primary, &crossBrush);

        for (int dy = -kCells / 2; dy <= kCells / 2; ++dy) {
            for (int dx = -kCells / 2; dx <= kCells / 2; ++dx) {
                uint32_t rgb = captured_.SamplePixelClamped(cx + dx, cy + dy);
                pxBrush->SetColor(Rgba(rgb, 1.0f));
                float x0 = gridRect.left + (dx + kCells / 2) * cellPx;
                float y0 = gridRect.top + (dy + kCells / 2) * cellPx;
                d2d->FillRectangle(D2D1::RectF(x0, y0, x0 + cellPx + 0.5f, y0 + cellPx + 0.5f), pxBrush.Get());
            }
        }
        for (int i = 1; i < kCells; ++i) {
            float gx = gridRect.left + i * cellPx;
            float gy = gridRect.top + i * cellPx;
            d2d->DrawLine(D2D1::Point2F(gx, gridRect.top), D2D1::Point2F(gx, gridRect.bottom), lineBrush.Get(), 0.7f);
            d2d->DrawLine(D2D1::Point2F(gridRect.left, gy), D2D1::Point2F(gridRect.right, gy), lineBrush.Get(), 0.7f);
        }
        // Center pixel highlight
        float mid0X = gridRect.left + (kCells / 2) * cellPx;
        float mid0Y = gridRect.top + (kCells / 2) * cellPx;
        d2d->DrawRectangle(D2D1::RectF(mid0X, mid0Y, mid0X + cellPx, mid0Y + cellPx), crossBrush.Get(), 1.8f * dpi);

        d2d->PopLayer();
    }

    ComPtr<ID2D1SolidColorBrush> borderBrush, textBrush, subBrush, swatchBrush;
    d2d->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.22f), &borderBrush);
    d2d->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.98f, 0.97f, 1.0f), &textBrush);
    d2d->CreateSolidColorBrush(D2D1::ColorF(0.72f, 0.80f, 0.78f, 1.0f), &subBrush);
    StrokeG2RoundRect(d2d, engine_->D2DFactory(), gridRect, innerRad, borderBrush.Get(), 1.0f * dpi);

    uint32_t hex = captured_.SamplePixelClamped(static_cast<int>(mousePos_.x), static_cast<int>(mousePos_.y));
    d2d->CreateSolidColorBrush(Rgba(hex, 1.0f), &swatchBrush);

    // Info text below zoomed grid
    D2D1_RECT_F s = NormalizedSelection();
    int dispW = static_cast<int>(s.right - s.left);
    int dispH = static_cast<int>(s.bottom - s.top);
    if (stage_ == Stage::Selecting && hasHoveredWin_) {
        dispW = hoveredWinRect_.right - hoveredWinRect_.left;
        dispH = hoveredWinRect_.bottom - hoveredWinRect_.top;
    }

    wchar_t line1[64]{}, line2[64]{};
    if (dispW > 0 && dispH > 0) {
        swprintf_s(line1, L"%d × %d px", dispW, dispH);
    } else {
        swprintf_s(line1, L"(%d, %d)", static_cast<int>(mousePos_.x), static_cast<int>(mousePos_.y));
    }
    swprintf_s(line2, L"#%06X  (按C复制)", hex);

    auto fmt1 = engine_->CreateTextFormat(11.5f * dpi, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    auto fmt2 = engine_->CreateTextFormat(10.5f * dpi, DWRITE_FONT_WEIGHT_NORMAL);
    float infoY = gridRect.bottom + 6.0f * dpi;
    if (fmt1) {
        d2d->DrawText(line1, static_cast<UINT32>(wcslen(line1)), fmt1.Get(),
                      D2D1::RectF(lx + pad, infoY, lx + cardW - pad, infoY + 16.0f * dpi), textBrush.Get());
    }
    float swY = infoY + 17.5f * dpi;
    D2D1_RECT_F swBox = D2D1::RectF(lx + pad, swY + 1.5f * dpi, lx + pad + 11.0f * dpi, swY + 12.5f * dpi);
    FillG2RoundRect(d2d, engine_->D2DFactory(), swBox, 3.5f * dpi, swatchBrush.Get());
    StrokeG2RoundRect(d2d, engine_->D2DFactory(), swBox, 3.5f * dpi, borderBrush.Get(), 0.8f * dpi);
    if (fmt2) {
        d2d->DrawText(line2, static_cast<UINT32>(wcslen(line2)), fmt2.Get(),
                      D2D1::RectF(swBox.right + 5.0f * dpi, swY, lx + cardW - 6.0f * dpi, swY + 16.0f * dpi), subBrush.Get());
    }
}

void CaptureOverlay::DrawFloatingToolbars() {
    if (stage_ != Stage::Editing) return;

    auto* d2d = engine_->D2D();
    D2D1_RECT_F sel = NormalizedSelection();
    float selMidX = (sel.left + sel.right) * 0.5f;
    float selMidY = (sel.top + sel.bottom) * 0.5f;
    float dpi = GetMonitorDpiScaleAt(selMidX, selMidY);
    RECT mon = GetMonitorLocalRectAt(selMidX, selMidY);

    const bool isDark = ResolveDarkTheme(config_->themeMode);
    const auto cs = BuildColorScheme(config_->seedAccent, isDark);

    // Build Mochi Floating Toolbar item list
    toolbarBtns_.clear();
    auto addTool = [&](EditTool t, MochiIcon ic, const wchar_t* tip) {
        ToolbarBtn b{};
        b.id = BTN_TOOL_BASE + static_cast<int>(t);
        b.icon = ic;
        b.tooltip = tip;
        b.isTool = true;
        b.tool = t;
        toolbarBtns_.push_back(b);
    };
    auto addAct = [&](int id, MochiIcon ic, const wchar_t* tip, bool enabled = true, bool primary = false) {
        ToolbarBtn b{};
        b.id = id;
        b.icon = ic;
        b.tooltip = tip;
        b.enabled = enabled;
        b.isPrimaryAccent = primary;
        toolbarBtns_.push_back(b);
    };

    addTool(EditTool::Move,        MochiIcon::CursorMove,   L"移动/调整选区");
    addTool(EditTool::Pen,         MochiIcon::Pen,          L"画笔");
    addTool(EditTool::Highlighter, MochiIcon::Highlighter,  L"荧光笔");
    addTool(EditTool::Rect,        MochiIcon::RectShape,    L"G2 圆角矩形");
    addTool(EditTool::Ellipse,     MochiIcon::EllipseShape, L"椭圆");
    addTool(EditTool::Arrow,       MochiIcon::ArrowTopRight,L"箭头");
    addTool(EditTool::StepCounter, MochiIcon::StepNumber,   L"序号标注");
    addTool(EditTool::Text,        MochiIcon::TextAa,       L"文字批注");
    addTool(EditTool::Mosaic,      MochiIcon::MosaicGrid,   L"马赛克 (按住Shift框选)");
    addTool(EditTool::Blur,        MochiIcon::BlurDrop,     L"模糊遮挡 (按住Shift框选)");

    addAct(BTN_ACT_UNDO,  MochiIcon::Undo,       L"撤销 (Ctrl+Z)", !annotations_.empty());
    addAct(BTN_ACT_REDO,  MochiIcon::Redo,       L"重做 (Ctrl+Y)", !redoStack_.empty());
    addAct(BTN_ACT_CLEAR, MochiIcon::Trash,      L"清空批注",      !annotations_.empty());
    addAct(BTN_ACT_PIN,   MochiIcon::Pin,        L"贴图到屏幕 (T)");
    addAct(BTN_ACT_SAVE,  MochiIcon::SaveDisk,   L"保存文件 (Ctrl+S)");
    addAct(BTN_ACT_CLOSE, MochiIcon::CloseCross, L"取消截图 (Esc)");
    addAct(BTN_ACT_COPY,  MochiIcon::CheckCopy,  L"复制到剪贴板 (Enter)", true, true);

    const float btnSize = 36.0f * dpi;
    const float btnGap  = 3.0f * dpi;
    const float sepGap  = 11.0f * dpi;
    const float padH    = 8.0f * dpi;
    const float padV    = 6.0f * dpi;
    const float barH    = btnSize + padV * 2.0f;

    float totalW = padH * 2.0f;
    for (size_t i = 0; i < toolbarBtns_.size(); ++i) {
        totalW += (toolbarBtns_[i].isPrimaryAccent ? btnSize * 1.35f : btnSize);
        if (i + 1 < toolbarBtns_.size()) {
            bool sepAfter = (i == 9 || i == 12);
            totalW += sepAfter ? sepGap : btnGap;
        }
    }

    const bool showProps = (activeTool_ != EditTool::Move);

    const float propH = 40.0f * dpi;
    const float barGap = 8.0f * dpi;
    const float neededBelow = barH + (showProps ? (propH + barGap) : 0.0f) + 18.0f * dpi;

    // Position main bar below selection, or above if near monitor bottom, or inside bottom
    float barX = sel.right - totalW;
    barX = std::clamp(barX, static_cast<float>(mon.left) + 10.0f * dpi, static_cast<float>(mon.right) - totalW - 10.0f * dpi);

    float barY = sel.bottom + 10.0f * dpi;
    bool propBelow = true;
    if (sel.bottom + neededBelow > mon.bottom) {
        if (sel.top - neededBelow >= mon.top) {
            barY = sel.top - 10.0f * dpi - barH;
            propBelow = false;
        } else {
            barY = static_cast<float>(mon.bottom) - neededBelow + 6.0f * dpi;
        }
    }

    // Appear directly at exact position (no pop/slide bounce animation!)
    mainBarRect_ = D2D1::RectF(barX, barY, barX + totalW, barY + barH);

    // Draw Main Mochi Liquid Glass Capsule Toolbar
    GlassSurfaceSpec mainSpec{};
    mainSpec.rect = mainBarRect_;
    mainSpec.cornerRadiusPx = barH * 0.5f; // MochiCapsule!
    mainSpec.dpiScale = dpi;
    mainSpec.isDark = cs.isDark;
    mainSpec.thick = false;
    mainSpec.mode = config_->glassMode;
    mainSpec.solidFallback = WithAlpha(cs.groupedContainer, 0.95f);
    mainSpec.hasPressGlow = (glowAlphaSpring_.value > 0.01f);
    mainSpec.glowCenterPx = pressGlowPos_;
    mainSpec.glowRadiusPx = 64.0f * dpi;
    mainSpec.glowIntensity = glowAlphaSpring_.value * 0.35f;
    engine_->DrawGlassSurface(swapChain_, mainSpec);

    // Compute button rects & active tool capsule indicator
    float curX = mainBarRect_.left + padH;
    float targetPillX = curX;
    for (size_t i = 0; i < toolbarBtns_.size(); ++i) {
        float bw = toolbarBtns_[i].isPrimaryAccent ? (btnSize * 1.35f) : btnSize;
        toolbarBtns_[i].rect = D2D1::RectF(curX, mainBarRect_.top + padV, curX + bw, mainBarRect_.top + padV + btnSize);
        if (toolbarBtns_[i].isTool && toolbarBtns_[i].tool == activeTool_) {
            targetPillX = curX;
        }
        curX += bw;
        if (i + 1 < toolbarBtns_.size()) {
            bool sepAfter = (i == 9 || i == 12);
            if (sepAfter) {
                ComPtr<ID2D1SolidColorBrush> sepBrush;
                d2d->CreateSolidColorBrush(cs.divider, &sepBrush);
                float sx = curX + sepGap * 0.5f;
                d2d->DrawLine(
                    D2D1::Point2F(sx, mainBarRect_.top + 11.0f * dpi),
                    D2D1::Point2F(sx, mainBarRect_.bottom - 11.0f * dpi),
                    sepBrush.Get(), 1.0f * dpi
                );
            }
            curX += sepAfter ? sepGap : btnGap;
        }
    }

    if (!pillInitialized_) {
        toolPillXSpring_.SnapTo(targetPillX);
        pillInitialized_ = true;
    } else {
        toolPillXSpring_.target = targetPillX;
    }

    // Draw sliding Mochi tonal capsule for active tool (with jelly stretch!)
    {
        float stretch = JellyStretch(toolPillXSpring_.velocity / std::max(1.0f, btnSize), 0.55f);
        float pillW = btnSize * (1.0f + std::fabs(stretch) * 0.6f);
        float pillH = btnSize * JellyCrossScale(std::fabs(stretch) * 0.4f);
        float px = toolPillXSpring_.value - (pillW - btnSize) * 0.5f;
        float py = mainBarRect_.top + padV + (btnSize - pillH) * 0.5f;
        D2D1_RECT_F pillRect = D2D1::RectF(px, py, px + pillW, py + pillH);

        ComPtr<ID2D1SolidColorBrush> pillBrush;
        d2d->CreateSolidColorBrush(cs.secondaryContainer, &pillBrush);
        FillG2RoundRect(d2d, engine_->D2DFactory(), pillRect, pillH * 0.5f, pillBrush.Get());
    }

    // Draw buttons & icons
    ComPtr<ID2D1SolidColorBrush> iconBrush, activeIconBrush, hoverBrush, primaryBrush, onPrimaryBrush;
    d2d->CreateSolidColorBrush(cs.onSurface, &iconBrush);
    d2d->CreateSolidColorBrush(cs.onSecondaryContainer, &activeIconBrush);
    d2d->CreateSolidColorBrush(cs.rowHighlight, &hoverBrush);
    d2d->CreateSolidColorBrush(cs.primary, &primaryBrush);
    d2d->CreateSolidColorBrush(cs.onPrimary, &onPrimaryBrush);

    const ToolbarBtn* hoveredPtr = nullptr;

    for (const auto& b : toolbarBtns_) {
        D2D1_RECT_F br = b.rect;
        if (b.id == pressedBtnId_) {
            float sc = pressScaleSpring_.value;
            float cx = (br.left + br.right) * 0.5f;
            float cy = (br.top + br.bottom) * 0.5f;
            float hw = (br.right - br.left) * 0.5f * sc;
            float hh = (br.bottom - br.top) * 0.5f * sc;
            br = D2D1::RectF(cx - hw, cy - hh, cx + hw, cy + hh);
        }

        if (b.isPrimaryAccent) {
            FillG2RoundRect(d2d, engine_->D2DFactory(), br, (br.bottom - br.top) * 0.5f, primaryBrush.Get());
        } else if (b.id == hoveredBtnId_ && b.enabled && !(b.isTool && b.tool == activeTool_)) {
            FillG2RoundRect(d2d, engine_->D2DFactory(), br, (br.bottom - br.top) * 0.5f, hoverBrush.Get());
        }

        if (b.id == hoveredBtnId_) hoveredPtr = &b;

        ID2D1Brush* fg = iconBrush.Get();
        if (b.isPrimaryAccent) {
            fg = onPrimaryBrush.Get();
        } else if (b.isTool && b.tool == activeTool_) {
            fg = activeIconBrush.Get();
        }
        iconBrush->SetOpacity(b.enabled ? 1.0f : 0.35f);

        float cx = (br.left + br.right) * 0.5f;
        float cy = (br.top + br.bottom) * 0.5f;
        DrawMochiIcon(d2d, engine_->D2DFactory(), b.icon, cx, cy, 19.0f * dpi, fg, engine_->RoundStrokeStyle().Get(), 1.85f * dpi);
        iconBrush->SetOpacity(1.0f);
    }

    // Secondary Mochi Liquid Glass Property Pill (Color swatches + Stroke / Mosaic intensity slider)
    colorSwatchRects_.clear();
    sliderTrackRect_ = {};
    if (showProps) {
        constexpr float pAlpha = 1.0f;
        const bool isFilter = (activeTool_ == EditTool::Mosaic || activeTool_ == EditTool::Blur);
        const float propW = isFilter ? (290.0f * dpi) : (366.0f * dpi);
        float propX = std::clamp(mainBarRect_.left, static_cast<float>(mon.left) + 10.0f * dpi,
                                 static_cast<float>(mon.right) - propW - 10.0f * dpi);
        float propY = propBelow
            ? (mainBarRect_.bottom + barGap)
            : (mainBarRect_.top - barGap - propH);

        propBarRect_ = D2D1::RectF(propX, propY, propX + propW, propY + propH);

        GlassSurfaceSpec ps{};
        ps.rect = propBarRect_;
        ps.cornerRadiusPx = propH * 0.5f;
        ps.dpiScale = dpi;
        ps.isDark = cs.isDark;
        ps.thick = false;
        ps.mode = config_->glassMode;
        ps.solidFallback = WithAlpha(cs.groupedContainer, 0.95f);
        ps.shadowOpacityScale = pAlpha;
        engine_->DrawGlassSurface(swapChain_, ps);

        float itemX = propBarRect_.left + 14.0f * dpi;
        const float midY = (propBarRect_.top + propBarRect_.bottom) * 0.5f;

        if (!isFilter) {
            // 7 curated color swatches
            const float swRad = 9.5f * dpi;
            for (uint32_t col : kAnnotationColors) {
                D2D1_RECT_F sr = D2D1::RectF(itemX, midY - swRad, itemX + swRad * 2.0f, midY + swRad);
                colorSwatchRects_.push_back({ col, sr });

                ComPtr<ID2D1SolidColorBrush> cb, ringB;
                d2d->CreateSolidColorBrush(Rgba(col, pAlpha), &cb);
                d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(itemX + swRad, midY), swRad, swRad), cb.Get());

                if (col == currentColorHex_) {
                    d2d->CreateSolidColorBrush(cs.onSurface, &ringB);
                    d2d->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(itemX + swRad, midY), swRad + 2.5f * dpi, swRad + 2.5f * dpi),
                                     ringB.Get(), 1.8f * dpi);
                } else if (col == 0xFFFFFF) {
                    d2d->CreateSolidColorBrush(cs.outline, &ringB);
                    d2d->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(itemX + swRad, midY), swRad, swRad), ringB.Get(), 1.0f * dpi);
                }
                itemX += swRad * 2.0f + 7.5f * dpi;
            }
            itemX += 6.0f * dpi;
        } else {
            auto hintFmt = engine_->CreateTextFormat(11.5f * dpi, DWRITE_FONT_WEIGHT_MEDIUM);
            ComPtr<ID2D1SolidColorBrush> lblB;
            d2d->CreateSolidColorBrush(WithAlpha(cs.onSurfaceVariant, pAlpha), &lblB);
            const wchar_t* lbl = (activeTool_ == EditTool::Mosaic) ? L"粒度 / 笔宽" : L"模糊度 / 笔宽";
            if (hintFmt) {
                hintFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                d2d->DrawText(lbl, static_cast<UINT32>(wcslen(lbl)), hintFmt.Get(),
                              D2D1::RectF(itemX, propBarRect_.top, itemX + 78.0f * dpi, propBarRect_.bottom), lblB.Get());
            }
            itemX += 82.0f * dpi;
        }

        // Mochi Slider for stroke width / mosaic block size
        float sliderRight = propBarRect_.right - 44.0f * dpi;
        float trackH = 6.0f * dpi;
        sliderTrackRect_ = D2D1::RectF(itemX, midY - trackH * 0.5f, sliderRight, midY + trackH * 0.5f);

        float normVal = 0.0f;
        int displayVal = 0;
        if (isFilter) {
            normVal = std::clamp((currentMosaicDp_ - 6.0f) / 30.0f, 0.0f, 1.0f);
            displayVal = static_cast<int>(std::round(currentMosaicDp_));
        } else {
            normVal = std::clamp((currentStrokeDp_ - 2.0f) / 22.0f, 0.0f, 1.0f);
            displayVal = static_cast<int>(std::round(currentStrokeDp_));
        }

        ComPtr<ID2D1SolidColorBrush> trackB, fillB, thumbB;
        d2d->CreateSolidColorBrush(cs.controlTrack, &trackB);
        d2d->CreateSolidColorBrush(cs.primary, &fillB);
        d2d->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &thumbB);

        FillG2RoundRect(d2d, engine_->D2DFactory(), sliderTrackRect_, trackH * 0.5f, trackB.Get());
        float thumbX = sliderTrackRect_.left + normVal * (sliderTrackRect_.right - sliderTrackRect_.left);
        D2D1_RECT_F activeTrack = D2D1::RectF(sliderTrackRect_.left, sliderTrackRect_.top,
                                              std::max(sliderTrackRect_.left + trackH, thumbX), sliderTrackRect_.bottom);
        FillG2RoundRect(d2d, engine_->D2DFactory(), activeTrack, trackH * 0.5f, fillB.Get());

        // Mochi capsule thumb
        D2D1_RECT_F thumbR = D2D1::RectF(thumbX - 8.5f * dpi, midY - 6.5f * dpi, thumbX + 8.5f * dpi, midY + 6.5f * dpi);
        DrawThumbContactShadow(d2d, engine_->D2DFactory(), thumbR, 6.5f * dpi, dpi, cs.isDark, 0.0f);
        FillG2RoundRect(d2d, engine_->D2DFactory(), thumbR, 6.5f * dpi, thumbB.Get());

        // Numeric readout (1:1 instant follow during drag per Mochi rule #1!)
        auto valFmt = engine_->CreateTextFormat(11.5f * dpi, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        if (valFmt) {
            valFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            valFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            std::wstring vStr = std::to_wstring(displayVal);
            d2d->DrawText(vStr.c_str(), static_cast<UINT32>(vStr.size()), valFmt.Get(),
                          D2D1::RectF(sliderRight + 4.0f * dpi, propBarRect_.top, propBarRect_.right - 6.0f * dpi, propBarRect_.bottom),
                          iconBrush.Get());
        }
    }

    // Hover Tooltip Capsule
    if (hoveredPtr && hoveredPtr->tooltip && hoveredPtr->tooltip[0] != L'\0') {
        auto tipFmt = engine_->CreateTextFormat(11.5f * dpi, DWRITE_FONT_WEIGHT_MEDIUM);
        if (tipFmt) {
            ComPtr<IDWriteTextLayout> tl;
            std::wstring ts = hoveredPtr->tooltip;
            if (SUCCEEDED(engine_->DWrite()->CreateTextLayout(ts.c_str(), static_cast<UINT32>(ts.size()), tipFmt.Get(), 400.0f * dpi, 40.0f * dpi, &tl))) {
                DWRITE_TEXT_METRICS tm{};
                tl->GetMetrics(&tm);
                float tw = tm.widthIncludingTrailingWhitespace + 18.0f * dpi;
                float th = 26.0f * dpi;
                float tx = (hoveredPtr->rect.left + hoveredPtr->rect.right) * 0.5f - tw * 0.5f;
                tx = std::clamp(tx, static_cast<float>(mon.left) + 6.0f * dpi, static_cast<float>(mon.right) - tw - 6.0f * dpi);
                float ty = mainBarRect_.top - th - 7.0f * dpi;
                if (ty < mon.top + 4.0f * dpi) ty = mainBarRect_.bottom + 7.0f * dpi;

                D2D1_RECT_F tipRect = D2D1::RectF(tx, ty, tx + tw, ty + th);
                ComPtr<ID2D1SolidColorBrush> tipBg, tipFg;
                d2d->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.11f, 0.11f, 0.88f), &tipBg);
                d2d->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.99f, 0.98f, 1.0f), &tipFg);
                FillG2RoundRect(d2d, engine_->D2DFactory(), tipRect, th * 0.5f, tipBg.Get());
                d2d->DrawTextLayout(D2D1::Point2F(tx + 9.0f * dpi, ty + (th - tm.height) * 0.5f), tl.Get(), tipFg.Get());
            }
        }
    }
}

void CaptureOverlay::RenderFrame() {
    if (!hwnd_ || !desktopBmp_) return;
    if (!engine_->EnsureWindowSwapChain(
            hwnd_,
            static_cast<UINT>(captured_.width),
            static_cast<UINT>(captured_.height),
            swapChain_)) {
        return;
    }

    RebuildFilterBitmapsIfNeeded();

    if (!engine_->BeginFrame(swapChain_, D2D1::ColorF(0, 0, 0, 1))) return;
    auto* d2d = engine_->D2D();

    const bool isDark = ResolveDarkTheme(config_->themeMode);
    const auto cs = BuildColorScheme(config_->seedAccent, isDark);

    // 1. Draw full captured desktop screenshot
    D2D1_RECT_F fullRect = D2D1::RectF(0.0f, 0.0f, static_cast<float>(captured_.width), static_cast<float>(captured_.height));
    d2d->DrawBitmap(desktopBmp_.Get(), fullRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);

    // 2. Determine active clear cutout (either hovered window in Selecting stage or selection_)
    D2D1_RECT_F clearBox = {0, 0, 0, 0};
    bool hasCutout = false;
    if (stage_ == Stage::Selecting) {
        if (hasHoveredWin_) {
            clearBox = D2D1::RectF(
                static_cast<float>(hoveredWinRect_.left),
                static_cast<float>(hoveredWinRect_.top),
                static_cast<float>(hoveredWinRect_.right),
                static_cast<float>(hoveredWinRect_.bottom)
            );
            hasCutout = true;
        }
    } else {
        clearBox = NormalizedSelection();
        hasCutout = (clearBox.right > clearBox.left && clearBox.bottom > clearBox.top);
    }

    // 3. Draw annotations clipped cleanly inside selection box
    if (stage_ == Stage::Editing && hasCutout) {
        d2d->PushAxisAlignedClip(clearBox, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        DrawAnnotations(d2d, mosaicBmp_.Get(), blurBmp_.Get(), 0.0f, 0.0f);
        d2d->PopAxisAlignedClip();
    }

    // 4. Draw dimmed scrim around the cutout (4 clean non-overlapping quads)
    ComPtr<ID2D1SolidColorBrush> scrimBrush;
    d2d->CreateSolidColorBrush(D2D1::ColorF(0.02f, 0.04f, 0.04f, 0.46f), &scrimBrush);
    if (hasCutout) {
        if (clearBox.top > 0)
            d2d->FillRectangle(D2D1::RectF(0, 0, fullRect.right, clearBox.top), scrimBrush.Get());
        if (clearBox.bottom < fullRect.bottom)
            d2d->FillRectangle(D2D1::RectF(0, clearBox.bottom, fullRect.right, fullRect.bottom), scrimBrush.Get());
        if (clearBox.left > 0)
            d2d->FillRectangle(D2D1::RectF(0, clearBox.top, clearBox.left, clearBox.bottom), scrimBrush.Get());
        if (clearBox.right < fullRect.right)
            d2d->FillRectangle(D2D1::RectF(clearBox.right, clearBox.top, fullRect.right, clearBox.bottom), scrimBrush.Get());
    } else {
        d2d->FillRectangle(fullRect, scrimBrush.Get());
    }

    // 5. Snapshot backdrop BEFORE drawing selection chrome & floating Liquid Glass bars
    //    so Liquid Glass refracts the clean desktop + annotations + scrim!
    if (config_->glassMode != GlassMode::Solid) {
        engine_->CaptureBackdrop(swapChain_);
    }

    // 6. Draw selection border, 8 Mochi handles, and size badge
    if (hasCutout) {
        float dpi = GetMonitorDpiScaleAt((clearBox.left + clearBox.right) * 0.5f, (clearBox.top + clearBox.bottom) * 0.5f);
        ComPtr<ID2D1SolidColorBrush> accentBrush, whiteBrush;
        d2d->CreateSolidColorBrush(cs.primary, &accentBrush);
        d2d->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1.0f), &whiteBrush);

        d2d->DrawRectangle(clearBox, accentBrush.Get(), (stage_ == Stage::Selecting ? 2.2f : 1.6f) * dpi);

        if (stage_ == Stage::Editing) {
            const float hr = 4.5f * dpi;
            const float mx = (clearBox.left + clearBox.right) * 0.5f;
            const float my = (clearBox.top + clearBox.bottom) * 0.5f;
            D2D1_POINT_2F handles[8] = {
                {clearBox.left, clearBox.top}, {mx, clearBox.top}, {clearBox.right, clearBox.top},
                {clearBox.right, my}, {clearBox.right, clearBox.bottom}, {mx, clearBox.bottom},
                {clearBox.left, clearBox.bottom}, {clearBox.left, my}
            };
            for (const auto& hp : handles) {
                d2d->FillEllipse(D2D1::Ellipse(hp, hr, hr), whiteBrush.Get());
                d2d->DrawEllipse(D2D1::Ellipse(hp, hr, hr), accentBrush.Get(), 1.6f * dpi);
            }
        }

        // Top-left size capsule badge (W × H)
        int cw = static_cast<int>(clearBox.right - clearBox.left);
        int ch = static_cast<int>(clearBox.bottom - clearBox.top);
        wchar_t szStr[64]{};
        swprintf_s(szStr, L"%d × %d", cw, ch);
        auto szFmt = engine_->CreateTextFormat(11.5f * dpi, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        if (szFmt) {
            ComPtr<IDWriteTextLayout> tl;
            if (SUCCEEDED(engine_->DWrite()->CreateTextLayout(szStr, static_cast<UINT32>(wcslen(szStr)), szFmt.Get(), 260.0f * dpi, 32.0f * dpi, &tl))) {
                DWRITE_TEXT_METRICS tm{};
                tl->GetMetrics(&tm);
                float bw = tm.widthIncludingTrailingWhitespace + 18.0f * dpi;
                float bh = 25.0f * dpi;
                float bx = clearBox.left;
                float by = clearBox.top - bh - 7.0f * dpi;
                RECT mon = GetMonitorLocalRectAt(clearBox.left, clearBox.top);
                if (by < mon.top + 4.0f * dpi) by = clearBox.top + 7.0f * dpi;
                bx = std::clamp(bx, static_cast<float>(mon.left) + 4.0f * dpi, static_cast<float>(mon.right) - bw - 4.0f * dpi);

                GlassSurfaceSpec bs{};
                bs.rect = D2D1::RectF(bx, by, bx + bw, by + bh);
                bs.cornerRadiusPx = bh * 0.5f;
                bs.dpiScale = dpi;
                bs.isDark = true;
                bs.thick = true;
                bs.mode = config_->glassMode;
                bs.solidFallback = D2D1::ColorF(0.08f, 0.11f, 0.11f, 0.88f);
                engine_->DrawGlassSurface(swapChain_, bs);

                d2d->DrawTextLayout(D2D1::Point2F(bx + 9.0f * dpi, by + (bh - tm.height) * 0.5f), tl.Get(), whiteBrush.Get());
            }
        }
    }

    // 7. Draw Mochi Liquid Glass Floating Toolbars & Magnifier Loupe
    DrawFloatingToolbars();
    DrawMagnifierLoupe();

    engine_->EndFrame(swapChain_, 1);
}

bool CaptureOverlay::RenderCroppedResult(std::vector<uint8_t>& outBgra, int& outW, int& outH) {
    CommitActiveText();
    D2D1_RECT_F sel = NormalizedSelection();
    int sx = std::clamp(static_cast<int>(sel.left), 0, captured_.width);
    int sy = std::clamp(static_cast<int>(sel.top), 0, captured_.height);
    int ex = std::clamp(static_cast<int>(sel.right), 0, captured_.width);
    int ey = std::clamp(static_cast<int>(sel.bottom), 0, captured_.height);
    outW = ex - sx;
    outH = ey - sy;
    if (outW <= 0 || outH <= 0) return false;

    // Fast path when zero annotations exist: direct exact crop!
    if (annotations_.empty()) {
        outBgra.resize(static_cast<size_t>(outW) * static_cast<size_t>(outH) * 4);
        for (int y = 0; y < outH; ++y) {
            const uint8_t* srcRow = captured_.bgra.data() +
                (static_cast<size_t>(sy + y) * captured_.width + sx) * 4;
            std::memcpy(outBgra.data() + static_cast<size_t>(y) * outW * 4, srcRow, static_cast<size_t>(outW) * 4);
        }
        return true;
    }

    // Render base cropped screenshot + vector/filter annotations onto an exact 1:1 WIC bitmap render target
    ComPtr<IWICBitmap> wicBmp;
    if (FAILED(engine_->WIC()->CreateBitmap(
            static_cast<UINT>(outW),
            static_cast<UINT>(outH),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnDemand,
            &wicBmp))) {
        return false;
    }

    D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f
    );
    ComPtr<ID2D1RenderTarget> wicRt;
    if (FAILED(engine_->D2DFactory()->CreateWicBitmapRenderTarget(wicBmp.Get(), rtp, &wicRt))) {
        return false;
    }

    // Extract cropped base BGRA
    std::vector<uint8_t> baseCrop(static_cast<size_t>(outW) * static_cast<size_t>(outH) * 4);
    for (int y = 0; y < outH; ++y) {
        const uint8_t* srcRow = captured_.bgra.data() +
            (static_cast<size_t>(sy + y) * captured_.width + sx) * 4;
        std::memcpy(baseCrop.data() + static_cast<size_t>(y) * outW * 4, srcRow, static_cast<size_t>(outW) * 4);
    }

    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        96.0f, 96.0f
    );
    ComPtr<ID2D1Bitmap> baseD2d, mosD2d, blrD2d;
    wicRt->CreateBitmap(D2D1::SizeU(outW, outH), baseCrop.data(), outW * 4, &bp, &baseD2d);

    float dpi = GetMonitorDpiScaleAt((sel.left + sel.right) * 0.5f, (sel.top + sel.bottom) * 0.5f);
    int mosaicBlockPx = std::max(4, static_cast<int>(std::round(currentMosaicDp_ * dpi)));
    int blurRadPx     = std::max(4, static_cast<int>(std::round(currentMosaicDp_ * 0.9f * dpi)));

    bool hasMos = false, hasBlr = false;
    for (const auto& a : annotations_) {
        if (a.type == EditTool::Mosaic) hasMos = true;
        if (a.type == EditTool::Blur)   hasBlr = true;
    }
    if (hasMos) {
        std::vector<uint8_t> mos;
        GenerateMosaicBuffer(baseCrop.data(), outW, outH, mosaicBlockPx, mos);
        wicRt->CreateBitmap(D2D1::SizeU(outW, outH), mos.data(), outW * 4, &bp, &mosD2d);
    }
    if (hasBlr) {
        std::vector<uint8_t> blr;
        GenerateBlurredBuffer(baseCrop.data(), outW, outH, blurRadPx, blr);
        wicRt->CreateBitmap(D2D1::SizeU(outW, outH), blr.data(), outW * 4, &bp, &blrD2d);
    }

    RECT savedFilterRect = cachedFilterSelRect_;
    cachedFilterSelRect_ = RECT{ sx, sy, ex, ey };

    wicRt->BeginDraw();
    wicRt->DrawBitmap(baseD2d.Get(), D2D1::RectF(0, 0, static_cast<float>(outW), static_cast<float>(outH)));
    DrawAnnotations(wicRt.Get(), mosD2d.Get(), blrD2d.Get(), -static_cast<float>(sx), -static_cast<float>(sy));
    HRESULT hr = wicRt->EndDraw();

    cachedFilterSelRect_ = savedFilterRect;
    if (FAILED(hr)) return false;

    WICRect lockRect{ 0, 0, outW, outH };
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(wicBmp->Lock(&lockRect, WICBitmapLockRead, &lock))) return false;

    UINT stride = 0, cbSize = 0;
    BYTE* data = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&cbSize, &data)) || !data) {
        return false;
    }

    outBgra.resize(static_cast<size_t>(outW) * static_cast<size_t>(outH) * 4);
    for (int y = 0; y < outH; ++y) {
        uint8_t* dst = outBgra.data() + static_cast<size_t>(y) * outW * 4;
        const uint8_t* src = data + static_cast<size_t>(y) * stride;
        std::memcpy(dst, src, static_cast<size_t>(outW) * 4);
        for (int x = 0; x < outW; ++x) dst[x * 4 + 3] = 255;
    }
    return true;
}

void CaptureOverlay::CommitActiveText() {
    if (!editingText_) return;
    editingText_ = false;
    if (hasActiveDraft_ && !activeDraft_.text.empty()) {
        annotations_.push_back(activeDraft_);
        redoStack_.clear();
    }
    hasActiveDraft_ = false;
}

void CaptureOverlay::PerformUndo() {
    CommitActiveText();
    if (!annotations_.empty()) {
        if (annotations_.back().type == EditTool::StepCounter && nextStepNumber_ > 1) {
            --nextStepNumber_;
        }
        redoStack_.push_back(std::move(annotations_.back()));
        annotations_.pop_back();
    }
}

void CaptureOverlay::PerformRedo() {
    CommitActiveText();
    if (!redoStack_.empty()) {
        if (redoStack_.back().type == EditTool::StepCounter) {
            ++nextStepNumber_;
        }
        annotations_.push_back(std::move(redoStack_.back()));
        redoStack_.pop_back();
    }
}

void CaptureOverlay::PerformClearAnnotations() {
    CommitActiveText();
    annotations_.clear();
    redoStack_.clear();
    nextStepNumber_ = 1;
}

void CaptureOverlay::PerformCopyAndFinish() {
    std::vector<uint8_t> cropped;
    int w = 0, h = 0;
    if (RenderCroppedResult(cropped, w, h)) {
        CopyBgraToClipboard(hwnd_, cropped.data(), w, h);
    }
    CloseSession();
}

void CaptureOverlay::PerformSaveToFile() {
    std::vector<uint8_t> cropped;
    int w = 0, h = 0;
    if (!RenderCroppedResult(cropped, w, h)) return;

    // Temporarily drop TOPMOST while native Save dialog is modal so dialog appears cleanly
    SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    bool asJpeg = false;
    std::wstring path = PromptSaveScreenshotPath(hwnd_, config_->defaultSaveFormat, asJpeg);
    if (!path.empty()) {
        SaveBgraToFile(engine_->WIC(), path, cropped.data(), w, h, asJpeg);
        if (config_->autoCopyOnCapture) {
            CopyBgraToClipboard(hwnd_, cropped.data(), w, h);
        }
        CloseSession();
        return;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

void CaptureOverlay::PerformPinToScreen() {
    std::vector<uint8_t> cropped;
    int w = 0, h = 0;
    if (!RenderCroppedResult(cropped, w, h)) return;

    D2D1_RECT_F sel = NormalizedSelection();
    int sx = static_cast<int>(sel.left) + captured_.originX;
    int sy = static_cast<int>(sel.top) + captured_.originY;
    if (config_->autoCopyOnCapture) {
        CopyBgraToClipboard(hwnd_, cropped.data(), w, h);
    }
    CreatePinWindow(hInstance_, engine_->WIC(), cropped.data(), w, h, sx, sy);
    CloseSession();
}

LRESULT CALLBACK CaptureOverlay::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    CaptureOverlay* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<CaptureOverlay*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<CaptureOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CaptureOverlay::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    auto ptIn = [](const D2D1_RECT_F& r, float px, float py) {
        return px >= r.left && px <= r.right && py >= r.top && py <= r.bottom;
    };

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        RenderFrame();
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            UpdateCursorForPos(mousePos_.x, mousePos_.y);
            return TRUE;
        }
        break;

    case WM_TIMER: {
        if (wParam == kAnimTimerId) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            float dt = static_cast<float>(now.QuadPart - lastQpc_.QuadPart) / static_cast<float>(qpcFreq_.QuadPart);
            lastQpc_ = now;

            bool anim = false;
            anim = toolPillXSpring_.Step(dt) || anim;
            anim = pressScaleSpring_.Step(dt) || anim;
            anim = glowAlphaSpring_.Step(dt) || anim;

            if (anim || editingText_) {
                RenderFrame();
            }
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        float x = static_cast<float>(GET_X_LPARAM(lParam));
        float y = static_cast<float>(GET_Y_LPARAM(lParam));
        mousePos_ = D2D1::Point2F(x, y);
        SetFocus(hwnd_);
        SetCapture(hwnd_);

        if (stage_ == Stage::Selecting) {
            stage_ = Stage::DraggingSelection;
            dragStartMouse_ = mousePos_;
            selection_ = D2D1::RectF(x, y, x, y);
            RenderFrame();
            return 0;
        }

        // Check Primary Toolbar click
        if (ptIn(mainBarRect_, x, y)) {
            CommitActiveText();
            for (const auto& b : toolbarBtns_) {
                if (ptIn(b.rect, x, y) && b.enabled) {
                    pressedBtnId_ = b.id;
                    pressScaleSpring_.target = 0.92f;
                    pressGlowPos_ = mousePos_;
                    glowAlphaSpring_.target = 1.0f;
                    RenderFrame();
                    return 0;
                }
            }
            return 0;
        }

        // Check Property Bar click (Color swatches / Slider)
        if (activeTool_ != EditTool::Move && ptIn(propBarRect_, x, y)) {
            for (const auto& sw : colorSwatchRects_) {
                if (ptIn(sw.second, x, y)) {
                    currentColorHex_ = sw.first;
                    config_->defaultColorHex = currentColorHex_;
                    if (editingText_ && hasActiveDraft_) {
                        activeDraft_.colorHex = currentColorHex_;
                    }
                    RenderFrame();
                    return 0;
                }
            }
            D2D1_RECT_F hitSlider = D2D1::RectF(
                sliderTrackRect_.left - 10.0f, propBarRect_.top,
                sliderTrackRect_.right + 10.0f, propBarRect_.bottom
            );
            if (ptIn(hitSlider, x, y) && sliderTrackRect_.right > sliderTrackRect_.left) {
                dragMode_ = DragMode::DraggingPropertySlider;
                float t = std::clamp((x - sliderTrackRect_.left) / (sliderTrackRect_.right - sliderTrackRect_.left), 0.0f, 1.0f);
                if (activeTool_ == EditTool::Mosaic || activeTool_ == EditTool::Blur) {
                    currentMosaicDp_ = 6.0f + t * 30.0f;
                    config_->defaultMosaicSize = static_cast<int>(std::round(currentMosaicDp_));
                } else {
                    currentStrokeDp_ = 2.0f + t * 22.0f;
                    config_->defaultStrokeSize = static_cast<int>(std::round(currentStrokeDp_));
                    if (editingText_ && hasActiveDraft_) {
                        float dpi = GetMonitorDpiScaleAt(x, y);
                        activeDraft_.strokeWidth = currentStrokeDp_ * dpi;
                    }
                }
                RenderFrame();
                return 0;
            }
            return 0;
        }

        // Check Resize Handles (available when Move tool active or before annotations, or outside interior)
        float dpi = GetMonitorDpiScaleAt(x, y);
        DragMode hm = HitTestSelectionHandles(x, y, dpi);
        if (hm != DragMode::None && (activeTool_ == EditTool::Move || annotations_.empty())) {
            CommitActiveText();
            dragMode_ = hm;
            dragStartMouse_ = mousePos_;
            dragStartSelection_ = NormalizedSelection();
            selection_ = dragStartSelection_;
            return 0;
        }

        D2D1_RECT_F sel = NormalizedSelection();
        if (ptIn(sel, x, y)) {
            if (activeTool_ == EditTool::Move) {
                CommitActiveText();
                dragMode_ = DragMode::MoveSelection;
                dragStartMouse_ = mousePos_;
                dragStartSelection_ = sel;
                selection_ = sel;
                return 0;
            }

            if (activeTool_ == EditTool::Text) {
                CommitActiveText();
                hasActiveDraft_ = true;
                editingText_ = true;
                caretBlinkTick_ = GetTickCount();
                activeDraft_ = AnnotationItem{};
                activeDraft_.type = EditTool::Text;
                activeDraft_.colorHex = currentColorHex_;
                activeDraft_.strokeWidth = currentStrokeDp_ * dpi;
                activeDraft_.startPt = mousePos_;
                UpdateImmCaretPosition();
                RenderFrame();
                return 0;
            }

            if (activeTool_ == EditTool::StepCounter) {
                CommitActiveText();
                AnnotationItem step{};
                step.type = EditTool::StepCounter;
                step.colorHex = currentColorHex_;
                step.strokeWidth = currentStrokeDp_ * dpi;
                step.startPt = mousePos_;
                step.stepIndex = nextStepNumber_++;
                annotations_.push_back(step);
                redoStack_.clear();
                RenderFrame();
                return 0;
            }

            // Start drawing stroke or geometric shape
            CommitActiveText();
            RebuildFilterBitmapsIfNeeded();
            dragMode_ = DragMode::DrawingAnnotation;
            hasActiveDraft_ = true;
            activeDraft_ = AnnotationItem{};
            activeDraft_.type = activeTool_;
            activeDraft_.colorHex = currentColorHex_;
            activeDraft_.startPt = mousePos_;
            activeDraft_.endPt = mousePos_;

            if (activeTool_ == EditTool::Mosaic || activeTool_ == EditTool::Blur) {
                activeDraft_.isBoxRegion = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                activeDraft_.strokeWidth = std::max(14.0f, currentMosaicDp_ * 1.6f) * dpi;
                activeDraft_.points.push_back(mousePos_);
            } else if (activeTool_ == EditTool::Pen || activeTool_ == EditTool::Highlighter) {
                activeDraft_.strokeWidth = currentStrokeDp_ * dpi;
                activeDraft_.points.push_back(mousePos_);
            } else {
                activeDraft_.isBoxRegion = true;
                activeDraft_.strokeWidth = currentStrokeDp_ * dpi;
            }
            RenderFrame();
            return 0;
        } else {
            CommitActiveText();
            RenderFrame();
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        float x = static_cast<float>(GET_X_LPARAM(lParam));
        float y = static_cast<float>(GET_Y_LPARAM(lParam));
        mousePos_ = D2D1::Point2F(x, y);

        if (pressedBtnId_ != -1) {
            pressGlowPos_ = mousePos_;
        }

        if (stage_ == Stage::Selecting) {
            hasHoveredWin_ = false;
            if (config_->windowAutoSnap) {
                POINT ptL{ static_cast<LONG>(x), static_cast<LONG>(y) };
                for (const auto& r : windowSnapRects_) {
                    if (PtInRect(&r, ptL)) {
                        hoveredWinRect_ = r;
                        hasHoveredWin_ = true;
                        break;
                    }
                }
            }
            RenderFrame();
            return 0;
        }

        if (stage_ == Stage::DraggingSelection) {
            selection_.right = x;
            selection_.bottom = y;
            RenderFrame();
            return 0;
        }

        if (dragMode_ == DragMode::DraggingPropertySlider && sliderTrackRect_.right > sliderTrackRect_.left) {
            float t = std::clamp((x - sliderTrackRect_.left) / (sliderTrackRect_.right - sliderTrackRect_.left), 0.0f, 1.0f);
            if (activeTool_ == EditTool::Mosaic || activeTool_ == EditTool::Blur) {
                currentMosaicDp_ = 6.0f + t * 30.0f;
                config_->defaultMosaicSize = static_cast<int>(std::round(currentMosaicDp_));
            } else {
                currentStrokeDp_ = 2.0f + t * 22.0f;
                config_->defaultStrokeSize = static_cast<int>(std::round(currentStrokeDp_));
            }
            RenderFrame();
            return 0;
        }

        if (dragMode_ == DragMode::MoveSelection) {
            float dx = x - dragStartMouse_.x;
            float dy = y - dragStartMouse_.y;
            float sw = dragStartSelection_.right - dragStartSelection_.left;
            float sh = dragStartSelection_.bottom - dragStartSelection_.top;
            float nl = std::clamp(dragStartSelection_.left + dx, 0.0f, static_cast<float>(captured_.width) - sw);
            float nt = std::clamp(dragStartSelection_.top + dy, 0.0f, static_cast<float>(captured_.height) - sh);
            selection_ = D2D1::RectF(nl, nt, nl + sw, nt + sh);
            RenderFrame();
            return 0;
        }

        if (dragMode_ >= DragMode::ResizeNW && dragMode_ <= DragMode::ResizeW) {
            float dx = x - dragStartMouse_.x;
            float dy = y - dragStartMouse_.y;
            D2D1_RECT_F r = dragStartSelection_;
            if (dragMode_ == DragMode::ResizeNW || dragMode_ == DragMode::ResizeW || dragMode_ == DragMode::ResizeSW) r.left += dx;
            if (dragMode_ == DragMode::ResizeNE || dragMode_ == DragMode::ResizeE || dragMode_ == DragMode::ResizeSE) r.right += dx;
            if (dragMode_ == DragMode::ResizeNW || dragMode_ == DragMode::ResizeN || dragMode_ == DragMode::ResizeNE) r.top += dy;
            if (dragMode_ == DragMode::ResizeSW || dragMode_ == DragMode::ResizeS || dragMode_ == DragMode::ResizeSE) r.bottom += dy;
            selection_ = r;
            RenderFrame();
            return 0;
        }

        if (dragMode_ == DragMode::DrawingAnnotation && hasActiveDraft_) {
            D2D1_RECT_F sel = NormalizedSelection();
            D2D1_POINT_2F clamped = D2D1::Point2F(
                std::clamp(x, sel.left, sel.right),
                std::clamp(y, sel.top, sel.bottom)
            );
            activeDraft_.endPt = clamped;
            if (!activeDraft_.isBoxRegion) {
                if (activeDraft_.points.empty() ||
                    std::hypot(clamped.x - activeDraft_.points.back().x, clamped.y - activeDraft_.points.back().y) >= 1.5f) {
                    activeDraft_.points.push_back(clamped);
                }
            }
            RenderFrame();
            return 0;
        }

        // Hover tracking on toolbar buttons
        int prevHover = hoveredBtnId_;
        hoveredBtnId_ = -1;
        if (ptIn(mainBarRect_, x, y)) {
            for (const auto& b : toolbarBtns_) {
                if (ptIn(b.rect, x, y)) {
                    hoveredBtnId_ = b.id;
                    break;
                }
            }
        }
        UpdateCursorForPos(x, y);
        if (prevHover != hoveredBtnId_ || config_->showMagnifier) {
            RenderFrame();
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        float x = static_cast<float>(GET_X_LPARAM(lParam));
        float y = static_cast<float>(GET_Y_LPARAM(lParam));
        mousePos_ = D2D1::Point2F(x, y);
        ReleaseCapture();

        if (pressedBtnId_ != -1) {
            int clickedId = pressedBtnId_;
            pressedBtnId_ = -1;
            pressScaleSpring_.target = 1.0f;
            glowAlphaSpring_.target = 0.0f;

            for (const auto& b : toolbarBtns_) {
                if (b.id == clickedId && ptIn(b.rect, x, y) && b.enabled) {
                    if (b.isTool) {
                        activeTool_ = b.tool;
                        RebuildFilterBitmapsIfNeeded();
                    } else if (b.id == BTN_ACT_UNDO) {
                        PerformUndo();
                    } else if (b.id == BTN_ACT_REDO) {
                        PerformRedo();
                    } else if (b.id == BTN_ACT_CLEAR) {
                        PerformClearAnnotations();
                    } else if (b.id == BTN_ACT_PIN) {
                        PerformPinToScreen();
                        return 0;
                    } else if (b.id == BTN_ACT_SAVE) {
                        PerformSaveToFile();
                        return 0;
                    } else if (b.id == BTN_ACT_CLOSE) {
                        CloseSession();
                        return 0;
                    } else if (b.id == BTN_ACT_COPY) {
                        PerformCopyAndFinish();
                        return 0;
                    }
                    break;
                }
            }
            RenderFrame();
            return 0;
        }

        if (stage_ == Stage::DraggingSelection) {
            float dragDist = std::hypot(x - dragStartMouse_.x, y - dragStartMouse_.y);
            if (dragDist < 5.0f) {
                if (hasHoveredWin_) {
                    selection_ = D2D1::RectF(
                        static_cast<float>(hoveredWinRect_.left),
                        static_cast<float>(hoveredWinRect_.top),
                        static_cast<float>(hoveredWinRect_.right),
                        static_cast<float>(hoveredWinRect_.bottom)
                    );
                    stage_ = Stage::Editing;
                    pillInitialized_ = false;
                } else {
                    stage_ = Stage::Selecting;
                }
            } else {
                selection_ = NormalizedSelection();
                if (selection_.right - selection_.left >= 4.0f && selection_.bottom - selection_.top >= 4.0f) {
                    stage_ = Stage::Editing;
                    pillInitialized_ = false;
                } else {
                    stage_ = Stage::Selecting;
                }
            }
            RenderFrame();
            return 0;
        }

        if (dragMode_ == DragMode::DrawingAnnotation && hasActiveDraft_) {
            bool valid = !activeDraft_.isBoxRegion
                ? (!activeDraft_.points.empty())
                : (std::hypot(activeDraft_.endPt.x - activeDraft_.startPt.x,
                              activeDraft_.endPt.y - activeDraft_.startPt.y) >= 3.0f);
            if (valid) {
                annotations_.push_back(std::move(activeDraft_));
                redoStack_.clear();
            }
            hasActiveDraft_ = false;
            dragMode_ = DragMode::None;
            RenderFrame();
            return 0;
        }

        if (dragMode_ != DragMode::None) {
            selection_ = NormalizedSelection();
            dragMode_ = DragMode::None;
            SaveAppConfig(*config_);
            RenderFrame();
            return 0;
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        float x = static_cast<float>(GET_X_LPARAM(lParam));
        float y = static_cast<float>(GET_Y_LPARAM(lParam));
        if (stage_ == Stage::Editing && !ptIn(mainBarRect_, x, y) && !ptIn(propBarRect_, x, y) &&
            ptIn(NormalizedSelection(), x, y) && activeTool_ == EditTool::Move) {
            PerformCopyAndFinish();
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        if (stage_ == Stage::Editing) {
            CommitActiveText();
            if (annotations_.empty()) {
                // Reset selection so user can re-select another window/region
                stage_ = Stage::Selecting;
                pillInitialized_ = false;
                RenderFrame();
            }
        } else {
            CloseSession();
        }
        return 0;
    }

    case WM_CHAR: {
        if (editingText_ && hasActiveDraft_) {
            wchar_t ch = static_cast<wchar_t>(wParam);
            if (ch == L'\b') {
                if (!activeDraft_.text.empty()) activeDraft_.text.pop_back();
            } else if (ch == L'\r') {
                if (GetKeyState(VK_SHIFT) & 0x8000) {
                    activeDraft_.text.push_back(L'\n');
                } else {
                    CommitActiveText();
                }
            } else if (ch >= 32) {
                activeDraft_.text.push_back(ch);
            }
            caretBlinkTick_ = GetTickCount();
            RenderFrame();
            return 0;
        }
        break;
    }

    case WM_KEYDOWN: {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (editingText_) {
            if (wParam == VK_ESCAPE) {
                CommitActiveText();
                RenderFrame();
                return 0;
            }
            if (ctrl && wParam == 'V' && hasActiveDraft_) {
                if (OpenClipboard(hwnd_)) {
                    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                    if (hData) {
                        const wchar_t* txt = static_cast<const wchar_t*>(GlobalLock(hData));
                        if (txt) {
                            for (const wchar_t* p = txt; *p; ++p) {
                                if (*p == L'\n' || *p >= 32) activeDraft_.text.push_back(*p);
                            }
                            GlobalUnlock(hData);
                        }
                    }
                    CloseClipboard();
                }
                caretBlinkTick_ = GetTickCount();
                RenderFrame();
                return 0;
            }
            return 0;
        }

        if (wParam == VK_ESCAPE) {
            CloseSession();
            return 0;
        }
        if (stage_ == Stage::Selecting && wParam == 'C' && !ctrl) {
            uint32_t hex = captured_.SamplePixelClamped(static_cast<int>(mousePos_.x), static_cast<int>(mousePos_.y));
            wchar_t hexStr[16]{};
            swprintf_s(hexStr, L"#%06X", hex);
            if (OpenClipboard(hwnd_)) {
                EmptyClipboard();
                HGLOBAL hm = GlobalAlloc(GMEM_MOVEABLE, sizeof(hexStr));
                if (hm) {
                    void* p = GlobalLock(hm);
                    if (p) { std::memcpy(p, hexStr, sizeof(hexStr)); GlobalUnlock(hm); }
                    SetClipboardData(CF_UNICODETEXT, hm);
                }
                CloseClipboard();
            }
            return 0;
        }
        if (stage_ == Stage::Editing) {
            if (wParam == VK_RETURN || (ctrl && wParam == 'C')) {
                PerformCopyAndFinish();
                return 0;
            }
            if (ctrl && wParam == 'S') {
                PerformSaveToFile();
                return 0;
            }
            if (ctrl && wParam == 'Z') {
                PerformUndo();
                RenderFrame();
                return 0;
            }
            if (ctrl && wParam == 'Y') {
                PerformRedo();
                RenderFrame();
                return 0;
            }
            if (!ctrl && wParam == 'T') {
                PerformPinToScreen();
                return 0;
            }
        }
        break;
    }
    }

    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

bool CaptureOverlay::SelfTestRenderPipeline() {
    captured_.originX = 0;
    captured_.originY = 0;
    captured_.width = 320;
    captured_.height = 200;
    captured_.bgra.resize(320 * 200 * 4);
    for (int y = 0; y < 200; ++y) {
        for (int x = 0; x < 320; ++x) {
            size_t idx = (static_cast<size_t>(y) * 320 + x) * 4;
            captured_.bgra[idx + 0] = static_cast<uint8_t>((x * 255) / 320);
            captured_.bgra[idx + 1] = static_cast<uint8_t>((y * 255) / 200);
            captured_.bgra[idx + 2] = 140;
            captured_.bgra[idx + 3] = 255;
        }
    }

    selection_ = D2D1::RectF(20, 20, 280, 180);
    annotations_.clear();

    // Add Pen stroke, Mosaic region, Arrow, Step badge, and Text
    AnnotationItem pen{};
    pen.type = EditTool::Pen;
    pen.colorHex = 0xFF3B30;
    pen.strokeWidth = 5.0f;
    pen.points = { {35, 40}, {80, 55}, {140, 45} };
    annotations_.push_back(pen);

    AnnotationItem mos{};
    mos.type = EditTool::Mosaic;
    mos.isBoxRegion = true;
    mos.startPt = {40, 80};
    mos.endPt = {150, 140};
    annotations_.push_back(mos);

    AnnotationItem arr{};
    arr.type = EditTool::Arrow;
    arr.colorHex = 0x34C759;
    arr.strokeWidth = 4.0f;
    arr.startPt = {170, 140};
    arr.endPt = {240, 60};
    annotations_.push_back(arr);

    AnnotationItem txt{};
    txt.type = EditTool::Text;
    txt.colorHex = 0xFFFFFF;
    txt.strokeWidth = 4.0f;
    txt.startPt = {160, 100};
    txt.text = L"MochiShot 测试";
    annotations_.push_back(txt);

    std::vector<uint8_t> outBgra;
    int outW = 0, outH = 0;
    bool ok = RenderCroppedResult(outBgra, outW, outH);
    captured_.Clear();
    annotations_.clear();
    return ok && outW == 260 && outH == 160 && outBgra.size() == 260u * 160u * 4u;
}

} // namespace mochi
