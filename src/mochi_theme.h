#pragma once
#include <windows.h>
#include <d2d1_1.h>
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace mochi {

constexpr float kPi = 3.14159265358979323846f;

inline D2D1_COLOR_F Rgba(uint32_t hexRgb, float alpha = 1.0f) {
    return D2D1::ColorF(
        ((hexRgb >> 16) & 0xFF) / 255.0f,
        ((hexRgb >> 8) & 0xFF) / 255.0f,
        (hexRgb & 0xFF) / 255.0f,
        alpha
    );
}

inline D2D1_COLOR_F LerpColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return D2D1::ColorF(
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t,
        a.a + (b.a - a.a) * t
    );
}

inline D2D1_COLOR_F WithAlpha(const D2D1_COLOR_F& c, float a) {
    return D2D1::ColorF(c.r, c.g, c.b, std::clamp(a, 0.0f, 1.0f));
}

enum class GlassMode : int {
    LiquidGlass = 0, // Full HLSL Liquid Glass refraction + mip blur + Fresnel rim
    Frosted     = 1, // Frosted blur without lens distortion
    Solid       = 2  // Solid tonal surface (ultra-low GPU fallback)
};

enum class ThemeMode : int {
    System = 0,
    Light  = 1,
    Dark   = 2
};

// Curated seed palettes inspired by feice11/mochi-compose (DefaultKeyColor = #2B6A5A)
enum class SeedAccent : int {
    MochiJade  = 0, // #2B6A5A (Mochi default)
    MochiTeal  = 1, // #196574 (Mochi preview glass)
    OceanBlue  = 2, // #295EA7
    IrisViolet = 3, // #63519F
    CoralWarm  = 4  // #9C4332
};

struct MochiColorScheme {
    bool isDark = false;
    D2D1_COLOR_F groupedBackground;      // Page background (#F2F5F4 / #0D1312)
    D2D1_COLOR_F groupedContainer;       // Section card (#FFFFFF / #17201E)
    D2D1_COLOR_F surfaceElevated;        // Floating solid fallback / thumb
    D2D1_COLOR_F primary;                // Active accent / switch checked track
    D2D1_COLOR_F onPrimary;              // Foreground on primary
    D2D1_COLOR_F secondaryContainer;     // Tonal pill / active tab capsule
    D2D1_COLOR_F onSecondaryContainer;   // Foreground on tonal pill
    D2D1_COLOR_F onSurface;              // Primary title text
    D2D1_COLOR_F onSurfaceVariant;       // Subtitle / section header text
    D2D1_COLOR_F controlTrack;           // Unchecked switch / segmented track
    D2D1_COLOR_F rowHighlight;           // Pressed list row highlight
    D2D1_COLOR_F divider;                // Hairline separator
    D2D1_COLOR_F outline;                // Subtle border
    D2D1_COLOR_F error;                  // Danger / conflict color
    D2D1_COLOR_F keyPreview;             // Seed swatch color
};

