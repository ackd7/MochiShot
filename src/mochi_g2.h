#pragma once
#include <windows.h>
#include <d2d1_1.h>
#include <wrl/client.h>

namespace mochi {

using Microsoft::WRL::ComPtr;

// Builds a G2 continuous curvature rounded rectangle (MochiRoundedRectangle / MochiCapsule).
// Uses Mochi's supercircle / Kyant0 Capsule continuous corner contour so straight edges
// transition into corners without tangent/curvature seams, and smoothly morphs into a
// perfect capsule when radius >= min(width, height) / 2.
ComPtr<ID2D1PathGeometry> CreateG2RoundRectGeometry(
    ID2D1Factory* factory,
    const D2D1_RECT_F& rect,
    float radius,
    float smoothing = 1.0f
);

// Per-corner G2 continuous rounded rectangle (TL, TR, BR, BL)
ComPtr<ID2D1PathGeometry> CreateG2RoundRectGeometry4(
    ID2D1Factory* factory,
    const D2D1_RECT_F& rect,
    float topLeft,
    float topRight,
    float bottomRight,
    float bottomLeft
);

void FillG2RoundRect(
    ID2D1RenderTarget* rt,
    ID2D1Factory* factory,
    const D2D1_RECT_F& rect,
    float radius,
    ID2D1Brush* brush,
    float smoothing = 1.0f
);

void StrokeG2RoundRect(
    ID2D1RenderTarget* rt,
    ID2D1Factory* factory,
    const D2D1_RECT_F& rect,
    float radius,
    ID2D1Brush* brush,
    float strokeWidth = 1.0f,
    ID2D1StrokeStyle* strokeStyle = nullptr,
    float smoothing = 1.0f
);

// Soft dual-layer ambient + contact shadow for G2 capsules and cards (MochiShadow)
void DrawMochiShadow(
    ID2D1RenderTarget* rt,
    ID2D1Factory* factory,
    const D2D1_RECT_F& rect,
    float radius,
    float elevationDp,
    float dpiScale,
    bool isDark,
    float opacityScale = 1.0f
);

// Exact G2 supercircle signed distance field (matches LiquidShaders.kt supercircle)
float SupercircleSdf(float px, float py, float halfW, float halfH, float radius, float smoothing = 1.0f);

} // namespace mochi
