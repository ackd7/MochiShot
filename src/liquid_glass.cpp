#include "liquid_glass.h"
#include "mochi_g2.h"
#include <d3dcompiler.h>
#include <psapi.h>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace mochi {

namespace {

// 16-byte aligned HLSL Constant Buffer matching LiquidGlassHLSL
struct alignas(16) GlassShaderCB {
    float quadRect[4];     // left, top, right, bottom (px)
    float viewSize[4];     // screenW, screenH, dpiScale, isDark (0/1)
    float shapeParams[4];  // centerX, centerY, halfW, halfH (pt)
    float cornerParams[4]; // radiusPt, extPt, superExpN, mode (0=Liquid, 1=Frosted)
    float refrParams[4];   // innerAmtPt, innerInvHPt, outerAmtPt, outerInvHPt
    float blurParams[4];   // blurBaseLod, refrOpacity, lumaComp, shadowOpacity
    float shadowParams[4]; // shadowInvRPt, shadowOffsetYPt, ringOpacity, highlightStrength
    float faceRow0[4];     // 3x4 Mochi face color matrix row 0
    float faceRow1[4];     // 3x4 Mochi face color matrix row 1
    float faceRow2[4];     // 3x4 Mochi face color matrix row 2
    float tintColor[4];    // r, g, b, a
    float glowParams[4];   // centerPtX, centerPtY, sigmaPt, intensity
};

// Authentic Mochi Liquid Glass HLSL Shader
// Ported from feice11/mochi-compose LiquidShaders.kt & LiquidRecipe.kt
static const char* kLiquidGlassHLSL = R"HLSL(
cbuffer GlassCB : register(b0) {
    float4 uQuadRect;     // left, top, right, bottom (px)
    float4 uViewSize;     // w, h, dpi, isDark
    float4 uShape;        // cx, cy, halfW, halfH (pt)
    float4 uCorner;       // rPt, extPt, expN, mode (0=Liquid, 1=Frosted)
    float4 uRefr;         // innerAmt, innerInvH, outerAmt, outerInvH
    float4 uBlur;         // baseLod, refrOpacity, lumaComp, shOpacity
    float4 uShadow;       // shInvR, shOffsetY, ringOpacity, hlStrength
    float4 uFace0;
    float4 uFace1;
    float4 uFace2;
    float4 uTint;
    float4 uGlow;         // gx, gy, sigmaPt, intensity
};

Texture2D<float4> uBackdrop : register(t0);
SamplerState uSampler : register(s0);

struct VSOut {
    float4 pos : SV_POSITION;
    float2 px  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID) {
    float2 uv = float2(vid & 1, (vid >> 1) & 1);
    float2 px = lerp(uQuadRect.xy, uQuadRect.zw, uv);
    float2 ndc = float2(
        (px.x / uViewSize.x) * 2.0 - 1.0,
        1.0 - (px.y / uViewSize.y) * 2.0
    );
    VSOut o;
    o.pos = float4(ndc, 0.0, 1.0);
    o.px = px;
    return o;
}

// Exact G2 superellipse SDF & analytical gradient
float3 g2SdfAndNormal(float2 p, float2 b, float ext, float n) {
    float2 ap = abs(p);
    float2 sgn = float2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    if (ext < 1e-3) {
        float2 q = ap - b;
        float2 m = max(q, 0.0);
        float d = length(m) + min(max(q.x, q.y), 0.0);
        float2 nrm = (dot(m, m) > 1e-6) ? normalize(m) : (q.x > q.y ? float2(1.0, 0.0) : float2(0.0, 1.0));
        return float3(d, nrm * sgn);
    }
    float2 q = ap - (b - ext);
    if (q.x <= 0.0 || q.y <= 0.0) {
        float2 d2 = ap - b;
        float d = max(d2.x, d2.y);
        float2 nrm = (d2.x > d2.y) ? float2(1.0, 0.0) : float2(0.0, 1.0);
        return float3(d, nrm * sgn);
    }
    float2 u = max(q / ext, 1e-5);
    float uxN = pow(u.x, n);
    float uyN = pow(u.y, n);
    float sumN = uxN + uyN;
    float norm = pow(sumN, 1.0 / n);
    float d = (norm - 1.0) * ext;
    float2 grad = float2(pow(u.x, n - 1.0), pow(u.y, n - 1.0));
    float gl = length(grad);
    float2 nrm = (gl > 1e-6) ? (grad / gl) : float2(0.70710678, 0.70710678);
    return float3(d, nrm * sgn);
}

float erfp(float x) {
    float s = saturate(x * 0.25 + 0.5) * 4.0 - 2.0;
    float s2 = s * s;
    return (((0.002954483 * s2 - 0.034454346) * s2 + 0.16821289) * s2 - 0.560546875) * s + 0.5;
}

float bevel(float amt, float invH, float d) {
    float t = saturate(invH * (-d));
    return amt - saturate(sqrt(max(0.0, (2.0 - t) * t))) * amt;
}

float3 applyFaceMatrix(float3 c) {
    float3 x = c;
    if (uBlur.z > 0.0) {
        float L = dot(float3(0.2126, 0.7152, 0.0722), c);
        float k = saturate(1.0 - L * uBlur.z);
        x = lerp(float3(k * L, k * L, k * L), k * c, (1.0 - k) * 0.30 + 1.0);
    }
    return float3(
        dot(x, uFace0.xyz) + uFace0.w,
        dot(x, uFace1.xyz) + uFace1.w,
        dot(x, uFace2.xyz) + uFace2.w
    );
}

float3 sampleBlur9(float2 uv, float2 texel, float lod) {
    float2 stepUv = texel * exp2(max(0.0, lod - 0.65));
    float3 c = uBackdrop.SampleLevel(uSampler, uv, lod).rgb * 0.28;
    c += uBackdrop.SampleLevel(uSampler, uv + float2( stepUv.x, 0.0), lod).rgb * 0.11;
    c += uBackdrop.SampleLevel(uSampler, uv + float2(-stepUv.x, 0.0), lod).rgb * 0.11;
    c += uBackdrop.SampleLevel(uSampler, uv + float2(0.0,  stepUv.y), lod).rgb * 0.11;
    c += uBackdrop.SampleLevel(uSampler, uv + float2(0.0, -stepUv.y), lod).rgb * 0.11;
    c += uBackdrop.SampleLevel(uSampler, uv + float2( stepUv.x,  stepUv.y), lod).rgb * 0.07;
    c += uBackdrop.SampleLevel(uSampler, uv + float2(-stepUv.x,  stepUv.y), lod).rgb * 0.07;
    c += uBackdrop.SampleLevel(uSampler, uv + float2( stepUv.x, -stepUv.y), lod).rgb * 0.07;
    c += uBackdrop.SampleLevel(uSampler, uv + float2(-stepUv.x, -stepUv.y), lod).rgb * 0.07;
    return c;
}

float4 PSMain(VSOut pin) : SV_TARGET {
    float dpi = max(uViewSize.z, 0.5);
    float2 P = pin.px / dpi;
    float2 localP = P - uShape.xy;

    float3 sn = g2SdfAndNormal(localP, uShape.zw, uCorner.y, uCorner.z);
    float d = sn.x;
    float2 n = sn.yz;

    float fw = max((abs(n.x) + abs(n.y)) / dpi, 1e-4);
    float cov = saturate(0.5 - d / fw);

    // Outer Mochi soft shadow (erfp integral from LiquidShaders.kt)
    float4 shadow = float4(0.0, 0.0, 0.0, 0.0);
    if (cov < 1.0 && uBlur.w > 0.001) {
        float shiftD = g2SdfAndNormal(localP - float2(0.0, uShadow.y), uShape.zw, uCorner.y, uCorner.z).x;
        float shAmt = uBlur.w * erfp(uShadow.x * shiftD);
        shadow = float4(0.0, 0.0, 0.0, shAmt);
    }

    if (cov <= 0.0005) {
        return shadow;
    }

    float2 invView = 1.0 / uViewSize.xy;
    float2 baseUv = pin.px * invView;
    float2 ptToUv = dpi * invView;

    float3 glassRgb;
    if (uCorner.w < 0.5) {
        // Full Mochi Liquid Glass: inner bevel lens refraction + chromatic dispersion + outer bevel
        float disp = bevel(uRefr.x, uRefr.y, d);
        float edgeFactor = saturate(1.0 - (-d) * uRefr.y);
        float lod = max(0.35, uBlur.x * (1.0 - 0.42 * edgeFactor));

        // Subtle optical RGB dispersion along curved glass bevel normal
        float2 dispUv = n * (disp * ptToUv);
        float2 chroma = n * (disp * 0.075 * ptToUv);

        float3 cCenter = sampleBlur9(clamp(baseUv + dispUv, invView * 0.5, 1.0 - invView * 0.5), invView, lod);
        float rCh = uBackdrop.SampleLevel(uSampler, clamp(baseUv + dispUv + chroma, invView * 0.5, 1.0 - invView * 0.5), lod).r;
        float bCh = uBackdrop.SampleLevel(uSampler, clamp(baseUv + dispUv - chroma, invView * 0.5, 1.0 - invView * 0.5), lod).b;
        float3 c = float3(lerp(cCenter.r, rCh, 0.42), cCenter.g, lerp(cCenter.b, bCh, 0.42));

        // Outer lip refraction near d in [-1.2, 0]
        if (d > -1.2 && uBlur.y > 0.0) {
            float disp2 = bevel(uRefr.z, uRefr.w, d);
            float3 c2 = uBackdrop.SampleLevel(uSampler, clamp(baseUv + n * (disp2 * ptToUv), invView * 0.5, 1.0 - invView * 0.5), max(0.0, lod * 0.4)).rgb;
            c = lerp(c, c2, saturate((d + 1.2) / 1.2) * uBlur.y);
        }
        glassRgb = applyFaceMatrix(c);
    } else {
        // Frosted mode: clean Gaussian/mip blur + Mochi face matrix, zero distortion
        float3 c = sampleBlur9(clamp(baseUv, invView * 0.5, 1.0 - invView * 0.5), invView, uBlur.x + 0.65);
        glassRgb = applyFaceMatrix(c);
    }

    glassRgb = saturate(glassRgb);

    // Optional tonal tint
    if (uTint.a > 0.001) {
        glassRgb = lerp(glassRgb, uTint.rgb, uTint.a);
    }

    // Inner subtle depth ring shadow near top interior
    if (uShadow.z > 0.001 && d > -18.0) {
        float shiftD = g2SdfAndNormal(localP - float2(0.0, 5.0), uShape.zw, uCorner.y, uCorner.z).x;
        float a1 = shiftD * 0.22;
        float a2 = a1 + 0.75;
        float ring = saturate(erfp(a1 * 0.7071) - erfp(a2 * 0.7071)) * uShadow.z;
        glassRgb *= (1.0 - ring);
    }

    // Mochi @2 Fresnel specular rim highlight (top-left key + bottom-right counter-fill)
    if (uShadow.w > 0.001 && d > -9.0) {
        float X = -d;
        float rim1 = exp(-max(0.0, X) * 1.35);
        float rimWide = exp(-max(0.0, X) * 0.28) * 0.22;
        float dirLight = saturate((-n.y * 0.78 - n.x * 0.42 + 0.25));
        float counterLight = saturate((n.y * 0.72 + n.x * 0.38 - 0.15)) * 0.52;
        float spec = (rim1 * (dirLight + counterLight) + rimWide * dirLight) * uShadow.w;
        float3 rimColor = (uViewSize.w > 0.5) ? float3(0.92, 0.98, 0.97) : float3(1.0, 1.0, 1.0);
        glassRgb = saturate(glassRgb + rimColor * spec);
    }

    // Interactive Mochi Press Glow (0.85 * exp(-r^3 * 1.6))
    if (uGlow.w > 0.001) {
        float distPt = length(P - uGlow.xy);
        float rn = saturate(distPt / max(uGlow.z, 1.0));
        float g = 0.85 * exp(-rn * rn * rn * 1.6) * uGlow.w;
        glassRgb = saturate(glassRgb + float3(g, g, g));
    }

    // Premultiplied alpha output: surface OVER outer shadow (zero dark edge halo!)
    float shA = shadow.a * (1.0 - cov);
    return float4(glassRgb * cov, cov + shA);
}
)HLSL";