inline MochiColorScheme BuildColorScheme(SeedAccent accent, bool dark) {
    struct AccentTokens {
        uint32_t lightPrimary, lightOnPrimary, lightSecCont, lightOnSecCont;
        uint32_t darkPrimary, darkOnPrimary, darkSecCont, darkOnSecCont;
        uint32_t lightBg, darkBg, darkCard;
        uint32_t swatch;
    };

    static constexpr AccentTokens kAccents[] = {
        // MochiJade (#2B6A5A - Mochi Compose DefaultKeyColor)
        { 0x1E6A58, 0xFFFFFF, 0xCBEBE1, 0x09382D,
          0x83D6C0, 0x00382D, 0x1A4E42, 0xB8F1E1,
          0xF1F5F4, 0x0C1210, 0x161F1D, 0x2B6A5A },
        // MochiTeal (#196574 - Mochi Compose Glass Demo)
        { 0x196574, 0xFFFFFF, 0xCBE7EE, 0x063640,
          0x84D1E2, 0x003640, 0x184B56, 0xBCEBF6,
          0xEFF4F6, 0x0C1214, 0x161E22, 0x196574 },
        // OceanBlue (#295EA7)
        { 0x295EA7, 0xFFFFFF, 0xD6E3FF, 0x082E66,
          0xA6C8FF, 0x003062, 0x1E457A, 0xD6E3FF,
          0xF1F4F9, 0x0C1016, 0x161C25, 0x295EA7 },
        // IrisViolet (#63519F)
        { 0x63519F, 0xFFFFFF, 0xE7DEFF, 0x23135C,
          0xCCBDFF, 0x33206E, 0x4A3984, 0xE7DEFF,
          0xF4F2F8, 0x100E16, 0x1B1824, 0x63519F },
        // CoralWarm (#9C4332)
        { 0x9C4332, 0xFFFFFF, 0xFFDBD3, 0x3E0B03,
          0xFFB4A4, 0x5E170A, 0x7B2D1E, 0xFFDBD3,
          0xF8F2F0, 0x150E0D, 0x221816, 0x9C4332 },
    };

    int idx = std::clamp(static_cast<int>(accent), 0, 4);
    const auto& t = kAccents[idx];

    MochiColorScheme cs{};
    cs.isDark = dark;
    cs.keyPreview = Rgba(t.swatch);
    if (!dark) {
        cs.groupedBackground    = Rgba(t.lightBg);
        cs.groupedContainer     = Rgba(0xFFFFFF);
        cs.surfaceElevated      = Rgba(0xFFFFFF);
        cs.primary              = Rgba(t.lightPrimary);
        cs.onPrimary            = Rgba(t.lightOnPrimary);
        cs.secondaryContainer   = Rgba(t.lightSecCont);
        cs.onSecondaryContainer = Rgba(t.lightOnSecCont);
        cs.onSurface            = Rgba(0x141B1A);
        cs.onSurfaceVariant     = Rgba(0x5B6664);
        cs.controlTrack         = Rgba(0x141F1D, 0.11f);
        cs.rowHighlight         = Rgba(0x141F1D, 0.055f);
        cs.divider              = Rgba(0x141F1D, 0.075f);
        cs.outline              = Rgba(0x141F1D, 0.13f);
        cs.error                = Rgba(0xBA1A1A);
    } else {
        cs.groupedBackground    = Rgba(t.darkBg);
        cs.groupedContainer     = Rgba(t.darkCard);
        cs.surfaceElevated      = Rgba(0x263230);
        cs.primary              = Rgba(t.darkPrimary);
        cs.onPrimary            = Rgba(t.darkOnPrimary);
        cs.secondaryContainer   = Rgba(t.darkSecCont);
        cs.onSecondaryContainer = Rgba(t.darkOnSecCont);
        cs.onSurface            = Rgba(0xE4ECEA);
        cs.onSurfaceVariant     = Rgba(0x93A19E);
        cs.controlTrack         = Rgba(0xE2F0ED, 0.13f);
        cs.rowHighlight         = Rgba(0xE2F0ED, 0.065f);
        cs.divider              = Rgba(0xE2F0ED, 0.08f);
        cs.outline              = Rgba(0xE2F0ED, 0.15f);
        cs.error                = Rgba(0xFFB4AB);
    }
    return cs;
}

inline MochiColorScheme LerpScheme(const MochiColorScheme& a, const MochiColorScheme& b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    MochiColorScheme r{};
    r.isDark               = b.isDark;
    r.groupedBackground    = LerpColor(a.groupedBackground, b.groupedBackground, t);
    r.groupedContainer     = LerpColor(a.groupedContainer, b.groupedContainer, t);
    r.surfaceElevated      = LerpColor(a.surfaceElevated, b.surfaceElevated, t);
    r.primary              = LerpColor(a.primary, b.primary, t);
    r.onPrimary            = LerpColor(a.onPrimary, b.onPrimary, t);
    r.secondaryContainer   = LerpColor(a.secondaryContainer, b.secondaryContainer, t);
    r.onSecondaryContainer = LerpColor(a.onSecondaryContainer, b.onSecondaryContainer, t);
    r.onSurface            = LerpColor(a.onSurface, b.onSurface, t);
    r.onSurfaceVariant     = LerpColor(a.onSurfaceVariant, b.onSurfaceVariant, t);
    r.controlTrack         = LerpColor(a.controlTrack, b.controlTrack, t);
    r.rowHighlight         = LerpColor(a.rowHighlight, b.rowHighlight, t);
    r.divider              = LerpColor(a.divider, b.divider, t);
    r.outline              = LerpColor(a.outline, b.outline, t);
    r.error                = LerpColor(a.error, b.error, t);
    r.keyPreview           = LerpColor(a.keyPreview, b.keyPreview, t);
    return r;
}

