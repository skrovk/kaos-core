#define _POSIX_C_SOURCE 200809L
#include "kaos_wamr.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static core_node_runtime runtime;

static core_runtime_state await(bool disposed)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    const time_t cutoff = now.tv_sec + 5;
    for (;;) {
        core_runtime_state state = *runtime.lock(runtime.context, 0);
        runtime.unlock(runtime.context, 0);
        if (disposed ? state.disposed : state.prepared || state.prepare_failed) return state;
        assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec < cutoff);
        const struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    const long length = ftell(file);
    assert(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    uint8_t *fixture = malloc((size_t)length);
    assert(fixture && fread(fixture, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    assert(core_wamr_init(&runtime, NULL, NULL));
    /* The fixture needs a full 64 KiB linear page plus its application heap.
     * With WAMR's default mmap path both small budgets incorrectly prepare:
     * the linear allocation escapes the arena. */
    const size_t budgets[] = {32768, 65536, 98304};
    for (size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); ++i) {
        uint8_t *copy = malloc((size_t)length);
        assert(copy);
        memcpy(copy, fixture, (size_t)length);
        assert(runtime.prepare(runtime.context, 0, copy, (size_t)length, NULL, 0, budgets[i]));
        core_runtime_state result = await(false);
        assert(result.prepared == (i == 2));
        assert(result.prepare_failed == (i != 2));
        assert(!result.main_active && !result.init_pending);
        core_runtime_state *commands = runtime.lock(runtime.context, 0);
        commands->cancel_requested = true;
        commands->dispose_requested = true;
        runtime.unlock(runtime.context, 0);
        assert(await(true).disposed);
    }
    core_wamr_shutdown();
    free(fixture);
    puts("WAMR linear memory stays inside the node arena: passed");
    return 0;
}
