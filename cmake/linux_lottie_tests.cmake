# The Lottie geometry of the Linux build: the in-house engine that stands in for Direct2D (cmake/linux_render.cmake),
# checked against what Direct2D answered on Windows (tests/render/effects/golden). Neither needs a GPU.
maicord_test(MaiCordLottieGeometryTests ENTRY maicord_lottie_geometry_entry
    SOURCES tests/render/effects/lottie_geometry_test.cpp
    LIBS maicord_vector_geometry maicord_core
    TIMEOUT 20 TEST maicord_lottie_geometry)

# The Lottie compiler end to end: every document of tests/render/effects/corpus at a few frames, its masks digested and
# compared with the ones Direct2D's geometry made of the same files.
maicord_test(MaiCordLottieProgramTests ENTRY maicord_lottie_program_entry
    SOURCES tests/render/effects/lottie_program_test.cpp
    LIBS maicord_paint_program maicord_vector_geometry maicord_render_core maicord_font maicord_media_core maicord_core
    TIMEOUT 20 TEST maicord_lottie_program)
