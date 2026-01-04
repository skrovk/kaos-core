idf_component_register(
        SRCS 
            "container_mgr.c"
            "cbor_parser.c"
            "cbor_composer"
            "network.c"
            "unreliable_message_queue.c"
            "kaos_monitor.c"
            "kaos_signals.c"
            "led.c"
            "kaos_button.c"
        INCLUDE_DIRS "include"
        REQUIRES wamr
        PRIV_REQUIRES nvs_flash tinycbor esp_wifi esp_driver_gpio esp_timer button
)
