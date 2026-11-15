#include "hail_internal.h"
#include <stdlib.h>

void *HA_CALL hail_host_allocate(void *user, size_t bytes, size_t alignment)
{
    void *base;
    uintptr_t aligned;
    size_t total = bytes;
    (void)user;
    if (!hail_add(&total, alignment - 1u) || !hail_add(&total, sizeof(void *))) return NULL;
    base = malloc(total);
    if (!base) return NULL;
    aligned = ((uintptr_t)base + sizeof(void *) + alignment - 1u) & ~(uintptr_t)(alignment - 1u);
    ((void **)aligned)[-1] = base;
    return (void *)aligned;
}

void HA_CALL hail_host_deallocate(void *user, void *ptr, size_t bytes, size_t alignment)
{
    (void)user; (void)bytes; (void)alignment;
    if (ptr) free(((void **)ptr)[-1]);
}