// Mochi LiquidRecipe.kt face matrices
static constexpr float kFaceRegularLight[12] = {
    0.86328f, -0.32617f, -0.03284f, 0.52002f,
   -0.09692f,  0.63379f, -0.03296f, 0.52002f,
   -0.09698f, -0.32617f,  0.92725f, 0.52002f
};
static constexpr float kFaceRegularDark[12] = {
    1.23633f, -0.21460f, -0.02156f, 0.11500f,
   -0.06378f,  1.08594f, -0.02168f, 0.11500f,
   -0.06384f, -0.21448f,  1.27832f, 0.11500f
};

void FillThickFaceMatrix(bool dark, float out12[12]) {
    const float w[3] = {0.2126f, 0.7152f, 0.0722f};
    const float s = dark ? 1.5f : 1.8f;
    const float k = dark ? 0.30f : 0.275f;
    const float o = dark ? (30.0f / 255.0f) : 0.72f;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            out12[i * 4 + j] = k * (w[j] * (1.0f - s) + (i == j ? s : 0.0f));
        }
        out12[i * 4 + 3] = o;
    }
}

} // namespace

bool LiquidGlassEngine::Initialize() {
    if (d3dDevice_) return true;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL achieved = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels,
        ARRAYSIZE(levels),
        D3D11_SDK_VERSION,
        &d3dDevice_,
        &achieved,
        &d3dContext_
    );
    if (FAILED(hr)) {
        // Fallback to WARP software rasterizer (works in headless CI, RDP, VMs!)
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            flags,
            levels,
            ARRAYSIZE(levels),
            D3D11_SDK_VERSION,
            &d3dDevice_,
            &achieved,
            &d3dContext_
        );
        if (FAILED(hr)) return false;
    }

    if (FAILED(d3dDevice_.As(&dxgiDevice_))) return false;
    dxgiDevice_->SetMaximumFrameLatency(1);

    D2D1_FACTORY_OPTIONS fo{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, &d2dFactory_))) {
        return false;
    }
    if (FAILED(d2dFactory_->CreateDevice(dxgiDevice_.Get(), &d2dDevice_))) return false;
    if (FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_))) return false;

    D2D1_STROKE_STYLE_PROPERTIES ssp = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND,
        2.0f
    );
    d2dFactory_->CreateStrokeStyle(ssp, nullptr, 0, &roundStrokeStyle_);

    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf())))) {
        return false;
    }

    if (!EnsureWIC()) {
        return false;
    }

    shaderReady_ = CompileGlassShaders();
    return true;
}

