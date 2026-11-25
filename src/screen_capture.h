#pragma once
#include <windows.h>
#include <wincodec.h>
#include <vector>
#include <string>
#include <cstdint>

namespace mochi {

struct CapturedImage {
    int originX = 0; // Virtual desktop X
    int originY = 0; // Virtual desktop Y
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra; // Stride = width * 4, premultiplied/opaque alpha = 255

    bool IsValid() const {
        return width > 0 && height > 0 && bgra.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    }

    void Clear() {
        originX = originY = width = height = 0;
        bgra.clear();
        bgra.shrink_to_fit();
    }

    uint32_t SamplePixelClamped(int x, int y) const {
        if (!IsValid()) return 0;
        int cx = (x < 0) ? 0 : (x >= width ? width - 1 : x);
        int cy = (y < 0) ? 0 : (y >= height ? height - 1 : y);
        const uint8_t* p = &bgra[(static_cast<size_t>(cy) * width + cx) * 4];
        return (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[1]) << 8) |
                static_cast<uint32_t>(p[0]);
    }
};

// Captures the entire multi-monitor virtual desktop in physical pixels
bool CaptureVirtualDesktop(CapturedImage& outImg);

// Enumerates visible, non-cloaked top-level window bounds in virtual desktop coordinates
std::vector<RECT> EnumerateVisibleWindowRects(HWND excludeHwnd = nullptr);

// Generates a pixel-block mosaic BGRA buffer from a sub-rect of src
void GenerateMosaicBuffer(
    const uint8_t* srcBgra,
    int width,
    int height,
    int blockSize,
    std::vector<uint8_t>& outBgra
);

// Generates a fast 3-pass separable box-Gaussian blur BGRA buffer
void GenerateBlurredBuffer(
    const uint8_t* srcBgra,
    int width,
    int height,
    int radius,
    std::vector<uint8_t>& outBgra
);

// Places a 32-bit BGRA image onto the Windows Clipboard (CF_DIB)
bool CopyBgraToClipboard(HWND ownerHwnd, const uint8_t* bgra, int width, int height);

// Saves a 32-bit BGRA image to PNG or JPEG via WIC
bool SaveBgraToFile(
    IWICImagingFactory* wic,
    const std::wstring& filePath,
    const uint8_t* bgra,
    int width,
    int height,
    bool asJpeg = false,
    float jpegQuality = 0.92f
);

// Shows native Save File dialog and returns chosen path (empty if cancelled)
std::wstring PromptSaveScreenshotPath(HWND ownerHwnd, int defaultFormatIndex, bool& outIsJpeg);

} // namespace mochi
