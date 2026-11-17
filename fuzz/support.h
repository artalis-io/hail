#ifndef HAIL_FUZZ_SUPPORT_H
#define HAIL_FUZZ_SUPPORT_H
#include "hail_internal.h"
#include <hail/virtual.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
typedef struct fuzz_input { const uint8_t *data; size_t size, at; } fuzz_input;
static inline uint8_t byte(fuzz_input *in)
{ return in->at < in->size ? in->data[in->at++] : 0; }
static inline uint32_t word(fuzz_input *in)
{ uint32_t n = 0; for (unsigned i = 0; i < 4; ++i) n |= (uint32_t)byte(in) << (i * 8u); return n; }
typedef struct fuzz_memory {
    void *ptr[2]; size_t bytes[2], align[2], live;
    unsigned calls, frees, fail_call;
} fuzz_memory;
static inline void *HA_CALL fuzz_allocate(void *user, size_t bytes, size_t alignment)
{
    fuzz_memory *m = user; ++m->calls;
    if (m->calls == m->fail_call || bytes > 1048576u) return NULL;
    for (unsigned i = 0; i < 2; ++i) if (!m->ptr[i]) {
        void *p = hail_host_allocate(NULL, bytes, alignment); CHECK(p != NULL);
        m->ptr[i] = p; m->bytes[i] = bytes; m->align[i] = alignment; m->live += bytes; return p;
    }
    CHECK(0); return NULL;
}
static inline void HA_CALL fuzz_deallocate(void *user, void *p, size_t bytes, size_t alignment)
{
    fuzz_memory *m = user;
    for (unsigned i = 0; i < 2; ++i) if (p && p == m->ptr[i]) {
        CHECK(bytes == m->bytes[i] && alignment == m->align[i] && m->live >= bytes);
        m->ptr[i] = NULL; m->live -= bytes; ++m->frees;
        hail_host_deallocate(NULL, p, bytes, alignment); return;
    }
    CHECK(0);
}
static inline ha_context_config context_config(fuzz_memory *m)
{
    ha_context_config cc = HA_INIT(ha_context_config); ha_allocator a = HA_INIT(ha_allocator);
    a.allocate = fuzz_allocate; a.deallocate = fuzz_deallocate; a.user = m;
    cc.backend = HA_BACKEND_VIRTUAL; cc.max_devices = 2; cc.max_streams = 1;
    cc.event_capacity = 4; cc.memory_limit = 1048576; cc.allocator = a; return cc;
}
static inline ha_stream_config stream_config(uint32_t quantum)
{
    ha_stream_config sc = HA_INIT(ha_stream_config); ha_device_selector d = HA_INIT(ha_device_selector);
    ha_format f = HA_FORMAT_INIT(48000, 1, HA_PCM_S16);
    sc.directions = HA_CAPTURE | HA_PLAYBACK; sc.capture_device = d; sc.playback_device = d;
    sc.capture_format = f; sc.playback_format = f; sc.quantum_frames = quantum;
    sc.capture_slots = 2; sc.playback_slots = 2; sc.reference_slots = 2; return sc;
}
static inline void poll_context(ha_context *c)
{
    ha_event events[4]; uint32_t n = UINT32_MAX;
    for (unsigned i = 0; i < 4; ++i) { ha_event e = HA_INIT(ha_event); events[i] = e; }
    CHECK(ha_context_poll(c, events, 4, &n) == HA_OK && n <= 4);
}
static inline ha_stream_status status(ha_stream *s)
{ ha_stream_status st = HA_INIT(ha_stream_status); CHECK(ha_stream_get_status(s, &st) == HA_OK); return st; }
static inline void close_stream(ha_context *c, ha_stream **s)
{
    CHECK(ha_stream_close_begin(*s) == HA_PENDING);
    for (unsigned i = 0; i < 16; ++i) {
        poll_context(c);
        if (ha_stream_close_finish(s) == HA_OK) { CHECK(!*s); return; }
    }
    CHECK(0);
}
#endif