IWICImagingFactory* LiquidGlassEngine::EnsureWIC() {
    if (!wicFactory_) {
        CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&wicFactory_)
        );
    }
    return wicFactory_.Get();
}

void LiquidGlassEngine::SuspendGpu() {
    if (d2dContext_) d2dContext_->SetTarget(nullptr);
    if (d3dContext_) {
        d3dContext_->ClearState();
        d3dContext_->Flush();
    }
    scissorRasterState_.Reset();
    premulBlendState_.Reset();
    linearClampSampler_.Reset();
    glassCB_.Reset();
    glassPS_.Reset();
    glassVS_.Reset();
    roundStrokeStyle_.Reset();
    dwriteFactory_.Reset();
    d2dContext_.Reset();
    d2dDevice_.Reset();
    d2dFactory_.Reset();
    dxgiDevice_.Reset();
    d3dContext_.Reset();
    d3dDevice_.Reset();
    shaderReady_ = false;
    EmptyWorkingSet(GetCurrentProcess());
}

void LiquidGlassEngine::Shutdown() {
    SuspendGpu();
    wicFactory_.Reset();
    cachedVsBytecode_.clear();
    cachedPsBytecode_.clear();
}

void LiquidGlassEngine::TrimIdleMemory() {
    if (d2dContext_) {
        d2dContext_->SetTarget(nullptr);
    }
    if (d3dContext_) {
        d3dContext_->ClearState();
        d3dContext_->Flush();
    }
    if (d2dDevice_) {
        d2dDevice_->ClearResources(0);
    }
    if (dxgiDevice_) {
        ComPtr<IDXGIDevice3> dxgi3;
        if (SUCCEEDED(dxgiDevice_.As(&dxgi3)) && dxgi3) {
            dxgi3->Trim();
        }
    }
    EmptyWorkingSet(GetCurrentProcess());
}

