# Shared by the default and four-node host qualification builds.
set(CORE_WASI_CLANG "/opt/wasi-sdk/bin/clang" CACHE FILEPATH "WASI clang for restricted test guests")
include("${CMAKE_CURRENT_LIST_DIR}/native.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/barriers.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/wamr_phase_barriers.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/transport_scheduling.cmake")
foreach(guest pipeline feedback alternatives mixed stalls)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${guest}.wasm"
        COMMAND "${CORE_WASI_CLANG}" --target=wasm32 -O1 -nostdlib -Wall -Wextra -Werror
            -Wl,--no-entry -Wl,--export=kaos_configure -Wl,--export=kaos_main
            -Wl,--allow-undefined -Wl,--initial-memory=65536 -Wl,--max-memory=65536
            -Wl,-z,stack-size=4096 "${CMAKE_CURRENT_LIST_DIR}/guests/${guest}.c"
            -o "${CMAKE_CURRENT_BINARY_DIR}/${guest}.wasm"
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/guests/${guest}.c"
            "${CMAKE_CURRENT_LIST_DIR}/guests/workload.h" VERBATIM)
endforeach()
add_custom_target(graph_workload_guests ALL DEPENDS
    "${CMAKE_CURRENT_BINARY_DIR}/pipeline.wasm" "${CMAKE_CURRENT_BINARY_DIR}/feedback.wasm"
    "${CMAKE_CURRENT_BINARY_DIR}/alternatives.wasm" "${CMAKE_CURRENT_BINARY_DIR}/mixed.wasm"
    "${CMAKE_CURRENT_BINARY_DIR}/stalls.wasm")
add_executable(graph_workload_stalls "${CMAKE_CURRENT_LIST_DIR}/tests/workload_stalls_test.c")
target_link_libraries(graph_workload_stalls PRIVATE kaos_graph_core kaos_wamr)
target_compile_options(graph_workload_stalls PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_dependencies(graph_workload_stalls graph_workload_guests)
add_test(NAME graph_workload_stalls COMMAND graph_workload_stalls "${CMAKE_CURRENT_BINARY_DIR}/stalls.wasm")
set_tests_properties(graph_workload_stalls PROPERTIES TIMEOUT 20
    ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0;UBSAN_OPTIONS=halt_on_error=1")
find_program(CORE_UV uv REQUIRED)
include("${CMAKE_CURRENT_LIST_DIR}/observation_loss.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/collector_crash.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/relay_overflow.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/host_crash.cmake")
add_test(NAME graph_optional_expiry COMMAND "${CORE_UV}"
    --directory "${CMAKE_CURRENT_LIST_DIR}/../../../sim_env" run --offline python
    "${CMAKE_CURRENT_LIST_DIR}/tests/optional_expiry_test.py"
    $<TARGET_FILE:kaos_host> "${CMAKE_CURRENT_BINARY_DIR}/alternatives.wasm")
set_tests_properties(graph_optional_expiry PROPERTIES TIMEOUT 30)
foreach(fixture workloads workload_timing alternatives mixed)
    add_test(NAME graph_p8_${fixture} COMMAND "${CMAKE_COMMAND}" -E env
        "KAOS_WORKLOAD_HOST=$<TARGET_FILE:kaos_host>"
        "KAOS_WORKLOAD_GUESTS=${CMAKE_CURRENT_BINARY_DIR}"
        "KAOS_WORKLOAD_FOUR=$<BOOL:${CORE_FOUR_NODE_PROFILE}>"
        "${CORE_UV}" --directory "${CMAKE_CURRENT_LIST_DIR}/../../../sim_env"
        run --offline python -m unittest discover -s tests -p test_graph_${fixture}.py -v)
    set_tests_properties(graph_p8_${fixture} PROPERTIES TIMEOUT 60)
endforeach()
