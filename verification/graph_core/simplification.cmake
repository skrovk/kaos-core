# New characterization coverage; existing fixture sources remain unchanged.
if(BUILD_TESTING)
    add_executable(graph_control_decoded_test "${CMAKE_CURRENT_LIST_DIR}/tests/control_decoded_test.c")
    target_link_libraries(graph_control_decoded_test PRIVATE kaos_graph_core)
    target_compile_options(graph_control_decoded_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME graph_control_decoded COMMAND graph_control_decoded_test)
endif()

if(BUILD_TESTING AND TARGET kaos_wamr)
    add_executable(graph_wamr_startup_unwind_test "${CMAKE_CURRENT_LIST_DIR}/tests/wamr_startup_unwind_test.c")
    target_link_libraries(graph_wamr_startup_unwind_test PRIVATE kaos_wamr)
    target_compile_options(graph_wamr_startup_unwind_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_options(graph_wamr_startup_unwind_test PRIVATE
        -Wl,--wrap=pthread_create -Wl,--wrap=pthread_join)
    add_test(NAME graph_wamr_startup_unwind COMMAND graph_wamr_startup_unwind_test)
    set_tests_properties(graph_wamr_startup_unwind PROPERTIES TIMEOUT 15
        ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0;UBSAN_OPTIONS=halt_on_error=1")
endif()