bool LiquidGlassEngine::CompileGlassShaders() {
    if (cachedVsBytecode_.empty() || cachedPsBytecode_.empty()) {
        ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
        UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;

        HRESULT hr = D3DCompile(
            kLiquidGlassHLSL,
            std::strlen(kLiquidGlassHLSL),
            "MochiLiquidGlass.hlsl",
            nullptr,
            nullptr,
            "VSMain",
            "vs_4_0",
            compileFlags,
            0,
            &vsBlob,
            &errBlob
        );
        if (FAILED(hr) || !vsBlob) return false;

        hr = D3DCompile(
            kLiquidGlassHLSL,
            std::strlen(kLiquidGlassHLSL),
            "MochiLiquidGlass.hlsl",
            nullptr,
            nullptr,
            "PSMain",
            "ps_4_0",
            compileFlags,
            0,
            &psBlob,
            &errBlob
        );
        if (FAILED(hr) || !psBlob) return false;

        const auto* vsPtr = static_cast<const uint8_t*>(vsBlob->GetBufferPointer());
        cachedVsBytecode_.assign(vsPtr, vsPtr + vsBlob->GetBufferSize());
        const auto* psPtr = static_cast<const uint8_t*>(psBlob->GetBufferPointer());
        cachedPsBytecode_.assign(psPtr, psPtr + psBlob->GetBufferSize());
    }

    if (FAILED(d3dDevice_->CreateVertexShader(cachedVsBytecode_.data(), cachedVsBytecode_.size(), nullptr, &glassVS_)))
        return false;
    if (FAILED(d3dDevice_->CreatePixelShader(cachedPsBytecode_.data(), cachedPsBytecode_.size(), nullptr, &glassPS_)))
        return false;

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(GlassShaderCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(d3dDevice_->CreateBuffer(&cbd, nullptr, &glassCB_)))
        return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(d3dDevice_->CreateSamplerState(&sd, &linearClampSampler_)))
        return false;

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(d3dDevice_->CreateBlendState(&bd, &premulBlendState_)))
        return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = TRUE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(d3dDevice_->CreateRasterizerState(&rd, &scissorRasterState_)))
        return false;

    return true;
}

