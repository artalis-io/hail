/* Native AUHAL rollback and listener-retirement regression. Faults affect only
 * this executable's source-private adapter copy; real SDK units/listeners are
 * created, and all audio output is silence. Not a simulated OS privacy test. */
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned start_calls,stop_calls,remove_calls;
static int fail_second_start,fail_first_stop,fail_first_remove;
static OSStatus test_start(AudioUnit unit)
{
    ++start_calls;
    if(fail_second_start && start_calls==2) return kAudioUnitErr_FailedInitialization;
    return AudioOutputUnitStart(unit);
}
static OSStatus test_stop(AudioUnit unit)
{
    ++stop_calls;
    if(fail_first_stop && stop_calls==1) return kAudioUnitErr_CannotDoInCurrentContext;
    return AudioOutputUnitStop(unit);
}
static OSStatus test_remove(AudioObjectID object,const AudioObjectPropertyAddress *address,
                           AudioObjectPropertyListenerProc listener,void *user)
{
    ++remove_calls;
    if(fail_first_remove && remove_calls==1) return kAudioHardwareIllegalOperationError;
    return AudioObjectRemovePropertyListener(object,address,listener,user);
}
#define AudioOutputUnitStart test_start
#define AudioOutputUnitStop test_stop
#define AudioObjectRemovePropertyListener test_remove
#define hail_coreaudio_backend hail_test_coreaudio_backend
#include "../src/backend_coreaudio.c"
#undef AudioOutputUnitStart
#undef AudioOutputUnitStop
#undef AudioObjectRemovePropertyListener
#undef hail_coreaudio_backend
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
static AudioDeviceID resolve(const char *key)
{
    CFStringRef uid=CFStringCreateWithCString(NULL,key,kCFStringEncodingUTF8); AudioDeviceID id=0; UInt32 bytes=sizeof(id);
    AudioObjectPropertyAddress address={kAudioHardwarePropertyTranslateUIDToDevice,kAudioObjectPropertyScopeGlobal,0};
    CHECK(!AudioObjectGetPropertyData(kAudioObjectSystemObject,&address,sizeof(uid),&uid,&bytes,&id)); CFRelease(uid); CHECK(id); return id;
}
int main(int argc,char **argv)
{
    ha_stream s; ca_stream a; ha_context c; ca_context p;
    ha_format f=HA_FORMAT_INIT(48000,1,HA_PCM_F32);
    CHECK(argc==3); memset(&s,0,sizeof(s)); memset(&a,0,sizeof(a));
    s.backend_state=&a; s.info.capture_format=f; s.info.playback_format=f;
    s.capture.bytes_per_frame=4; s.playback.bytes_per_frame=4;
    atomic_init(&s.state,HA_STOPPED); hail_rt_init(&s);
    atomic_init(&a.callbacks,0); atomic_init(&a.legs[0].callbacks,0); atomic_init(&a.legs[1].callbacks,0);
    CHECK(!ca_unit_open(&s,&a.legs[0],resolve(argv[1]),0));
    CHECK(!ca_unit_open(&s,&a.legs[1],resolve(argv[2]),1));
    fail_second_start=1; fail_first_stop=1;
    CHECK(ca_start(&s)==HA_STREAM_FAILED);
    printf("partial_start calls=%u rollback_stop_calls=%u capture_started_after_failed_stop=%u\n",start_calls,stop_calls,a.legs[0].started);
    CHECK(a.legs[0].started==1); /* Failed stop must not claim retirement. */
    fail_first_stop=0;
    CHECK(ca_stop(&s)==HA_OK || atomic_load(&a.callbacks));
    CHECK(stop_calls>=3 && !a.legs[0].started && !a.legs[1].started);
    CHECK(ca_retire(&s)==HA_OK); ca_stream_free(&s);
    puts("PASS partial native start rollback retains failed-stop state and retries stop before retirement");
    memset(&c,0,sizeof(c)); memset(&p,0,sizeof(p)); p.owner=&c; c.backend_state=&p;
    atomic_init(&p.callbacks,0); atomic_init(&c.registry_dirty,0);
    AudioObjectPropertyAddress address={ca_properties[0],kAudioObjectPropertyScopeGlobal,0};
    CHECK(!AudioObjectAddPropertyListener(kAudioObjectSystemObject,&address,ca_changed,&p)); p.listeners=1;
    fail_first_remove=1;
    CHECK(ca_context_free(&c)==HA_PENDING && p.listeners==1);
    fail_first_remove=0; atomic_store(&p.callbacks,1);
    CHECK(ca_context_free(&c)==HA_PENDING && p.listeners==0);
    atomic_store(&p.callbacks,0); CHECK(ca_context_free(&c)==HA_OK && remove_calls==2);
    puts("PASS real listener removal retries after injected error and waits for callback retirement");
    CHECK(ca_result(kAudioUnitErr_Unauthorized)==HA_PERMISSION_DENIED);
    CHECK(ca_result(kAudioComponentErr_NotPermitted)==HA_PERMISSION_DENIED);
    CHECK(ca_result(kAudioDevicePermissionsError)==HA_PERMISSION_DENIED);
    puts("PASS SDK permission error mappings (not an OS denial trial)"); return 0;
}
