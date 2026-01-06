/**
 * @file port_logging.h
 * @brief Platform-specific logging macros for KAOS.
 *
 * This header provides port-specific implementations of KAOS_LOG* macros.
 * Include this in all source files that use logging.
 */

#ifndef PORT_LOGGING_H
#define PORT_LOGGING_H

#if defined(KAOS_PORT_XTENSA)
    /* ESP-IDF / Xtensa port */
    #define KAOS_LOGI(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
    #define KAOS_LOGE(tag, ...) ESP_LOGE(tag, __VA_ARGS__)
    #define KAOS_LOGW(tag, ...) ESP_LOGW(tag, __VA_ARGS__)
    #define KAOS_LOGD(tag, ...) ESP_LOGD(tag, __VA_ARGS__)

#elif defined(KAOS_PORT_GENERIC)
    /* Generic/POSIX port with ANSI color codes */
    #include <stdio.h>
    #define KAOS_LOGI(tag, ...) printf("\033[32m[INFO][%s] \033[0m" __VA_ARGS__ "\n", tag)
    #define KAOS_LOGE(tag, ...) printf("\033[31m[ERROR][%s] \033[0m" __VA_ARGS__ "\n", tag)
    #define KAOS_LOGW(tag, ...) printf("\033[33m[WARN][%s] \033[0m" __VA_ARGS__ "\n", tag)
    #define KAOS_LOGD(tag, ...) printf("\033[36m[DEBUG][%s] \033[0m" __VA_ARGS__ "\n", tag)

#else
    /* Fallback: no-op logging */
    #define KAOS_LOGI(tag, ...) ((void)0)
    #define KAOS_LOGE(tag, ...) ((void)0)
    #define KAOS_LOGW(tag, ...) ((void)0)
    #define KAOS_LOGD(tag, ...) ((void)0)
    #warning "KAOS port not defined; logging disabled. Define KAOS_PORT_XTENSA or KAOS_PORT_GENERIC."
#endif

#endif /* PORT_LOGGING_H */
