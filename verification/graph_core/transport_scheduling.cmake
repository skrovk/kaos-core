if(TARGET kaos_graph_host)
    add_executable(graph_host_transport_scheduling_test
        "${CMAKE_CURRENT_LIST_DIR}/tests/host_transport_scheduling_test.c")
    target_link_libraries(graph_host_transport_scheduling_test PRIVATE kaos_graph_host)
    target_compile_options(graph_host_transport_scheduling_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME graph_host_transport_scheduling COMMAND graph_host_transport_scheduling_test)
    set_tests_properties(graph_host_transport_scheduling PROPERTIES TIMEOUT 20
        ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0;UBSAN_OPTIONS=halt_on_error=1")
endif()
