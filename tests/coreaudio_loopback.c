#define _POSIX_C_SOURCE 200809L
#include <hail.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define Q 256u
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
static ha_context *context;
static ha_stream *stream;
static uint64_t allocations, frees, bytes, peak;
static uint64_t captured, verified[4], zero_frames, bad, frame_gaps, last_frame, epoch;
static uint32_t last_code, last_phase, seen;
static void *HA_CALL allocate(void *user,size_t n,size_t alignment)
{ void *p=NULL; (void)user; if(posix_memalign(&p,alignment,n)) return NULL; ++allocations; bytes+=n; if(bytes>peak) peak=bytes; return p; }
static void HA_CALL deallocate(void *user,void *p,size_t n,size_t alignment)
{ (void)user; (void)alignment; ++frees; bytes-=n; free(p); }
static uint64_t now_ns(void)
{ struct timespec t; CHECK(!clock_gettime(CLOCK_MONOTONIC,&t)); return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec; }
static void pause_control(void) { struct timespec t={0,1000000}; nanosleep(&t,NULL); }
static void progress(void) { uint32_t n; CHECK(ha_context_poll(context,NULL,0,&n)==HA_OK); }
static ha_stream_status status(void)
{
    ha_stream_status s=HA_INIT(ha_stream_status); CHECK(ha_stream_get_status(stream,&s)==HA_OK);
    if(s.state==HA_FAILED) fprintf(stderr,"stream failed: %s native=%lld\n",ha_result_string(s.terminal_result),(long long)s.diagnostic.native_code);
    CHECK(s.state!=HA_FAILED); return s;
}
static void read_capture(void)
{
    float pcm[Q*2u]; uint32_t n,i,attempt;
    for(attempt=0;attempt<64;++attempt) {
        ha_block_info block=HA_INIT(ha_block_info); ha_result r=ha_stream_read(stream,pcm,Q,&n,&block);
        if(r==HA_WOULD_BLOCK || r==HA_BUSY) break;
        CHECK(r==HA_OK);
        if(captured && block.first_frame!=last_frame) ++frame_gaps;
        if(!epoch) epoch=block.clock_epoch;
        if(block.clock_epoch!=epoch) fprintf(stderr,"capture epoch changed %llu -> %llu flags=%u\n",(unsigned long long)epoch,(unsigned long long)block.clock_epoch,block.flags);
        CHECK(block.clock_epoch==epoch);
        captured+=n; last_frame=block.first_frame+n;
        for(i=0;i<n;++i) {
            float left=pcm[i*2u],right=pcm[i*2u+1u]; uint32_t value,phase,code;
            if(left==0 && right==0) { ++zero_frames; continue; }
            if(!(left>=0.125f && left<0.8125f) || right!=-left) { ++bad; continue; }
            value=(uint32_t)(left*1048576.0f); phase=value/262144u;
            if(phase==0 && value>=131072u && value<196608u) { phase=1; code=value-131072u; }
            else if(phase==2 && value<589824u) { phase=2; code=value-524288u; }
            else if(phase==3 && value<851968u) { phase=3; code=value-786432u; }
            else { ++bad; continue; }
            if((float)value/1048576.0f!=left || (seen && phase<last_phase)) ++bad;
            if(seen && phase==last_phase && code!=((last_code+1u)&65535u)) ++bad;
            seen=1; last_phase=phase; last_code=code; ++verified[phase];
        }
    }
}
static void write_pattern(uint32_t phase,uint32_t *position,uint64_t generation)
{
    float pcm[Q*2u]; uint32_t i,n; ha_result r; uint32_t base=phase==1?131072u:phase==2?524288u:786432u;
    for(i=0;i<Q;++i) { pcm[2u*i]=(float)(base+((*position+i)&65535u))/1048576.0f; pcm[2u*i+1u]=-pcm[2u*i]; }
    r=ha_stream_write(stream,pcm,Q,generation,&n); CHECK(r==HA_OK || r==HA_WOULD_BLOCK || r==HA_BUSY);
    if(r==HA_OK) *position+=n;
}
static void start(void)
{
    uint64_t deadline=now_ns()+5000000000u; ha_stream_status s;
    CHECK(ha_stream_start(stream)==HA_PENDING);
    do { progress(); s=status(); CHECK(now_ns()<deadline); pause_control(); } while(s.state==HA_STARTING);
    CHECK(s.state==HA_RUNNING);
}
static void phase(uint32_t marker,uint64_t generation)
{
    uint32_t position=0; uint64_t deadline=now_ns()+1000000000u;
    while(now_ns()<deadline) { progress(); read_capture(); write_pattern(marker,&position,generation); pause_control(); }
    CHECK(verified[marker]>16000 && !bad && !frame_gaps);
}
static void fixture_rate(const char *uid)
{
    AudioDeviceID devices[64],selected=0; UInt32 n=sizeof(devices),i;
    AudioObjectPropertyAddress address={kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,0};
    CHECK(!AudioObjectGetPropertyData(kAudioObjectSystemObject,&address,0,NULL,&n,devices));
    CHECK(n<=sizeof(devices) && n%sizeof(AudioDeviceID)==0);
    for(i=0;i<n/sizeof(AudioDeviceID);++i) {
        CFStringRef value=NULL; UInt32 size=sizeof(value); char key[1024];
        address.mSelector=kAudioDevicePropertyDeviceUID;
        if(!AudioObjectGetPropertyData(devices[i],&address,0,NULL,&size,&value) && value) {
            if(CFStringGetCString(value,key,sizeof(key),kCFStringEncodingUTF8) && !strcmp(key,uid)) selected=devices[i];
            CFRelease(value);
        }
    }
    CHECK(selected);
    address.mSelector=kAudioDevicePropertyNominalSampleRate;
    { Float64 rate=48000; uint64_t deadline=now_ns()+5000000000u;
      CHECK(!AudioObjectSetPropertyData(selected,&address,0,NULL,sizeof(rate),&rate));
      do { UInt32 size=sizeof(rate); CHECK(!AudioObjectGetPropertyData(selected,&address,0,NULL,&size,&rate));
           if(rate==48000) break; CHECK(now_ns()<deadline); pause_control(); } while(1);
    }
}
int main(void)
{
    ha_context_config cc=HA_INIT(ha_context_config); ha_allocator allocator=HA_INIT(ha_allocator);
    ha_device_info devices[HA_MAX_DEVICES]; ha_stream_config sc=HA_INIT(ha_stream_config);
    ha_device_selector selector=HA_INIT(ha_device_selector); ha_format format=HA_FORMAT_INIT(48000,2,HA_PCM_F32);
    ha_stream_status s; uint32_t i,n; uint64_t input=0,output=0,generation=1,steady,deadline,before,flush_begin,flush_ack;
    const char *uid=NULL; float stale[2]={0}; ha_result result;
    CHECK(getenv("HA_TEST_COREAUDIO_LOOPBACK") && !strcmp(getenv("HA_TEST_COREAUDIO_LOOPBACK"),"1"));
    allocator.allocate=allocate; allocator.deallocate=deallocate; cc.allocator=allocator; cc.backend=HA_BACKEND_COREAUDIO;
    result=ha_context_create(&cc,&context); if(result!=HA_OK) fprintf(stderr,"context: %s\n",ha_result_string(result)); CHECK(result==HA_OK);
    deadline=now_ns()+15000000000u;
    do {
        CHECK(ha_context_refresh(context)==HA_OK);
        for(i=0;i<HA_MAX_DEVICES;++i) { ha_device_info d=HA_INIT(ha_device_info); devices[i]=d; }
        CHECK(ha_context_enumerate(context,devices,HA_MAX_DEVICES,&n)==HA_OK);
        for(i=0;i<n;++i) if(!strcmp(devices[i].name,"BlackHole 2ch")) {
            if(devices[i].direction==HA_CAPTURE) { input=devices[i].runtime_id; uid=devices[i].persistent_key; }
            if(devices[i].direction==HA_PLAYBACK) output=devices[i].runtime_id;
        }
        if(input && output) break;
        CHECK(now_ns()<deadline); pause_control();
    } while(1);
    CHECK(uid && uid[0]); fixture_rate(uid);
    printf("fixture=BlackHole 2ch uid=%s rate=48000\n",uid); fflush(stdout);
    selector.kind=HA_SELECT_RUNTIME_ID; selector.runtime_id=input; sc.capture_device=selector;
    selector.runtime_id=output; sc.playback_device=selector; sc.capture_format=format; sc.playback_format=format;
    sc.directions=HA_CAPTURE|HA_PLAYBACK; sc.negotiation=HA_EXACT_APPLICATION; sc.quantum_frames=Q;
    sc.capture_slots=64; sc.playback_slots=8;
    result=ha_stream_open(context,&sc,&stream); if(result!=HA_OK) fprintf(stderr,"open: %s\n",ha_result_string(result)); CHECK(result==HA_OK);
    steady=allocations; start(); phase(1,generation); before=captured;
    flush_begin=now_ns(); CHECK(ha_stream_flush_playback(stream,HA_FLUSH_NATIVE_REQUIRED,&generation)==HA_PENDING);
    deadline=now_ns()+5000000000u;
    do { progress(); read_capture(); s=status(); CHECK(now_ns()<deadline); if(s.flush_completed_generation==generation) break; pause_control(); } while(1);
    flush_ack=now_ns(); CHECK(s.flush_result==HA_OK);
    CHECK(ha_stream_write(stream,stale,1,generation-1u,&n)==HA_STALE_GENERATION);
    phase(2,generation); CHECK(captured>before && !s.capture_lost_frames && allocations==steady);
    CHECK(ha_stream_stop(stream)==HA_PENDING); deadline=now_ns()+5000000000u;
    do { progress(); s=status(); if(s.state==HA_STOPPED) break; CHECK(now_ns()<deadline); pause_control(); } while(1);
    CHECK(ha_stream_write(stream,stale,1,generation,&n)==HA_STALE_GENERATION);
    generation=s.playback_generation; captured=0; epoch=0; last_frame=0; seen=0;
    start(); phase(3,generation); s=status(); CHECK(!s.capture_lost_frames && allocations==steady);
    printf("{\"verified_frames\":[%llu,%llu,%llu],\"bad_samples_or_order\":%llu,\"capture_frame_gaps\":%llu,\"zero_frames\":%llu,\"flush_ack_us\":%.3f,\"capture_lost_frames\":%llu,\"underrun_frames\":%llu,\"hail_peak_bytes\":%llu,\"steady_hail_allocations\":0}\n",
      (unsigned long long)verified[1],(unsigned long long)verified[2],(unsigned long long)verified[3],(unsigned long long)bad,(unsigned long long)frame_gaps,
      (unsigned long long)zero_frames,(double)(flush_ack-flush_begin)/1000.0,(unsigned long long)s.capture_lost_frames,(unsigned long long)s.playback_underrun_frames,(unsigned long long)peak);
    CHECK(ha_stream_close_begin(stream)==HA_PENDING); deadline=now_ns()+5000000000u;
    while(stream) { progress(); if(ha_stream_close_finish(&stream)==HA_OK) break; CHECK(now_ns()<deadline); pause_control(); }
    deadline=now_ns()+5000000000u;
    do { result=ha_context_destroy(&context); CHECK(result==HA_OK || result==HA_PENDING); CHECK(now_ns()<deadline); if(context) pause_control(); } while(context);
    CHECK(!bytes && allocations==frees); puts("PASS CoreAudio nonzero duplex, flush/capture continuity, stale writes, stop/restart and teardown"); return 0;
}
