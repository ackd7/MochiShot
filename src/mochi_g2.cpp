#include "mochi_g2.h"
#include "mochi_theme.h"
#include <cmath>
#include <algorithm>

namespace mochi {

namespace {

// Exact Mochi G2 superellipse corner contour (matches G2Shape.kt g2Corner & LiquidShaders.kt supercircle).
// At smoothing = 1.0, corner extent ext = min(halfMin, 1.528665 * r).
// When ext == r (capsule end where r == halfMin), exponent n == 2.0 (exact circular arc),
// and when ext == 1.528665 * r, n == 3.32 (G2 curvature-continuous squircle passing through
// the exact circular 45-degree midpoint).
void AddCornerG2(
    ID2D1GeometrySink* sink,
    float cx, float cy,
    float dirInX, float dirInY,
    float dirOutX, float dirOutY,
    float r, float halfMin, float smoothing
) {
    if (r <= 0.05f) {
        sink->AddLine(D2D1::Point2F(cx, cy));
        return;
    }

    r = std::min(r, halfMin);
    const float ext = std::min(r * (1.0f + 0.528665f * std::clamp(smoothing, 0.0f, 1.0f)), halfMin);
    if (ext < 0.05f) {
        sink->AddLine(D2D1::Point2F(cx, cy));
        return;
    }

    constexpr float kSqrt2 = 1.41421356f;
    const float a = std::clamp(((kSqrt2 - 1.0f) * r) / (kSqrt2 * ext), 1e-4f, 0.2928932f);
    const float n = -std::log(2.0f) / std::log(1.0f - a);
    const float invN = 1.0f / n;

    const float ox = cx - dirInX * ext + dirOutX * ext;
    const float oy = cy - dirInY * ext + dirOutY * ext;

    constexpr int kSegments = 20;
    D2D1_POINT_2F pts[kSegments + 1];
    for (int i = 0; i <= kSegments; ++i) {
        float cp = 0.0f;
        float sp = 0.0f;
        if (i == 0) {
            cp = 1.0f;
            sp = 0.0f;
        } else if (i == kSegments) {
            cp = 0.0f;
            sp = 1.0f;
        } else {
            const float phi = (kPi * 0.5f) * (static_cast<float>(i) / static_cast<float>(kSegments));
            const float c = std::max(0.0f, std::cos(phi));
            const float s = std::max(0.0f, std::sin(phi));
            cp = (c > 0.0f) ? std::pow(c, 2.0f * invN) : 0.0f;
            sp = (s > 0.0f) ? std::pow(s, 2.0f * invN) : 0.0f;
        }
        pts[i] = D2D1::Point2F(
            ox - dirOutX * (ext * cp) + dirInX * (ext * sp),
            oy - dirOutY * (ext * cp) + dirInY * (ext * sp)
        );
    }

    sink->AddLine(pts[0]);
    sink->AddLines(&pts[1], kSegments);
}

} // namespace

ComPtr<ID2D1PathGeometry> CreateG2RoundRectGeometry4(
    ID2D1Factory* factory,
    const D2D1_RECT_F& rect,
    float topLeft,
    float topRight,
    float bottomRight,
    float bottomLeft
) {
    if (!factory) return nullptr;
    const float w = rect.right - rect.left;
    const float h = rect.bottom - rect.top;
    if (w <= 0.0f || h <= 0.0f) return nullptr;

    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory->CreatePathGeometry(&path))) return nullptr;

    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) return nullptr;

    const float halfMin = std::min(w, h) * 0.5f;
    const float tl = std::clamp(topLeft, 0.0f, halfMin);
    const float tr = std::clamp(topRight, 0.0f, halfMin);
    const float br = std::clamp(bottomRight, 0.0f, halfMin);
    const float bl = std::clamp(bottomLeft, 0.0f, halfMin);

    const float tlExt = std::min(tl * 1.528665f, halfMin);
    sink->BeginFigure(D2D1::Point2F(rect.left + tlExt, rect.top), D2D1_FIGURE_BEGIN_FILLED);
    AddCornerG2(sink.Get(), rect.right, rect.top, 1.0f, 0.0f, 0.0f, 1.0f, tr, halfMin, 1.0f);
    AddCornerG2(sink.Get(), rect.right, rect.bottom, 0.0f, 1.0f, -1.0f, 0.0f, br, halfMin, 1.0f);
    AddCornerG2(sink.Get(), rect.left, rect.bottom, -1.0f, 0.0f, 0.0f, -1.0f, bl, halfMin, 1.0f);
    AddCornerG2(sink.Get(), rect.left, rect.top, 0.0f, -1.0f, 1.0f, 0.0f, tl, halfMin, 1.0f);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return nullptr;
    return path;
}

