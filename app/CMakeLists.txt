cmake_minimum_required(VERSION 3.21)

project(TextureGeneratorQt VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Concurrent OpenGL OpenGLWidgets)

qt_standard_project_setup()

qt_add_executable(TextureGeneratorQt
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/TextureGenerator.h
    src/TextureGenerator.cpp
    src/LightingRemoval.h
    src/LightingRemoval.cpp
    src/DepthEstimator.h
    src/DepthEstimator.cpp
    src/OnnxRuntime.h
    src/BlenderBridge.h
    src/BlenderBridge.cpp
    src/UpscaleEngine.h
    src/UpscaleEngine.cpp
    src/TiledPreviewWidget.h
    src/TiledPreviewWidget.cpp
    src/MaterialPreviewWidget.h
    src/MaterialPreviewWidget.cpp
    src/ZoomPanImageWidget.h
    src/ZoomPanImageWidget.cpp
)

target_link_libraries(TextureGeneratorQt
    PRIVATE
        Qt6::Widgets
        Qt6::Network
        Qt6::Concurrent
        Qt6::OpenGL
        Qt6::OpenGLWidgets
)

target_include_directories(TextureGeneratorQt
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/include
)

target_compile_definitions(TextureGeneratorQt
    PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
)

# ONNX Runtime is dlopen'd at run time rather than linked, so it just has to sit
# next to the executable. The model is copied too, so a dev build behaves like
# the portable package without any extra setup.
set(TG_ORT_DLL "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/bin/onnxruntime.dll")
set(TG_DEPTH_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/models/depth_anything_v2_small_fp16.onnx")
if (WIN32 AND EXISTS "${TG_ORT_DLL}")
    add_custom_command(TARGET TextureGeneratorQt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TG_ORT_DLL}" "$<TARGET_FILE_DIR:TextureGeneratorQt>"
        COMMENT "Copying onnxruntime.dll"
        VERBATIM
    )
endif()
if (EXISTS "${TG_DEPTH_MODEL}")
    add_custom_command(TARGET TextureGeneratorQt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TextureGeneratorQt>/models"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TG_DEPTH_MODEL}" "$<TARGET_FILE_DIR:TextureGeneratorQt>/models/"
        COMMENT "Copying the depth model"
        VERBATIM
    )
endif()

if (WIN32)
    get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT_BIN_DIR}")

    if (WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET TextureGeneratorQt POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --dir "$<TARGET_FILE_DIR:TextureGeneratorQt>"
                --no-system-d3d-compiler
                "$<TARGET_FILE:TextureGeneratorQt>"
            COMMENT "Deploying Qt runtime dependencies"
            VERBATIM
        )
    endif()
endif()

# Sanity target: renders a synthetic material ball and reports mean brightness
# so the GL preview path can be verified without real maps.
qt_add_executable(tg_preview_sanity
    src/MaterialPreviewWidget.h
    src/MaterialPreviewWidget.cpp
    sanity_preview_main.cpp
)

target_include_directories(tg_preview_sanity PRIVATE src)

target_link_libraries(tg_preview_sanity
    PRIVATE
        Qt6::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
)

target_compile_definitions(tg_preview_sanity
    PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
)

qt_add_executable(tg_pitch_test
    src/MaterialPreviewWidget.h
    src/MaterialPreviewWidget.cpp
    pitch_test_main.cpp
)

target_include_directories(tg_pitch_test PRIVATE src)

target_link_libraries(tg_pitch_test
    PRIVATE
        Qt6::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
)

target_compile_definitions(tg_pitch_test
    PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
)

qt_add_executable(tg_seam_check
    src/TextureGenerator.h
    src/TextureGenerator.cpp
    src/LightingRemoval.h
    src/LightingRemoval.cpp
    src/DepthEstimator.h
    src/DepthEstimator.cpp
    src/OnnxRuntime.h
    seam_check_main.cpp
)

target_include_directories(tg_seam_check
    PRIVATE
        src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/include
)

target_link_libraries(tg_seam_check
    PRIVATE
        Qt6::Core
        Qt6::Gui
)

target_compile_definitions(tg_seam_check
    PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
)

qt_add_executable(tg_orientation_test
    src/MaterialPreviewWidget.h
    src/MaterialPreviewWidget.cpp
    orientation_test_main.cpp
)

target_include_directories(tg_orientation_test PRIVATE src)

target_link_libraries(tg_orientation_test
    PRIVATE
        Qt6::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
)

target_compile_definitions(tg_orientation_test
    PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
)

qt_add_executable(tg_style_check
    src/TextureGenerator.h
    src/TextureGenerator.cpp
    src/LightingRemoval.h
    src/LightingRemoval.cpp
    src/DepthEstimator.h
    src/DepthEstimator.cpp
    src/OnnxRuntime.h
    style_check_main.cpp
)
target_include_directories(tg_style_check
    PRIVATE
        src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/include
)

target_link_libraries(tg_style_check
    PRIVATE
        Qt6::Core
        Qt6::Gui
)

target_compile_definitions(tg_style_check
    PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
)

qt_add_executable(tg_height_precision
    src/TextureGenerator.h
    src/TextureGenerator.cpp
    src/LightingRemoval.h
    src/LightingRemoval.cpp
    src/DepthEstimator.h
    src/DepthEstimator.cpp
    src/OnnxRuntime.h
    height_precision_main.cpp
)
target_include_directories(tg_height_precision
    PRIVATE
        src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/include
)

