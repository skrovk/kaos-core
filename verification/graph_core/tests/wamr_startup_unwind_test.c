#define _POSIX_C_SOURCE 200809L
#include "kaos_wamr.h"
#include "kaos_admission.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int __real_pthread_join(pthread_t, void **);
static size_t attempts, fail_at, created, joined;

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                         void *(*start)(void *), void *context)
{
    if (attempts++ == fail_at) return EAGAIN;
    const int result = __real_pthread_create(thread, attributes, start, context);
    if (result == 0) ++created;
    return result;
}

int __wrap_pthread_join(pthread_t thread, void **result)
{
    const int status = __real_pthread_join(thread, result);
    if (status == 0) ++joined;
    return status;
}

int main(void)
{
    for (size_t failure = 0; failure < CORE_MAX_NODE_RESERVATIONS; ++failure) {
        attempts = created = joined = 0;
        fail_at = failure;
        core_node_runtime runtime, untouched;
        memset(&runtime, 0xa5, sizeof(runtime));
        memcpy(&untouched, &runtime, sizeof(runtime));
        assert(!core_wamr_init(&runtime, NULL, NULL));
        assert(attempts == failure + 1 && created == failure && joined == created);
        assert(memcmp(&runtime, &untouched, sizeof(runtime)) == 0);
        core_wamr_shutdown(); /* failed initialization has already unwound */
        assert(joined == created);

        /* Every failure position must leave global runtime state usable. */
        attempts = created = joined = 0;
        fail_at = SIZE_MAX;
        assert(core_wamr_init(&runtime, NULL, NULL));
        assert(created == CORE_MAX_NODE_RESERVATIONS && joined == 0);
        assert(runtime.prepare != NULL && runtime.lock != NULL && runtime.unlock != NULL);
        core_wamr_shutdown();
        assert(joined == created);
        core_wamr_shutdown();
        assert(joined == created);
    }
    printf("WAMR startup unwind: %u failure positions plus successful reuse\n",
           (unsigned)CORE_MAX_NODE_RESERVATIONS);
    return 0;
}
