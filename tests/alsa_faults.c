/* Fault-injected native control, no PCM devices or workers created. */
#define _POSIX_C_SOURCE 200809L
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
static int drop_errors[2], join_error;
static unsigned drops[2], joins, closes;
static char pcm_tokens[2];
static int test_drop(snd_pcm_t *pcm)
{
    unsigned i = pcm == (snd_pcm_t *)(void *)&pcm_tokens[0] ? 0u : 1u;
    ++drops[i]; return drop_errors[i];
}
static int test_close(snd_pcm_t *pcm) { (void)pcm; ++closes; return 0; }
static int test_join(pthread_t thread, void **result)
{ (void)thread; (void)result; ++joins; return join_error; }
#define snd_pcm_drop test_drop
#define snd_pcm_close test_close
#define pthread_join test_join
#define hail_alsa_backend hail_test_alsa_backend
#include "../src/backend_alsa.c"
#undef hail_alsa_backend
#undef pthread_join
#undef snd_pcm_close
#undef snd_pcm_drop
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
int main(void)
{
    ha_context c = {0}; ha_stream s = {0}; alsa_stream a = {0};
    hail_backend_ops backend = hail_test_alsa_backend;
    ha_stream *streams[] = {&s}; ha_event events[8]; uint32_t count;
    backend.context_poll = NULL; c.backend = &backend; c.streams = streams; c.max_streams = 1;
    c.events = events; c.event_capacity = 8; atomic_init(&c.registry_dirty, 0);
    s.context = &c; s.backend_state = &a; s.concurrent = 1;
    atomic_init(&s.state, HA_STOPPING); atomic_init(&s.read_gate, HAIL_GATE_IDLE);
    atomic_init(&s.write_gate, HAIL_GATE_IDLE); atomic_init(&s.reference_gate, HAIL_GATE_IDLE);
    hail_rt_init(&s); atomic_init(&a.run, 1); atomic_init(&a.exit, 0);
    a.pcm[0] = (snd_pcm_t *)(void *)&pcm_tokens[0]; a.pcm[1] = (snd_pcm_t *)(void *)&pcm_tokens[1];
    /* First failure wins, but both legs are attempted. */
    drop_errors[0] = -EACCES; drop_errors[1] = -ENODEV;
    /* context_poll must not query inotify against this synthetic context. */
    CHECK(ha_context_poll(&c, NULL, 0, &count) == HA_OK);
    CHECK(atomic_load(&s.state) == HA_FAILED && s.status.terminal_result == HA_PERMISSION_DENIED);
    CHECK(drops[0] == 1 && drops[1] == 1 && !atomic_load(&a.run));
    CHECK(c.event_count == 1 && events[0].state == HA_FAILED);
    drop_errors[0] = 0; CHECK(alsa_stop(&s) == HA_DEVICE_LOST);
    drop_errors[1] = 0; drop_errors[0] = -EACCES; CHECK(alsa_stop(&s) == HA_PERMISSION_DENIED);
    drop_errors[0] = 0; CHECK(alsa_stop(&s) == HA_OK);
    CHECK(drops[0] == 4 && drops[1] == 4);
    a.created = 1; join_error = EDEADLK;
    CHECK(alsa_retire(&s) != HA_OK && a.created && !closes && joins == 1);
    CHECK(a.pcm[0] && a.pcm[1] && atomic_load(&a.exit));
    join_error = 0; CHECK(alsa_retire(&s) == HA_OK);
    CHECK(!a.created && !a.pcm[0] && !a.pcm[1] && closes == 2 && joins == 2);
    CHECK(alsa_retire(&s) == HA_OK && closes == 2 && joins == 2);
    puts("ALSA drop failures propagate; failed join retains resources until retry");
    return 0;
}
