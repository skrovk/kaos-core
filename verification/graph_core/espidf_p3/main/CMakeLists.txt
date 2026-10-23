idf_component_register(SRCS "main.c" "node_lifecycle.c" REQUIRES runtime graph_core esp_timer)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(CORE_WASI_CLANG "/opt/wasi-sdk/bin/clang" CACHE FILEPATH "WASI clang")
    set(guest "${CMAKE_CURRENT_BINARY_DIR}/runtime_guest.wasm")
    add_custom_command(OUTPUT "${guest}"
        COMMAND "${CORE_WASI_CLANG}" --target=wasm32 -O1 -nostdlib
            -Wl,--no-entry -Wl,--export=kaos_configure -Wl,--export=kaos_main
            -Wl,--allow-undefined -Wl,--initial-memory=65536 -Wl,--max-memory=65536
            -Wl,-z,stack-size=4096 "${CMAKE_CURRENT_LIST_DIR}/../../tests/runtime_guest.c"
            -o "${guest}"
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../../tests/runtime_guest.c" VERBATIM)
    add_custom_target(p3_guest DEPENDS "${guest}")
    add_dependencies(${COMPONENT_LIB} p3_guest)
    target_add_binary_data(${COMPONENT_LIB} "${guest}" BINARY)
endif()
