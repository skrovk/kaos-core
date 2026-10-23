# Native import qualification uses the same runtime and restricted guest flags.
foreach(variant sensor wrong_module unknown wrong_signature bad_standard)
    set(native_options "")
    if(variant STREQUAL "wrong_module")
        set(native_options "-DNATIVE_MODULE=\"wrong\"")
    elseif(variant STREQUAL "unknown")
        set(native_options "-DNATIVE_NAME=\"unknown\"")
    elseif(variant STREQUAL "wrong_signature")
        set(native_options "-DNATIVE_BAD_SIGNATURE=1")
    elseif(variant STREQUAL "bad_standard")
        set(native_options "-DNATIVE_MODULE=\"kaos\"" "-DNATIVE_NAME=\"init\"")
    endif()
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_${variant}.wasm"
        COMMAND "${CORE_WASI_CLANG}" --target=wasm32 -O1 -nostdlib
            -Wall -Wextra -Werror ${native_options}
            -Wl,--no-entry -Wl,--export=kaos_configure -Wl,--export=kaos_main
            -Wl,--allow-undefined -Wl,--initial-memory=65536 -Wl,--max-memory=65536
            -Wl,-z,stack-size=4096 "${CMAKE_CURRENT_LIST_DIR}/guests/native_sensor.c"
            -o "${CMAKE_CURRENT_BINARY_DIR}/native_${variant}.wasm"
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/guests/native_sensor.c" VERBATIM)
endforeach()
add_custom_target(graph_native_guests ALL DEPENDS
    "${CMAKE_CURRENT_BINARY_DIR}/native_sensor.wasm"
    "${CMAKE_CURRENT_BINARY_DIR}/native_wrong_module.wasm"
    "${CMAKE_CURRENT_BINARY_DIR}/native_unknown.wasm"
    "${CMAKE_CURRENT_BINARY_DIR}/native_wrong_signature.wasm"
    "${CMAKE_CURRENT_BINARY_DIR}/native_bad_standard.wasm")
add_executable(graph_native_runtime_test "${CMAKE_CURRENT_LIST_DIR}/tests/native_runtime_test.c")
target_link_libraries(graph_native_runtime_test PRIVATE kaos_wamr)
target_compile_options(graph_native_runtime_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_dependencies(graph_native_runtime_test graph_native_guests)
add_test(NAME graph_native_runtime COMMAND graph_native_runtime_test
    "${CMAKE_CURRENT_BINARY_DIR}/native_sensor.wasm"
    "${CMAKE_CURRENT_BINARY_DIR}/native_wrong_module.wasm"
    "${CMAKE_CURRENT_BINARY_DIR}/native_unknown.wasm"
    "${CMAKE_CURRENT_BINARY_DIR}/native_wrong_signature.wasm"
    "${CMAKE_CURRENT_BINARY_DIR}/native_bad_standard.wasm")
set_tests_properties(graph_native_runtime PROPERTIES TIMEOUT 30
    ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0;UBSAN_OPTIONS=halt_on_error=1")
add_executable(graph_native_removal_test "${CMAKE_CURRENT_LIST_DIR}/tests/native_removal_test.c")
target_link_libraries(graph_native_removal_test PRIVATE kaos_graph_core kaos_wamr)
target_compile_options(graph_native_removal_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_dependencies(graph_native_removal_test graph_native_guests)
add_test(NAME graph_native_removal COMMAND graph_native_removal_test
    "${CMAKE_CURRENT_BINARY_DIR}/native_sensor.wasm")
set_tests_properties(graph_native_removal PROPERTIES TIMEOUT 30
    ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0;UBSAN_OPTIONS=halt_on_error=1")
add_executable(graph_native_request_test "${CMAKE_CURRENT_LIST_DIR}/tests/native_request_test.c")
target_link_libraries(graph_native_request_test PRIVATE kaos_graph_core)
target_compile_options(graph_native_request_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME graph_native_request COMMAND graph_native_request_test)
find_program(CORE_UV uv REQUIRED)
add_test(NAME graph_native_protocol COMMAND "${CORE_UV}"
    --directory "${CMAKE_CURRENT_LIST_DIR}/../../../sim_env" run --offline python
    -m unittest discover -s tests -p test_graph_native_protocol.py -v)
add_test(NAME graph_native_process COMMAND "${CORE_UV}"
    --directory "${CMAKE_CURRENT_LIST_DIR}/../../../sim_env" run --offline python
    "${CMAKE_CURRENT_LIST_DIR}/tests/native_process_test.py"
    $<TARGET_FILE:kaos_host> "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(graph_native_process PROPERTIES TIMEOUT 60)