// ============================================================================
// MochiSprings & FollowSpring (240Hz substep integration, interruptible)
// Mirrors MochiSprings.kt: k = (2*pi / duration)^2, zeta = 1 - bounce
// ============================================================================
struct MochiSpring {
    float value = 0.0f;
    float velocity = 0.0f;
    float target = 0.0f;
    float response = 0.30f;     // Duration seconds
    float dampingRatio = 0.85f; // 1.0 = critical, <1.0 = bouncy

    MochiSpring() = default;
    MochiSpring(float initial, float resp = 0.30f, float damp = 0.85f)
        : value(initial), velocity(0.0f), target(initial), response(resp), dampingRatio(damp) {}

    void SnapTo(float v) {
        value = v;
        target = v;
        velocity = 0.0f;
    }

    void ConfigureSwiftUI(float durationSec, float bounce) {
        response = std::max(0.04f, durationSec);
        float b = std::clamp(bounce, -0.95f, 0.95f);
        dampingRatio = (b >= 0.0f) ? (1.0f - b) : (1.0f / (1.0f + b));
    }

    bool IsSettled(float posEps = 0.0008f, float velEps = 0.008f) const {
        return std::fabs(value - target) <= posEps && std::fabs(velocity) <= velEps;
    }

    // Returns true while still animating
    bool Step(float dt) {
        if (dt <= 0.0f) return !IsSettled();
        dt = std::min(dt, 0.05f); // Guard against window move stalls

        const float omega = 2.0f * kPi / std::max(0.04f, response);
        const float k = omega * omega;
        const float c = 2.0f * dampingRatio * omega;

        // 240Hz substep semi-implicit Euler (matches MochiFollowSpring stability)
        constexpr float kSubStep = 1.0f / 240.0f;
        float rem = dt;
        while (rem > 1e-6f) {
            float h = std::min(rem, kSubStep);
            float force = -k * (value - target) - c * velocity;
            velocity += force * h;
            value += velocity * h;
            rem -= h;
        }

        if (IsSettled()) {
            value = target;
            velocity = 0.0f;
            return false;
        }
        return true;
    }
};

// iOS UIScrollView rubberBand formula (from mochi-compose elastic/RubberBand.kt)
inline float RubberBand(float overshoot, float dimension, float maxLimit) {
    if (dimension <= 0.0f || maxLimit <= 0.0f || std::fabs(overshoot) < 1e-5f) return 0.0f;
    float sign = overshoot < 0.0f ? -1.0f : 1.0f;
    float x = std::fabs(overshoot);
    constexpr float c = 0.55f;
    float result = (1.0f - (1.0f / ((x * c / maxLimit) + 1.0f))) * maxLimit;
    return sign * result;
}

// Mochi jelly deformation along motion axis (mochiJelly: [-0.10, +0.16])
inline float JellyStretch(float normVelocity, float gel = 0.6f) {
    float raw = std::tanh(normVelocity * 0.08f) * 0.16f * gel;
    return std::clamp(raw, -0.10f, 0.16f);
}

inline float JellyCrossScale(float stretch) {
    return 1.0f / std::sqrt(std::max(0.5f, 1.0f + stretch));
}

// Concentric inner corner radius (MochiConcentric.inner)
inline float ConcentricInner(float outerRadius, float padding, float minRadius = 4.0f) {
    if (outerRadius <= 0.0f) return 0.0f;
    return std::max(minRadius, outerRadius - padding);
}

// Radial press glow falloff: 0.85 * exp(-r^3 * 1.6) from mochiPressGlow
inline float PressGlowFalloff(float dist, float radius) {
    if (radius <= 1.0f) return 0.0f;
    float r = std::clamp(dist / radius, 0.0f, 2.0f);
    return 0.85f * std::exp(-r * r * r * 1.6f);
}

} // namespace mochi
