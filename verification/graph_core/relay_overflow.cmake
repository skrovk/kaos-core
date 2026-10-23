add_test(NAME graph_relay_overflow COMMAND "${CORE_UV}"
    --directory "${CMAKE_CURRENT_LIST_DIR}/../../../sim_env" run --offline python
    "${CMAKE_CURRENT_LIST_DIR}/tests/relay_overflow_test.py"
    $<TARGET_FILE:kaos_host> "${CMAKE_CURRENT_BINARY_DIR}/relay-overflow")
set_tests_properties(graph_relay_overflow PROPERTIES TIMEOUT 20)
