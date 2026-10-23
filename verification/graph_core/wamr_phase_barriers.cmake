# Wrap only this executable's KaOS-to-WAMR calls; the firmware stays unchanged.
add_executable(graph_wamr_phase_barrier_test
    "${CMAKE_CURRENT_LIST_DIR}/tests/wamr_phase_barrier_test.c")
target_link_libraries(graph_wamr_phase_barrier_test PRIVATE kaos_graph_core kaos_wamr)
target_include_directories(graph_wamr_phase_barrier_test PRIVATE
    "${CORE_WAMR_ROOT}/core/iwasm/include")
target_compile_options(graph_wamr_phase_barrier_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_options(graph_wamr_phase_barrier_test PRIVATE
    -Wl,--wrap=wasm_runtime_instantiate
    -Wl,--wrap=wasm_runtime_deinstantiate
    -Wl,--wrap=wasm_runtime_unload)
add_dependencies(graph_wamr_phase_barrier_test graph_barrier_guest)
add_test(NAME graph_wamr_phase_barriers COMMAND graph_wamr_phase_barrier_test
    "${CMAKE_CURRENT_BINARY_DIR}/barrier_node.wasm")
set_tests_properties(graph_wamr_phase_barriers PROPERTIES TIMEOUT 60
    ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0;UBSAN_OPTIONS=halt_on_error=1")
