/* Explicit opt-in native test. Never registered as an automatic audio CTest.
 * Only public Hail operations; SDK queries below describe the selected devices.
 * No captured PCM is written to disk. */
#define COBJMACROS
#define CONST_VTABLE
#define _WIN32_WINNT 0x0A00
#include <hail.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <psapi.h>
#include <malloc.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define Q 256u
#define NS UINT64_C(1000000000)
#define CHECK(x) do { if (!(x)) fail(#x,__LINE__); } while (0)
typedef union pcm_buffer { float f[Q*2]; int16_t s[Q*2]; } pcm_buffer;
typedef struct allocation { void *p; size_t bytes, alignment; } allocation;
static allocation ledger[HA_MAX_STREAMS+1];
static uint64_t allocations, frees, live_bytes, peak_bytes, frequency;
static ha_context *context;
static ha_stream *stream;
static uint32_t rate=48000, channels=2, pcm=HA_PCM_F32, directions=HA_CAPTURE|HA_PLAYBACK;
static unsigned seconds=5, physical, unprimed, instrumented;
static unsigned lifecycle_cycles;
static unsigned low_grid;
static atomic_uint quit, write_generation, hold_capture, io_failed, io_ready, expect_loss;
static atomic_ullong captured;
/* The I/O thread owns all plain fields below until it has joined. */
static uint64_t verified[4], zero_frames, nonzero_frames, bad;
static atomic_ullong metadata_gaps, clock_changes;
static atomic_uint phase;
static uint64_t host_valid_blocks, device_valid_blocks, silent_blocks, unknown_gap_blocks;
static uint64_t capture_age_hist[4096], capture_age_count, capture_age_estimated, capture_age_future;
static uint64_t capture_age_min=UINT64_MAX, capture_age_max;
static uint64_t last_frame, last_epoch, last_raw, last_host, capture_blocks, reference_frames, reference_silence;
static uint32_t last_flags, last_marker, last_code, seen, gap_credit;
static uint64_t poll_hist[1024], io_hist[1024], poll_calls, poll_max, io_max;
static double capture_peak, capture_square;
static uint64_t capture_samples;
static uint64_t observation_ns, marker_write_ns[4], marker_capture_ns[4], marker_last_capture_ns[4];
static HANDLE io_thread;
static void fail(const char *expression,int line);
static uint64_t now_ns(void)
{
    LARGE_INTEGER t; CHECK(QueryPerformanceCounter(&t));
    return (uint64_t)t.QuadPart/frequency*NS + (uint64_t)t.QuadPart%frequency*NS/frequency;
}
static void fail(const char *expression,int line)
{
    fprintf(stderr,"FAIL line=%d check=%s native_error=%lu\n",line,expression,(unsigned long)GetLastError());
    fflush(NULL);
    /* OS process teardown retires native threads. Never free storage on timeout. */
    ExitProcess(1);
}
static void *HA_CALL allocate(void *u,size_t bytes,size_t alignment)
{
    unsigned i; void *p; (void)u;
    for(i=0;i<HA_MAX_STREAMS+1 && ledger[i].p;++i) {}
    CHECK(i<HA_MAX_STREAMS+1); p=_aligned_malloc(bytes,alignment); if(!p) return NULL;
    ledger[i].p=p; ledger[i].bytes=bytes; ledger[i].alignment=alignment;
    ++allocations; live_bytes+=bytes; if(live_bytes>peak_bytes) peak_bytes=live_bytes;
    return p;
}
static void HA_CALL deallocate(void *u,void *p,size_t bytes,size_t alignment)
{
    unsigned i; (void)u;
    for(i=0;i<HA_MAX_STREAMS+1 && ledger[i].p!=p;++i) {}
    CHECK(i<HA_MAX_STREAMS+1 && ledger[i].bytes==bytes && ledger[i].alignment==alignment);
    ledger[i].p=NULL; ++frees; live_bytes-=bytes; _aligned_free(p);
}
static void json_string(const char *s)
{
    const unsigned char *p=(const unsigned char *)s; putchar('"');
    for(;*p;++p) { if(*p=='"' || *p=='\\') { putchar('\\'); putchar(*p); }
        else if(*p<32) printf("\\u%04x",*p); else putchar(*p); }
    putchar('"');
}
static void enumerate(ha_device_info *items,uint32_t *count)
{
    unsigned i; for(i=0;i<HA_MAX_DEVICES;++i) { ha_device_info v=HA_INIT(ha_device_info); items[i]=v; }
    CHECK(ha_context_enumerate(context,items,HA_MAX_DEVICES,count)==HA_OK);
}
static ha_device_selector select_key(const char *key,uint32_t direction)
{
    ha_device_info items[HA_MAX_DEVICES]; uint32_t n,i,matches=0;
    ha_device_selector s=HA_INIT(ha_device_selector);
    CHECK(key && strlen(key)<HA_DEVICE_KEY_CAPACITY); enumerate(items,&n);
    for(i=0;i<n;++i) if(items[i].direction==direction && !strcmp(items[i].persistent_key,key)) ++matches;
    CHECK(matches==1); s.kind=HA_SELECT_PERSISTENT_KEY; s.key_bytes=(uint32_t)strlen(key);
    memcpy(s.key,key,s.key_bytes+1u); return s;
}
static void describe(const char *key,uint32_t direction)
{
    static const GUID cls={0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
    static const GUID en={0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
    static const GUID cl={0x1cb9ad4c,0xdbfa,0x4c32,{0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}};
    static const GUID vol={0x5cdf2c82,0x841e,0x4546,{0x97,0x22,0x0c,0xf7,0x40,0x78,0x22,0x9a}};
    static const GUID integer={1,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
    static const GUID floating={3,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
    IMMDeviceEnumerator *e=NULL; IMMDevice *d=NULL; IAudioClient *c=NULL; WAVEFORMATEX *mix=NULL;
    IAudioEndpointVolume *volume=NULL; float gain=0; BOOL mute=FALSE; UINT32 buffer_frames=0; WAVEFORMATEXTENSIBLE w;
    REFERENCE_TIME normal=0,minimum=0; WCHAR wide_key[HA_DEVICE_KEY_CAPACITY]; HRESULT hr;
    CHECK(SUCCEEDED(CoInitializeEx(NULL,COINIT_MULTITHREADED)));
    CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,key,-1,wide_key,HA_DEVICE_KEY_CAPACITY)>0);
    hr=CoCreateInstance(&cls,NULL,CLSCTX_ALL,&en,(void **)&e);
    if(SUCCEEDED(hr)) hr=IMMDeviceEnumerator_GetDevice(e,wide_key,&d);
    if(SUCCEEDED(hr)) hr=IMMDevice_Activate(d,&cl,CLSCTX_ALL,NULL,(void **)&c);
    if(SUCCEEDED(hr)) hr=IAudioClient_GetMixFormat(c,&mix);
    if(SUCCEEDED(hr)) hr=IAudioClient_GetDevicePeriod(c,&normal,&minimum);
    CHECK(SUCCEEDED(hr));
    printf("native direction=%u mix_rate=%lu mix_channels=%u mix_bits=%u mix_tag=%u period_100ns=%lld minimum_100ns=%lld physical_format=unknown\n",
        direction,(unsigned long)mix->nSamplesPerSec,mix->nChannels,mix->wBitsPerSample,mix->wFormatTag,(long long)normal,(long long)minimum);
    CHECK(SUCCEEDED(IMMDevice_Activate(d,&vol,CLSCTX_ALL,NULL,(void **)&volume)));
    CHECK(SUCCEEDED(IAudioEndpointVolume_GetMasterVolumeLevelScalar(volume,&gain)));
    CHECK(SUCCEEDED(IAudioEndpointVolume_GetMute(volume,&mute)));
    printf("endpoint direction=%u master_gain=%.9g muted=%u\n",direction,(double)gain,(unsigned)mute);
    if(!physical) {
        CHECK(mix->nSamplesPerSec==rate && mix->nChannels==channels && gain==1.0f && !mute);
        { UINT channel_count=0,j; CHECK(SUCCEEDED(IAudioEndpointVolume_GetChannelCount(volume,&channel_count)));
          for(j=0;j<channel_count;++j) CHECK(SUCCEEDED(IAudioEndpointVolume_GetChannelVolumeLevelScalar(volume,j,&gain)) && gain==1.0f); }
    }
    IAudioEndpointVolume_Release(volume);
    memset(&w,0,sizeof(w)); w.Format.wFormatTag=WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels=(WORD)channels; w.Format.nSamplesPerSec=rate; w.Format.wBitsPerSample=pcm==HA_PCM_S16?16:32;
    w.Format.nBlockAlign=(WORD)(channels*w.Format.wBitsPerSample/8u); w.Format.nAvgBytesPerSec=rate*w.Format.nBlockAlign;
    w.Format.cbSize=22; w.Samples.wValidBitsPerSample=w.Format.wBitsPerSample;
    w.dwChannelMask=channels==1?4u:3u; w.SubFormat=pcm==HA_PCM_S16?integer:floating;
    CHECK(SUCCEEDED(IAudioClient_Initialize(c,AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,200000,0,&w.Format,NULL)));
    CHECK(SUCCEEDED(IAudioClient_GetBufferSize(c,&buffer_frames)));
    printf("native direction=%u requested_buffer_100ns=200000 observed_buffer_frames=%u application_rate=%u application_channels=%u application_pcm=%u\n",direction,buffer_frames,rate,channels,pcm);
    CoTaskMemFree(mix); IAudioClient_Release(c); IMMDevice_Release(d); IMMDeviceEnumerator_Release(e); CoUninitialize();
}
static void progress(void)
{
    ha_event events[64]; unsigned i; uint32_t n; uint64_t begin=now_ns(),elapsed;
    for(i=0;i<64;++i) { ha_event v=HA_INIT(ha_event); events[i]=v; }
    CHECK(ha_context_poll(context,events,64,&n)==HA_OK);
    elapsed=now_ns()-begin; ++poll_calls; ++poll_hist[elapsed/1000000u<1023?elapsed/1000000u:1023];
    if(elapsed>poll_max) poll_max=elapsed;
}
static ha_stream_status status(void)
{
    ha_stream_status s=HA_INIT(ha_stream_status); CHECK(ha_stream_get_status(stream,&s)==HA_OK);
    if(s.state==HA_FAILED) fprintf(stderr,"terminal=%d native=%lld\n",s.terminal_result,(long long)s.diagnostic.native_code);
    CHECK(s.state!=HA_FAILED && !atomic_load(&io_failed)); return s;
}
static double sample(const pcm_buffer *p,uint32_t i)
{ return pcm==HA_PCM_F32?(double)p->f[i]:(double)p->s[i]/32768.0; }
static void verify_frame(double left,double right)
{
    uint32_t value,marker,code,span=low_grid?2048u:8192u;
    if(!isfinite(left) || !isfinite(right)) { ++bad; return; }
    if(fabs(left)>capture_peak) capture_peak=fabs(left);
    capture_square+=left*left; ++capture_samples;
    if(left==0 && right==0) { ++zero_frames; return; }
    ++nonzero_frames;
    if(physical) return;
    if(left<(double)span/32768.0 || left>=(double)(span*4u)/32768.0 || right!=-left || left*32768.0!=(double)(uint32_t)(left*32768.0)) {
        if(bad<8) printf("oracle_sample_failure phase=%u left_s16_grid=%.17g right_s16_grid=%.17g\n",atomic_load(&phase),left*32768.0,right*32768.0);
        ++bad; return;
    }
    value=(uint32_t)(left*32768.0); marker=value/span; code=value&(span-1u);
    if(marker<1 || marker>3 || (seen && marker<last_marker)) { ++bad; return; }
    if(seen && marker==last_marker && code!=((last_code+1u)&(span-1u))) {
        if(instrumented && gap_credit && ((code-last_code-1u)&(span-1u))<span/2u) gap_credit=0;
        else {
            if(bad<8) printf("oracle_sequence_failure phase=%u marker=%u code=%u previous_code=%u gap_credit=%u\n",atomic_load(&phase),marker,code,last_code,gap_credit);
            ++bad;
        }
    }
    seen=1; last_marker=marker; last_code=code; ++verified[marker];
    if(!marker_capture_ns[marker]) marker_capture_ns[marker]=observation_ns;
    marker_last_capture_ns[marker]=observation_ns;
}
static void read_capture(void)
{
    pcm_buffer p; uint32_t n,i,attempt;
    for(attempt=0;attempt<64;++attempt) {
        ha_block_info b=HA_INIT(ha_block_info); ha_result r=ha_stream_read(stream,&p,Q,&n,&b);
        if(r==HA_WOULD_BLOCK || r==HA_BUSY) return;
        if(r==HA_INVALID_STATE && atomic_load(&expect_loss)) return;
        if(r!=HA_OK || !n || n>Q) { atomic_store(&io_failed,1); return; }
        if(capture_blocks) {
            uint32_t marked=b.flags&(HA_BLOCK_GAP|HA_BLOCK_DISCONTINUITY|HA_BLOCK_UNKNOWN_GAP);
            if(b.first_frame!=last_frame) { ++metadata_gaps; if(!marked) ++bad; gap_credit=1; }
            if(b.clock_epoch!=last_epoch) {
                ++clock_changes; if(b.clock_epoch<=last_epoch || !(b.flags&HA_BLOCK_DISCONTINUITY)) ++bad;
                gap_credit=1;
            }
            if(b.clock_epoch==last_epoch && (b.flags&last_flags&HA_TIME_DEVICE_VALID) && b.device_position!=last_raw && !marked) ++bad;
            if(b.clock_epoch==last_epoch && (b.flags&last_flags&HA_TIME_HOST_VALID) && b.host_time_ns<last_host) ++bad;
            if(marked) gap_credit=1;
            if(marked || b.first_frame!=last_frame || b.clock_epoch!=last_epoch)
                printf("capture_transition phase=%u logical=%llu expected_logical=%llu epoch=%llu previous_epoch=%llu raw=%llu expected_raw=%llu host_ns=%llu previous_host_ns=%llu flags=%u lost=%llu\n",
                    atomic_load(&phase),(unsigned long long)b.first_frame,(unsigned long long)last_frame,
                    (unsigned long long)b.clock_epoch,(unsigned long long)last_epoch,(unsigned long long)b.device_position,
                    (unsigned long long)last_raw,(unsigned long long)b.host_time_ns,(unsigned long long)last_host,b.flags,(unsigned long long)b.lost_frames);
        }
        if(b.flags&HA_TIME_HOST_VALID) ++host_valid_blocks;
        if(b.flags&HA_TIME_DEVICE_VALID) ++device_valid_blocks;
        if(b.flags&HA_BLOCK_SILENCE) ++silent_blocks;
        if(b.flags&HA_BLOCK_UNKNOWN_GAP) ++unknown_gap_blocks;
        ++capture_blocks; last_frame=b.first_frame+n; last_epoch=b.clock_epoch;
        last_raw=b.device_position+n; last_host=b.host_time_ns; last_flags=b.flags;
        atomic_fetch_add(&captured,n);
        observation_ns=now_ns();
        if(atomic_load(&phase)==1u && (b.flags&HA_TIME_HOST_VALID)) {
            if(observation_ns<b.host_time_ns) ++capture_age_future;
            else {
                uint64_t age=observation_ns-b.host_time_ns, bucket=age/100000u;
                ++capture_age_count; ++capture_age_hist[bucket<4095?bucket:4095];
                if(b.flags&HA_TIME_ESTIMATED) ++capture_age_estimated;
                if(age<capture_age_min) capture_age_min=age;
                if(age>capture_age_max) capture_age_max=age;
            }
        }
        for(i=0;i<n;++i) {
            double left=sample(&p,i*channels), right=channels==2?sample(&p,i*channels+1u):-left;
            verify_frame(left,right);
        }
    }
}
static void drain_reference(void)
{
    unsigned attempt; pcm_buffer p;
    for(attempt=0;attempt<64;++attempt) {
        uint32_t n,i; ha_block_info b=HA_INIT(ha_block_info);
        ha_result r=ha_stream_read_reference(stream,&p,Q,&n,&b);
        if(r==HA_WOULD_BLOCK || r==HA_BUSY) return;
        if(r==HA_INVALID_STATE && atomic_load(&expect_loss)) return;
        if(r!=HA_OK || !n || n>Q) { atomic_store(&io_failed,1); return; }
        reference_frames+=n;
        if(b.flags&HA_BLOCK_SILENCE) for(i=0;i<n;++i) if(sample(&p,i*channels)==0) ++reference_silence;
        if(b.flags&HA_TIME_DEVICE_VALID) ++bad; /* WASAPI render does not report DAC frames. */
    }
}
static void pattern(pcm_buffer *p,uint32_t marker,uint32_t position)
{
    uint32_t i,c,span=low_grid?2048u:8192u;
    for(i=0;i<Q;++i) {
        int32_t value=(int32_t)(marker*span+((position+i)&(span-1u)));
        if(physical) { int32_t t=(int32_t)((position+i)%96u); value=(t<48?t:96-t)*21-504; }
        for(c=0;c<channels;++c) {
            int32_t v=c?-value:value;
            if(pcm==HA_PCM_F32) p->f[i*channels+c]=(float)v/32768.0f;
            else p->s[i*channels+c]=(int16_t)v;
        }
    }
}
static DWORD WINAPI io_worker(void *unused)
{
    uint32_t generation=1,position=unprimed?0u:8u*Q; uint64_t previous=now_ns(); (void)unused;
    atomic_store(&io_ready,1);
    while(!atomic_load(&quit)) {
        unsigned attempt; uint32_t wanted=atomic_load(&write_generation); uint64_t current=now_ns(),elapsed=current-previous;
        previous=current; ++io_hist[elapsed/1000000u<1023?elapsed/1000000u:1023]; if(elapsed>io_max) io_max=elapsed;
        if((directions&HA_CAPTURE) && !atomic_load(&hold_capture)) read_capture();
        if(directions&HA_PLAYBACK) drain_reference();
        if(wanted && (directions&HA_PLAYBACK)) {
            if(wanted!=generation) { generation=wanted; position=0; }
            for(attempt=0;attempt<8;++attempt) {
                pcm_buffer p; uint32_t n=0; ha_result r; uint64_t write_time=now_ns(); pattern(&p,generation,position);
                r=ha_stream_write(stream,&p,Q,generation,&n);
                if(r==HA_WOULD_BLOCK || r==HA_BUSY || r==HA_STALE_GENERATION) break;
                if(r==HA_INVALID_STATE && atomic_load(&expect_loss)) break;
                if(r!=HA_OK) { atomic_store(&io_failed,1); break; }
                if(n && generation<=3 && !marker_write_ns[generation]) marker_write_ns[generation]=write_time;
                position+=n;
            }
        }
        Sleep(1);
    }
    return 0;
}
static void launch_io(uint32_t generation)
{
    atomic_store(&quit,0); atomic_store(&io_ready,0); atomic_store(&write_generation,generation);
    io_thread=CreateThread(NULL,0,io_worker,NULL,0,NULL); CHECK(io_thread);
    while(!atomic_load(&io_ready)) Sleep(1);
}
static void join_io(void)
{
    atomic_store(&quit,1); CHECK(WaitForSingleObject(io_thread,5000)==WAIT_OBJECT_0);
    CHECK(CloseHandle(io_thread)); io_thread=NULL; CHECK(!atomic_load(&io_failed));
}
static void run_for(unsigned ms)
{
    uint64_t deadline=now_ns()+(uint64_t)ms*1000000u;
    while(now_ns()<deadline) { progress(); (void)status(); Sleep(1); }
}
static void await_state(uint32_t wanted)
{
    uint64_t deadline=now_ns()+5u*NS;
    while(status().state!=wanted) { CHECK(now_ns()<deadline); progress(); Sleep(1); }
}
static void close_stream(void)
{
    uint64_t deadline=now_ns()+5u*NS; ha_result r;
    CHECK(ha_stream_close_begin(stream)==HA_PENDING);
    do { progress(); r=ha_stream_close_finish(&stream); CHECK(r==HA_OK || r==HA_PENDING); CHECK(now_ns()<deadline); Sleep(1); } while(r==HA_PENDING);
    CHECK(!stream && ha_stream_close_finish(&stream)==HA_OK);
}
static unsigned number(const char *s,unsigned min,unsigned max)
{
    char *end; unsigned long n; CHECK(s[0]>='0' && s[0]<='9'); n=strtoul(s,&end,10);
    CHECK(!*end && n>=min && n<=max); return (unsigned)n;
}
static int oracle_self_test(void)
{
    uint32_t format,c,m,i; uint64_t previous_bad;
    /* Hand-authored values independently exercise the oracle's negative paths. */
    verify_frame(0,0); CHECK(!nonzero_frames && !verified[1] && !verified[2] && !verified[3]);
    verify_frame(0.25,-0.25); verify_frame(8193.0/32768.0,-8193.0/32768.0); CHECK(!bad && verified[1]==2);
    previous_bad=bad; verify_frame(0.25,0.25); CHECK(bad==previous_bad+1);
    previous_bad=bad; verify_frame(8195.0/32768.0,-8195.0/32768.0); CHECK(bad==previous_bad+1);
    verify_frame(0.5,-0.5); previous_bad=bad; verify_frame(0.25,-0.25); CHECK(bad==previous_bad+1);
    previous_bad=bad; verify_frame(0.25001,-0.25001); CHECK(bad==previous_bad+1);
    for(format=HA_PCM_S16;format<=HA_PCM_F32;++format) for(c=1;c<=2;++c) {
        pcm=format; channels=c; bad=0; seen=0;
        for(m=1;m<=3;++m) {
            pcm_buffer p; pattern(&p,m,8190);
            for(i=0;i<Q;++i) { double l=sample(&p,i*c); verify_frame(l,c==2?sample(&p,i*c+1u):-l); }
        }
        CHECK(!bad);
    }
    puts("PASS independent oracle rejects zeros-only evidence, channel corruption, skipped samples, old generations and rounding errors");
    return 0;
}
static HANDLE loss_event(const char *prefix,const char *suffix)
{
    char name[192]; CHECK(strlen(prefix)<128);
    CHECK(snprintf(name,sizeof(name),"%s-%s",prefix,suffix)>0);
    { HANDLE event=OpenEventA(EVENT_MODIFY_STATE|SYNCHRONIZE,FALSE,name); CHECK(event); return event; }
}
static int loss_trial(const char *prefix,const ha_stream_config *config,const ha_stream_info *original)
{
    HANDLE ready=loss_event(prefix,"ready"),closed=loss_event(prefix,"closed"),restored=loss_event(prefix,"restored");
    ha_stream_status s=HA_INIT(ha_stream_status); ha_stream_info replacement=HA_INIT(ha_stream_info);
    uint64_t deadline; uint32_t count,i; ha_device_info items[HA_MAX_DEVICES];
    CHECK(directions==(HA_CAPTURE|HA_PLAYBACK));
    CHECK(ha_stream_start(stream)==HA_PENDING); await_state(HA_RUNNING);
    launch_io(1); run_for(1000); atomic_store(&expect_loss,1); CHECK(SetEvent(ready)); deadline=now_ns()+30u*NS;
    do {
        progress(); CHECK(ha_stream_get_status(stream,&s)==HA_OK); CHECK(now_ns()<deadline); Sleep(1);
    } while(s.state!=HA_FAILED);
    CHECK(s.terminal_result==HA_DEVICE_LOST);
    join_io(); CHECK(ha_context_refresh(context)==HA_OK); enumerate(items,&count);
    for(i=0;i<count;++i) CHECK(items[i].runtime_id!=original->capture_device_id && items[i].runtime_id!=original->playback_device_id);
    printf("loss terminal=%d native=%lld old_capture_id=%llu old_render_id=%llu\n",s.terminal_result,(long long)s.diagnostic.native_code,
        (unsigned long long)original->capture_device_id,(unsigned long long)original->playback_device_id);
    close_stream(); CHECK(SetEvent(closed));
    CHECK(WaitForSingleObject(restored,30000)==WAIT_OBJECT_0);
    CHECK(ha_context_refresh(context)==HA_OK);
    CHECK(ha_stream_open(context,config,&stream)==HA_OK && ha_stream_get_info(stream,&replacement)==HA_OK);
    CHECK(replacement.capture_device_id!=original->capture_device_id && replacement.playback_device_id!=original->playback_device_id);
    CHECK(replacement.capture_clock_domain!=original->capture_clock_domain && replacement.playback_clock_domain!=original->playback_clock_domain);
    CHECK(!bad);
    capture_blocks=0; seen=0; gap_credit=0; memset(verified,0,sizeof(verified)); atomic_store(&expect_loss,0);
    { uint64_t previous_nonzero=nonzero_frames;
      CHECK(ha_stream_start(stream)==HA_PENDING); await_state(HA_RUNNING); launch_io(1); run_for(1000); join_io();
      CHECK(!bad && (physical?nonzero_frames-previous_nonzero>rate/3u:verified[1]>rate/3u)); }
    close_stream();
    CHECK(ha_context_destroy(&context)==HA_OK && allocations==frees && !live_bytes);
    CHECK(CloseHandle(ready) && CloseHandle(closed) && CloseHandle(restored));
    printf("PASS explicit loss/reopen capture_id=%llu render_id=%llu allocations=%llu frees=%llu\n",
        (unsigned long long)replacement.capture_device_id,(unsigned long long)replacement.playback_device_id,
        (unsigned long long)allocations,(unsigned long long)frees); return 0;
}
static int lifecycle_trial(const ha_stream_config *config)
{
    unsigned i;
    for(i=0;i<lifecycle_cycles;++i) {
        uint64_t old_generation=status().playback_generation,flush_generation=0;
        CHECK(ha_stream_start(stream)==HA_PENDING); progress();
        if(i%3u==2u) {
            await_state(HA_RUNNING);
            if(directions&HA_PLAYBACK) {
                CHECK(ha_stream_flush_playback(stream,HA_FLUSH_NATIVE_REQUIRED,&flush_generation)==HA_PENDING);
                progress();
            }
        }
        if(i%3u!=1u) {
            CHECK(ha_stream_stop(stream)==HA_PENDING); await_state(HA_STOPPED);
            CHECK(status().playback_generation>old_generation);
            if(directions&HA_PLAYBACK) {
                pcm_buffer p; uint32_t n=99; pattern(&p,1,0);
                CHECK(ha_stream_write(stream,&p,Q,old_generation,&n)==HA_STALE_GENERATION && !n);
                if(flush_generation) CHECK(status().flush_completed_generation==flush_generation &&
                    (status().flush_result==HA_CANCELLED || status().flush_result==HA_OK));
            }
        }
        close_stream();
        printf("lifecycle_cycle=%u transition=%s balanced_stream_bytes=%llu\n",i,
            i%3u==0u?"stop-during-start":i%3u==1u?"close-during-start":"stop-during-flush",
            (unsigned long long)live_bytes);
        if(i+1u<lifecycle_cycles) CHECK(ha_stream_open(context,config,&stream)==HA_OK);
    }
    CHECK(ha_context_destroy(&context)==HA_OK && ha_context_destroy(&context)==HA_OK && allocations==frees && !live_bytes);
    printf("PASS lifecycle_cycles=%u allocations=%llu frees=%llu ledger_bytes=0\n",lifecycle_cycles,
        (unsigned long long)allocations,(unsigned long long)frees); return 0;
}
int main(int argc,char **argv)
{
    ha_context_config cc=HA_INIT(ha_context_config); ha_stream_config sc=HA_INIT(ha_stream_config);
    ha_allocator a=HA_INIT(ha_allocator); ha_memory_plan cp=HA_INIT(ha_memory_plan),sp=HA_INIT(ha_memory_plan);
    ha_stream_info info=HA_INIT(ha_stream_info); ha_diagnostic diagnostic=HA_INIT(ha_diagnostic);
    const char *capture_key=NULL,*playback_key=NULL,*loss_events=NULL; unsigned i,list=0; uint32_t generation=1;
    uint64_t open_allocations,begin,flush_ns=0,flush_request_ns=0,capture_at_flush=0,flush_capture_progress=0;
    uint64_t baseline_gaps,baseline_clocks,steady_gaps,steady_clocks,preflush_gaps,preflush_clocks,flush_gaps=0,flush_clocks=0;
    ha_stream_status baseline,after,starved,overflow; LARGE_INTEGER f; PROCESS_MEMORY_COUNTERS memory;
    FILETIME create,exit_time,kernel0,user0,kernel1,user1; uint64_t cpu0,cpu1;
    CHECK(QueryPerformanceFrequency(&f) && f.QuadPart>0); frequency=(uint64_t)f.QuadPart;
    CHECK(frequency<=UINT64_MAX/NS);
    CHECK(setvbuf(stdout,NULL,_IONBF,0)==0);
    if(argc==2 && !strcmp(argv[1],"--self-test")) return oracle_self_test();
    for(i=1;i<(unsigned)argc;++i) {
        if(!strcmp(argv[i],"--list")) list=1;
        else if(!strcmp(argv[i],"--physical")) physical=1;
        else if(!strcmp(argv[i],"--unprimed")) unprimed=1;
        else if(!strcmp(argv[i],"--instrumented")) instrumented=1;
        else if(!strcmp(argv[i],"--low-grid")) low_grid=1;
        else {
            CHECK(i+1u<(unsigned)argc);
            if(!strcmp(argv[i],"--capture")) capture_key=argv[++i];
            else if(!strcmp(argv[i],"--playback")) playback_key=argv[++i];
            else if(!strcmp(argv[i],"--loss-events")) loss_events=argv[++i];
            else if(!strcmp(argv[i],"--seconds")) seconds=number(argv[++i],1,3600);
            else if(!strcmp(argv[i],"--lifecycle-cycles")) lifecycle_cycles=number(argv[++i],1,256);
            else if(!strcmp(argv[i],"--rate")) rate=number(argv[++i],8000,192000);
            else if(!strcmp(argv[i],"--channels")) channels=number(argv[++i],1,2);
            else if(!strcmp(argv[i],"--pcm")) { ++i; CHECK(!strcmp(argv[i],"s16") || !strcmp(argv[i],"f32")); pcm=!strcmp(argv[i],"s16")?HA_PCM_S16:HA_PCM_F32; }
            else CHECK(0);
        }
    }
    a.allocate=allocate; a.deallocate=deallocate; cc.allocator=a; cc.backend=HA_BACKEND_WASAPI;
    printf("numerical_profile=%s\n",low_grid?"transparent-s16-grid-low-v1":"transparent-s16-grid");
    cc.max_devices=HA_MAX_DEVICES; cc.max_streams=2; cc.event_capacity=128; cc.memory_limit=4u*1024u*1024u; cc.diagnostic=&diagnostic;
    CHECK(ha_context_plan(&cc,&cp)==HA_OK); CHECK(ha_context_create(&cc,&context)==HA_OK);
    CHECK(live_bytes==cp.bytes);
    if(list) {
        ha_device_info items[HA_MAX_DEVICES]; uint32_t n; enumerate(items,&n);
        for(i=0;i<n;++i) { printf("{\"direction\":%u,\"key\":",items[i].direction); json_string(items[i].persistent_key);
            printf(",\"name\":"); json_string(items[i].name); printf("}\n"); }
        CHECK(ha_context_destroy(&context)==HA_OK); CHECK(!live_bytes && allocations==frees); return 0;
    }
    directions=(capture_key?HA_CAPTURE:0u)|(playback_key?HA_PLAYBACK:0u);
    CHECK(directions && (physical || directions==(HA_CAPTURE|HA_PLAYBACK)));
    sc.directions=directions; sc.negotiation=HA_EXACT_APPLICATION;
    sc.quantum_frames=Q; sc.capture_slots=capture_key?32u:0u; sc.playback_slots=playback_key?32u:0u; sc.reference_slots=playback_key?64:0;
    { ha_format format=HA_FORMAT_INIT(rate,channels,pcm); sc.capture_format=format; sc.playback_format=format; }
    if(capture_key) { sc.capture_device=select_key(capture_key,HA_CAPTURE); describe(capture_key,HA_CAPTURE); }
    if(playback_key) { sc.playback_device=select_key(playback_key,HA_PLAYBACK); describe(playback_key,HA_PLAYBACK); }
    sc.diagnostic=&diagnostic;
    CHECK(ha_stream_plan(context,&sc,&sp)==HA_OK);
    { ha_result r=ha_stream_open(context,&sc,&stream); if(r!=HA_OK) fprintf(stderr,"open_result=%d native=%lld\n",r,(long long)diagnostic.native_code); CHECK(r==HA_OK); }
    CHECK(ha_stream_get_info(stream,&info)==HA_OK && info.allocated_bytes==sp.bytes && live_bytes==cp.bytes+sp.bytes);
    for(i=0;i<2;++i) if(directions&(i?HA_PLAYBACK:HA_CAPTURE)) {
        const ha_probe_info *negotiated=i?&info.playback_negotiated:&info.capture_negotiated;
        printf("negotiated direction=%u app_rate=%u app_channels=%u app_pcm=%u engine_valid=%u engine_rate=%u engine_channels=%u engine_pcm=%u physical_valid=%u conversion=%u\n",
            i?HA_PLAYBACK:HA_CAPTURE,negotiated->application_format.sample_rate,negotiated->application_format.channels,
            negotiated->application_format.sample_format,negotiated->backend_format_valid,negotiated->backend_format.sample_rate,
            negotiated->backend_format.channels,negotiated->backend_format.sample_format,negotiated->device_format_valid,negotiated->conversion);
    }
    printf("profile=%s physical=%u rate=%u channels=%u pcm=%u quantum=%u capture_slots=%u playback_slots=%u reference_slots=%u seconds=%u unprimed=%u conversion_capture=%u conversion_playback=%u clock_relationship=%u capture_domain=%llu render_domain=%llu\n",
        instrumented?"instrumented":"strict",physical,rate,channels,pcm,Q,info.capture_slots,info.playback_slots,info.reference_slots,seconds,unprimed,
        info.capture_negotiated.conversion,info.playback_negotiated.conversion,info.clock_relationship,
        (unsigned long long)info.capture_clock_domain,(unsigned long long)info.playback_clock_domain);
    open_allocations=allocations;
    if(loss_events) return loss_trial(loss_events,&sc,&info);
    if(lifecycle_cycles) return lifecycle_trial(&sc);
    if(playback_key && !unprimed) for(i=0;i<8;++i) { pcm_buffer p; uint32_t n; pattern(&p,1,i*Q);
        if(!i) marker_write_ns[1]=now_ns(); CHECK(ha_stream_write(stream,&p,Q,1,&n)==HA_OK && n==Q); }
    CHECK(ha_stream_start(stream)==HA_PENDING); await_state(HA_RUNNING);
    if(unprimed && playback_key) Sleep(60);
    launch_io(playback_key?1u:0u); run_for(1000); baseline=status();
    atomic_store(&phase,1); baseline_gaps=atomic_load(&metadata_gaps); baseline_clocks=atomic_load(&clock_changes);
    if(unprimed && playback_key) CHECK(baseline.playback_underrun_frames>0);
    CHECK(GetProcessTimes(GetCurrentProcess(),&create,&exit_time,&kernel0,&user0)); begin=now_ns();
    run_for(seconds*1000u); after=status();
    steady_gaps=atomic_load(&metadata_gaps)-baseline_gaps; steady_clocks=atomic_load(&clock_changes)-baseline_clocks;
    printf("sustained_capture_metadata_gaps=%llu sustained_capture_epoch_changes=%llu startup_underruns=%llu\n",
        (unsigned long long)steady_gaps,(unsigned long long)steady_clocks,(unsigned long long)baseline.playback_underrun_frames);
    CHECK(GetProcessTimes(GetCurrentProcess(),&create,&exit_time,&kernel1,&user1));
    cpu0=(((uint64_t)kernel0.dwHighDateTime<<32)|kernel0.dwLowDateTime)+(((uint64_t)user0.dwHighDateTime<<32)|user0.dwLowDateTime);
    cpu1=(((uint64_t)kernel1.dwHighDateTime<<32)|kernel1.dwLowDateTime)+(((uint64_t)user1.dwHighDateTime<<32)|user1.dwLowDateTime);
    printf("sustained_ns=%llu capture_loss=%llu playback_underruns=%llu reference_loss=%llu process_cpu_ns=%llu\n",
        (unsigned long long)(now_ns()-begin),(unsigned long long)(after.capture_lost_frames-baseline.capture_lost_frames),
        (unsigned long long)(after.playback_underrun_frames-baseline.playback_underrun_frames),
        (unsigned long long)(after.reference_lost_frames-baseline.reference_lost_frames),(unsigned long long)((cpu1-cpu0)*100u));
    if(!instrumented) CHECK(after.capture_lost_frames==baseline.capture_lost_frames && after.playback_underrun_frames==baseline.playback_underrun_frames && after.reference_lost_frames==baseline.reference_lost_frames && !steady_gaps && !steady_clocks);
    if(playback_key) {
        uint64_t new_generation,deadline=now_ns()+5u*NS;
        begin=now_ns(); flush_request_ns=begin; capture_at_flush=atomic_load(&captured);
        atomic_store(&phase,2); preflush_gaps=atomic_load(&metadata_gaps); preflush_clocks=atomic_load(&clock_changes);
        CHECK(ha_stream_flush_playback(stream,HA_FLUSH_NATIVE_REQUIRED,&new_generation)==HA_PENDING);
        while(status().flush_completed_generation!=new_generation) { CHECK(now_ns()<deadline); progress(); Sleep(1); }
        flush_ns=now_ns()-begin; CHECK(status().flush_result==HA_OK && new_generation==2);
        generation=(uint32_t)new_generation; atomic_store(&write_generation,generation);
        run_for(1000); flush_capture_progress=atomic_load(&captured)-capture_at_flush;
        if(capture_key) CHECK(flush_capture_progress>rate/3u);
        /* Join before testing stale writes on the one application writer role. */
        join_io(); { pcm_buffer p; uint32_t n=99; pattern(&p,1,0); CHECK(ha_stream_write(stream,&p,Q,1,&n)==HA_STALE_GENERATION && n==0); }
        flush_gaps=atomic_load(&metadata_gaps)-preflush_gaps; flush_clocks=atomic_load(&clock_changes)-preflush_clocks;
        printf("flush_capture_metadata_gaps=%llu flush_capture_epoch_changes=%llu\n",(unsigned long long)flush_gaps,(unsigned long long)flush_clocks);
        if(!instrumented) CHECK(!flush_gaps && !flush_clocks);
        if(!physical) {
            CHECK(verified[1]>rate/3u && verified[2]>rate/3u);
            if(!instrumented) CHECK(!metadata_gaps && !clock_changes);
        }
    } else join_io();
    CHECK(ha_stream_stop(stream)==HA_PENDING); await_state(HA_STOPPED);
    generation=(uint32_t)status().playback_generation;
    last_frame=last_epoch=last_raw=last_host=0; capture_blocks=0; seen=0; gap_credit=0;
    atomic_store(&phase,3);
    CHECK(ha_stream_start(stream)==HA_PENDING); await_state(HA_RUNNING); launch_io(playback_key?generation:0u); run_for(1000);
    if(playback_key) { atomic_store(&write_generation,0); run_for(400); }
    starved=status();
    if(playback_key) CHECK(starved.playback_underrun_frames>after.playback_underrun_frames);
    atomic_store(&phase,4);
    if(capture_key) { atomic_store(&hold_capture,1); run_for(32u*Q*1000u/rate+200u); atomic_store(&hold_capture,0); run_for(500); }
    overflow=status(); if(capture_key) CHECK(overflow.capture_lost_frames>starved.capture_lost_frames);
    join_io();
    printf("injected_capture_loss_frames=%llu injected_playback_underrun_frames=%llu host_valid_blocks=%llu device_valid_blocks=%llu native_silent_blocks=%llu unknown_gap_blocks=%llu meaningful_capture_minus90dbfs=%u\n",
        (unsigned long long)(overflow.capture_lost_frames-starved.capture_lost_frames),
        (unsigned long long)(starved.playback_underrun_frames-after.playback_underrun_frames),
        (unsigned long long)host_valid_blocks,(unsigned long long)device_valid_blocks,(unsigned long long)silent_blocks,
        (unsigned long long)unknown_gap_blocks,capture_peak>=0.00003162277660168379?1u:0u);
    CHECK(allocations==open_allocations);
    memory.cb=sizeof(memory); CHECK(GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory)));
    printf("capture_frames=%llu nonzero=%llu zeros=%llu peak=%.9g rms=%.9g verified_A=%llu verified_B=%llu verified_C=%llu bad=%llu metadata_gaps=%llu clock_changes=%llu reference_frames=%llu reference_silence=%llu flush_ack_ns=%llu capture_frames_during_flush_trial=%llu poll_calls=%llu poll_max_ns=%llu io_max_ns=%llu hail_peak_bytes=%llu working_set=%llu peak_working_set=%llu\n",
        (unsigned long long)atomic_load(&captured),(unsigned long long)nonzero_frames,(unsigned long long)zero_frames,capture_peak,capture_samples?sqrt(capture_square/(double)capture_samples):0,
        (unsigned long long)verified[1],(unsigned long long)verified[2],(unsigned long long)verified[3],(unsigned long long)bad,
        (unsigned long long)metadata_gaps,(unsigned long long)clock_changes,(unsigned long long)reference_frames,(unsigned long long)reference_silence,
        (unsigned long long)flush_ns,(unsigned long long)flush_capture_progress,(unsigned long long)poll_calls,(unsigned long long)poll_max,(unsigned long long)io_max,
        (unsigned long long)peak_bytes,(unsigned long long)memory.WorkingSetSize,(unsigned long long)memory.PeakWorkingSetSize);
    for(i=0;i<1024;++i) if(poll_hist[i] || io_hist[i]) printf("hist_ms=%u poll=%llu io=%llu\n",i,(unsigned long long)poll_hist[i],(unsigned long long)io_hist[i]);
    printf("capture_delivery phase=sustained correlation_blocks=%llu estimated_blocks=%llu future_correlations=%llu min_ns=%llu max_ns=%llu bucket_ns=100000 physical_latency_claim=0\n",
        (unsigned long long)capture_age_count,(unsigned long long)capture_age_estimated,(unsigned long long)capture_age_future,
        (unsigned long long)(capture_age_count?capture_age_min:0),(unsigned long long)capture_age_max);
    for(i=0;i<4096;++i) if(capture_age_hist[i]) printf("capture_delivery_bucket=%u count=%llu\n",i,(unsigned long long)capture_age_hist[i]);
    if(!physical) {
        for(i=1;i<=3;++i) {
            CHECK(marker_write_ns[i] && marker_capture_ns[i]>=marker_write_ns[i]);
            printf("marker=%u write_to_first_capture_read_ns=%llu\n",i,(unsigned long long)(marker_capture_ns[i]-marker_write_ns[i]));
        }
        printf("last_A_capture_read_relative_to_flush_request_ns=%lld downstream_software_path_only=1\n",
            (long long)((int64_t)marker_last_capture_ns[1]-(int64_t)flush_request_ns));
    }
    if(!physical) CHECK(verified[3]>rate/3u);
    CHECK(!bad); if(capture_key) CHECK(nonzero_frames>rate/3u);
    if(playback_key) CHECK(reference_silence>0);
    close_stream(); CHECK(ha_context_destroy(&context)==HA_OK && !context);
    CHECK(ha_context_destroy(&context)==HA_OK && allocations==frees && !live_bytes);
    printf("PASS allocations=%llu frees=%llu ledger_bytes=%llu\n",(unsigned long long)allocations,(unsigned long long)frees,(unsigned long long)live_bytes);
    return 0;
}
