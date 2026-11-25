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

// Curated seed palettes matching feice11/mochi-compose preview (glass.png & MochiColorScheme.kt)
enum class SeedAccent : int {
    MochiTeal  = 0, // #196574 (Exact Mochi glass.png preview palette!)
    MochiJade  = 1, // #2B6A5A (Mochi DefaultKeyColor)
    OceanBlue  = 2, // #295EA7
    IrisViolet = 3, // #63519F
    CoralWarm  = 4  // #9C4332
};

struct MochiColorScheme {
    bool isDark = false;
    D2D1_COLOR_F groupedBackground;      // Page background (#EEF2F5 / #0B0F12)
    D2D1_COLOR_F groupedContainer;       // Section card (#FFFFFF / #161D21)
    D2D1_COLOR_F surfaceElevated;        // Segmented thumb (#FFFFFF / #38444A)
    D2D1_COLOR_F primary;                // Text/icon accent
    D2D1_COLOR_F onPrimary;              // Foreground on primary
    D2D1_COLOR_F switchCheckedTrack;     // Saturated high-contrast ON track so white thumb ALWAYS pops!
    D2D1_COLOR_F switchUncheckedTrack;   // Opaque neutral gray OFF track (#DCE2E6 / #30393E)
    D2D1_COLOR_F segmentedTrack;         // Opaque segmented capsule slot (#D7DDE1 / #252D31)
    D2D1_COLOR_F secondaryContainer;     // Tonal pill / active tab capsule
    D2D1_COLOR_F onSecondaryContainer;   // Foreground on tonal pill
    D2D1_COLOR_F onSurface;              // Primary title text
    D2D1_COLOR_F onSurfaceVariant;       // Subtitle / section header text
    D2D1_COLOR_F controlTrack;           // Subtle button / slider track
    D2D1_COLOR_F rowHighlight;           // Hover / pressed list row highlight
    D2D1_COLOR_F divider;                // Hairline separator
    D2D1_COLOR_F outline;                // Subtle border
    D2D1_COLOR_F error;                  // Danger / conflict color
    // 3-part tonal pie preview colors (matching glass.png "主题色" circles!)
    D2D1_COLOR_F pieTop;
    D2D1_COLOR_F pieBottomLeft;
    D2D1_COLOR_F pieBottomRight;
};

