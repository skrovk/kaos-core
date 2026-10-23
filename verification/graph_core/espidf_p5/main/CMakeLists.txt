idf_component_register(SRCS "main.c" "../../p5_capacity.c" REQUIRES runtime graph_core esp_timer)
target_compile_definitions(${COMPONENT_LIB} PUBLIC CORE_FOUR_NODE_PROFILE=1)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(guest "${CMAKE_CURRENT_BINARY_DIR}/capacity.wasm")
    add_custom_command(OUTPUT "${guest}"
        COMMAND /opt/wasi-sdk/bin/clang --target=wasm32 -O1 -nostdlib
            -Wl,--no-entry -Wl,--export=kaos_configure -Wl,--export=kaos_main
            -Wl,--export=__heap_base -Wl,--export=__data_end -Wl,--allow-undefined
            -Wl,--initial-memory=65536 -Wl,--max-memory=65536 -Wl,-z,stack-size=4096
            "${CMAKE_CURRENT_LIST_DIR}/../../guests/capacity.c" -o "${guest}"
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../../guests/capacity.c" VERBATIM)
    add_custom_target(p5_guest DEPENDS "${guest}")
    add_dependencies(${COMPONENT_LIB} p5_guest)
    target_add_binary_data(${COMPONENT_LIB} "${guest}" BINARY)
endif()
