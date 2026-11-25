#pragma once
#include <windows.h>
#include <wincodec.h>
#include <cstdint>
#include <vector>

namespace mochi {

// Spawns an independent topmost floating Pin-to-Screen window ("贴图") at (screenX, screenY).
// Supports:
//   - Drag anywhere to move
//   - Mouse wheel to scale (25% - 400%)
//   - Ctrl + Mouse wheel to adjust opacity (20% - 100%)
//   - Right-click menu: Copy, Save PNG/JPG, Reset 100%, Close
//   - Double-click or Esc to close (frees its bitmap memory immediately on close)
bool CreatePinWindow(
    HINSTANCE hInstance,
    IWICImagingFactory* wic,
    const uint8_t* bgra,
    int width,
    int height,
    int screenX,
    int screenY
);

int GetActivePinWindowCount();
void CloseAllPinWindows();

} // namespace mochi
