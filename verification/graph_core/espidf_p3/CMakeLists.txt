cmake_minimum_required(VERSION 3.18)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../graph_core"
    "${CMAKE_CURRENT_LIST_DIR}/../../../graph_core/runtime")
set(COMPONENTS main)
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kaos_p3_qualification)
