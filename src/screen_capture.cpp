#include "screen_capture.h"
#include <dwmapi.h>
#include <commdlg.h>
#include <wrl/client.h>
#include <algorithm>
#include <ctime>
#include <cstring>

namespace mochi {

using Microsoft::WRL::ComPtr;

bool CaptureVirtualDesktop(CapturedImage& outImg) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) return false;

    HDC screenDc = GetDC(nullptr);
    if (!screenDc) return false;

    HDC memDc = CreateCompatibleDC(screenDc);
    if (!memDc) {
        ReleaseDC(nullptr, screenDc);
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = vw;
    bmi.bmiHeader.biHeight = -vh; // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        return false;
    }

    HGDIOBJ oldBmp = SelectObject(memDc, dib);
    BOOL bltOk = BitBlt(memDc, 0, 0, vw, vh, screenDc, vx, vy, SRCCOPY | CAPTUREBLT);
    GdiFlush();

    if (bltOk) {
        outImg.originX = vx;
        outImg.originY = vy;
        outImg.width = vw;
        outImg.height = vh;
        const size_t totalBytes = static_cast<size_t>(vw) * static_cast<size_t>(vh) * 4;
        outImg.bgra.resize(totalBytes);
        std::memcpy(outImg.bgra.data(), bits, totalBytes);
        // Force alpha channel = 255 so layered windows never punch transparent holes
        uint8_t* p = outImg.bgra.data();
        for (size_t i = 3; i < totalBytes; i += 4) {
            p[i] = 255;
        }
    }

    SelectObject(memDc, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
    return bltOk != FALSE && outImg.IsValid();
}

namespace {
struct EnumWinCtx {
    HWND exclude = nullptr;
    RECT virtualBounds{};
    std::vector<RECT> rects;
};

BOOL CALLBACK EnumWinProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<EnumWinCtx*>(lParam);
    if (hwnd == ctx->exclude) return TRUE;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TRANSPARENT) return TRUE;

    // Skip cloaked UWP / background desktop windows on Windows 10/11
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        return TRUE;
    }

    RECT r{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) {
        if (!GetWindowRect(hwnd, &r)) return TRUE;
    }

    RECT clipped{};
    if (!IntersectRect(&clipped, &r, &ctx->virtualBounds)) return TRUE;
    if ((clipped.right - clipped.left) < 16 || (clipped.bottom - clipped.top) < 16) return TRUE;

    ctx->rects.push_back(clipped);
    return TRUE;
}
} // namespace

