# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.1.65")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Metalio-E-Ink4)

# Override esp-sr packed model with custom wakeword when present
if(EXISTS "${CMAKE_SOURCE_DIR}/wakeword/srmodels.bin")
    if(TARGET srmodels_bin)
        add_custom_target(custom_wakeword_srmodels ALL
            COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_SOURCE_DIR}/wakeword/srmodels.bin"
                "${CMAKE_BINARY_DIR}/srmodels/srmodels.bin"
            COMMENT "Override srmodels.bin with custom wakeword"
        )
        add_dependencies(custom_wakeword_srmodels srmodels_bin)
        if(TARGET flash)
            add_dependencies(flash custom_wakeword_srmodels)
        endif()
        message(STATUS "Custom wakeword enabled: wakeword/srmodels.bin -> build/srmodels/srmodels.bin")
    else()
        message(WARNING "wakeword/srmodels.bin exists but srmodels_bin target not found; custom wakeword override skipped")
    endif()
endif()


# LVGL 渲染是界面响应的大头：单独用 -O2 编（全局是 -Os），换页/开面板更快
if(TARGET __idf_lvgl__lvgl)
    target_compile_options(__idf_lvgl__lvgl PRIVATE -O2)
    message(STATUS "lvgl compiled with -O2")
endif()
