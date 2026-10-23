cmake_minimum_required(VERSION 3.18)
project(kaos_graph_verification C)

# Host: cmake -S verification/graph_core -B /tmp/kaos-verification-build
# ESP32: use the espidf subdirectory as the ESP-IDF project.
# TLC: use model/Lifetime.tla with model/Lifetime.cfg.
# verification.json preserves the original run, including its original paths
# and hashes; it is historical evidence, not a manifest of this new layout.
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../graph_core" core)

option(CORE_SANITIZE "Run the shared core and tests with ASan and UBSan" OFF)
if(CORE_SANITIZE)
    target_compile_options(kaos_graph_core PUBLIC
        -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    target_link_options(kaos_graph_core PUBLIC -fsanitize=address,undefined)
    if(TARGET kaos_wamr)
        foreach(runtime_target kaos_wamr kaos_wamr_engine)
            target_compile_options(${runtime_target} PRIVATE
                -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        endforeach()
        target_link_options(kaos_wamr PUBLIC -fsanitize=address,undefined)
    endif()
endif()

include(CTest)
if(BUILD_TESTING AND CORE_BUILD_HOST)
    set(CORE_WASI_CLANG "/opt/wasi-sdk/bin/clang" CACHE FILEPATH "WASI clang for restricted test guests")
    if(NOT EXISTS "${CORE_WASI_CLANG}")
        message(FATAL_ERROR "Set CORE_WASI_CLANG to a WASI SDK clang")
    endif()
    foreach(guest runtime_guest node channel traffic)
        if(guest STREQUAL "runtime_guest")
            set(guest_source "${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_guest.c")
        else()
            set(guest_source "${CMAKE_CURRENT_SOURCE_DIR}/guests/${guest}.c")
        endif()
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${guest}.wasm"
            COMMAND "${CORE_WASI_CLANG}" --target=wasm32 -O1 -nostdlib
                -Wl,--no-entry -Wl,--export=kaos_configure -Wl,--export=kaos_main
                -Wl,--allow-undefined -Wl,--initial-memory=65536 -Wl,--max-memory=65536
                -Wl,-z,stack-size=4096 "${guest_source}"
                -o "${CMAKE_CURRENT_BINARY_DIR}/${guest}.wasm"
            DEPENDS "${guest_source}" VERBATIM)
    endforeach()
    add_custom_target(graph_runtime_guests ALL DEPENDS
        "${CMAKE_CURRENT_BINARY_DIR}/node.wasm" "${CMAKE_CURRENT_BINARY_DIR}/runtime_guest.wasm"
        "${CMAKE_CURRENT_BINARY_DIR}/channel.wasm" "${CMAKE_CURRENT_BINARY_DIR}/traffic.wasm")
    if(CORE_BUILD_HOST)
        include("${CMAKE_CURRENT_LIST_DIR}/workloads.cmake")
        find_program(CORE_UV uv REQUIRED)
        add_test(NAME graph_p4_process COMMAND "${CORE_UV}"
            --directory "${CMAKE_CURRENT_LIST_DIR}/../../../sim_env" run --offline python
            "${CMAKE_CURRENT_LIST_DIR}/tests/p4_process_test.py"
            $<TARGET_FILE:kaos_host> "${CMAKE_CURRENT_BINARY_DIR}/traffic.wasm")
        set_tests_properties(graph_p4_process PROPERTIES TIMEOUT 40)
        add_test(NAME graph_p5_process COMMAND "${CORE_UV}"
            --directory "${CMAKE_CURRENT_LIST_DIR}/../../../sim_env" run --offline python
            "${CMAKE_CURRENT_LIST_DIR}/tests/p5_process_test.py"
            $<TARGET_FILE:kaos_host> "${CMAKE_CURRENT_BINARY_DIR}/traffic.wasm")
        set_tests_properties(graph_p5_process PROPERTIES TIMEOUT 60)
        add_test(NAME graph_p6_process COMMAND "${CORE_UV}"
            --directory "${CMAKE_CURRENT_LIST_DIR}/../../../sim_env" run --offline python
            "${CMAKE_CURRENT_LIST_DIR}/tests/p6_process_test.py"
            $<TARGET_FILE:kaos_host> "${CMAKE_CURRENT_BINARY_DIR}/traffic.wasm")
        set_tests_properties(graph_p6_process PROPERTIES TIMEOUT 60)
        add_test(NAME graph_p7_process COMMAND "${CORE_UV}"
            --directory "${CMAKE_CURRENT_LIST_DIR}/../../../sim_env" run --offline python
            "${CMAKE_CURRENT_LIST_DIR}/tests/p7_process_test.py"
            $<TARGET_FILE:kaos_host> "${CMAKE_CURRENT_BINARY_DIR}/traffic.wasm")
        set_tests_properties(graph_p7_process PROPERTIES TIMEOUT 60)
        add_test(NAME graph_p8_relay COMMAND "${CORE_UV}"
            --directory "${CMAKE_CURRENT_LIST_DIR}/../../../sim_env" run --offline python
            -m unittest discover -s tests -p test_graph_relay.py -v)
        add_test(NAME graph_p8_process COMMAND "${CMAKE_COMMAND}" -E env
            "KAOS_GRAPH_HOST=$<TARGET_FILE:kaos_host>"
            "KAOS_GRAPH_GUEST=${CMAKE_CURRENT_BINARY_DIR}/traffic.wasm"
            "${CORE_UV}" --directory "${CMAKE_CURRENT_LIST_DIR}/../../../sim_env"
            run --offline python -m unittest discover -s tests -p test_graph_run.py -v)
        set_tests_properties(graph_p8_relay graph_p8_process PROPERTIES TIMEOUT 40)
        if(CORE_BUILD_P2_HOST)
            add_test(NAME graph_python_regression COMMAND "${CMAKE_COMMAND}" -E env
                "KAOS_P2_HOST=$<TARGET_FILE:kaos_p2_host>"
                "KAOS_P3_HOST=$<TARGET_FILE:kaos_host>"
                "KAOS_P3_GUEST=${CMAKE_CURRENT_BINARY_DIR}/node.wasm"
                "ASAN_OPTIONS=detect_stack_use_after_return=0"
                "${CORE_UV}" --directory "${CMAKE_CURRENT_LIST_DIR}/../../../sim_env"
                run --offline python -m unittest discover -s tests)
            set_tests_properties(graph_python_regression PROPERTIES TIMEOUT 90)
        endif()
    endif()
    foreach(slice host host_udp)
        add_executable(graph_${slice}_test tests/${slice}_test.c)
        target_link_libraries(graph_${slice}_test PRIVATE kaos_graph_host)
        target_compile_options(graph_${slice}_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME graph_${slice} COMMAND graph_${slice}_test)
        set_tests_properties(graph_${slice} PROPERTIES TIMEOUT 15
            ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0;UBSAN_OPTIONS=halt_on_error=1")
    endforeach()
    add_executable(graph_host_scheduling_test tests/host_scheduling_test.c)
    target_link_libraries(graph_host_scheduling_test PRIVATE kaos_graph_host)
    target_compile_options(graph_host_scheduling_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_dependencies(graph_host_scheduling_test graph_runtime_guests)
    add_test(NAME graph_host_scheduling COMMAND graph_host_scheduling_test
        "${CMAKE_CURRENT_BINARY_DIR}/node.wasm")
    set_tests_properties(graph_host_scheduling PROPERTIES TIMEOUT 20
        ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0;UBSAN_OPTIONS=halt_on_error=1")
    add_executable(graph_wamr_test tests/wamr_test.c)
    target_link_libraries(graph_wamr_test PRIVATE kaos_wamr)
    target_compile_options(graph_wamr_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_dependencies(graph_wamr_test graph_runtime_guests)
    add_test(NAME graph_wamr COMMAND graph_wamr_test "${CMAKE_CURRENT_BINARY_DIR}/runtime_guest.wasm")
    add_executable(graph_wamr_entry_test tests/wamr_entry_test.c)
    target_link_libraries(graph_wamr_entry_test PRIVATE kaos_wamr)
    target_compile_options(graph_wamr_entry_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_dependencies(graph_wamr_entry_test graph_runtime_guests)
    add_test(NAME graph_wamr_entry COMMAND graph_wamr_entry_test "${CMAKE_CURRENT_BINARY_DIR}/runtime_guest.wasm")
    set_tests_properties(graph_wamr_entry PROPERTIES TIMEOUT 30
        ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0;UBSAN_OPTIONS=halt_on_error=1")
    add_executable(graph_wamr_channel_test tests/wamr_channel_test.c)
    target_link_libraries(graph_wamr_channel_test PRIVATE kaos_wamr)
    target_compile_options(graph_wamr_channel_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_dependencies(graph_wamr_channel_test graph_runtime_guests)
    add_test(NAME graph_wamr_channel COMMAND graph_wamr_channel_test
        "${CMAKE_CURRENT_BINARY_DIR}/channel.wasm")
    set_tests_properties(graph_wamr_channel PROPERTIES TIMEOUT 30
        ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0;UBSAN_OPTIONS=halt_on_error=1")
    add_executable(graph_wamr_budget_test tests/wamr_budget_test.c)
    target_link_libraries(graph_wamr_budget_test PRIVATE kaos_wamr)
    target_compile_options(graph_wamr_budget_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_dependencies(graph_wamr_budget_test graph_runtime_guests)
    add_test(NAME graph_wamr_budget COMMAND graph_wamr_budget_test
        "${CMAKE_CURRENT_BINARY_DIR}/runtime_guest.wasm")
    set_tests_properties(graph_wamr_budget PROPERTIES TIMEOUT 30
        ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0;UBSAN_OPTIONS=halt_on_error=1")
    # A separate artifact exercises the compact linker layout and OOM sweep.
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/compact_guest.wasm"
        COMMAND "${CORE_WASI_CLANG}" --target=wasm32 -O1 -nostdlib
            -Wl,--no-entry -Wl,--export=kaos_configure -Wl,--export=kaos_main
            -Wl,--export=__heap_base -Wl,--export=__data_end -Wl,--allow-undefined
            -Wl,--initial-memory=65536 -Wl,--max-memory=65536 -Wl,-z,stack-size=4096
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_guest.c"
            -o "${CMAKE_CURRENT_BINARY_DIR}/compact_guest.wasm"
        DEPENDS tests/runtime_guest.c VERBATIM)
    add_custom_target(graph_capacity_guest DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/compact_guest.wasm")
    add_executable(graph_wamr_capacity_test capacity/probe.c)
    target_link_libraries(graph_wamr_capacity_test PRIVATE kaos_wamr)
    target_compile_options(graph_wamr_capacity_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_dependencies(graph_wamr_capacity_test graph_capacity_guest)
    add_test(NAME graph_wamr_capacity COMMAND graph_wamr_capacity_test
        "${CMAKE_CURRENT_BINARY_DIR}/compact_guest.wasm")
    set_tests_properties(graph_wamr_capacity PROPERTIES TIMEOUT 30
        ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0;UBSAN_OPTIONS=halt_on_error=1")
    set_tests_properties(graph_wamr PROPERTIES TIMEOUT 30)
    if(CORE_SANITIZE)
        # WAMR checks local addresses against the native pthread stack. ASan
        # fake stacks invalidate that check; retain address/UB/leak checking.
        set_tests_properties(graph_wamr PROPERTIES
            ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0")
    endif()
endif()

if(BUILD_TESTING)
    include("${CMAKE_CURRENT_LIST_DIR}/runtime_phase_barriers.cmake")
    add_executable(graph_core_test tests/core_test.c)
    target_link_libraries(graph_core_test PRIVATE kaos_graph_core)
    target_compile_options(graph_core_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME graph_core_foundations COMMAND graph_core_test)
    foreach(slice admission admission_reuse request dynamic_binding report object_facts coap coap_server artifact control host_control p2_host node endpoint edge p5 channel_admission p6 p6_admission p6_control p7 p7_control p7_lifecycle)
        add_executable(graph_${slice}_test tests/${slice}_test.c)
        target_link_libraries(graph_${slice}_test PRIVATE kaos_graph_core)
        target_compile_options(graph_${slice}_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME graph_${slice} COMMAND graph_${slice}_test)
    endforeach()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/simplification.cmake")