ComPtr<ID2D1PathGeometry> CreateG2RoundRectGeometry(
    ID2D1Factory* factory,
    const D2D1_RECT_F& rect,
    float radius,
    float smoothing
) {
    if (!factory) return nullptr;
    const float w = rect.right - rect.left;
    const float h = rect.bottom - rect.top;
    if (w <= 0.0f || h <= 0.0f) return nullptr;

    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory->CreatePathGeometry(&path))) return nullptr;

    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) return nullptr;

    const float halfMin = std::min(w, h) * 0.5f;
    const float r = std::clamp(radius, 0.0f, halfMin);
    const float ext = std::min(r * (1.0f + 0.528665f * std::clamp(smoothing, 0.0f, 1.0f)), halfMin);

    sink->BeginFigure(D2D1::Point2F(rect.left + ext, rect.top), D2D1_FIGURE_BEGIN_FILLED);
    AddCornerG2(sink.Get(), rect.right, rect.top, 1.0f, 0.0f, 0.0f, 1.0f, r, halfMin, smoothing);
    AddCornerG2(sink.Get(), rect.right, rect.bottom, 0.0f, 1.0f, -1.0f, 0.0f, r, halfMin, smoothing);
    AddCornerG2(sink.Get(), rect.left, rect.bottom, -1.0f, 0.0f, 0.0f, -1.0f, r, halfMin, smoothing);
    AddCornerG2(sink.Get(), rect.left, rect.top, 0.0f, -1.0f, 1.0f, 0.0f, r, halfMin, smoothing);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return nullptr;
    return path;
}

void FillG2RoundRect(
    ID2D1RenderTarget* rt,
    ID2D1Factory* factory,
    const D2D1_RECT_F& rect,
    float radius,
    ID2D1Brush* brush,
    float smoothing
) {
    if (!rt || !factory || !brush) return;
    auto geom = CreateG2RoundRectGeometry(factory, rect, radius, smoothing);
    if (geom) {
        rt->FillGeometry(geom.Get(), brush);
    }
}

void StrokeG2RoundRect(
    ID2D1RenderTarget* rt,
    ID2D1Factory* factory,
    const D2D1_RECT_F& rect,
    float radius,
    ID2D1Brush* brush,
    float strokeWidth,
    ID2D1StrokeStyle* strokeStyle,
    float smoothing
) {
    if (!rt || !factory || !brush || strokeWidth <= 0.0f) return;
    auto geom = CreateG2RoundRectGeometry(factory, rect, radius, smoothing);
    if (geom) {
        rt->DrawGeometry(geom.Get(), brush, strokeWidth, strokeStyle);
    }
}

