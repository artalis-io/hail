/* No audio devices opened. A barrier holds the source-private SDK query so
 * poll independence, cached denial, fresh checks and worker retirement have
 * deterministic oracles, independent of TCC state and machine speed. */
#include <hail.h>
#include <stdatomic.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static dispatch_semaphore_t entered,release_query,destroy_entered,destroy_done;
static atomic_uint hold_query,query_calls;
static atomic_int query_result;
static ha_result test_permission(void)
{
    atomic_fetch_add(&query_calls,1);
    if(atomic_exchange(&hold_query,0)) {
        dispatch_semaphore_signal(entered);
        dispatch_semaphore_wait(release_query,DISPATCH_TIME_FOREVER);
    }
    return atomic_load(&query_result);
}
#define hail_coreaudio_capture_permission test_permission
#define hail_coreaudio_backend hail_test_coreaudio_backend
#include "../src/backend_coreaudio.c"
#undef hail_coreaudio_backend
#undef hail_coreaudio_capture_permission
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
static void wait_for(dispatch_semaphore_t semaphore)
{ CHECK(!dispatch_semaphore_wait(semaphore,dispatch_time(DISPATCH_TIME_NOW,5000000000ll))); }
static void *destroy_context(void *user)
{
    dispatch_semaphore_signal(destroy_entered);
    CHECK(ca_context_free(user)==HA_OK);
    dispatch_semaphore_signal(destroy_done);
    return NULL;
}
int main(void)
{
    ca_context p; ha_context c; ha_stream s; ha_stream *streams[]={&s}; pthread_t destroy_thread;
    memset(&p,0,sizeof(p)); memset(&c,0,sizeof(c)); memset(&s,0,sizeof(s));
    entered=dispatch_semaphore_create(0); release_query=dispatch_semaphore_create(0);
    destroy_entered=dispatch_semaphore_create(0); destroy_done=dispatch_semaphore_create(0);
    atomic_init(&hold_query,1); atomic_init(&query_calls,0); atomic_init(&query_result,HA_PERMISSION_DENIED);
    p.owner=&c; c.backend_state=&p; c.streams=streams; c.max_streams=1;
    s.context=&c; s.concurrent=1; s.info.capture_slots=1; atomic_init(&s.state,HA_RUNNING); hail_rt_init(&s);
    atomic_init(&c.registry_dirty,0); atomic_init(&p.callbacks,0); atomic_init(&p.permission_result,HA_OK);
    CHECK(!pthread_mutex_init(&p.permission_mutex,NULL)); p.mutex_initialized=1;
    CHECK(!pthread_cond_init(&p.permission_condition,NULL)); p.condition_initialized=1;
    p.permission_users=1;
    CHECK(!pthread_create(&p.permission_thread,NULL,ca_permission_worker,&p)); p.worker_started=1;
    wait_for(entered);
    for(unsigned i=0;i<1000;++i) ca_context_poll(&c);
    CHECK(atomic_load(&query_calls)==1 && atomic_load(&s.rt[0].error)==HA_OK);
    puts("PASS 1000 control polls finish while SDK authorization query is blocked");
    dispatch_semaphore_signal(release_query);
    CHECK(!pthread_mutex_lock(&p.permission_mutex));
    CHECK(atomic_load(&p.permission_result)==HA_PERMISSION_DENIED);
    p.permission_users=0; CHECK(!pthread_mutex_unlock(&p.permission_mutex));
    ca_context_poll(&c); CHECK(atomic_load(&s.rt[0].error)==HA_PERMISSION_DENIED);
    puts("PASS worker denial is published to the capture leg");
    atomic_store(&query_result,HA_OK);
    CHECK(ca_permission(&p)==HA_OK && atomic_load(&p.permission_result)==HA_OK);
    puts("PASS fresh synchronous check replaces cached denial");
    atomic_store(&hold_query,1);
    CHECK(!pthread_mutex_lock(&p.permission_mutex)); p.permission_users=1;
    CHECK(!pthread_cond_signal(&p.permission_condition)); CHECK(!pthread_mutex_unlock(&p.permission_mutex));
    wait_for(entered);
    CHECK(!pthread_create(&destroy_thread,NULL,destroy_context,&c)); wait_for(destroy_entered);
    CHECK(dispatch_semaphore_wait(destroy_done,DISPATCH_TIME_NOW)!=0);
    dispatch_semaphore_signal(release_query); wait_for(destroy_done); CHECK(!pthread_join(destroy_thread,NULL));
    CHECK(!p.worker_started && !p.mutex_initialized && !p.condition_initialized);
    dispatch_release(entered); dispatch_release(release_query); dispatch_release(destroy_entered); dispatch_release(destroy_done);
    puts("PASS context retirement joins blocked worker before retiring its synchronization storage");
    return 0;
}
