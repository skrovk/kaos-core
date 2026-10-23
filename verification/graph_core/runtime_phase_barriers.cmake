add_executable(graph_runtime_phase_barrier_test
    "${CMAKE_CURRENT_LIST_DIR}/tests/runtime_phase_barrier_test.c")
target_link_libraries(graph_runtime_phase_barrier_test PRIVATE kaos_graph_core)
target_compile_options(graph_runtime_phase_barrier_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME graph_runtime_phase_barriers COMMAND graph_runtime_phase_barrier_test)
set_tests_properties(graph_runtime_phase_barriers PROPERTIES TIMEOUT 10
    ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