bool LiquidGlassEngine::EnsureWindowSwapChain(HWND hwnd, UINT width, UINT height, WindowSwapChain& chain) {
    if (!d3dDevice_ || !hwnd) return false;
    width = std::max(width, 1u);
    height = std::max(height, 1u);

    if (chain.swapChain && chain.hwnd == hwnd && chain.width == width && chain.height == height && chain.d2dTarget) {
        return true;
    }

    if (d2dContext_) d2dContext_->SetTarget(nullptr);
    chain.d2dTarget.Reset();
    chain.rtv.Reset();
    chain.backBuffer.Reset();

    if (!chain.swapChain || chain.hwnd != hwnd) {
        chain.Reset();
        chain.hwnd = hwnd;

        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice_->GetAdapter(&adapter))) return false;
        ComPtr<IDXGIFactory2> factory2;
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory2)))) return false;

        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = width;
        scd.Height = height;
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.Scaling = DXGI_SCALING_NONE;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        HRESULT hr = factory2->CreateSwapChainForHwnd(d3dDevice_.Get(), hwnd, &scd, nullptr, nullptr, &chain.swapChain);
        if (FAILED(hr)) {
            // Fallback to DISCARD on older DXGI configurations
            scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            scd.Scaling = DXGI_SCALING_STRETCH;
            if (FAILED(factory2->CreateSwapChainForHwnd(d3dDevice_.Get(), hwnd, &scd, nullptr, nullptr, &chain.swapChain))) {
                return false;
            }
        }
        factory2->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    } else {
        if (FAILED(chain.swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) {
            chain.Reset();
            return false;
        }
    }

    chain.width = width;
    chain.height = height;

    if (FAILED(chain.swapChain->GetBuffer(0, IID_PPV_ARGS(&chain.backBuffer)))) return false;
    if (FAILED(d3dDevice_->CreateRenderTargetView(chain.backBuffer.Get(), nullptr, &chain.rtv))) return false;

    ComPtr<IDXGISurface> dxgiSurface;
    if (FAILED(chain.backBuffer.As(&dxgiSurface))) return false;

    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f,
        96.0f
    );
    if (FAILED(d2dContext_->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &bp, &chain.d2dTarget))) return false;

    return true;
}

