cmake_minimum_required(VERSION 3.18)
set(CORE_BUILD_HOST ON CACHE BOOL "Complete shared host" FORCE)
set(CORE_FOUR_NODE_PROFILE ON CACHE BOOL "Four-node profile" FORCE)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../.."
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime"
    "$ENV{IDF_PATH}/examples/common_components/protocol_examples_common")
set(COMPONENTS main)
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kaos_graph_host)
