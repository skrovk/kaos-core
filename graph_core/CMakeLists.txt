cmake_minimum_required(VERSION 3.18)

# Host: cmake -S graph_core -B /tmp/kaos-graph-build
# ESP-IDF: add this directory to EXTRA_COMPONENT_DIRS. This isolated component
# has no dependency on the legacy kaos_core component or simulator patch.
if(ESP_PLATFORM)
    idf_component_register(SRCS "kaos_graph.c" "ports/clock_espidf.c"
        INCLUDE_DIRS "include" PRIV_REQUIRES esp_timer)
    target_compile_features(${COMPONENT_LIB} PUBLIC c_std_11)
    return()
endif()

project(kaos_graph_core C)
add_library(kaos_graph_core STATIC kaos_graph.c ports/clock_posix.c)
target_include_directories(kaos_graph_core PUBLIC include)
target_compile_features(kaos_graph_core PUBLIC c_std_11)
set_target_properties(kaos_graph_core PROPERTIES C_EXTENSIONS OFF)
target_compile_options(kaos_graph_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

option(GO_SANITIZE "Run the shared core and tests with ASan and UBSan" OFF)
if(GO_SANITIZE)
    target_compile_options(kaos_graph_core PUBLIC
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(kaos_graph_core PUBLIC -fsanitize=address,undefined)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(graph_core_test tests/core_test.c)
    target_link_libraries(graph_core_test PRIVATE kaos_graph_core)
    target_compile_options(graph_core_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME graph_core_foundations COMMAND graph_core_test)
endif()
