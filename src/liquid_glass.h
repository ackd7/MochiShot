#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>
#include <functional>
#include "mochi_theme.h"

namespace mochi {

using Microsoft::WRL::ComPtr;

struct GlassSurfaceSpec {
    D2D1_RECT_F rect;                 // Glass bounds in physical pixels
    float cornerRadiusPx = 24.0f;     // G2 corner radius in physical pixels
    float dpiScale = 1.0f;            // Window DPI / 96.0f
    bool isDark = false;
    bool thick = false;               // Regular (bars) vs Thick (menus/popovers)
    GlassMode mode = GlassMode::LiquidGlass;
    D2D1_COLOR_F tintColor = D2D1::ColorF(1, 1, 1, 0.0f); // Optional surface tint
    D2D1_COLOR_F solidFallback = D2D1::ColorF(1, 1, 1, 0.94f);
    float shadowOpacityScale = 1.0f;
    // Optional pointer press glow (in physical pixels)
    bool hasPressGlow = false;
    D2D1_POINT_2F glowCenterPx = {0.0f, 0.0f};
    float glowRadiusPx = 80.0f;
    float glowIntensity = 0.0f;
};

struct WindowSwapChain {
    HWND hwnd = nullptr;
    UINT width = 0;
    UINT height = 0;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID3D11Texture2D> backBuffer;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID2D1Bitmap1> d2dTarget;

    // Mipmapped backdrop copy for HLSL Liquid Glass sampling
    UINT backdropW = 0;
    UINT backdropH = 0;
    ComPtr<ID3D11Texture2D> backdropTex;
    ComPtr<ID3D11ShaderResourceView> backdropSrv;

    void Reset() {
        d2dTarget.Reset();
        rtv.Reset();
        backBuffer.Reset();
        backdropSrv.Reset();
        backdropTex.Reset();
        swapChain.Reset();
        width = height = backdropW = backdropH = 0;
    }
};

class LiquidGlassEngine {
public:
    LiquidGlassEngine() = default;
    ~LiquidGlassEngine() = default;

    bool Initialize();
    void SuspendGpu();
    void Shutdown();
    void TrimIdleMemory();
    void SetIdleCheckCallback(std::function<void()> cb) { onIdleCheck_ = std::move(cb); }
    void NotifyWindowClosed() { if (onIdleCheck_) onIdleCheck_(); }

    IWICImagingFactory* EnsureWIC();

    ID2D1Factory1* D2DFactory() const { return d2dFactory_.Get(); }
    ID2D1DeviceContext* D2D() const { return d2dContext_.Get(); }
    IDWriteFactory* DWrite() const { return dwriteFactory_.Get(); }
    IWICImagingFactory* WIC() const { return wicFactory_.Get(); }
    ID3D11Device* D3DDevice() const { return d3dDevice_.Get(); }
    bool SupportsShaderGlass() const { return shaderReady_; }

    bool EnsureWindowSwapChain(HWND hwnd, UINT width, UINT height, WindowSwapChain& outChain);
    bool BeginFrame(WindowSwapChain& chain, const D2D1_COLOR_F& clearColor);
    // Snapshots current D2D backbuffer into mipmapped backdrop texture so multiple glass
    // capsules on the same layer sample the clean background without sampling each other
    bool CaptureBackdrop(WindowSwapChain& chain);
    // Draws a Mochi Liquid Glass G2 surface (or Frosted / Solid fallback)
    void DrawGlassSurface(WindowSwapChain& chain, const GlassSurfaceSpec& spec);
    bool EndFrame(WindowSwapChain& chain, UINT syncInterval = 1);

    // Helper text format creation
    ComPtr<IDWriteTextFormat> CreateTextFormat(
        float fontSizePx,
        DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
        const wchar_t* fontFamily = L"Segoe UI Variable Text"
    );

    ComPtr<ID2D1StrokeStyle> RoundStrokeStyle() const { return roundStrokeStyle_; }

private:
    bool CompileGlassShaders();
    bool EnsureBackdropTexture(WindowSwapChain& chain);

    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<ID3D11DeviceContext> d3dContext_;
    ComPtr<IDXGIDevice1> dxgiDevice_;
    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> d2dContext_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<IWICImagingFactory> wicFactory_;
    ComPtr<ID2D1StrokeStyle> roundStrokeStyle_;

    // HLSL Liquid Glass pipeline
    bool shaderReady_ = false;
    ComPtr<ID3D11VertexShader> glassVS_;
    ComPtr<ID3D11PixelShader> glassPS_;
    ComPtr<ID3D11Buffer> glassCB_;
    ComPtr<ID3D11SamplerState> linearClampSampler_;
    ComPtr<ID3D11BlendState> premulBlendState_;
    ComPtr<ID3D11RasterizerState> scissorRasterState_;

    // Cached compiled HLSL bytecode (~3 KB) so waking from tray never re-runs D3DCompile
    std::vector<uint8_t> cachedVsBytecode_;
    std::vector<uint8_t> cachedPsBytecode_;
    std::function<void()> onIdleCheck_;
};

} // namespace mochi
