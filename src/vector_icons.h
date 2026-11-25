#pragma once
#include <d2d1_1.h>
#include <wrl/client.h>
#include <cmath>
#include "mochi_g2.h"
#include "mochi_theme.h"

namespace mochi {

// Crisp geometric vector icons inspired by mochi-icons (Ionicons outline style),
// optically centered inside [cx - size/2, cy - size/2, cx + size/2, cy + size/2].
enum class MochiIcon {
    CursorMove,
    Pen,
    Highlighter,
    RectShape,
    EllipseShape,
    ArrowTopRight,
    StepNumber,
    TextAa,
    MosaicGrid,
    BlurDrop,
    Undo,
    Redo,
    Trash,
    Pin,
    SaveDisk,
    CheckCopy,
    CloseCross,
    CameraCrop,
    SparklesGlass,
    KeyboardKey,
    RocketBoot,
    PaletteSwatch
};

inline void DrawMochiIcon(
    ID2D1RenderTarget* rt,
    ID2D1Factory* factory,
    MochiIcon icon,
    float cx,
    float cy,
    float sizePx,
    ID2D1Brush* brush,
    ID2D1StrokeStyle* roundStroke,
    float strokeWidthPx = 1.8f
) {
    if (!rt || !brush || sizePx <= 2.0f) return;
    const float r = sizePx * 0.42f;
    const float sw = strokeWidthPx;

    auto line = [&](float x1, float y1, float x2, float y2) {
        rt->DrawLine(D2D1::Point2F(cx + x1 * r, cy + y1 * r),
                     D2D1::Point2F(cx + x2 * r, cy + y2 * r),
                     brush, sw, roundStroke);
    };

    switch (icon) {
    case MochiIcon::CursorMove: {
        line(-0.85f, 0.0f, 0.85f, 0.0f);
        line(0.0f, -0.85f, 0.0f, 0.85f);
        line(-0.85f, 0.0f, -0.52f, -0.32f);
        line(-0.85f, 0.0f, -0.52f,  0.32f);
        line( 0.85f, 0.0f,  0.52f, -0.32f);
        line( 0.85f, 0.0f,  0.52f,  0.32f);
        line(0.0f, -0.85f, -0.32f, -0.52f);
        line(0.0f, -0.85f,  0.32f, -0.52f);
        line(0.0f,  0.85f, -0.32f,  0.52f);
        line(0.0f,  0.85f,  0.32f,  0.52f);
        break;
    }
    case MochiIcon::Pen: {
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> pg;
        if (SUCCEEDED(factory->CreatePathGeometry(&pg))) {
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> s;
            if (SUCCEEDED(pg->Open(&s))) {
                s->BeginFigure(D2D1::Point2F(cx - 0.78f * r, cy + 0.78f * r), D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1::Point2F(cx - 0.62f * r, cy + 0.22f * r));
                s->AddLine(D2D1::Point2F(cx + 0.36f * r, cy - 0.76f * r));
                s->AddLine(D2D1::Point2F(cx + 0.76f * r, cy - 0.36f * r));
                s->AddLine(D2D1::Point2F(cx - 0.22f * r, cy + 0.62f * r));
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                s->Close();
                rt->DrawGeometry(pg.Get(), brush, sw, roundStroke);
            }
        }
        line(0.14f, -0.54f, 0.54f, -0.14f);
        break;
    }
    case MochiIcon::Highlighter: {
        line(-0.72f, 0.72f, -0.32f, 0.72f);
        line(-0.52f, 0.45f, 0.35f, -0.42f);
        line(-0.12f, 0.68f, 0.72f, -0.16f);
        line(0.35f, -0.42f, 0.72f, -0.16f);
        line(-0.52f, 0.45f, -0.58f, 0.72f);
        line(-0.12f, 0.68f, -0.32f, 0.72f);
        break;
    }
    case MochiIcon::RectShape: {
        D2D1_RECT_F rr = D2D1::RectF(cx - 0.82f * r, cy - 0.66f * r, cx + 0.82f * r, cy + 0.66f * r);
        StrokeG2RoundRect(rt, factory, rr, 0.32f * r, brush, sw, roundStroke);
        break;
    }
    case MochiIcon::EllipseShape: {
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 0.84f * r, 0.68f * r), brush, sw, roundStroke);
        break;
    }
    case MochiIcon::ArrowTopRight: {
        line(-0.72f, 0.72f, 0.72f, -0.72f);
        line(0.05f, -0.72f, 0.72f, -0.72f);
        line(0.72f, -0.72f, 0.72f, -0.05f);
        break;
    }
    case MochiIcon::StepNumber: {
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 0.84f * r, 0.84f * r), brush, sw, roundStroke);
        line(-0.16f, -0.26f, 0.06f, -0.44f);
        line(0.06f, -0.44f, 0.06f, 0.44f);
        line(-0.22f, 0.44f, 0.30f, 0.44f);
        break;
    }
    case MochiIcon::TextAa: {
        line(-0.72f, 0.68f, -0.22f, -0.68f);
        line(-0.22f, -0.68f, 0.28f, 0.68f);
        line(-0.55f, 0.22f, 0.11f, 0.22f);
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 0.58f * r, cy + 0.28f * r), 0.24f * r, 0.34f * r), brush, sw, roundStroke);
        line(0.82f, -0.06f, 0.82f, 0.66f);
        break;
    }
    case MochiIcon::MosaicGrid: {
        D2D1_RECT_F outer = D2D1::RectF(cx - 0.80f * r, cy - 0.80f * r, cx + 0.80f * r, cy + 0.80f * r);
        StrokeG2RoundRect(rt, factory, outer, 0.26f * r, brush, sw * 0.9f, roundStroke);
        float cell = 0.533f * r;
        float x0 = cx - 0.80f * r;
        float y0 = cy - 0.80f * r;
        for (int iy = 0; iy < 3; ++iy) {
            for (int ix = 0; ix < 3; ++ix) {
                if ((ix + iy) % 2 == 0) {
                    rt->FillRectangle(D2D1::RectF(x0 + ix * cell + 0.8f, y0 + iy * cell + 0.8f,
                                                  x0 + (ix + 1) * cell - 0.8f, y0 + (iy + 1) * cell - 0.8f), brush);
                }
            }
        }
        break;
    }
    case MochiIcon::BlurDrop: {
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> pg;
        if (SUCCEEDED(factory->CreatePathGeometry(&pg))) {
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> s;
            if (SUCCEEDED(pg->Open(&s))) {
                s->BeginFigure(D2D1::Point2F(cx, cy - 0.85f * r), D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddBezier(D2D1::BezierSegment(
                    D2D1::Point2F(cx + 0.65f * r, cy - 0.15f * r),
                    D2D1::Point2F(cx + 0.72f * r, cy + 0.78f * r),
                    D2D1::Point2F(cx, cy + 0.82f * r)));
                s->AddBezier(D2D1::BezierSegment(
                    D2D1::Point2F(cx - 0.72f * r, cy + 0.78f * r),
                    D2D1::Point2F(cx - 0.65f * r, cy - 0.15f * r),
                    D2D1::Point2F(cx, cy - 0.85f * r)));
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                s->Close();
                rt->DrawGeometry(pg.Get(), brush, sw, roundStroke);
            }
        }
        line(-0.30f, 0.25f, 0.30f, 0.25f);
        line(-0.20f, 0.50f, 0.20f, 0.50f);
        break;
    }
    case MochiIcon::Undo: {
        line(-0.75f, -0.18f, -0.32f, -0.58f);
        line(-0.75f, -0.18f, -0.32f,  0.22f);
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> pg;
        if (SUCCEEDED(factory->CreatePathGeometry(&pg))) {
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> s;
            if (SUCCEEDED(pg->Open(&s))) {
                s->BeginFigure(D2D1::Point2F(cx - 0.72f * r, cy - 0.18f * r), D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1::Point2F(cx + 0.18f * r, cy - 0.18f * r));
                s->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(cx + 0.18f * r, cy + 0.66f * r),
                    D2D1::SizeF(0.42f * r, 0.42f * r), 0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                s->AddLine(D2D1::Point2F(cx - 0.35f * r, cy + 0.66f * r));
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                s->Close();
                rt->DrawGeometry(pg.Get(), brush, sw, roundStroke);
            }
        }
        break;
    }
    case MochiIcon::Redo: {
        line(0.75f, -0.18f, 0.32f, -0.58f);
        line(0.75f, -0.18f, 0.32f,  0.22f);
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> pg;
        if (SUCCEEDED(factory->CreatePathGeometry(&pg))) {
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> s;
            if (SUCCEEDED(pg->Open(&s))) {
                s->BeginFigure(D2D1::Point2F(cx + 0.72f * r, cy - 0.18f * r), D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1::Point2F(cx - 0.18f * r, cy - 0.18f * r));
                s->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(cx - 0.18f * r, cy + 0.66f * r),
                    D2D1::SizeF(0.42f * r, 0.42f * r), 0.0f,
                    D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                s->AddLine(D2D1::Point2F(cx + 0.35f * r, cy + 0.66f * r));
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                s->Close();
                rt->DrawGeometry(pg.Get(), brush, sw, roundStroke);
            }
        }
        break;
    }
    case MochiIcon::Trash: {
        line(-0.72f, -0.45f, 0.72f, -0.45f);
        line(-0.28f, -0.45f, -0.20f, -0.72f);
        line(-0.20f, -0.72f,  0.20f, -0.72f);
        line( 0.20f, -0.72f,  0.28f, -0.45f);
        D2D1_RECT_F body = D2D1::RectF(cx - 0.54f * r, cy - 0.45f * r, cx + 0.54f * r, cy + 0.78f * r);
        StrokeG2RoundRect(rt, factory, body, 0.22f * r, brush, sw, roundStroke);
        line(-0.18f, -0.12f, -0.18f, 0.45f);
        line( 0.18f, -0.12f,  0.18f, 0.45f);
        break;
    }
    case MochiIcon::Pin: {
        line(0.0f, 0.22f, 0.0f, 0.85f);
        line(-0.62f, 0.22f, 0.62f, 0.22f);
        line(-0.42f, -0.68f, 0.42f, -0.68f);
        line(-0.34f, -0.68f, -0.52f, 0.22f);
        line( 0.34f, -0.68f,  0.52f, 0.22f);
        break;
    }
    case MochiIcon::SaveDisk: {
        line(0.0f, -0.78f, 0.0f, 0.28f);
        line(0.0f, 0.28f, -0.42f, -0.12f);
        line(0.0f, 0.28f,  0.42f, -0.12f);
        line(-0.74f, 0.38f, -0.74f, 0.72f);
        line(-0.74f, 0.72f,  0.74f, 0.72f);
        line( 0.74f, 0.72f,  0.74f, 0.38f);
        break;
    }
    case MochiIcon::CheckCopy: {
        line(-0.72f, 0.06f, -0.18f, 0.60f);
        line(-0.18f, 0.60f,  0.76f, -0.48f);
        break;
    }
    case MochiIcon::CloseCross: {
        line(-0.62f, -0.62f, 0.62f, 0.62f);
        line( 0.62f, -0.62f, -0.62f, 0.62f);
        break;
    }
    case MochiIcon::CameraCrop: {
        line(-0.85f, -0.48f, 0.48f, -0.48f);
        line( 0.48f, -0.48f, 0.48f,  0.85f);
        line(-0.48f, -0.85f, -0.48f, 0.48f);
        line(-0.48f,  0.48f,  0.85f, 0.48f);
        break;
    }
    case MochiIcon::SparklesGlass: {
        auto star = [&](float sx, float sy, float sr) {
            line(sx, sy - sr, sx + sr * 0.28f, sy - sr * 0.28f);
            line(sx + sr * 0.28f, sy - sr * 0.28f, sx + sr, sy);
            line(sx + sr, sy, sx + sr * 0.28f, sy + sr * 0.28f);
            line(sx + sr * 0.28f, sy + sr * 0.28f, sx, sy + sr);
            line(sx, sy + sr, sx - sr * 0.28f, sy + sr * 0.28f);
            line(sx - sr * 0.28f, sy + sr * 0.28f, sx - sr, sy);
            line(sx - sr, sy, sx - sr * 0.28f, sy - sr * 0.28f);
            line(sx - sr * 0.28f, sy - sr * 0.28f, sx, sy - sr);
        };
        star(-0.12f, 0.08f, 0.66f);
        star( 0.56f, -0.52f, 0.30f);
        break;
    }
    case MochiIcon::KeyboardKey: {
        D2D1_RECT_F kr = D2D1::RectF(cx - 0.88f * r, cy - 0.58f * r, cx + 0.88f * r, cy + 0.58f * r);
        StrokeG2RoundRect(rt, factory, kr, 0.26f * r, brush, sw, roundStroke);
        line(-0.48f, -0.16f, -0.34f, -0.16f);
        line(-0.07f, -0.16f,  0.07f, -0.16f);
        line( 0.34f, -0.16f,  0.48f, -0.16f);
        line(-0.42f,  0.22f,  0.42f,  0.22f);
        break;
    }
    case MochiIcon::RocketBoot: {
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 0.78f * r, 0.78f * r), brush, sw, roundStroke);
        line(0.0f, -0.82f, 0.0f, -0.05f);
        break;
    }
    case MochiIcon::PaletteSwatch: {
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 0.84f * r, 0.78f * r), brush, sw, roundStroke);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - 0.32f * r, cy - 0.22f * r), 0.14f * r, 0.14f * r), brush);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 0.08f * r, cy - 0.36f * r), 0.14f * r, 0.14f * r), brush);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 0.38f * r, cy - 0.05f * r), 0.14f * r, 0.14f * r), brush);
        break;
    }
    }
}

} // namespace mochi
