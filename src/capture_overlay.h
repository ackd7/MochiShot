#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include <functional>
#include "liquid_glass.h"
#include "screen_capture.h"
#include "config.h"
#include "vector_icons.h"

namespace mochi {

enum class EditTool : int {
    Move        = 0,
    Pen         = 1,
    Highlighter = 2,
    Rect        = 3,
    Ellipse     = 4,
    Arrow       = 5,
    StepCounter = 6,
    Text        = 7,
    Mosaic      = 8,
    Blur        = 9
};

struct AnnotationItem {
    EditTool type = EditTool::Pen;
    uint32_t colorHex = 0xFF3B30;
    float strokeWidth = 4.0f;         // In physical pixels
    int mosaicBlockPx = 14;
    bool isBoxRegion = false;         // True when Mosaic/Blur dragged with Shift or Rect/Ellipse/Arrow
    D2D1_POINT_2F startPt{0, 0};      // Virtual-canvas local coords (0..captureW, 0..captureH)
    D2D1_POINT_2F endPt{0, 0};
    std::vector<D2D1_POINT_2F> points;// Freehand stroke points (Pen, Highlighter, Mosaic brush, Blur brush)
    std::wstring text;                // For Text tool
    int stepIndex = 1;                // For StepCounter (1, 2, 3...)
};

class CaptureOverlay {
public:
    CaptureOverlay(HINSTANCE hInstance, LiquidGlassEngine* engine, AppConfig* config);
    ~CaptureOverlay();

    bool StartCaptureSession();
    bool StartInstantFullscreenCopy();
    bool IsActive() const { return hwnd_ != nullptr && IsWindow(hwnd_); }
    void CloseSession();

    // Headless automated test helper: renders synthetic annotations onto an image buffer
    bool SelfTestRenderPipeline();

private:
    enum class Stage {
        Selecting,
        DraggingSelection,
        Editing
    };

    enum class DragMode {
        None,
        MoveSelection,
        ResizeNW, ResizeN, ResizeNE, ResizeE,
        ResizeSE, ResizeS, ResizeSW, ResizeW,
        DrawingAnnotation,
        DraggingPropertySlider,
        DraggingTextItem
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void RenderFrame();
    void DrawAnnotations(
        ID2D1RenderTarget* rt,
        ID2D1Bitmap* mosaicBmp,
        ID2D1Bitmap* blurBmp,
        float offsetX,
        float offsetY
    );
    void DrawSingleAnnotation(
        ID2D1RenderTarget* rt,
        const AnnotationItem& item,
        ID2D1Brush* mosaicBrush,
        ID2D1Brush* blurBrush,
        float offsetX,
        float offsetY,
        bool showCaret = false
    );
    void DrawFloatingToolbars();
    void DrawMagnifierLoupe();

    void RebuildFilterBitmapsIfNeeded();
    bool RenderCroppedResult(std::vector<uint8_t>& outBgra, int& outW, int& outH);

    void CommitActiveText();
    void PerformUndo();
    void PerformRedo();
    void PerformClearAnnotations();
    void PerformCopyAndFinish();
    void PerformSaveToFile();
    void PerformPinToScreen();

    float GetMonitorDpiScaleAt(float localX, float localY) const;
    RECT GetMonitorLocalRectAt(float localX, float localY) const;
    D2D1_RECT_F NormalizedSelection() const;
    DragMode HitTestSelectionHandles(float x, float y, float dpi) const;
    void UpdateCursorForPos(float x, float y);
    void UpdateImmCaretPosition();

    struct ToolbarBtn {
        int id = 0;
        MochiIcon icon = MochiIcon::Pen;
        const wchar_t* tooltip = L"";
        D2D1_RECT_F rect{};
        bool isTool = false;
        EditTool tool = EditTool::Move;
        bool isPrimaryAccent = false;
        bool enabled = true;
    };

    HINSTANCE hInstance_ = nullptr;
    LiquidGlassEngine* engine_ = nullptr;
    AppConfig* config_ = nullptr;
    HWND hwnd_ = nullptr;

    WindowSwapChain swapChain_;
    CapturedImage captured_;
    std::vector<RECT> windowSnapRects_; // In local overlay coords [0..W, 0..H]

    ComPtr<ID2D1Bitmap> desktopBmp_;
    ComPtr<ID2D1Bitmap> mosaicBmp_;
    ComPtr<ID2D1Bitmap> blurBmp_;
    int cachedMosaicBlockPx_ = 0;
    int cachedBlurRadiusPx_ = 0;
    RECT cachedFilterSelRect_{};

    Stage stage_ = Stage::Selecting;
    DragMode dragMode_ = DragMode::None;
    EditTool activeTool_ = EditTool::Move;

    D2D1_POINT_2F mousePos_{0, 0};
    D2D1_POINT_2F dragStartMouse_{0, 0};
    D2D1_RECT_F selection_{0, 0, 0, 0};
    D2D1_RECT_F dragStartSelection_{0, 0, 0, 0};
    RECT hoveredWinRect_{0, 0, 0, 0};
    bool hasHoveredWin_ = false;

    // Annotations & Undo/Redo stacks
    std::vector<AnnotationItem> annotations_;
    std::vector<AnnotationItem> redoStack_;
    AnnotationItem activeDraft_;
    bool hasActiveDraft_ = false;
    bool editingText_ = false;
    DWORD caretBlinkTick_ = 0;
    int nextStepNumber_ = 1;

    // Style state
    uint32_t currentColorHex_ = 0xFF3B30;
    float currentStrokeDp_ = 4.0f;
    float currentMosaicDp_ = 14.0f;

    // Toolbar geometry & Mochi animations
    std::vector<ToolbarBtn> toolbarBtns_;
    D2D1_RECT_F mainBarRect_{0, 0, 0, 0};
    D2D1_RECT_F propBarRect_{0, 0, 0, 0};
    D2D1_RECT_F sliderTrackRect_{0, 0, 0, 0};
    std::vector<std::pair<uint32_t, D2D1_RECT_F>> colorSwatchRects_;
    int hoveredBtnId_ = -1;
    int pressedBtnId_ = -1;

    MochiSpring toolPillXSpring_{0.0f, 0.16f, 1.0f};
    MochiSpring pressScaleSpring_{1.0f, 0.16f, 0.92f};
    MochiSpring glowAlphaSpring_{0.0f, 0.22f, 0.92f};
    D2D1_POINT_2F pressGlowPos_{0, 0};
    bool pillInitialized_ = false;
    LARGE_INTEGER lastQpc_{};
    LARGE_INTEGER qpcFreq_{};
};

} // namespace mochi