bool LiquidGlassEngine::EnsureBackdropTexture(WindowSwapChain& chain) {
    if (chain.backdropTex && chain.backdropW == chain.width && chain.backdropH == chain.height) {
        return true;
    }
    chain.backdropSrv.Reset();
    chain.backdropTex.Reset();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = chain.width;
    td.Height = chain.height;
    td.MipLevels = 6;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    if (FAILED(d3dDevice_->CreateTexture2D(&td, nullptr, &chain.backdropTex))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = td.Format;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MostDetailedMip = 0;
    srvd.Texture2D.MipLevels = td.MipLevels;
    if (FAILED(d3dDevice_->CreateShaderResourceView(chain.backdropTex.Get(), &srvd, &chain.backdropSrv))) {
        chain.backdropTex.Reset();
        return false;
    }
    chain.backdropW = chain.width;
    chain.backdropH = chain.height;
    return true;
}

bool LiquidGlassEngine::BeginFrame(WindowSwapChain& chain, const D2D1_COLOR_F& clearColor) {
    if (!d2dContext_ || !chain.d2dTarget) return false;
    d2dContext_->SetTarget(chain.d2dTarget.Get());
    // Use raw physical pixel coordinates (96 DPI on D2D context, explicit dpiScale in layouts)
    d2dContext_->SetDpi(96.0f, 96.0f);
    d2dContext_->BeginDraw();
    d2dContext_->Clear(clearColor);
    return true;
}

bool LiquidGlassEngine::CaptureBackdrop(WindowSwapChain& chain) {
    if (!shaderReady_ || !chain.backBuffer) return false;
    if (!EnsureBackdropTexture(chain)) return false;

    d2dContext_->EndDraw();
    d3dContext_->CopySubresourceRegion(
        chain.backdropTex.Get(),
        0,
        0, 0, 0,
        chain.backBuffer.Get(),
        0,
        nullptr
    );
    d3dContext_->GenerateMips(chain.backdropSrv.Get());

    ID3D11ShaderResourceView* nullSrvs[] = { nullptr };
    d3dContext_->PSSetShaderResources(0, 1, nullSrvs);
    d3dContext_->RSSetState(nullptr);
    d3dContext_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

    d2dContext_->SetTarget(chain.d2dTarget.Get());
    d2dContext_->SetDpi(96.0f, 96.0f);
    d2dContext_->BeginDraw();
    return true;
}

void LiquidGlassEngine::DrawGlassSurface(WindowSwapChain& chain, const GlassSurfaceSpec& spec) {
    const float w = spec.rect.right - spec.rect.left;
    const float h = spec.rect.bottom - spec.rect.top;
    if (w <= 1.0f || h <= 1.0f) return;

    // Fallback path when user chose Solid mode or GPU shader unavailable
    if (spec.mode == GlassMode::Solid || !shaderReady_ || !chain.backdropSrv) {
        DrawMochiShadow(d2dContext_.Get(), d2dFactory_.Get(), spec.rect, spec.cornerRadiusPx, 14.0f, spec.dpiScale, spec.isDark, spec.shadowOpacityScale);
        ComPtr<ID2D1SolidColorBrush> fillBrush, rimBrush;
        d2dContext_->CreateSolidColorBrush(spec.solidFallback, &fillBrush);
        FillG2RoundRect(d2dContext_.Get(), d2dFactory_.Get(), spec.rect, spec.cornerRadiusPx, fillBrush.Get(), 1.0f);

        D2D1_COLOR_F borderCol = spec.isDark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.08f);
        d2dContext_->CreateSolidColorBrush(borderCol, &rimBrush);
        StrokeG2RoundRect(d2dContext_.Get(), d2dFactory_.Get(), spec.rect, spec.cornerRadiusPx, rimBrush.Get(), 1.0f * spec.dpiScale);
        return;
    }

    // Cleanly close D2D batch before mutating D3D11 pipeline state, then reopen D2D batch
    // afterward so Direct2D never uses stale scissor/shader/blend state on any GPU driver!
    d2dContext_->EndDraw();

    const float dpi = std::max(spec.dpiScale, 0.5f);
    const float wPt = w / dpi;
    const float hPt = h / dpi;
    const float halfMinPt = std::min(wPt, hPt) * 0.5f;
    const float rPt = std::clamp(spec.cornerRadiusPx / dpi, 0.0f, halfMinPt);
    const float extPt = std::min(rPt * 1.528665f, halfMinPt);
    constexpr float kSqrt2 = 1.41421356f;
    const float a = (extPt > 1e-3f) ? std::clamp(((kSqrt2 - 1.0f) * rPt) / (kSqrt2 * extPt), 1e-4f, 0.2928932f) : 0.2928932f;
    const float expN = (extPt > 1e-3f) ? (-std::log(2.0f) / std::log(1.0f - a)) : 2.0f;

    const float shortSidePt = std::min(wPt, hPt);
    const float reachPx = (28.0f * dpi);
    const float qLeft   = std::max(0.0f, std::floor(spec.rect.left - reachPx));
    const float qTop    = std::max(0.0f, std::floor(spec.rect.top - reachPx * 0.6f));
    const float qRight  = std::min(static_cast<float>(chain.width), std::ceil(spec.rect.right + reachPx));
    const float qBottom = std::min(static_cast<float>(chain.height), std::ceil(spec.rect.bottom + reachPx * 1.3f));
    if (qRight <= qLeft || qBottom <= qTop) {
        d2dContext_->BeginDraw();
        return;
    }

    GlassShaderCB cb{};
    cb.quadRect[0] = qLeft;
    cb.quadRect[1] = qTop;
    cb.quadRect[2] = qRight;
    cb.quadRect[3] = qBottom;

    cb.viewSize[0] = static_cast<float>(chain.width);
    cb.viewSize[1] = static_cast<float>(chain.height);
    cb.viewSize[2] = dpi;
    cb.viewSize[3] = spec.isDark ? 1.0f : 0.0f;

    cb.shapeParams[0] = (spec.rect.left + spec.rect.right) * 0.5f / dpi;
    cb.shapeParams[1] = (spec.rect.top + spec.rect.bottom) * 0.5f / dpi;
    cb.shapeParams[2] = wPt * 0.5f;
    cb.shapeParams[3] = hPt * 0.5f;

    cb.cornerParams[0] = rPt;
    cb.cornerParams[1] = extPt;
    cb.cornerParams[2] = expN;
    cb.cornerParams[3] = (spec.mode == GlassMode::Frosted) ? 1.0f : 0.0f;

    // LiquidRecipe.kt regularRecipe refraction parameters
    cb.refrParams[0] = -std::min(shortSidePt * 0.42f, 42.0f);           // innerAmtPt
    cb.refrParams[1] = 1.0f / std::max(4.0f, std::min(18.0f, shortSidePt * 0.30f)); // innerInvHPt
    cb.refrParams[2] = std::max(shortSidePt * 0.22f, 12.0f);            // outerAmtPt
    cb.refrParams[3] = 1.0f / std::max(shortSidePt * 0.20f, 12.0f);     // outerInvHPt

    cb.blurParams[0] = spec.thick ? 3.2f : 2.35f;                       // mip LOD
    cb.blurParams[1] = 0.52f;                                           // outer refrOpacity
    cb.blurParams[2] = spec.isDark ? 0.44f : 0.06f;                     // lumaComp
    cb.blurParams[3] = (spec.isDark ? 0.30f : 0.15f) * spec.shadowOpacityScale; // shadowOpacity

    cb.shadowParams[0] = 1.0f / 11.0f;                                  // shInvR (pt^-1)
    cb.shadowParams[1] = 6.5f;                                          // shOffsetY (pt)
    cb.shadowParams[2] = spec.isDark ? 0.09f : 0.05f;                   // ringOpacity
    cb.shadowParams[3] = spec.isDark ? 0.40f : 0.62f;                   // Fresnel rim highlight

    float face12[12]{};
    if (spec.thick) {
        FillThickFaceMatrix(spec.isDark, face12);
    } else {
        std::memcpy(face12, spec.isDark ? kFaceRegularDark : kFaceRegularLight, sizeof(face12));
    }
    std::memcpy(cb.faceRow0, &face12[0], 4 * sizeof(float));
    std::memcpy(cb.faceRow1, &face12[4], 4 * sizeof(float));
    std::memcpy(cb.faceRow2, &face12[8], 4 * sizeof(float));

    cb.tintColor[0] = spec.tintColor.r;
    cb.tintColor[1] = spec.tintColor.g;
    cb.tintColor[2] = spec.tintColor.b;
    cb.tintColor[3] = spec.tintColor.a;

    if (spec.hasPressGlow && spec.glowIntensity > 0.001f) {
        cb.glowParams[0] = spec.glowCenterPx.x / dpi;
        cb.glowParams[1] = spec.glowCenterPx.y / dpi;
        cb.glowParams[2] = std::max(20.0f, spec.glowRadiusPx / dpi);
        cb.glowParams[3] = spec.glowIntensity;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(d3dContext_->Map(glassCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, &cb, sizeof(cb));
        d3dContext_->Unmap(glassCB_.Get(), 0);
    }

    ID3D11RenderTargetView* rtvs[] = { chain.rtv.Get() };
    d3dContext_->OMSetRenderTargets(1, rtvs, nullptr);

    float blendFactor[4] = {0, 0, 0, 0};
    d3dContext_->OMSetBlendState(premulBlendState_.Get(), blendFactor, 0xFFFFFFFF);
    d3dContext_->RSSetState(scissorRasterState_.Get());

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = static_cast<float>(chain.width);
    vp.Height = static_cast<float>(chain.height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    d3dContext_->RSSetViewports(1, &vp);

    D3D11_RECT sc{};
    sc.left   = static_cast<LONG>(qLeft);
    sc.top    = static_cast<LONG>(qTop);
    sc.right  = static_cast<LONG>(qRight);
    sc.bottom = static_cast<LONG>(qBottom);
    d3dContext_->RSSetScissorRects(1, &sc);

    d3dContext_->IASetInputLayout(nullptr);
    d3dContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    d3dContext_->VSSetShader(glassVS_.Get(), nullptr, 0);
    d3dContext_->PSSetShader(glassPS_.Get(), nullptr, 0);

    ID3D11Buffer* cbs[] = { glassCB_.Get() };
    d3dContext_->VSSetConstantBuffers(0, 1, cbs);
    d3dContext_->PSSetConstantBuffers(0, 1, cbs);

    ID3D11ShaderResourceView* srvs[] = { chain.backdropSrv.Get() };
    d3dContext_->PSSetShaderResources(0, 1, srvs);
    ID3D11SamplerState* sams[] = { linearClampSampler_.Get() };
    d3dContext_->PSSetSamplers(0, 1, sams);

    d3dContext_->Draw(4, 0);

    // Unbind SRV & reset D3D11 state cleanly before resuming D2D drawing
    ID3D11ShaderResourceView* nullSrvs[] = { nullptr };
    d3dContext_->PSSetShaderResources(0, 1, nullSrvs);
    d3dContext_->RSSetState(nullptr);
    d3dContext_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

    d2dContext_->SetTarget(chain.d2dTarget.Get());
    d2dContext_->SetDpi(96.0f, 96.0f);
    d2dContext_->BeginDraw();
}

bool LiquidGlassEngine::EndFrame(WindowSwapChain& chain, UINT syncInterval) {
    if (!d2dContext_ || !chain.swapChain) return false;
    HRESULT hr = d2dContext_->EndDraw();
    d2dContext_->SetTarget(nullptr);
    if (hr == D2DERR_RECREATE_TARGET) {
        chain.Reset();
        return false;
    }
    hr = chain.swapChain->Present(syncInterval, 0);
    return SUCCEEDED(hr);
}

ComPtr<IDWriteTextFormat> LiquidGlassEngine::CreateTextFormat(
    float fontSizePx,
    DWRITE_FONT_WEIGHT weight,
    const wchar_t* fontFamily
) {
    if (!dwriteFactory_) return nullptr;
    ComPtr<IDWriteTextFormat> fmt;
    HRESULT hr = dwriteFactory_->CreateTextFormat(
        fontFamily,
        nullptr,
        weight,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        std::max(1.0f, fontSizePx),
        L"zh-CN",
        &fmt
    );
    if (FAILED(hr) || !fmt) {
        dwriteFactory_->CreateTextFormat(
            L"Microsoft YaHei UI",
            nullptr,
            weight,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            std::max(1.0f, fontSizePx),
            L"zh-CN",
            &fmt
        );
    }
    return fmt;
}

} // namespace mochi
