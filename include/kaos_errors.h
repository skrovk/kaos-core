#ifndef KAOS_ERRORS_H
#define KAOS_ERRORS_H

// TODO: error handling strategy
// TODO: errors in container include
typedef enum kaos_error_t {
    KaosSuccess,
    // Warnings
    KaosNothingToSend,
    KaosQueueNotEstablished,
    KaosNothingToReceive,
    KaosOperationIncomplete,
    // Errors
    KaosError,
    KaosTimerError,
    KaosRuntimeError,
    KaosInvalidCommandError,
    KaosMemoryAllocationError,
    KaosQueueFullError,
    KaosQueueEmptyError,
    KaosContainerMemoryAllocationError,
    KaosMutexError,
    KaosContainerLocationNotFoundError,
    KaosParserError,
    KaosInvalidCommandFieldError,
    KaosModuleConfigError,
    KaosExportRegistrationError,
    KaosModuleInitializationError,
    KaosModuleExecutionError,
    KaosModuleRuntimeError,
    KaosRegistryNotFoundError,
    KaosRegistryPoolFullError,
    KaosContainerTimerNotFoundError,
    KaosChannelNotFoundError,
    KaosRegistryNameConflictError,
    KaosPeripheralError,

    KaosTCPSendError,
    KaosTCPRecvError,
    KaosUDPSendError,
    KaosUDPRecvError,
} kaos_error_t;

#endif
