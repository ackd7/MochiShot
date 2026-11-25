#include "pin_window.h"
#include "screen_capture.h"
#include "config.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <vector>
#include <cstring>

namespace mochi {

namespace {

constexpr const wchar_t* kPinClassName = L"MochiShot_PinWindow";
constexpr UINT IDM_PIN_COPY  = 4001;
constexpr UINT IDM_PIN_SAVE  = 4002;
constexpr UINT IDM_PIN_RESET = 4003;
constexpr UINT IDM_PIN_CLOSE = 4004;

struct PinState {
    HWND hwnd = nullptr;
    IWICImagingFactory* wic = nullptr;
    int imgW = 0;
    int imgH = 0;
    std::vector<uint8_t> bgra;
    HBITMAP hBitmap = nullptr;
    float scale = 1.0f;
    BYTE alpha = 255;
    bool dragging = false;
    POINT dragOffset{};
};

std::vector<HWND> g_pinWindows;

void RegisterPinClass(HINSTANCE hInstance);

HBITMAP CreateDibFromBgra(const uint8_t* bgra, int w, int h) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (hbm && bits) {
        std::memcpy(bits, bgra, static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    }
    return hbm;
}

LRESULT CALLBACK PinWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<PinState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* init = static_cast<PinState*>(cs->lpCreateParams);
        init->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(init));
        return TRUE;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        if (st && st->hBitmap) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            int cw = rc.right - rc.left;
            int ch = rc.bottom - rc.top;

            HDC bufDc = CreateCompatibleDC(hdc);
            HBITMAP bufBmp = CreateCompatibleBitmap(hdc, cw, ch);
            HGDIOBJ oldBuf = SelectObject(bufDc, bufBmp);

            HDC srcDc = CreateCompatibleDC(hdc);
            HGDIOBJ oldSrc = SelectObject(srcDc, st->hBitmap);

            SetStretchBltMode(bufDc, HALFTONE);
            SetBrushOrgEx(bufDc, 0, 0, nullptr);
            StretchBlt(bufDc, 0, 0, cw, ch, srcDc, 0, 0, st->imgW, st->imgH, SRCCOPY);

