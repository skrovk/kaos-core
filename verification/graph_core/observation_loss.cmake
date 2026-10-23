add_test(NAME graph_observation_loss COMMAND "${CORE_UV}"
    --directory "${CMAKE_CURRENT_LIST_DIR}/../../../sim_env" run --offline python
    "${CMAKE_CURRENT_LIST_DIR}/tests/observation_loss_test.py"
    $<TARGET_FILE:kaos_host> "${CMAKE_CURRENT_BINARY_DIR}/feedback.wasm"
    "${CMAKE_CURRENT_BINARY_DIR}/observation-loss")
set_tests_properties(graph_observation_loss PROPERTIES TIMEOUT 30)
