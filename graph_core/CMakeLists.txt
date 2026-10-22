cmake_minimum_required(VERSION 3.18)

option(CORE_BUILD_HOST "Build the complete shared graph host" OFF)
option(CORE_FOUR_NODE_PROFILE "Qualify four nodes and eight incident channels per node" OFF)
if(CORE_FOUR_NODE_PROFILE)
    add_compile_definitions(CORE_FOUR_NODE_PROFILE=1)
endif()

# Host: cmake -S graph_core -B /tmp/kaos-graph-build
# Tests and qualification builds live in ../verification/graph_core.
# ESP-IDF: add this directory to EXTRA_COMPONENT_DIRS. This isolated component
# has no dependency on the legacy kaos_core component or simulator patch.
if(ESP_PLATFORM)
    set(host_sources)
    if(CORE_BUILD_HOST)
        list(APPEND host_sources kaos_host.c host_udp.c)
    endif()
    idf_component_register(SRCS "kaos_graph.c" "kaos_predicate.c" "kaos_admission.c" "kaos_request.c"
        "kaos_report.c" "kaos_coap.c" "kaos_artifact.c" "kaos_control.c" "kaos_node.c"
        "kaos_endpoint.c" "kaos_edge.c"
        "ports/espidf/clock.c" ${host_sources}
        INCLUDE_DIRS "include" PRIV_REQUIRES esp_timer pthread lwip vfs)
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
    ports/posix/clock.c)
target_include_directories(kaos_graph_core PUBLIC include)
target_compile_features(kaos_graph_core PUBLIC c_std_11)
if(CORE_FOUR_NODE_PROFILE)
    target_compile_definitions(kaos_graph_core PUBLIC CORE_FOUR_NODE_PROFILE=1)
endif()
set_target_properties(kaos_graph_core PROPERTIES C_EXTENSIONS OFF)
target_compile_options(kaos_graph_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

# Historical names remain build entrypoints while callers migrate. P2 is a
# verification-only artifact fixture; every runtime host uses the complete loop.
option(CORE_BUILD_P2_HOST "Build the historical artifact-only verification fixture" OFF)
option(CORE_BUILD_P3_HOST "Build the legacy-named complete runtime host" OFF)
option(CORE_BUILD_P4_HOST "Build the legacy-named complete graph host" OFF)
if(CORE_BUILD_P2_HOST)
    add_executable(kaos_p2_host ../verification/graph_core/p2_fixture_host.c)
    target_link_libraries(kaos_p2_host PRIVATE kaos_graph_core)
    target_compile_options(kaos_p2_host PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
if(CORE_BUILD_HOST OR CORE_BUILD_P3_HOST OR CORE_BUILD_P4_HOST)
    add_subdirectory(runtime)
    add_library(kaos_graph_host STATIC kaos_host.c host_udp.c)
    target_include_directories(kaos_graph_host PUBLIC include)
    target_link_libraries(kaos_graph_host PUBLIC kaos_graph_core kaos_wamr)
    target_compile_options(kaos_graph_host PRIVATE -Wall -Wextra -Wpedantic -Werror)
    set(host_targets kaos_host)
    if(CORE_BUILD_P3_HOST OR CORE_BUILD_P4_HOST)
        list(APPEND host_targets kaos_p3_host)
    endif()
    if(CORE_BUILD_P4_HOST)
        list(APPEND host_targets kaos_p4_host)
    endif()
    foreach(host_target IN LISTS host_targets)
        add_executable(${host_target} ports/posix/main.c)
        target_link_libraries(${host_target} PRIVATE kaos_graph_host)
        target_compile_options(${host_target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endforeach()
endif()