void DrawMochiShadow(
    ID2D1RenderTarget* rt,
    ID2D1Factory* factory,
    const D2D1_RECT_F& rect,
    float radius,
    float elevationDp,
    float dpiScale,
    bool isDark,
    float opacityScale
) {
    if (!rt || !factory || elevationDp <= 0.1f || opacityScale <= 0.01f) return;
    const float spread = elevationDp * dpiScale;
    const float yShift = std::min(spread * 0.42f, 10.0f * dpiScale);
    const float baseAlpha = (isDark ? 0.34f : 0.11f) * std::clamp(opacityScale, 0.0f, 1.0f);

    ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &brush);
    if (!brush) return;

    constexpr int kLayers = 6;
    for (int i = kLayers; i >= 1; --i) {
        float t = static_cast<float>(i) / static_cast<float>(kLayers);
        float expand = spread * (t * t * 1.15f);
        float dy = yShift * (0.35f + 0.65f * t);
        D2D1_RECT_F sr = D2D1::RectF(
            rect.left - expand,
            rect.top - expand * 0.65f + dy,
            rect.right + expand,
            rect.bottom + expand + dy
        );
        float weight = std::exp(-t * t * 2.1f) * (baseAlpha / 2.6f);
        brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, weight));
        FillG2RoundRect(rt, factory, sr, radius + expand, brush.Get(), 1.0f);
    }
}

void DrawThumbContactShadow(
    ID2D1RenderTarget* rt,
    ID2D1Factory* factory,
    const D2D1_RECT_F& thumbRect,
    float radius,
    float dpiScale,
    bool isDark,
    float pressFactor
) {
    if (!rt || !factory) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &brush);
    if (!brush) return;

    // Tight, crisp iOS-style contact shadow directly beneath thumb (does NOT muddy the switch track!)
    const float extra = pressFactor * 1.5f * dpiScale;
    const float alphaBase = (isDark ? 0.24f : 0.13f) * (1.0f + pressFactor * 0.35f);
    for (int i = 3; i >= 1; --i) {
        float t = static_cast<float>(i) / 3.0f;
        float expPx = (0.8f + t * 1.4f) * dpiScale + extra * t;
        float dy = (0.7f + t * 1.1f) * dpiScale;
        D2D1_RECT_F sr = D2D1::RectF(
            thumbRect.left - expPx * 0.6f,
            thumbRect.top + dy - expPx * 0.2f,
            thumbRect.right + expPx * 0.6f,
            thumbRect.bottom + dy + expPx * 0.6f
        );
        brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, alphaBase * 0.30f * (1.2f - t * 0.5f)));
        FillG2RoundRect(rt, factory, sr, radius + expPx * 0.5f, brush.Get(), 1.0f);
    }
}

float SupercircleSdf(float px, float py, float halfW, float halfH, float radius, float smoothing) {
    const float halfMin = std::min(halfW, halfH);
    if (halfMin <= 0.0f) return 1e5f;
    const float r = std::clamp(std::fabs(radius), 0.0f, halfMin);
    const float ext = std::min(r * (1.0f + 0.528665f * std::clamp(smoothing, 0.0f, 1.0f)), halfMin);
    const float ax = std::fabs(px);
    const float ay = std::fabs(py);

    if (ext < 1e-3f) {
        float qx = ax - halfW;
        float qy = ay - halfH;
        float ox = std::max(qx, 0.0f);
        float oy = std::max(qy, 0.0f);
        return std::hypot(ox, oy) + std::min(std::max(qx, qy), 0.0f);
    }

    constexpr float kSqrt2 = 1.41421356f;
    const float a = std::clamp(((kSqrt2 - 1.0f) * r) / (kSqrt2 * ext), 1e-4f, 0.2928932f);
    const float n = -std::log(2.0f) / std::log(1.0f - a);

    const float qx = ax - (halfW - ext);
    const float qy = ay - (halfH - ext);
    if (qx <= 0.0f || qy <= 0.0f) {
        float bx = ax - halfW;
        float by = ay - halfH;
        return std::max(bx, by);
    }
    const float ux = qx / ext;
    const float uy = qy / ext;
    const float norm = std::pow(std::pow(ux, n) + std::pow(uy, n), 1.0f / n);
    return (norm - 1.0f) * ext;
}

} // namespace mochi
