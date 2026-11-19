/* Device-free ASBD validation through the actual adapter. No unit, registry,
 * permission service or device is opened. Only the property lease is replaced. */
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static AudioStreamBasicDescription native_format;
static OSStatus property_result;
static OSStatus test_property(AudioObjectID object, const AudioObjectPropertyAddress *address,
                              UInt32 qualifier_size, const void *qualifier, UInt32 *size, void *data)
{
    (void)object; (void)qualifier_size; (void)qualifier;
    if (address->mSelector != kAudioDevicePropertyStreamFormat || *size != sizeof(native_format)) abort();
    memcpy(data, &native_format, sizeof(native_format));
    return property_result;
}
#define AudioObjectGetPropertyData test_property
#define hail_coreaudio_backend hail_test_coreaudio_format_backend
#include "../src/backend_coreaudio.c"
#undef hail_coreaudio_backend
#undef AudioObjectGetPropertyData
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while(0)
int main(void)
{
    ha_format f = HA_INIT(ha_format); size_t bytes;
    ha_stream_info plan = HA_INIT(ha_stream_info);
    const double invalid[] = {NAN, INFINITY, -INFINITY, 0.0, -1.0, 192001.0, 48000.5};
    const uint32_t channels[] = {1,2,4,HA_MAX_CHANNELS};
    native_format.mFormatID=kAudioFormatLinearPCM;
    native_format.mFormatFlags=kAudioFormatFlagIsFloat;
    native_format.mBitsPerChannel=32; native_format.mChannelsPerFrame=2;
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        native_format.mSampleRate=invalid[i]; CHECK(!ca_native_format(1,0,&f));
    }
    native_format.mSampleRate=48000;
    for(unsigned i=0;i<sizeof(channels)/sizeof(channels[0]);++i) {
        native_format.mChannelsPerFrame=channels[i];
        CHECK(ca_native_format(1,0,&f) && f.sample_rate==48000 && f.channels==channels[i]);
        CHECK(f.sample_format==HA_PCM_F32 && f.storage==HA_INTERLEAVED && f.channel_layout==HA_LAYOUT_DISCRETE);
        plan.capture_slots=2; plan.capture_format=f;
        CHECK(ca_extra_bytes(&plan,&bytes)==HA_OK && bytes==(size_t)HA_MAX_QUANTUM*channels[i]*sizeof(float));
    }
    property_result=kAudioHardwareIllegalOperationError; CHECK(!ca_native_format(1,0,&f));
    puts("CoreAudio rejects nonfinite/fractional rates and preserves discrete channel counts");
    return 0;
}
