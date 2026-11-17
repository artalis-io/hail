/* Compile with the real Windows SDK; no COM clients or devices are opened. */
#define COBJMACROS
#define CONST_VTABLE
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
static DWORD wait_result;
static BOOL signal_result;
static unsigned closes, signals;
static unsigned acknowledge, thread_waits;
static DWORD join_result;
static HANDLE reply_handle;
static atomic_uint *completion;
static DWORD WINAPI test_wait(HANDLE handle, DWORD timeout)
{
    (void)timeout;
    if (acknowledge) {
        if (handle == reply_handle) return WAIT_OBJECT_0;
        return ++thread_waits == 1 ? WAIT_TIMEOUT : join_result;
    }
    return wait_result;
}
static BOOL WINAPI test_signal(HANDLE handle)
{ (void)handle; ++signals; if (acknowledge) atomic_store(completion, 1); return signal_result; }
static BOOL WINAPI test_reset(HANDLE handle) { (void)handle; return TRUE; }
static BOOL WINAPI test_close(HANDLE handle)
{ (void)handle; ++closes; return TRUE; }
#define WaitForSingleObject test_wait
#define SetEvent test_signal
#define ResetEvent test_reset
#define CloseHandle test_close
#define hail_wasapi_backend hail_test_wasapi_backend
#include "../src/backend_wasapi.c"
#undef hail_wasapi_backend
#undef CloseHandle
#undef SetEvent
#undef ResetEvent
#undef WaitForSingleObject
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
int main(void)
{
    ha_stream s = {0}; wa_stream a = {0}; ha_context c = {0}; wa_context p = {0};
    char token, reply_token;
    s.backend_state = &a; c.backend_state = &p;
    a.thread = &token; a.request = &token; a.reply = &token;
    atomic_init(&a.command, 0); atomic_init(&a.done, 0);
    wait_result = WAIT_FAILED;
    CHECK(wa_retire(&s) == HA_STREAM_FAILED && a.thread && !closes);
    wait_result = WAIT_TIMEOUT; signal_result = FALSE;
    CHECK(wa_retire(&s) == HA_STREAM_FAILED && a.thread && !closes);
    signal_result = TRUE;
    CHECK(wa_retire(&s) == HA_PENDING && a.thread && !closes && atomic_load(&a.command) == WA_EXIT);
    wait_result = WAIT_OBJECT_0;
    CHECK(wa_retire(&s) == HA_OK && !a.thread && closes == 1);
    CHECK(wa_retire(&s) == HA_OK && closes == 1);
    wa_stream_free(&s); CHECK(closes == 3 && !a.request && !a.reply);
    p.thread = &token; p.request = &token; p.reply = &token;
    atomic_init(&p.command, 0); atomic_init(&p.done, 0); atomic_init(&p.live, 0);
    wait_result = WAIT_FAILED;
    CHECK(wa_context_free(&c) == HA_STREAM_FAILED && p.thread && closes == 3);
    wait_result = WAIT_TIMEOUT; signal_result = FALSE;
    CHECK(wa_context_free(&c) == HA_STREAM_FAILED && p.thread && closes == 3);
    signal_result = TRUE;
    CHECK(wa_context_free(&c) == HA_PENDING && p.thread && closes == 3 && atomic_load(&p.command) == WA_EXIT);
    wait_result = WAIT_OBJECT_0;
    CHECK(wa_context_free(&c) == HA_OK && !p.thread && closes == 6);
    CHECK(wa_context_free(&c) == HA_OK && closes == 6 && signals == 4);
    /* Acknowledging EXIT is distinct from proving thread termination. */
    p.thread = &token; p.request = &token; p.reply = &reply_token;
    atomic_init(&p.command, 0); atomic_init(&p.done, 1); atomic_init(&p.live, 1); p.result = HA_OK;
    acknowledge = 1; completion = &p.done; reply_handle = p.reply; join_result = WAIT_FAILED;
    CHECK(wa_context_free(&c) == HA_STREAM_FAILED && p.thread && closes == 6);
    thread_waits = 0; join_result = WAIT_OBJECT_0;
    CHECK(wa_context_free(&c) == HA_OK && !p.thread && closes == 9 && signals == 6);
    puts("WASAPI failed waits/signals retain resources until worker exit proof");
    return 0;
}
