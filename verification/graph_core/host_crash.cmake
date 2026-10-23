add_test(NAME graph_host_crash COMMAND "${CORE_UV}"
    --directory "${CMAKE_CURRENT_LIST_DIR}/../../../sim_env" run --offline python
    "${CMAKE_CURRENT_LIST_DIR}/tests/host_crash_test.py"
    $<TARGET_FILE:kaos_host> "${CMAKE_CURRENT_BINARY_DIR}/alternatives.wasm"
    "${CMAKE_CURRENT_BINARY_DIR}/host-crash")
set_tests_properties(graph_host_crash PROPERTIES TIMEOUT 30)
