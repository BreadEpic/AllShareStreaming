# ============================================================================
# mw-scaler-bench — a Windows/D3D11 bench of downscale shaders.
#
# Answers one question for the native host: which resampling filter should the
# conversion pass use when the stream is smaller than the screen? It runs every
# candidate (bilinear, bicubic, Lanczos, Catmull-Rom, FSR1, NIS, SGSR1, the
# driver's ID3D11VideoProcessor) on every GPU of the machine, times the shader
# pass alone with GPU timestamps, scores the picture against a CPU reference,
# and writes a self-contained interactive HTML report.
#
# Deliberately NOT linked to mw-native-host: the tool vendors SDK shaders
# (FidelityFX, NIS, SGSR) that have no business inside the library's licence
# boundary, and it is never installed nor shipped — it is a lab instrument,
# built on demand with -DMW_BUILD_TOOLS=ON.
# ============================================================================

add_executable(mw-scaler-bench
    src/main.cpp
    src/Adapters.cpp
    src/Image.cpp
    src/Reference.cpp
    src/Metrics.cpp
    src/Scalers.cpp
    src/Pass.cpp
    src/Timing.cpp
    src/Report.cpp
)

set_target_properties(mw-scaler-bench PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_compile_features(mw-scaler-bench PRIVATE cxx_std_17)
target_compile_definitions(mw-scaler-bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_include_directories(mw-scaler-bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# The FSR constant setup (FsrEasuCon / FsrRcasCon) is meant to run on the CPU
# too: the same header, with A_CPU defined, computes the constant buffer.
target_include_directories(mw-scaler-bench SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(mw-scaler-bench PRIVATE d3d11 dxgi d3dcompiler windowscodecs ole32)

if(MSVC)
    target_compile_options(mw-scaler-bench PRIVATE /W4 /utf-8)
endif()

# Shaders and the report template are read at run time from next to the
# executable, so a filter can be edited and re-run without a rebuild. The
# vendored SDK headers go along because the wrapper shaders #include them.
add_custom_command(TARGET mw-scaler-bench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders
            $<TARGET_FILE_DIR:mw-scaler-bench>/scaler-bench/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party
            $<TARGET_FILE_DIR:mw-scaler-bench>/scaler-bench/shaders/third_party
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/report.html
            $<TARGET_FILE_DIR:mw-scaler-bench>/scaler-bench/report.html
    COMMENT "Staging mw-scaler-bench shaders and report template"
    VERBATIM
)