            // Crisp 1px Mochi Jade accent border
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(43, 106, 90));
            HGDIOBJ oldPen = SelectObject(bufDc, pen);
            HGDIOBJ oldBrush = SelectObject(bufDc, GetStockObject(NULL_BRUSH));
            Rectangle(bufDc, 0, 0, cw, ch);
            SelectObject(bufDc, oldBrush);
            SelectObject(bufDc, oldPen);
            DeleteObject(pen);

            BitBlt(hdc, 0, 0, cw, ch, bufDc, 0, 0, SRCCOPY);

            SelectObject(srcDc, oldSrc);
            DeleteDC(srcDc);
            SelectObject(bufDc, oldBuf);
            DeleteObject(bufBmp);
            DeleteDC(bufDc);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (st) {
            SetFocus(hwnd);
            SetCapture(hwnd);
            st->dragging = true;
            POINT pt{};
            GetCursorPos(&pt);
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            st->dragOffset.x = pt.x - wr.left;
            st->dragOffset.y = pt.y - wr.top;
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (st && st->dragging) {
            POINT pt{};
            GetCursorPos(&pt);
            SetWindowPos(hwnd, HWND_TOPMOST,
                         pt.x - st->dragOffset.x,
                         pt.y - st->dragOffset.y,
                         0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (st && st->dragging) {
            st->dragging = false;
            ReleaseCapture();
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK:
        DestroyWindow(hwnd);
        return 0;

    case WM_MOUSEWHEEL: {
        if (!st) return 0;
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            int nextA = static_cast<int>(st->alpha) + (delta > 0 ? 18 : -18);
            st->alpha = static_cast<BYTE>(std::clamp(nextA, 50, 255));
            SetLayeredWindowAttributes(hwnd, 0, st->alpha, LWA_ALPHA);
        } else {
            float factor = (delta > 0) ? 1.10f : (1.0f / 1.10f);
            float nextScale = std::clamp(st->scale * factor, 0.20f, 4.0f);
            if (std::fabs(nextScale - st->scale) > 1e-3f) {
                POINT cursor{};
                GetCursorPos(&cursor);
                RECT wr{};
                GetWindowRect(hwnd, &wr);
                float relX = static_cast<float>(cursor.x - wr.left) / std::max(1L, wr.right - wr.left);
                float relY = static_cast<float>(cursor.y - wr.top) / std::max(1L, wr.bottom - wr.top);

                st->scale = nextScale;
                int nw = std::max(24, static_cast<int>(st->imgW * st->scale + 0.5f));
                int nh = std::max(24, static_cast<int>(st->imgH * st->scale + 0.5f));
                int nx = static_cast<int>(cursor.x - relX * nw + 0.5f);
                int ny = static_cast<int>(cursor.y - relY * nh + 0.5f);
                SetWindowPos(hwnd, HWND_TOPMOST, nx, ny, nw, nh, SWP_NOACTIVATE);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        if (!st) return 0;
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, IDM_PIN_COPY, L"复制图像 (Ctrl+C)");
        AppendMenuW(menu, MF_STRING, IDM_PIN_SAVE, L"另存为文件 (Ctrl+S)...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_PIN_RESET, L"重置缩放与透明度 (100%)");
        AppendMenuW(menu, MF_STRING, IDM_PIN_CLOSE, L"关闭贴图 (Esc / 双击)");
        POINT pt{};
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd);
        UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (cmd == IDM_PIN_COPY) {
            CopyBgraToClipboard(hwnd, st->bgra.data(), st->imgW, st->imgH);
        } else if (cmd == IDM_PIN_SAVE) {
            bool isJpg = false;
            std::wstring path = PromptSaveScreenshotPath(hwnd, 0, isJpg);
            if (!path.empty()) {
                SaveBgraToFile(st->wic, path, st->bgra.data(), st->imgW, st->imgH, isJpg);
            }
        } else if (cmd == IDM_PIN_RESET) {
            st->scale = 1.0f;
            st->alpha = 255;
            SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            SetWindowPos(hwnd, HWND_TOPMOST, wr.left, wr.top, st->imgW, st->imgH, SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (cmd == IDM_PIN_CLOSE) {
            DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_KEYDOWN: {
        if (!st) return 0;
        if (wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'C') {
            CopyBgraToClipboard(hwnd, st->bgra.data(), st->imgW, st->imgH);
            return 0;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'S') {
            bool isJpg = false;
            std::wstring path = PromptSaveScreenshotPath(hwnd, 0, isJpg);
            if (!path.empty()) {
                SaveBgraToFile(st->wic, path, st->bgra.data(), st->imgW, st->imgH, isJpg);
            }
            return 0;
        }
        break;
    }

    case WM_NCDESTROY: {
        g_pinWindows.erase(std::remove(g_pinWindows.begin(), g_pinWindows.end(), hwnd), g_pinWindows.end());
        if (st) {
            if (st->hBitmap) DeleteObject(st->hBitmap);
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        if (g_pinWindows.empty()) {
            TrimProcessMemory();
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void RegisterPinClass(HINSTANCE hInstance) {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = PinWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_SIZEALL);
    wc.lpszClassName = kPinClassName;
    RegisterClassExW(&wc);
    registered = true;
}

} // namespace

bool CreatePinWindow(
    HINSTANCE hInstance,
    IWICImagingFactory* wic,
    const uint8_t* bgra,
    int width,
    int height,
    int screenX,
    int screenY
) {
    if (!bgra || width <= 0 || height <= 0) return false;
    RegisterPinClass(hInstance);

    auto* st = new PinState();
    st->wic = wic;
    st->imgW = width;
    st->imgH = height;
    st->bgra.assign(bgra, bgra + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    st->hBitmap = CreateDibFromBgra(bgra, width, height);
    if (!st->hBitmap) {
        delete st;
        return false;
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        kPinClassName,
        L"MochiShot Pin",
        WS_POPUP,
        screenX, screenY, width, height,
        nullptr, nullptr, hInstance, st
    );
    if (!hwnd) {
        if (st->hBitmap) DeleteObject(st->hBitmap);
        delete st;
        return false;
    }

    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

    // Enable subtle Windows 11 DWM rounded corner & drop shadow if supported
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &cornerPref, sizeof(cornerPref));

    g_pinWindows.push_back(hwnd);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    return true;
}

int GetActivePinWindowCount() {
    return static_cast<int>(g_pinWindows.size());
}

void CloseAllPinWindows() {
    auto copy = g_pinWindows;
    for (HWND h : copy) {
        if (IsWindow(h)) DestroyWindow(h);
    }
    g_pinWindows.clear();
}

} // namespace mochi
