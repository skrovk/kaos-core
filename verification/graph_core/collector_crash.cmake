add_test(NAME graph_collector_crash COMMAND "${CORE_UV}"
    --directory "${CMAKE_CURRENT_LIST_DIR}/../../../sim_env" run --offline python
    "${CMAKE_CURRENT_LIST_DIR}/tests/collector_crash_test.py"
    $<TARGET_FILE:kaos_host> "${CMAKE_CURRENT_BINARY_DIR}/barrier_node.wasm"
    "${CMAKE_CURRENT_BINARY_DIR}/collector-crash")
set_tests_properties(graph_collector_crash PROPERTIES TIMEOUT 45)
