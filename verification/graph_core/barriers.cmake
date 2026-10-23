# Real WAMR entry gates use an unchanged restricted guest.
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/barrier_node.wasm"
    COMMAND "${CORE_WASI_CLANG}" --target=wasm32 -O1 -nostdlib
        -Wall -Wextra -Werror -Wl,--no-entry
        -Wl,--export=kaos_configure -Wl,--export=kaos_main -Wl,--allow-undefined
        -Wl,--initial-memory=65536 -Wl,--max-memory=65536 -Wl,-z,stack-size=4096
        "${CMAKE_CURRENT_LIST_DIR}/guests/node.c"
        -o "${CMAKE_CURRENT_BINARY_DIR}/barrier_node.wasm"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/guests/node.c" VERBATIM)
add_custom_target(graph_barrier_guest DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/barrier_node.wasm")
add_executable(graph_lifecycle_barrier_test "${CMAKE_CURRENT_LIST_DIR}/tests/lifecycle_barrier_test.c")
target_link_libraries(graph_lifecycle_barrier_test PRIVATE kaos_graph_core kaos_wamr)
target_compile_options(graph_lifecycle_barrier_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_dependencies(graph_lifecycle_barrier_test graph_barrier_guest)
add_test(NAME graph_lifecycle_barriers COMMAND graph_lifecycle_barrier_test
    "${CMAKE_CURRENT_BINARY_DIR}/barrier_node.wasm")
set_tests_properties(graph_lifecycle_barriers PROPERTIES TIMEOUT 30
    ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0;UBSAN_OPTIONS=halt_on_error=1")
