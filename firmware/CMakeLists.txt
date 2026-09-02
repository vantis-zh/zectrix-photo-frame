# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "6.5.9")

# Local project components under main/components
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/main/components")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-include${CMAKE_CURRENT_LIST_DIR}/compat/cxx_math_compat.h>")

# 本板无摄像头。esp_video(本地 components/ 下 vendored)会被自动构建，但其
# esp_cam_sensor 依赖与 IDF 6.1-rc1 的 SPI slave API 不兼容，直接编译失败。
# main 源码零引用 esp_video/esp_cam_sensor/usb_host_uvc，整个相机集群可安全排除。
# 若以后要接 USB 摄像头(UVC)，删掉此行即可恢复。
set(EXCLUDE_COMPONENTS "espressif__esp_video;espressif__esp_cam_sensor;espressif__esp_sccb_intf;espressif__usb_host_uvc" CACHE STRING "" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi)
