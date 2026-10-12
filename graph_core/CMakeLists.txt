cmake_minimum_required(VERSION 3.18)

option(CORE_FOUR_NODE_PROFILE "Qualify four nodes and eight incident channels per node" OFF)
if(CORE_FOUR_NODE_PROFILE)
    add_compile_definitions(CORE_FOUR_NODE_PROFILE=1)
endif()

# Host: cmake -S graph_core -B /tmp/kaos-graph-build
# Tests and qualification builds live in ../verification/graph_core.
# ESP-IDF: add this directory to EXTRA_COMPONENT_DIRS. This isolated component
# has no dependency on the legacy kaos_core component or simulator patch.
if(ESP_PLATFORM)
    idf_component_register(SRCS "kaos_graph.c" "kaos_predicate.c" "kaos_admission.c" "kaos_request.c"
        "kaos_report.c" "kaos_coap.c" "kaos_artifact.c" "kaos_control.c" "kaos_node.c"
        "kaos_endpoint.c" "kaos_edge.c"
        "ports/clock_espidf.c"
        INCLUDE_DIRS "include" PRIV_REQUIRES esp_timer)
    target_compile_features(${COMPONENT_LIB} PUBLIC c_std_11)
    if(CORE_FOUR_NODE_PROFILE)
        target_compile_definitions(${COMPONENT_LIB} PUBLIC CORE_FOUR_NODE_PROFILE=1)
    endif()
    return()
endif()

project(kaos_graph_core C)
add_library(kaos_graph_core STATIC kaos_graph.c kaos_predicate.c kaos_admission.c kaos_request.c
    kaos_report.c kaos_coap.c kaos_artifact.c kaos_control.c kaos_node.c
    kaos_endpoint.c kaos_edge.c
    ports/clock_posix.c)
target_include_directories(kaos_graph_core PUBLIC include)
target_compile_features(kaos_graph_core PUBLIC c_std_11)
if(CORE_FOUR_NODE_PROFILE)
    target_compile_definitions(kaos_graph_core PUBLIC CORE_FOUR_NODE_PROFILE=1)
endif()
set_target_properties(kaos_graph_core PROPERTIES C_EXTENSIONS OFF)
target_compile_options(kaos_graph_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

option(CORE_BUILD_P2_HOST "Build the UDP admission/artifact exercise port" OFF)
if(CORE_BUILD_P2_HOST)
    add_executable(kaos_p2_host ports/p2_host.c)
    target_link_libraries(kaos_p2_host PRIVATE kaos_graph_core)
    target_compile_options(kaos_p2_host PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

option(CORE_BUILD_P3_HOST "Build the no-edge WAMR execution port" OFF)
option(CORE_BUILD_P4_HOST "Build the WAMR endpoint and edge UDP port" OFF)
if(CORE_BUILD_P3_HOST OR CORE_BUILD_P4_HOST)
    add_subdirectory(runtime)
    add_executable(kaos_p3_host ports/p2_host.c)
    target_compile_definitions(kaos_p3_host PRIVATE CORE_HOST_RUNTIME=1)
    target_link_libraries(kaos_p3_host PRIVATE kaos_graph_core kaos_wamr)
    target_compile_options(kaos_p3_host PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(CORE_BUILD_P4_HOST)
    add_executable(kaos_p4_host ports/p2_host.c)
    target_compile_definitions(kaos_p4_host PRIVATE CORE_HOST_RUNTIME=1 CORE_HOST_ENDPOINTS=1)
    target_link_libraries(kaos_p4_host PRIVATE kaos_graph_core kaos_wamr)
    target_compile_options(kaos_p4_host PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