target_link_libraries(tg_height_precision
    PRIVATE
        Qt6::Core
        Qt6::Gui
)

target_compile_definitions(tg_height_precision
    PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
)

qt_add_executable(tg_tiled_test
    src/TiledPreviewWidget.h
    src/TiledPreviewWidget.cpp
    tiled_preview_test_main.cpp
)

target_include_directories(tg_tiled_test PRIVATE src)

target_link_libraries(tg_tiled_test
    PRIVATE
        Qt6::Widgets
)

target_compile_definitions(tg_tiled_test
    PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
)

# Rotation sweep: drives the material ball through a full rotation + pitch
# extremes and checks no angle drops to the background (backface-culling hole).
qt_add_executable(tg_rotate_sweep
    src/MaterialPreviewWidget.h
    src/MaterialPreviewWidget.cpp
    sanity_rotate_main.cpp
)

target_include_directories(tg_rotate_sweep PRIVATE src)

target_link_libraries(tg_rotate_sweep
    PRIVATE
        Qt6::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
)

target_compile_definitions(tg_rotate_sweep
    PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
)

# Lighting removal: synthetic albedo * shading scene with a known answer, so
# the classical de-lighting pass is measured rather than eyeballed. Also gates
# the "strength 0 is a byte-exact passthrough" contract and seamless tiling.
qt_add_executable(tg_delight_check
    src/LightingRemoval.h
    src/LightingRemoval.cpp
    delight_check_main.cpp
)

target_include_directories(tg_delight_check PRIVATE src)

target_link_libraries(tg_delight_check
    PRIVATE
        Qt6::Core
        Qt6::Gui
)

target_compile_definitions(tg_delight_check
    PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
)

# End-to-end: de-lighting and the neural depth path as TextureGenerator wires
# them, including the "options off means byte-identical to the old pipeline"
# regression gate.
qt_add_executable(tg_neural_check
    src/TextureGenerator.h
    src/TextureGenerator.cpp
    src/LightingRemoval.h
    src/LightingRemoval.cpp
    src/DepthEstimator.h
    src/DepthEstimator.cpp
    src/OnnxRuntime.h
    neural_check_main.cpp
)

target_include_directories(tg_neural_check
    PRIVATE
        src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/include
)

target_link_libraries(tg_neural_check
    PRIVATE
        Qt6::Core
        Qt6::Gui
)

target_compile_definitions(tg_neural_check
    PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
)

# Monocular depth: the height source for normals/AO when the Neural backend is
# selected. Skips itself cleanly when the model or the ONNX Runtime DLL is not
# installed, so the target stays green on a bare checkout.
qt_add_executable(tg_depth_check
    src/DepthEstimator.h
    src/DepthEstimator.cpp
    src/OnnxRuntime.h
    depth_check_main.cpp
)

target_include_directories(tg_depth_check
    PRIVATE
        src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/include
)

target_link_libraries(tg_depth_check
    PRIVATE
        Qt6::Core
        Qt6::Gui
)

target_compile_definitions(tg_depth_check
    PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
)

# AI super-resolution: verifies the bundled Real-ESRGAN link. Runs the real
# model when realesrgan-ncnn-vulkan is present (tools/ or TG_REALESRGAN_BIN),
# otherwise confirms the "binary missing" detection path.
qt_add_executable(tg_upscale_test
    src/TextureGenerator.h
    src/TextureGenerator.cpp
    src/LightingRemoval.h
    src/LightingRemoval.cpp
    src/DepthEstimator.h
    src/DepthEstimator.cpp
    src/OnnxRuntime.h
    src/UpscaleEngine.h
    src/UpscaleEngine.cpp
    upscale_test_main.cpp
)

target_include_directories(tg_upscale_test
    PRIVATE
        src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/include
)

target_link_libraries(tg_upscale_test
    PRIVATE
        Qt6::Core
        Qt6::Gui
)

target_compile_definitions(tg_upscale_test
    PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
)

# Drives the real MainWindow, so it pulls in the whole application apart from
# main.cpp. Worth the build cost: dropping a file is wiring, and wiring is what
# compiles cleanly while being broken.
qt_add_executable(tg_drop_test
    src/MainWindow.h
    src/MainWindow.cpp
    src/TextureGenerator.h
    src/TextureGenerator.cpp
    src/LightingRemoval.h
    src/LightingRemoval.cpp
    src/DepthEstimator.h
    src/DepthEstimator.cpp
    src/OnnxRuntime.h
    src/BlenderBridge.h
    src/BlenderBridge.cpp
    src/UpscaleEngine.h
    src/UpscaleEngine.cpp
    src/TiledPreviewWidget.h
    src/TiledPreviewWidget.cpp
    src/MaterialPreviewWidget.h
    src/MaterialPreviewWidget.cpp
    src/ZoomPanImageWidget.h
    src/ZoomPanImageWidget.cpp
    drop_test_main.cpp
)

target_include_directories(tg_drop_test
    PRIVATE
        src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/include
)

target_link_libraries(tg_drop_test
    PRIVATE
        Qt6::Widgets
        Qt6::Network
        Qt6::Concurrent
        Qt6::OpenGL
        Qt6::OpenGLWidgets
)

target_compile_definitions(tg_drop_test
    PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
)
