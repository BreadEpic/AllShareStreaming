# ============================================================================
# mw-gpu-load — a "AAA game" GPU load, to see how the encoder copes.
#
# A game that takes the whole GPU (Resident Evil 9, 21/09) fills the GPU queue,
# and the encoder of the stream waits behind it. Nothing else in the lab does
# that: the bench films a clip or a scrolling desktop, which leave the GPU idle.
# This tool spins a neon object whose cost is tuned until it renders at ~45 fps
# on the GPU it is told to use — the one MoonlightWeb encodes on — then holds
# that load.
#
# Two safeguards are not options: a run never lasts more than 60 s, and it
# stops at once when the GPU's temperature crosses the limit.
#
# One renderer for every OS through Qt RHI: D3D12 on Windows, Vulkan on Linux,
# Metal on macOS, the API a game would use there. Shaders are written once in
# Vulkan GLSL and baked by qt_add_shaders, which needs the qtshadertools module
# (`aqt install-qt … -m qtshadertools`).
#
# Built on demand with -DMW_BUILD_TOOLS=ON, never installed nor shipped. It also
# configures on its own — `cmake -S backend/native-host/tools/gpu-load` — so a
# Mac or a Linux box needs Qt and nothing of the backend's dependencies.
# ============================================================================

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.21)
    project(mw-gpu-load LANGUAGES CXX)
    if(APPLE)
        enable_language(OBJCXX)
    endif()
endif()

# QRhi lives behind GuiPrivate: the tool is tied to the Qt it was built with,
# which is fine for a lab instrument built on the machine that runs it.
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 QUIET COMPONENTS Gui Widgets ShaderTools)
# A package of its own from Qt 6.9; before, Qt6Gui brings the target along.
find_package(Qt6 QUIET OPTIONAL_COMPONENTS GuiPrivate)
if(NOT Qt6ShaderTools_FOUND OR NOT Qt6Widgets_FOUND OR NOT TARGET Qt6::GuiPrivate)
    message(STATUS "mw-gpu-load skipped: needs Qt Widgets and the qtshadertools module")
    return()
endif()

# The music — an Atari ST-style tune, also the stream's audio test — plays
# through Qt Multimedia (`aqt install-qt … -m qtmultimedia`). Without the module
# the tool still builds, silent.
find_package(Qt6 QUIET OPTIONAL_COMPONENTS Multimedia)

set(MW_GPU_LOAD_SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/GpuList.cpp
    src/RenderWindow.cpp
    src/LoadController.cpp
    src/ChipSong.cpp
)
if(Qt6Multimedia_FOUND)
    list(APPEND MW_GPU_LOAD_SOURCES src/MusicPlayer.cpp)
else()
    message(STATUS "mw-gpu-load: no Qt Multimedia, built without music")
    list(APPEND MW_GPU_LOAD_SOURCES src/MusicPlayer_none.cpp)
endif()
if(WIN32)
    list(APPEND MW_GPU_LOAD_SOURCES src/ThermalGuard_win.cpp)
elseif(APPLE)
    list(APPEND MW_GPU_LOAD_SOURCES src/ThermalGuard_mac.mm src/GpuList_mac.mm)
else()
    list(APPEND MW_GPU_LOAD_SOURCES src/ThermalGuard_linux.cpp)
endif()

qt_add_executable(mw-gpu-load ${MW_GPU_LOAD_SOURCES})
set_target_properties(mw-gpu-load PROPERTIES AUTOMOC ON AUTOUIC OFF AUTORCC OFF
                                             WIN32_EXECUTABLE ON MACOSX_BUNDLE OFF)
target_compile_features(mw-gpu-load PRIVATE cxx_std_17)
target_include_directories(mw-gpu-load PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mw-gpu-load PRIVATE Qt6::Gui Qt6::GuiPrivate Qt6::Widgets)
if(Qt6Multimedia_FOUND)
    target_link_libraries(mw-gpu-load PRIVATE Qt6::Multimedia)
endif()

if(WIN32)
    target_compile_definitions(mw-gpu-load PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(mw-gpu-load PRIVATE dxgi gdi32 user32)
elseif(APPLE)
    target_link_libraries(mw-gpu-load PRIVATE "-framework Foundation" "-framework Metal")
else()
    target_link_libraries(mw-gpu-load PRIVATE ${CMAKE_DL_LIBS})
endif()
if(MSVC)
    target_compile_options(mw-gpu-load PRIVATE /W4 /utf-8)
endif()

qt_add_shaders(mw-gpu-load "gpu_load_shaders"
    PREFIX "/"
    FILES
        shaders/sky.vert
        shaders/sky.frag
        shaders/knot.vert
        shaders/knot.frag
)
