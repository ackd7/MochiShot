cmake_minimum_required(VERSION 3.20)
project(MochiShot VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Use static MSVC runtime in Release so single .exe works on any clean Windows 10/11 PC
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(MochiShot WIN32
    src/main.cpp
    src/config.cpp
    src/mochi_g2.cpp
    src/liquid_glass.cpp
    src/screen_capture.cpp
    src/capture_overlay.cpp
    src/pin_window.cpp
    src/settings_window.cpp
    res/app.rc
    res/app.manifest
)

target_include_directories(MochiShot PRIVATE src res)

target_compile_definitions(MochiShot PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(MochiShot PRIVATE
        /utf-8
        /W4
        /MP
        $<$<CONFIG:Release>:/O2 /Oi /GL /Gy>
    )
    target_link_options(MochiShot PRIVATE
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
    )
endif()

target_link_libraries(MochiShot PRIVATE
    user32
    gdi32
    shell32
    ole32
    shlwapi
    advapi32
    dwmapi
    imm32
    psapi
    d3d11
    dxgi
    d2d1
    dwrite
    d3dcompiler
    windowscodecs
    comdlg32
)