std::vector<RECT> EnumerateVisibleWindowRects(HWND excludeHwnd) {
    EnumWinCtx ctx{};
    ctx.exclude = excludeHwnd;
    ctx.virtualBounds.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    ctx.virtualBounds.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    ctx.virtualBounds.right = ctx.virtualBounds.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    ctx.virtualBounds.bottom = ctx.virtualBounds.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    EnumWindows(EnumWinProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.rects;
}

void GenerateMosaicBuffer(
    const uint8_t* srcBgra,
    int width,
    int height,
    int blockSize,
    std::vector<uint8_t>& outBgra
) {
    if (!srcBgra || width <= 0 || height <= 0) return;
    blockSize = std::clamp(blockSize, 2, 128);
    const size_t total = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    outBgra.resize(total);

    for (int by = 0; by < height; by += blockSize) {
        const int yEnd = std::min(by + blockSize, height);
        for (int bx = 0; bx < width; bx += blockSize) {
            const int xEnd = std::min(bx + blockSize, width);
            uint64_t sumB = 0, sumG = 0, sumR = 0;
            const uint64_t count = static_cast<uint64_t>(xEnd - bx) * static_cast<uint64_t>(yEnd - by);

            for (int y = by; y < yEnd; ++y) {
                const uint8_t* row = srcBgra + (static_cast<size_t>(y) * width + bx) * 4;
                for (int x = bx; x < xEnd; ++x) {
                    sumB += row[0];
                    sumG += row[1];
                    sumR += row[2];
                    row += 4;
                }
            }

            const uint8_t avgB = static_cast<uint8_t>((sumB + count / 2) / count);
            const uint8_t avgG = static_cast<uint8_t>((sumG + count / 2) / count);
            const uint8_t avgR = static_cast<uint8_t>((sumR + count / 2) / count);
            const uint32_t packed = (0xFFu << 24) | (static_cast<uint32_t>(avgR) << 16) |
                                    (static_cast<uint32_t>(avgG) << 8) | static_cast<uint32_t>(avgB);

            for (int y = by; y < yEnd; ++y) {
                uint32_t* dstRow = reinterpret_cast<uint32_t*>(outBgra.data() + (static_cast<size_t>(y) * width + bx) * 4);
                std::fill(dstRow, dstRow + (xEnd - bx), packed);
            }
        }
    }
}

namespace {
void BoxBlurHorizontal(const uint8_t* src, uint8_t* dst, int w, int h, int r) {
    const float inv = 1.0f / static_cast<float>(2 * r + 1);
    for (int y = 0; y < h; ++y) {
        const uint8_t* sRow = src + static_cast<size_t>(y) * w * 4;
        uint8_t* dRow = dst + static_cast<size_t>(y) * w * 4;
        int sumB = 0, sumG = 0, sumR = 0;
        for (int ix = -r; ix <= r; ++ix) {
            int cx = std::clamp(ix, 0, w - 1);
            sumB += sRow[cx * 4 + 0];
            sumG += sRow[cx * 4 + 1];
            sumR += sRow[cx * 4 + 2];
        }
        for (int x = 0; x < w; ++x) {
            dRow[x * 4 + 0] = static_cast<uint8_t>(sumB * inv + 0.5f);
            dRow[x * 4 + 1] = static_cast<uint8_t>(sumG * inv + 0.5f);
            dRow[x * 4 + 2] = static_cast<uint8_t>(sumR * inv + 0.5f);
            dRow[x * 4 + 3] = 255;

            int xOut = std::clamp(x - r, 0, w - 1);
            int xIn  = std::clamp(x + r + 1, 0, w - 1);
            sumB += sRow[xIn * 4 + 0] - sRow[xOut * 4 + 0];
            sumG += sRow[xIn * 4 + 1] - sRow[xOut * 4 + 1];
            sumR += sRow[xIn * 4 + 2] - sRow[xOut * 4 + 2];
        }
    }
}

void BoxBlurVertical(const uint8_t* src, uint8_t* dst, int w, int h, int r) {
    const float inv = 1.0f / static_cast<float>(2 * r + 1);
    const size_t stride = static_cast<size_t>(w) * 4;
    for (int x = 0; x < w; ++x) {
        int sumB = 0, sumG = 0, sumR = 0;
        for (int iy = -r; iy <= r; ++iy) {
            int cy = std::clamp(iy, 0, h - 1);
            const uint8_t* p = src + static_cast<size_t>(cy) * stride + x * 4;
            sumB += p[0];
            sumG += p[1];
            sumR += p[2];
        }
        for (int y = 0; y < h; ++y) {
            uint8_t* d = dst + static_cast<size_t>(y) * stride + x * 4;
            d[0] = static_cast<uint8_t>(sumB * inv + 0.5f);
            d[1] = static_cast<uint8_t>(sumG * inv + 0.5f);
            d[2] = static_cast<uint8_t>(sumR * inv + 0.5f);
            d[3] = 255;

            int yOut = std::clamp(y - r, 0, h - 1);
            int yIn  = std::clamp(y + r + 1, 0, h - 1);
            const uint8_t* pOut = src + static_cast<size_t>(yOut) * stride + x * 4;
            const uint8_t* pIn  = src + static_cast<size_t>(yIn) * stride + x * 4;
            sumB += pIn[0] - pOut[0];
            sumG += pIn[1] - pOut[1];
            sumR += pIn[2] - pOut[2];
        }
    }
}
} // namespace

void GenerateBlurredBuffer(
    const uint8_t* srcBgra,
    int width,
    int height,
    int radius,
    std::vector<uint8_t>& outBgra
) {
    if (!srcBgra || width <= 0 || height <= 0) return;
    radius = std::clamp(radius, 1, 64);
    const size_t total = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    outBgra.resize(total);
    std::vector<uint8_t> tmp(total);

    // 2 separable passes approximate smooth Gaussian cleanly in O(W*H) independent of radius
    BoxBlurHorizontal(srcBgra, tmp.data(), width, height, radius);
    BoxBlurVertical(tmp.data(), outBgra.data(), width, height, radius);
    BoxBlurHorizontal(outBgra.data(), tmp.data(), width, height, std::max(1, radius / 2));
    BoxBlurVertical(tmp.data(), outBgra.data(), width, height, std::max(1, radius / 2));
}

bool CopyBgraToClipboard(HWND ownerHwnd, const uint8_t* bgra, int width, int height) {
    if (!bgra || width <= 0 || height <= 0) return false;

    const size_t rowBytes = static_cast<size_t>(width) * 4;
    const size_t imgBytes = rowBytes * static_cast<size_t>(height);
    const size_t totalSize = sizeof(BITMAPINFOHEADER) + imgBytes;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, totalSize);
    if (!hMem) return false;

    void* ptr = GlobalLock(hMem);
    if (!ptr) {
        GlobalFree(hMem);
        return false;
    }

    auto* bih = static_cast<BITMAPINFOHEADER*>(ptr);
    std::memset(bih, 0, sizeof(BITMAPINFOHEADER));
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = width;
    bih->biHeight = height; // Bottom-up DIB for maximum Win32 clipboard compatibility
    bih->biPlanes = 1;
    bih->biBitCount = 32;
    bih->biCompression = BI_RGB;
    bih->biSizeImage = static_cast<DWORD>(imgBytes);

    uint8_t* dst = static_cast<uint8_t*>(ptr) + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRow = bgra + static_cast<size_t>(height - 1 - y) * rowBytes;
        std::memcpy(dst + static_cast<size_t>(y) * rowBytes, srcRow, rowBytes);
    }
    GlobalUnlock(hMem);

    if (!OpenClipboard(ownerHwnd)) {
        GlobalFree(hMem);
        return false;
    }
    EmptyClipboard();
    HANDLE res = SetClipboardData(CF_DIB, hMem);
    CloseClipboard();

    if (!res) {
        GlobalFree(hMem);
        return false;
    }
    return true;
}

