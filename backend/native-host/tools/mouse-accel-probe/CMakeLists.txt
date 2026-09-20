# ============================================================================
# mouse-accel-probe — what curve does this Mac apply to this mouse?
#
# MacPointerAccel has to reproduce Apple's pointer acceleration, because
# CGEventPost bypasses the HID driver that would otherwise apply it and no API
# runs the real table on our behalf. The shape it uses takes three numbers, and
# this tool measures them from real mouse events rather than leaving them to
# taste: every event carries both the mouse's own counts and the points the
# driver turned them into, so the gain is there to be read.
#
# A lab instrument like the scaler bench: listen-only, never installed, never
# linked into mw-native-host, built on demand with -DMW_BUILD_TOOLS=ON.
# ============================================================================

add_executable(mouse-accel-probe src/main.mm)

set_target_properties(mouse-accel-probe PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_compile_features(mouse-accel-probe PRIVATE cxx_std_17)
target_link_libraries(mouse-accel-probe PRIVATE
    "-framework ApplicationServices"
    "-framework CoreFoundation"
    "-framework IOKit")
target_compile_options(mouse-accel-probe PRIVATE -Wall -Wextra)
