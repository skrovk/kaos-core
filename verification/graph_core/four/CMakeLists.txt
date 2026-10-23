cmake_minimum_required(VERSION 3.18)
project(kaos_four_node_qualification C)
set(CORE_FOUR_NODE_PROFILE ON CACHE BOOL "Four-node qualification profile" FORCE)
set(CORE_BUILD_HOST ON CACHE BOOL "Build graph host and WAMR" FORCE)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../../graph_core" core)
option(CORE_SANITIZE "Address/undefined sanitizers" ON)
if(CORE_SANITIZE)
    foreach(t kaos_graph_core kaos_wamr kaos_wamr_engine)
        target_compile_options(${t} PUBLIC -fsanitize=address,undefined
            -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        target_link_options(${t} PUBLIC -fsanitize=address,undefined)
    endforeach()
endif()
set(guest "${CMAKE_CURRENT_BINARY_DIR}/capacity.wasm")
add_custom_command(OUTPUT "${guest}"
    COMMAND /opt/wasi-sdk/bin/clang --target=wasm32 -O1 -nostdlib
        -Wl,--no-entry -Wl,--export=kaos_configure -Wl,--export=kaos_main
        -Wl,--export=__heap_base -Wl,--export=__data_end -Wl,--allow-undefined
        -Wl,--initial-memory=65536 -Wl,--max-memory=65536 -Wl,-z,stack-size=4096
        "${CMAKE_CURRENT_LIST_DIR}/../guests/capacity.c" -o "${guest}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../guests/capacity.c" VERBATIM)
add_custom_target(capacity_guest DEPENDS "${guest}")
add_executable(graph_p4_capacity "${CMAKE_CURRENT_LIST_DIR}/../p4_capacity.c")
target_link_libraries(graph_p4_capacity PRIVATE kaos_graph_core kaos_wamr)
target_compile_options(graph_p4_capacity PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_dependencies(graph_p4_capacity capacity_guest)
include(CTest)
include("${CMAKE_CURRENT_LIST_DIR}/../runtime_phase_barriers.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../workloads.cmake")
add_test(NAME graph_p4_capacity COMMAND graph_p4_capacity "${guest}")
set_tests_properties(graph_p4_capacity PROPERTIES TIMEOUT 60
    ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0;UBSAN_OPTIONS=halt_on_error=1")

add_executable(graph_p5_capacity "${CMAKE_CURRENT_LIST_DIR}/../p5_capacity.c")
target_link_libraries(graph_p5_capacity PRIVATE kaos_graph_core kaos_wamr)
target_compile_options(graph_p5_capacity PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_dependencies(graph_p5_capacity capacity_guest)
add_test(NAME graph_p5_capacity COMMAND graph_p5_capacity "${guest}")
set_tests_properties(graph_p5_capacity PROPERTIES TIMEOUT 60
    ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0;UBSAN_OPTIONS=halt_on_error=1")

add_executable(graph_p5_test "${CMAKE_CURRENT_LIST_DIR}/../tests/p5_test.c")
target_link_libraries(graph_p5_test PRIVATE kaos_graph_core)
target_compile_options(graph_p5_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME graph_p5 COMMAND graph_p5_test)

foreach(slice admission_reuse host_control p6 p6_admission p6_control p7 p7_control p7_lifecycle)
    add_executable(graph_${slice}_test "${CMAKE_CURRENT_LIST_DIR}/../tests/${slice}_test.c")
    target_link_libraries(graph_${slice}_test PRIVATE kaos_graph_core)
    target_compile_options(graph_${slice}_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME graph_${slice} COMMAND graph_${slice}_test)
endforeach()

add_executable(graph_p6_capacity "${CMAKE_CURRENT_LIST_DIR}/../p6_capacity.c")
target_link_libraries(graph_p6_capacity PRIVATE kaos_graph_core kaos_wamr)
target_compile_options(graph_p6_capacity PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_dependencies(graph_p6_capacity capacity_guest)
add_test(NAME graph_p6_capacity COMMAND graph_p6_capacity "${guest}")
set_tests_properties(graph_p6_capacity PROPERTIES TIMEOUT 60
    ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0;UBSAN_OPTIONS=halt_on_error=1")

add_executable(graph_p7_capacity "${CMAKE_CURRENT_LIST_DIR}/../p7_capacity.c")
target_link_libraries(graph_p7_capacity PRIVATE kaos_graph_core kaos_wamr)
target_compile_options(graph_p7_capacity PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_dependencies(graph_p7_capacity capacity_guest)
add_test(NAME graph_p7_capacity COMMAND graph_p7_capacity "${guest}")
set_tests_properties(graph_p7_capacity PROPERTIES TIMEOUT 60
    ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0;UBSAN_OPTIONS=halt_on_error=1")

include("${CMAKE_CURRENT_LIST_DIR}/../simplification.cmake")