bool SaveBgraToFile(
    IWICImagingFactory* wic,
    const std::wstring& filePath,
    const uint8_t* bgra,
    int width,
    int height,
    bool asJpeg,
    float jpegQuality
) {
    if (!wic || filePath.empty() || !bgra || width <= 0 || height <= 0) return false;

    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(filePath.c_str(), GENERIC_WRITE))) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    const GUID fmt = asJpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng;
    if (FAILED(wic->CreateEncoder(fmt, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) return false;

    if (asJpeg && props) {
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT varValue;
        VariantInit(&varValue);
        varValue.vt = VT_R4;
        varValue.fltVal = std::clamp(jpegQuality, 0.1f, 1.0f);
        props->Write(1, &option, &varValue);
    }

    if (FAILED(frame->Initialize(props.Get()))) return false;
    if (FAILED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)))) return false;

    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&pixelFormat))) return false;

    ComPtr<IWICBitmap> srcBitmap;
    if (FAILED(wic->CreateBitmapFromMemory(
            static_cast<UINT>(width),
            static_cast<UINT>(height),
            GUID_WICPixelFormat32bppBGRA,
            static_cast<UINT>(width * 4),
            static_cast<UINT>(static_cast<size_t>(width) * static_cast<size_t>(height) * 4),
            const_cast<BYTE*>(bgra),
            &srcBitmap))) {
        return false;
    }

    if (FAILED(frame->WriteSource(srcBitmap.Get(), nullptr))) return false;
    if (FAILED(frame->Commit())) return false;
    if (FAILED(encoder->Commit())) return false;
    return true;
}

std::wstring PromptSaveScreenshotPath(HWND ownerHwnd, int defaultFormatIndex, bool& outIsJpeg) {
    std::time_t now = std::time(nullptr);
    std::tm tmLocal{};
    localtime_s(&tmLocal, &now);

    wchar_t fileBuf[MAX_PATH]{};
    const bool defJpg = (defaultFormatIndex == 1);
    swprintf_s(
        fileBuf,
        L"MochiShot_%04d%02d%02d_%02d%02d%02d.%s",
        tmLocal.tm_year + 1900,
        tmLocal.tm_mon + 1,
        tmLocal.tm_mday,
        tmLocal.tm_hour,
        tmLocal.tm_min,
        tmLocal.tm_sec,
        defJpg ? L"jpg" : L"png"
    );

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = ownerHwnd;
    ofn.lpstrFilter = L"PNG 无损图像 (*.png)\0*.png\0JPEG 图像 (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0";
    ofn.nFilterIndex = defJpg ? 2 : 1;
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = defJpg ? L"jpg" : L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameW(&ofn)) {
        return L"";
    }

    std::wstring res = fileBuf;
    outIsJpeg = (ofn.nFilterIndex == 2);
    if (res.size() >= 4) {
        std::wstring ext = res.substr(res.size() - 4);
        for (auto& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
        if (ext == L".jpg" || ext == L"jpeg") outIsJpeg = true;
        else if (ext == L".png") outIsJpeg = false;
    }
    return res;
}

} // namespace mochi