inline MochiColorScheme BuildColorScheme(SeedAccent accent, bool dark) {
    struct AccentTokens {
        uint32_t lightPrimary, lightSwitchOn, lightSecCont, lightOnSecCont;
        uint32_t darkPrimary, darkSwitchOn, darkSecCont, darkOnSecCont;
        uint32_t lightBg, darkBg, darkCard;
        uint32_t pieTop, pieBL, pieBR;
    };

    static constexpr AccentTokens kAccents[] = {
        // 0: MochiTeal (#196574 — Exact match to feice11/mochi-compose glass.png!)
        { 0x196170, 0x196170, 0xCDE7EE, 0x073742,
          0x7AD0E2, 0x1E788A, 0x194B56, 0xC2EEF8,
          0xEEF2F5, 0x0B1013, 0x161E22,
          0x7BC3D1, 0xB5DCE4, 0xD6EEF3 },
        // 1: MochiJade (#2B6A5A — Mochi DefaultKeyColor)
        { 0x1E6A58, 0x1E6A58, 0xCBEBE1, 0x08382D,
          0x7ED6BF, 0x1F7A64, 0x1A4E42, 0xBDF2E3,
          0xEEF3F1, 0x0B110F, 0x151E1C,
          0x86C7B5, 0xB7DFD4, 0xD7F2EA },
        // 2: OceanBlue (#295EA7)
        { 0x265CA6, 0x265CA6, 0xD6E3FF, 0x082E66,
          0x9ECAFF, 0x2B66B5, 0x1E457A, 0xD6E3FF,
          0xEEF2F8, 0x0B0F15, 0x151B24,
          0x94B5E8, 0xC2D6F5, 0xE0ECFF },
        // 3: IrisViolet (#63519F)
        { 0x604E9E, 0x604E9E, 0xE7DEFF, 0x23135C,
          0xCABEFF, 0x6D59B3, 0x473780, 0xECE4FF,
          0xF2F0F7, 0x0F0D15, 0x1A1723,
          0xB5A6E6, 0xD4CAFA, 0xECE6FF },
        // 4: CoralWarm (#9C4332)
        { 0x9E4231, 0x9E4231, 0xFFDBD3, 0x3E0B03,
          0xFFB4A4, 0xB34B37, 0x782C1D, 0xFFE2DC,
          0xF6F0EE, 0x130D0C, 0x201715,
          0xE89B8E, 0xF5C6BE, 0xFFE3DE },
    };

    int idx = std::clamp(static_cast<int>(accent), 0, 4);
    const auto& t = kAccents[idx];

    MochiColorScheme cs{};
    cs.isDark         = dark;
    cs.pieTop         = Rgba(t.pieTop);
    cs.pieBottomLeft  = Rgba(t.pieBL);
    cs.pieBottomRight = Rgba(t.pieBR);

    if (!dark) {
        cs.groupedBackground    = Rgba(t.lightBg);
        cs.groupedContainer     = Rgba(0xFFFFFF);
        cs.surfaceElevated      = Rgba(0xFFFFFF);
        cs.primary              = Rgba(t.lightPrimary);
        cs.onPrimary            = Rgba(0xFFFFFF);
        cs.switchCheckedTrack   = Rgba(t.lightSwitchOn);
        cs.switchUncheckedTrack = Rgba(0xDCE1E5); // Crisp opaque neutral gray like glass.png!
        cs.segmentedTrack       = Rgba(0xD7DCE0); // Crisp opaque segmented slot like glass.png!
        cs.secondaryContainer   = Rgba(t.lightSecCont);
        cs.onSecondaryContainer = Rgba(t.lightOnSecCont);
        cs.onSurface            = Rgba(0x161D20);
        cs.onSurfaceVariant     = Rgba(0x5F6B70);
        cs.controlTrack         = Rgba(0x161D20, 0.09f);
        cs.rowHighlight         = Rgba(0x161D20, 0.048f);
        cs.divider              = Rgba(0x161D20, 0.075f);
        cs.outline              = Rgba(0x161D20, 0.12f);
        cs.error                = Rgba(0xC62828);
    } else {
        cs.groupedBackground    = Rgba(t.darkBg);
        cs.groupedContainer     = Rgba(t.darkCard);
        cs.surfaceElevated      = Rgba(0x3A464C); // Elevated bright pill in dark segmented control
        cs.primary              = Rgba(t.darkPrimary);
        cs.onPrimary            = Rgba(0x062930);
        cs.switchCheckedTrack   = Rgba(t.darkSwitchOn); // Rich saturated dark-mode switch track!
        cs.switchUncheckedTrack = Rgba(0x323B40);       // Distinct dark-gray OFF track
        cs.segmentedTrack       = Rgba(0x232B30);       // Distinct recessed capsule track
        cs.secondaryContainer   = Rgba(t.darkSecCont);
        cs.onSecondaryContainer = Rgba(t.darkOnSecCont);
        cs.onSurface            = Rgba(0xE8EFF2);
        cs.onSurfaceVariant     = Rgba(0x8F9EA5);
        cs.controlTrack         = Rgba(0xE8EFF2, 0.12f);
        cs.rowHighlight         = Rgba(0xE8EFF2, 0.06f);
        cs.divider              = Rgba(0xE8EFF2, 0.08f);
        cs.outline              = Rgba(0xE8EFF2, 0.14f);
        cs.error                = Rgba(0xFF6B6B);
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
    r.switchCheckedTrack   = LerpColor(a.switchCheckedTrack, b.switchCheckedTrack, t);
    r.switchUncheckedTrack = LerpColor(a.switchUncheckedTrack, b.switchUncheckedTrack, t);
    r.segmentedTrack       = LerpColor(a.segmentedTrack, b.segmentedTrack, t);
    r.secondaryContainer   = LerpColor(a.secondaryContainer, b.secondaryContainer, t);
    r.onSecondaryContainer = LerpColor(a.onSecondaryContainer, b.onSecondaryContainer, t);
    r.onSurface            = LerpColor(a.onSurface, b.onSurface, t);
    r.onSurfaceVariant     = LerpColor(a.onSurfaceVariant, b.onSurfaceVariant, t);
    r.controlTrack         = LerpColor(a.controlTrack, b.controlTrack, t);
    r.rowHighlight         = LerpColor(a.rowHighlight, b.rowHighlight, t);
    r.divider              = LerpColor(a.divider, b.divider, t);
    r.outline              = LerpColor(a.outline, b.outline, t);
    r.error                = LerpColor(a.error, b.error, t);
    r.pieTop               = LerpColor(a.pieTop, b.pieTop, t);
    r.pieBottomLeft        = LerpColor(a.pieBottomLeft, b.pieBottomLeft, t);
    r.pieBottomRight       = LerpColor(a.pieBottomRight, b.pieBottomRight, t);
    return r;
}

// ============================================================================
// MochiSprings & FollowSpring (240Hz substep integration, interruptible)
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

    bool IsSettled(float posEps = 0.0006f, float velEps = 0.006f) const {
        return std::fabs(value - target) <= posEps && std::fabs(velocity) <= velEps;
    }

    bool Step(float dt) {
        if (dt <= 0.0f) return !IsSettled();
        dt = std::min(dt, 0.05f);

        const float omega = 2.0f * kPi / std::max(0.04f, response);
        const float k = omega * omega;
        const float c = 2.0f * dampingRatio * omega;

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

// iOS UIScrollView rubberBand formula
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

inline float ConcentricInner(float outerRadius, float padding, float minRadius = 4.0f) {
    if (outerRadius <= 0.0f) return 0.0f;
    return std::max(minRadius, outerRadius - padding);
}

inline float PressGlowFalloff(float dist, float radius) {
    if (radius <= 1.0f) return 0.0f;
    float r = std::clamp(dist / radius, 0.0f, 2.0f);
    return 0.85f * std::exp(-r * r * r * 1.6f);
}

} // namespace mochi
