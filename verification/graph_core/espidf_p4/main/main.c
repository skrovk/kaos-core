#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern const uint8_t guest_start[] __asm__("_binary_capacity_wasm_start");
extern const uint8_t guest_end[] __asm__("_binary_capacity_wasm_end");
void p4_capacity_run(const uint8_t *guest, size_t size);

void app_main(void)
{
    /* Control must remain runnable while priority-2 guests compute or leave
     * a notification unread, matching the qualified P3 scheduling profile. */
    vTaskPrioritySet(NULL, 3);
    p4_capacity_run(guest_start, (size_t)(guest_end - guest_start));
}
