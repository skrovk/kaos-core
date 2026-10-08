cmake_minimum_required(VERSION 3.18)

# Host: cmake -S graph_core -B /tmp/kaos-graph-build
# Tests and qualification builds live in ../verification/graph_core.
# ESP-IDF: add this directory to EXTRA_COMPONENT_DIRS. This isolated component
# has no dependency on the legacy kaos_core component or simulator patch.
if(ESP_PLATFORM)
    idf_component_register(SRCS "kaos_graph.c" "kaos_predicate.c" "kaos_admission.c" "kaos_request.c"
        "kaos_report.c" "kaos_coap.c" "kaos_artifact.c" "kaos_control.c"
        "ports/clock_espidf.c"
        INCLUDE_DIRS "include" PRIV_REQUIRES esp_timer)
    target_compile_features(${COMPONENT_LIB} PUBLIC c_std_11)
    return()
endif()

project(kaos_graph_core C)
add_library(kaos_graph_core STATIC kaos_graph.c kaos_predicate.c kaos_admission.c kaos_request.c
    kaos_report.c kaos_coap.c kaos_artifact.c kaos_control.c
    ports/clock_posix.c)
target_include_directories(kaos_graph_core PUBLIC include)
target_compile_features(kaos_graph_core PUBLIC c_std_11)
set_target_properties(kaos_graph_core PROPERTIES C_EXTENSIONS OFF)
target_compile_options(kaos_graph_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

option(CORE_BUILD_P2_HOST "Build the UDP admission/artifact exercise port" OFF)
if(CORE_BUILD_P2_HOST)
    add_executable(kaos_p2_host ports/p2_host.c)
    target_link_libraries(kaos_p2_host PRIVATE kaos_graph_core)
    target_compile_options(kaos_p2_host PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
