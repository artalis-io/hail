#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <hail.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <sys/resource.h>
#include <mach/mach_time.h>

/* Explicit opt-in physical exercise. Never records or routes microphone PCM.
 * No arguments: enumerate only. Otherwise:
 * CAPTURE_NAME|- PLAYBACK_NAME|- RATE CHANNELS PCM(1=S16,2=F32) SECONDS
 * A low-level 375 Hz tone (-46 dBFS) is submitted to the selected output.
 * Device rates, defaults and volume are never changed. */
#define Q 256u
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
extern int hail_test_authorize(void);
static ha_context *context;
static ha_stream *stream;
static uint64_t allocations, frees, live_bytes, peak_bytes, poll_max_ns;
static atomic_uint quit_reader;
static atomic_uint_fast64_t captured;
/* Only the reader touches these until pthread_join. */
static uint64_t nonzero, gaps, resets, age_count, age_total, age_max, future_times;
static uint64_t age_hist[1001];
static uint32_t channels, pcm_kind;
static void *HA_CALL allocate(void *u,size_t n,size_t a)
{ void *p=NULL; (void)u; if(posix_memalign(&p,a,n)) return NULL; ++allocations; live_bytes+=n; if(live_bytes>peak_bytes) peak_bytes=live_bytes; return p; }
static void HA_CALL deallocate(void *u,void *p,size_t n,size_t a)
{ (void)u; (void)a; ++frees; live_bytes-=n; free(p); }
static uint64_t now(void)
{ struct timespec t; CHECK(!clock_gettime(CLOCK_MONOTONIC,&t)); return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec; }
static void pause_control(void) { struct timespec t={0,1000000}; nanosleep(&t,NULL); }
static void progress(void)
{
    ha_event events[64]; uint32_t n,i; uint64_t begin=now(),elapsed;
    for(i=0;i<64;++i) { ha_event e=HA_INIT(ha_event); events[i]=e; }
    CHECK(ha_context_poll(context,events,64,&n)==HA_OK);
    elapsed=now()-begin; if(elapsed>poll_max_ns) poll_max_ns=elapsed;
}
static ha_stream_status status(void)
{
    ha_stream_status s=HA_INIT(ha_stream_status); CHECK(ha_stream_get_status(stream,&s)==HA_OK);
    if(s.state==HA_FAILED) fprintf(stderr,"terminal=%s native=%lld\n",ha_result_string(s.terminal_result),(long long)s.diagnostic.native_code);
    CHECK(s.state!=HA_FAILED); return s;
}
static void wait_state(uint32_t target)
{
    uint64_t deadline=now()+10000000000ull;
    do { progress(); if(status().state==target) return; CHECK(now()<deadline); pause_control(); } while(1);
}
static void *read_capture(void *unused)
{
    union { float f[Q*2]; int16_t s[Q*2]; } data;
    uint64_t previous_end=0,previous_epoch=0; (void)unused;
    while(!atomic_load_explicit(&quit_reader,memory_order_acquire)) {
        ha_block_info b=HA_INIT(ha_block_info); uint32_t n,i;
        ha_result r=ha_stream_read(stream,&data,Q,&n,&b);
        if(r==HA_WOULD_BLOCK || r==HA_BUSY || r==HA_INVALID_STATE) { pause_control(); continue; }
        CHECK(r==HA_OK && n);
        if(previous_epoch && b.clock_epoch!=previous_epoch) ++resets;
        if(previous_epoch==b.clock_epoch && b.first_frame!=previous_end) ++gaps;
        if(b.flags & (HA_BLOCK_GAP|HA_BLOCK_UNKNOWN_GAP)) ++gaps;
        previous_end=b.first_frame+n; previous_epoch=b.clock_epoch;
        for(i=0;i<n*channels;++i) {
            if(pcm_kind==HA_PCM_F32) { CHECK(isfinite(data.f[i])); if(data.f[i]!=0) ++nonzero; }
            else if(data.s[i]!=0) ++nonzero;
        }
        if(b.flags & HA_TIME_HOST_VALID) {
            /* CoreAudio timestamps use mach_absolute_time, whereas macOS
             * CLOCK_MONOTONIC includes time asleep. Never subtract domains. */
            uint64_t t=AudioConvertHostTimeToNanos(mach_absolute_time());
            if(t>=b.host_time_ns) {
                uint64_t age=t-b.host_time_ns,bin=age/1000000u;
                ++age_count; age_total+=age; if(age>age_max) age_max=age;
                ++age_hist[bin<1000?bin:1000];
            } else ++future_times;
        }
        atomic_fetch_add_explicit(&captured,n,memory_order_release);
    }
    return NULL;
}
static uint64_t age_percentile(uint32_t percent)
{
    uint64_t sum=0,target=(age_count*percent+99u)/100u; uint32_t i;
    if(!age_count) return 0;
    for(i=0;i<=1000;++i) { sum+=age_hist[i]; if(sum>=target) return i; }
    return 1000;
}
static void fill_playback(uint64_t generation,uint32_t rate,uint64_t *position)
{
    union { float f[Q*2]; int16_t s[Q*2]; } data; uint32_t attempt,i,c,n;
    for(attempt=0;attempt<64;++attempt) {
        for(i=0;i<Q;++i) {
            /* Integer-period triangle; test signal only, no library DSP. */
            uint32_t period=rate/375u,phase=(uint32_t)((*position+i)%period);
            float sample=0.005f*(4.0f*(float)phase/(float)period-1.0f);
            if(phase>=period/2u) sample=0.005f*(3.0f-4.0f*(float)phase/(float)period);
            for(c=0;c<channels;++c) { if(pcm_kind==HA_PCM_F32) data.f[i*channels+c]=sample; else data.s[i*channels+c]=(int16_t)(sample*32767.0f); }
        }
        ha_result r=ha_stream_write(stream,&data,Q,generation,&n);
        if(r==HA_WOULD_BLOCK || r==HA_BUSY) return;
        CHECK(r==HA_OK && n); *position+=n;
    }
}
static void run_for(uint64_t duration,uint64_t generation,uint32_t rate,int playback,uint64_t *position)
{
    uint64_t end=now()+duration;
    while(now()<end) { progress(); (void)status(); if(playback) fill_playback(generation,rate,position); pause_control(); }
}
static void device_facts(const ha_device_info *d)
{
    CFStringRef uid=CFStringCreateWithCString(NULL,d->persistent_key,kCFStringEncodingUTF8);
    AudioDeviceID id=0;
    AudioObjectPropertyAddress a={kAudioHardwarePropertyTranslateUIDToDevice,kAudioObjectPropertyScopeGlobal,0};
    UInt32 bytes=sizeof(id),period=0,transport=0; Float64 rate=0;
    CHECK(!AudioObjectGetPropertyData(kAudioObjectSystemObject,&a,sizeof(uid),&uid,&bytes,&id)); CFRelease(uid);
    a.mSelector=kAudioDevicePropertyNominalSampleRate; bytes=sizeof(rate);
    CHECK(!AudioObjectGetPropertyData(id,&a,0,NULL,&bytes,&rate));
    a.mSelector=kAudioDevicePropertyBufferFrameSize; bytes=sizeof(period);
    CHECK(!AudioObjectGetPropertyData(id,&a,0,NULL,&bytes,&period));
    a.mSelector=kAudioDevicePropertyTransportType; bytes=sizeof(transport);
    CHECK(!AudioObjectGetPropertyData(id,&a,0,NULL,&bytes,&transport));
    printf("device direction=%u name=%s uid=%s nominal_rate=%.0f native_period=%u transport=%08x\n",d->direction,d->name,d->persistent_key,rate,period,transport);
}
int main(int argc,char **argv)
{
    ha_context_config cc=HA_INIT(ha_context_config); ha_allocator allocator=HA_INIT(ha_allocator);
    ha_device_info devices[HA_MAX_DEVICES]; ha_stream_config sc=HA_INIT(ha_stream_config);
    ha_stream_info info=HA_INIT(ha_stream_info); ha_diagnostic diagnostic=HA_INIT(ha_diagnostic);
    uint32_t n,i,rate,seconds; uint64_t input=0,output=0,generation=1,position=0,steady_alloc;
    uint64_t start_ns,end_ns,flush_begin,flush_end,before_capture,after_capture,initial_epoch;
    uint64_t steady_underrun,steady_loss,baseline_underrun,baseline_loss;
    pthread_t reader; int has_reader=0,ok=1; struct rusage usage; ha_result r;
    setvbuf(stdout,NULL,_IOLBF,0); atomic_init(&captured,0); atomic_init(&quit_reader,0);
    allocator.allocate=allocate; allocator.deallocate=deallocate; cc.allocator=allocator; cc.backend=HA_BACKEND_COREAUDIO;
    CHECK(ha_context_create(&cc,&context)==HA_OK);
    for(i=0;i<HA_MAX_DEVICES;++i) { ha_device_info d=HA_INIT(ha_device_info); devices[i]=d; }
    CHECK(ha_context_enumerate(context,devices,HA_MAX_DEVICES,&n)==HA_OK);
    for(i=0;i<n;++i) {
        device_facts(&devices[i]);
        if(argc==7 && devices[i].direction==HA_CAPTURE && !strcmp(devices[i].name,argv[1])) { CHECK(!input); input=devices[i].runtime_id; }
        if(argc==7 && devices[i].direction==HA_PLAYBACK && !strcmp(devices[i].name,argv[2])) { CHECK(!output); output=devices[i].runtime_id; }
    }
    if(argc==1) goto destroy;
    CHECK(argc==7); CHECK((input || !strcmp(argv[1],"-")) && (output || !strcmp(argv[2],"-")) && (input || output));
    rate=(uint32_t)strtoul(argv[3],NULL,10); channels=(uint32_t)strtoul(argv[4],NULL,10);
    pcm_kind=(uint32_t)strtoul(argv[5],NULL,10); seconds=(uint32_t)strtoul(argv[6],NULL,10);
    CHECK(seconds>=1 && seconds<=3600 && (channels==1 || channels==2) && (pcm_kind==1 || pcm_kind==2));
    ha_format format=HA_FORMAT_INIT(rate,channels,pcm_kind);
    sc.directions=(input?HA_CAPTURE:0u)|(output?HA_PLAYBACK:0u); sc.negotiation=HA_EXACT_APPLICATION;
    ha_device_selector selector=HA_INIT(ha_device_selector); selector.kind=HA_SELECT_RUNTIME_ID;
    selector.runtime_id=input; sc.capture_device=selector; selector.runtime_id=output; sc.playback_device=selector;
    sc.capture_format=format; sc.playback_format=format; sc.quantum_frames=Q;
    sc.capture_slots=input?64u:0u; sc.playback_slots=output?16u:0u; sc.diagnostic=&diagnostic;
    if(input && !hail_test_authorize()) { puts("RESULT permission_denied_by_host"); ok=0; goto destroy; }
    r=ha_stream_open(context,&sc,&stream);
    if(r!=HA_OK) { printf("RESULT open=%s native=%lld rate=%u channels=%u pcm=%u\n",ha_result_string(r),(long long)diagnostic.native_code,rate,channels,pcm_kind); ok=0; goto destroy; }
    CHECK(ha_stream_get_info(stream,&info)==HA_OK);
    printf("negotiated rate=%u channels=%u pcm=%u capture_slots=%u playback_slots=%u quantum=%u bytes=%llu conversion=%u clocks=%u\n",rate,channels,pcm_kind,info.capture_slots,info.playback_slots,info.quantum_frames,(unsigned long long)info.allocated_bytes,info.conversion,info.clock_relationship);
    steady_alloc=allocations;
    if(output) fill_playback(generation,rate,&position);
    if(input) { CHECK(!pthread_create(&reader,NULL,read_capture,NULL)); has_reader=1; }
    CHECK(ha_stream_start(stream)==HA_PENDING); wait_state(HA_RUNNING);
    run_for(1000000000u,generation,rate,output!=0,&position);
    ha_stream_status s=status(); baseline_underrun=s.playback_underrun_frames; baseline_loss=s.capture_lost_frames;
    initial_epoch=s.capture_epoch; start_ns=now();
    run_for((uint64_t)seconds*1000000000u,generation,rate,output!=0,&position);
    end_ns=now(); s=status(); steady_underrun=s.playback_underrun_frames-baseline_underrun; steady_loss=s.capture_lost_frames-baseline_loss;
    before_capture=atomic_load_explicit(&captured,memory_order_acquire); flush_begin=now();
    if(output) {
        CHECK(ha_stream_flush_playback(stream,HA_FLUSH_NATIVE_REQUIRED,&generation)==HA_PENDING);
        uint64_t deadline=now()+10000000000ull;
        do { progress(); s=status(); if(s.flush_completed_generation==generation) break; CHECK(now()<deadline); pause_control(); } while(1);
        CHECK(s.flush_result==HA_OK);
    }
    flush_end=now(); after_capture=atomic_load_explicit(&captured,memory_order_acquire);
    if(output) CHECK(ha_stream_write(stream,NULL,0,generation-1u,&n)==HA_STALE_GENERATION);
    CHECK(!input || s.capture_epoch==initial_epoch);
    run_for(250000000u,generation,rate,output!=0,&position);
    CHECK(ha_stream_stop(stream)==HA_PENDING); wait_state(HA_STOPPED); s=status(); generation=s.playback_generation;
    if(output) fill_playback(generation,rate,&position);
    CHECK(ha_stream_start(stream)==HA_PENDING); wait_state(HA_RUNNING);
    run_for(250000000u,generation,rate,output!=0,&position);
    CHECK(allocations==steady_alloc);
    CHECK(ha_stream_close_begin(stream)==HA_PENDING);
    if(has_reader) { atomic_store_explicit(&quit_reader,1,memory_order_release); CHECK(!pthread_join(reader,NULL)); }
    uint64_t deadline=now()+10000000000ull;
    do { progress(); CHECK(now()<deadline); r=ha_stream_close_finish(&stream); CHECK(r==HA_OK || r==HA_PENDING); if(stream) pause_control(); } while(stream);
    CHECK(!getrusage(RUSAGE_SELF,&usage));
    printf("{\"duration_s\":%.6f,\"rate\":%u,\"channels\":%u,\"pcm\":%u,\"captured_frames\":%llu,\"nonzero_capture_samples\":%llu,\"capture_gap_observations\":%llu,\"capture_epoch_changes_including_restart\":%llu,\"steady_underrun_frames\":%llu,\"startup_underrun_frames\":%llu,\"steady_capture_loss\":%llu,\"flush_ack_us\":%.3f,\"capture_frames_during_flush\":%llu,\"max_poll_us\":%.3f,\"capture_host_age_mean_us\":%.3f,\"capture_host_age_max_us\":%.3f,\"capture_host_age_p50_bin_ms\":%llu,\"capture_host_age_p95_bin_ms\":%llu,\"capture_host_age_p99_bin_ms\":%llu,\"future_host_times\":%llu,\"cpu_user_s\":%.6f,\"cpu_system_s\":%.6f,\"rss_peak_bytes\":%ld,\"hail_peak_bytes\":%llu,\"steady_hail_allocations\":0}\n",
        (double)(end_ns-start_ns)/1e9,rate,channels,pcm_kind,(unsigned long long)atomic_load(&captured),(unsigned long long)nonzero,(unsigned long long)gaps,(unsigned long long)resets,(unsigned long long)steady_underrun,(unsigned long long)baseline_underrun,(unsigned long long)steady_loss,(double)(flush_end-flush_begin)/1000.0,(unsigned long long)(after_capture-before_capture),(double)poll_max_ns/1000.0,age_count?(double)age_total/(double)age_count/1000.0:0.0,(double)age_max/1000.0,(unsigned long long)age_percentile(50),(unsigned long long)age_percentile(95),(unsigned long long)age_percentile(99),(unsigned long long)future_times,(double)usage.ru_utime.tv_sec+(double)usage.ru_utime.tv_usec/1e6,(double)usage.ru_stime.tv_sec+(double)usage.ru_stime.tv_usec/1e6,usage.ru_maxrss,(unsigned long long)peak_bytes);
    if(input && (!atomic_load(&captured) || gaps || resets!=1)) ok=0;
    if(steady_underrun || steady_loss) ok=0;
    /* Repeat construction and active retirement in the same live context,
     * rather than relying only on process exit to reclaim native resources. */
    for(i=0;i<8;++i) {
        uint64_t cycle_alloc=allocations;
        CHECK(ha_stream_open(context,&sc,&stream)==HA_OK);
        generation=1; position=0;
        if(output) fill_playback(generation,rate,&position);
        CHECK(ha_stream_start(stream)==HA_PENDING); wait_state(HA_RUNNING);
        run_for(20000000u,generation,rate,output!=0,&position);
        CHECK(allocations==cycle_alloc+1u);
        CHECK(ha_stream_close_begin(stream)==HA_PENDING); deadline=now()+10000000000ull;
        do { progress(); CHECK(now()<deadline); r=ha_stream_close_finish(&stream);
             CHECK(r==HA_OK || r==HA_PENDING); if(stream) pause_control(); } while(stream);
        CHECK(allocations-frees==1u);
    }
    puts("active_reopen_close_cycles=8 balanced_stream_allocations=PASS");
    puts(ok?"RESULT PASS physical transport (no known-PCM or physical latency claim)":"RESULT FAIL physical transport");
 destroy:
    { uint64_t deadline=now()+10000000000ull;
      do { r=ha_context_destroy(&context); CHECK(r==HA_OK || r==HA_PENDING); CHECK(now()<deadline); if(context) pause_control(); } while(context); }
    CHECK(!live_bytes && allocations==frees); return ok?0:1;
}
