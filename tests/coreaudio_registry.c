#define _POSIX_C_SOURCE 200809L
#include <hail.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Opt-in real HAL registry fixture. A private aggregate wraps only the explicit
 * output UID. --defaults makes it public briefly to exercise default changes.
 * Silence only; no capture, rate/volume edits, or third-party device removal.
 * Cleanup restores the original default and destroys only our aggregate. */
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
static AudioDeviceID fixture,original_default;
static int defaults_mode,changed_default;
static ha_context *context;
static ha_stream *pinned,*replacement;
static uint64_t now(void)
{ struct timespec t; CHECK(!clock_gettime(CLOCK_MONOTONIC,&t)); return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec; }
static void pause_control(void) { struct timespec t={0,2000000}; nanosleep(&t,NULL); }
static OSStatus set_default(AudioDeviceID id)
{
    AudioObjectPropertyAddress a={kAudioHardwarePropertyDefaultOutputDevice,kAudioObjectPropertyScopeGlobal,0};
    return AudioObjectSetPropertyData(kAudioObjectSystemObject,&a,0,NULL,sizeof(id),&id);
}
static void cleanup(void)
{
    if(changed_default) {
        OSStatus r=set_default(original_default);
        fprintf(stderr,"restore_default=%d id=%u\n",(int)r,(unsigned)original_default);
        if(r) fprintf(stderr,"RESTORE REQUIRED: original output AudioDeviceID=%u\n",(unsigned)original_default);
        else changed_default=0;
    }
    if(fixture) {
        OSStatus r=AudioHardwareDestroyAggregateDevice(fixture);
        fprintf(stderr,"destroy_owned_aggregate=%d id=%u\n",(int)r,(unsigned)fixture);
        if(!r) fixture=0;
    }
}
static void progress(void)
{ uint32_t n; CHECK(ha_context_poll(context,NULL,0,&n)==HA_OK); }
static ha_stream_status status(ha_stream *s)
{ ha_stream_status v=HA_INIT(ha_stream_status); CHECK(ha_stream_get_status(s,&v)==HA_OK); return v; }
static uint64_t lookup(const char *uid)
{
    ha_device_info d[HA_MAX_DEVICES]; uint32_t n,i;
    CHECK(ha_context_refresh(context)==HA_OK);
    for(i=0;i<HA_MAX_DEVICES;++i) { ha_device_info value=HA_INIT(ha_device_info); d[i]=value; }
    CHECK(ha_context_enumerate(context,d,HA_MAX_DEVICES,&n)==HA_OK);
    for(i=0;i<n;++i) if(d[i].direction==HA_PLAYBACK && !strcmp(d[i].persistent_key,uid)) return d[i].runtime_id;
    return 0;
}
static uint64_t wait_device(const char *uid,int present)
{
    uint64_t deadline=now()+5000000000ull,id;
    do { progress(); id=lookup(uid); if((id!=0)==present) return id; CHECK(now()<deadline); pause_control(); } while(1);
}
static void create_fixture(const char *parent,const char *uid)
{
    CFStringRef parent_string=CFStringCreateWithCString(NULL,parent,kCFStringEncodingUTF8);
    CFStringRef fixture_uid=CFStringCreateWithCString(NULL,uid,kCFStringEncodingUTF8);
    const void *subkeys[]={CFSTR(kAudioSubDeviceUIDKey)};
    const void *subvalues[]={parent_string};
    CFDictionaryRef sub=CFDictionaryCreate(NULL,subkeys,subvalues,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    const void *subs[]={sub}; CFArrayRef list=CFArrayCreate(NULL,subs,1,&kCFTypeArrayCallBacks);
    int privacy=defaults_mode?0:1;
    CFNumberRef private_number=CFNumberCreate(NULL,kCFNumberIntType,&privacy);
    const void *keys[]={CFSTR(kAudioAggregateDeviceNameKey),CFSTR(kAudioAggregateDeviceUIDKey),CFSTR(kAudioAggregateDeviceSubDeviceListKey),CFSTR(kAudioAggregateDeviceMainSubDeviceKey),CFSTR(kAudioAggregateDeviceIsPrivateKey)};
    const void *values[]={CFSTR("Hail owned qualification output"),fixture_uid,list,parent_string,private_number};
    CFDictionaryRef description=CFDictionaryCreate(NULL,keys,values,5,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    OSStatus r=AudioHardwareCreateAggregateDevice(description,&fixture);
    CFRelease(description); CFRelease(private_number); CFRelease(list); CFRelease(sub); CFRelease(fixture_uid); CFRelease(parent_string);
    printf("create_aggregate=%d id=%u private=%d\n",(int)r,(unsigned)fixture,privacy); CHECK(!r && fixture);
}
static ha_stream *open_output(uint64_t id,uint32_t rate)
{
    ha_stream_config sc=HA_INIT(ha_stream_config); ha_device_selector selector=HA_INIT(ha_device_selector);
    ha_format f=HA_FORMAT_INIT(rate,2,HA_PCM_F32); ha_stream *s=NULL; ha_result r;
    if(id) { selector.kind=HA_SELECT_RUNTIME_ID; selector.runtime_id=id; }
    sc.playback_device=selector; sc.playback_format=f; sc.directions=HA_PLAYBACK;
    sc.negotiation=HA_EXACT_APPLICATION; sc.quantum_frames=256; sc.playback_slots=16;
    r=ha_stream_open(context,&sc,&s); printf("open=%s selected=%llu\n",ha_result_string(r),(unsigned long long)id); CHECK(r==HA_OK);
    return s;
}
static void start(ha_stream *s)
{
    float silence[8192]={0}; uint32_t n; uint64_t deadline=now()+5000000000ull;
    CHECK(ha_stream_write(s,silence,4096,1,&n)==HA_OK && n==4096);
    CHECK(ha_stream_start(s)==HA_PENDING);
    do { progress(); ha_stream_status v=status(s); CHECK(v.state!=HA_FAILED);
         if(v.state==HA_RUNNING) return; CHECK(now()<deadline); pause_control(); } while(1);
}
static void close_stream(ha_stream **s)
{
    uint64_t deadline=now()+5000000000ull;
    CHECK(ha_stream_close_begin(*s)==HA_PENDING);
    do { progress(); ha_result r=ha_stream_close_finish(s); CHECK(r==HA_OK || r==HA_PENDING);
         CHECK(now()<deadline); if(*s) pause_control(); } while(*s);
}
int main(int argc,char **argv)
{
    ha_context_config cc=HA_INIT(ha_context_config); char uid[128]; uint64_t first,second; uint32_t rate;
    ha_stream_info info=HA_INIT(ha_stream_info); AudioObjectPropertyAddress a={kAudioHardwarePropertyDefaultOutputDevice,kAudioObjectPropertyScopeGlobal,0};
    UInt32 bytes=sizeof(original_default); uint64_t deadline;
    CHECK(argc==3 || (argc==4 && !strcmp(argv[3],"--defaults")));
    defaults_mode=argc==4; rate=(uint32_t)strtoul(argv[2],NULL,10);
    CHECK(!AudioObjectGetPropertyData(kAudioObjectSystemObject,&a,0,NULL,&bytes,&original_default));
    CHECK(!atexit(cleanup));
    snprintf(uid,sizeof(uid),"io.artalis.hail.qualification.%ld.%llu",(long)getpid(),(unsigned long long)now());
    cc.backend=HA_BACKEND_COREAUDIO; CHECK(ha_context_create(&cc,&context)==HA_OK);
    create_fixture(argv[1],uid); first=wait_device(uid,1);
    if(defaults_mode) {
        pinned=open_output(0,rate); CHECK(ha_stream_get_info(pinned,&info)==HA_OK);
        uint64_t pinned_id=info.playback_device_id; start(pinned);
        changed_default=1; CHECK(!set_default(fixture));
        deadline=now()+5000000000ull;
        do {
            progress(); CHECK(ha_context_refresh(context)==HA_OK);
            replacement=open_output(0,rate); CHECK(ha_stream_get_info(replacement,&info)==HA_OK);
            if(info.playback_device_id==first) break;
            close_stream(&replacement); CHECK(now()<deadline); pause_control();
        } while(1);
        CHECK(status(pinned).state==HA_RUNNING); CHECK(ha_stream_get_info(pinned,&info)==HA_OK && info.playback_device_id==pinned_id);
        CHECK(!set_default(original_default)); changed_default=0;
        puts("PASS default selection changes only future opens; active stream stays pinned; restored original output");
        close_stream(&pinned); close_stream(&replacement);
    }
    pinned=open_output(first,rate); start(pinned);
    CHECK(!AudioHardwareDestroyAggregateDevice(fixture)); fixture=0;
    (void)wait_device(uid,0); CHECK(status(pinned).state==HA_FAILED && status(pinned).terminal_result==HA_DEVICE_LOST);
    puts("PASS active owned aggregate removal produces terminal DEVICE_LOST");
    close_stream(&pinned);
    create_fixture(argv[1],uid); second=wait_device(uid,1); CHECK(second!=first);
    replacement=open_output(second,rate); start(replacement); close_stream(&replacement);
    printf("PASS replacement incarnation old=%llu new=%llu; explicit reopen succeeds\n",(unsigned long long)first,(unsigned long long)second);
    CHECK(!AudioHardwareDestroyAggregateDevice(fixture)); fixture=0; (void)wait_device(uid,0);
    deadline=now()+5000000000ull;
    do { ha_result r=ha_context_destroy(&context); CHECK(r==HA_OK || r==HA_PENDING); CHECK(now()<deadline); if(context) pause_control(); } while(context);
    puts("PASS native aggregate registry lifecycle (not physical unplug)"); return 0;
}
