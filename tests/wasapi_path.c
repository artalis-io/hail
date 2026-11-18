/* Explicit synthetic-cable diagnostic, never an automatic audio test.
 * Observe Hail reference, native render loopback, native cable capture in both
 * representations, and Hail capture simultaneously. No PCM files are written. */
#define COBJMACROS
#define CONST_VTABLE
#define _WIN32_WINNT 0x0A00
#include <hail.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define Q 256u
#define REQUIRE(x) do { if(!(x)) { fprintf(stderr,"FAIL line=%d expression=%s\n",__LINE__,#x); fflush(NULL); ExitProcess(1); } } while(0)
#define SDK(x) do { HRESULT result=(x); if(FAILED(result)) { fprintf(stderr,"SDK line=%d hr=0x%08lx\n",__LINE__,(unsigned long)result); fflush(NULL); ExitProcess(1); } } while(0)
static const GUID cls={0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
static const GUID en={0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
static const GUID cl={0x1cb9ad4c,0xdbfa,0x4c32,{0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}};
static const GUID cap={0xc8adbd64,0xe71e,0x48a0,{0xa4,0xde,0x18,0x5c,0x39,0x5c,0xd3,0x17}};
static const GUID integer={1,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
static const GUID floating={3,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
typedef union pcm_buffer { float f[Q*2]; int16_t s[Q*2]; } pcm_buffer;
typedef struct observation { uint64_t frames,mismatches,nonzero,flags; double max_error; } observation;
typedef struct tap { IAudioClient *client; IAudioCaptureClient *capture; unsigned pcm; UINT32 capacity; } tap;
static ha_context *context;
static ha_stream *stream;
static uint64_t frequency;
static unsigned pcm;
static const char *names[]={"hail_reference","native_render_loopback_f32","native_cable_f32","native_cable_s16","hail_capture"};
static uint64_t ticks(void) { LARGE_INTEGER t; REQUIRE(QueryPerformanceCounter(&t)); return (uint64_t)t.QuadPart; }
static void progress(void) { uint32_t count; REQUIRE(ha_context_poll(context,NULL,0,&count)==HA_OK); }
static void await_state(unsigned state) {
    uint64_t deadline=ticks()+5*frequency;
    for(;;) { ha_stream_status s=HA_INIT(ha_stream_status); progress(); REQUIRE(ha_stream_get_status(stream,&s)==HA_OK && s.state!=HA_FAILED);
        if(s.state==state) return; REQUIRE(ticks()<deadline); Sleep(1); }
}
static ha_device_selector selector(const char *key) {
    ha_device_selector s=HA_INIT(ha_device_selector); size_t length=strlen(key);
    REQUIRE(length<sizeof(s.key)); s.kind=HA_SELECT_PERSISTENT_KEY; s.key_bytes=(uint32_t)length; memcpy(s.key,key,length+1); return s;
}
static void open_tap(tap *t,IMMDeviceEnumerator *e,const char *key,unsigned representation,unsigned loopback) {
    WCHAR wide[HA_DEVICE_KEY_CAPACITY]; IMMDevice *d=NULL; WAVEFORMATEXTENSIBLE w; WAVEFORMATEX *mix=NULL;
    REQUIRE(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,key,-1,wide,HA_DEVICE_KEY_CAPACITY)>0);
    SDK(IMMDeviceEnumerator_GetDevice(e,wide,&d)); SDK(IMMDevice_Activate(d,&cl,CLSCTX_ALL,NULL,(void **)&t->client));
    SDK(IAudioClient_GetMixFormat(t->client,&mix)); REQUIRE(mix->nChannels==2 && mix->nSamplesPerSec==48000);
    printf("tap loopback=%u pcm=%u mix_rate=%lu mix_channels=%u mix_bits=%u\n",loopback,representation,(unsigned long)mix->nSamplesPerSec,mix->nChannels,mix->wBitsPerSample); CoTaskMemFree(mix);
    memset(&w,0,sizeof(w)); w.Format.wFormatTag=WAVE_FORMAT_EXTENSIBLE; w.Format.nChannels=2; w.Format.nSamplesPerSec=48000;
    w.Format.wBitsPerSample=representation==HA_PCM_F32?32:16; w.Format.nBlockAlign=(WORD)(2u*w.Format.wBitsPerSample/8u);
    w.Format.nAvgBytesPerSec=48000u*w.Format.nBlockAlign; w.Format.cbSize=22; w.Samples.wValidBitsPerSample=w.Format.wBitsPerSample;
    w.dwChannelMask=3; w.SubFormat=representation==HA_PCM_F32?floating:integer;
    SDK(IAudioClient_Initialize(t->client,AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY|
        (loopback?AUDCLNT_STREAMFLAGS_LOOPBACK:0),200000,0,&w.Format,NULL));
    SDK(IAudioClient_GetBufferSize(t->client,&t->capacity)); SDK(IAudioClient_GetService(t->client,&cap,(void **)&t->capture));
    t->pcm=representation; IMMDevice_Release(d);
}
static void observe(observation *o,const void *data,unsigned representation,unsigned frames,unsigned silent,int expected) {
    unsigned i,c; const float *f=data; const int16_t *s=data;
    for(i=0;i<frames;++i) { unsigned mismatch=0,nonzero=0;
        for(c=0;c<2;++c) { double actual=silent?0:(representation==HA_PCM_F32?(double)f[i*2+c]*32768.0:(double)s[i*2+c]);
            double error=fabs(actual-(c?-expected:expected));
            if(!isfinite(actual)) { mismatch=1; o->max_error=HUGE_VAL; }
            else { if(error!=0) mismatch=1; if(error>o->max_error) o->max_error=error; }
            if(actual!=0) nonzero=1;
        }
        ++o->frames; o->mismatches+=mismatch; o->nonzero+=nonzero;
    }
}
static void drain_tap(tap *t,observation *o,int expected,unsigned measured) {
    unsigned attempt; for(attempt=0;attempt<64;++attempt) { BYTE *data=NULL; UINT32 n=0,next=0; DWORD flags=0; UINT64 position=0,qpc=0;
        SDK(IAudioCaptureClient_GetNextPacketSize(t->capture,&next)); if(!next) return;
        SDK(IAudioCaptureClient_GetBuffer(t->capture,&data,&n,&flags,&position,&qpc)); REQUIRE(n && n<=t->capacity && (data || (flags&AUDCLNT_BUFFERFLAGS_SILENT)));
        if(measured) { o->flags|=flags; observe(o,data,t->pcm,n,(flags&AUDCLNT_BUFFERFLAGS_SILENT)!=0,expected); }
        SDK(IAudioCaptureClient_ReleaseBuffer(t->capture,n));
    }
}
static void hail_io(observation *capture,observation *reference,int expected,unsigned measured) {
    pcm_buffer p; unsigned i,attempt; uint32_t n;
    for(attempt=0;attempt<64;++attempt) { ha_block_info b=HA_INIT(ha_block_info); ha_result r=ha_stream_read(stream,&p,Q,&n,&b);
        if(r==HA_WOULD_BLOCK || r==HA_BUSY) break; REQUIRE(r==HA_OK && n && n<=Q);
        if(measured) { capture->flags|=b.flags; observe(capture,&p,pcm,n,0,expected); }
    }
    for(attempt=0;attempt<64;++attempt) { ha_block_info b=HA_INIT(ha_block_info); ha_result r=ha_stream_read_reference(stream,&p,Q,&n,&b);
        if(r==HA_WOULD_BLOCK || r==HA_BUSY) break; REQUIRE(r==HA_OK && n && n<=Q);
        if(measured) { reference->flags|=b.flags; observe(reference,&p,pcm,n,0,expected); }
    }
    for(i=0;i<Q*2;++i) { int v=(i&1)?-expected:expected; if(pcm==HA_PCM_F32) p.f[i]=(float)v/32768.0f; else p.s[i]=(int16_t)v; }
    for(attempt=0;attempt<8;++attempt) { ha_result r=ha_stream_write(stream,&p,Q,1,&n); if(r==HA_WOULD_BLOCK || r==HA_BUSY) break; REQUIRE(r==HA_OK && n<=Q); }
}
static int self_test(void) {
    observation o; float f[2]={0.25f,-0.25f}; int16_t s[2]={8192,-8192};
    memset(&o,0,sizeof(o)); observe(&o,f,HA_PCM_F32,1,0,8192); observe(&o,s,HA_PCM_S16,1,0,8192);
    REQUIRE(o.frames==2 && !o.mismatches && o.nonzero==2 && o.max_error==0);
    s[1]=-8191; observe(&o,s,HA_PCM_S16,1,0,8192); REQUIRE(o.mismatches==1 && o.max_error==1);
    observe(&o,NULL,HA_PCM_F32,1,1,8192); REQUIRE(o.mismatches==2 && o.max_error==8192);
    f[0]=NAN; observe(&o,f,HA_PCM_F32,1,0,8192); REQUIRE(o.mismatches==3 && !isfinite(o.max_error));
    puts("PASS constant observer rejects LSB errors, zeros-only substitution and nonfinite PCM"); return 0;
}
int main(int argc,char **argv) {
    static const int levels[]={0,8192,16384,24576,30000,32000,32700,0};
    ha_context_config cc=HA_INIT(ha_context_config); ha_stream_config sc=HA_INIT(ha_stream_config);
    IMMDeviceEnumerator *e=NULL; tap taps[3]; unsigned phase,j; LARGE_INTEGER f; uint64_t deadline; ha_result r;
    if(argc==2 && !strcmp(argv[1],"--self-test")) return self_test();
    REQUIRE(argc==4 && (!strcmp(argv[3],"f32") || !strcmp(argv[3],"s16"))); pcm=!strcmp(argv[3],"f32")?HA_PCM_F32:HA_PCM_S16;
    /* Exact local cable identities are deliberately supplied; no default fallback. */
    REQUIRE(QueryPerformanceFrequency(&f) && f.QuadPart>0); frequency=(uint64_t)f.QuadPart;
    SDK(CoInitializeEx(NULL,COINIT_MULTITHREADED)); SDK(CoCreateInstance(&cls,NULL,CLSCTX_ALL,&en,(void **)&e));
    memset(taps,0,sizeof(taps)); open_tap(&taps[0],e,argv[2],HA_PCM_F32,1); open_tap(&taps[1],e,argv[1],HA_PCM_F32,0); open_tap(&taps[2],e,argv[1],HA_PCM_S16,0);
    cc.backend=HA_BACKEND_WASAPI; cc.max_devices=HA_MAX_DEVICES; cc.max_streams=1; cc.event_capacity=64;
    REQUIRE(ha_context_create(&cc,&context)==HA_OK);
    sc.directions=HA_CAPTURE|HA_PLAYBACK; sc.negotiation=HA_EXACT_APPLICATION;
    sc.capture_device=selector(argv[1]); sc.playback_device=selector(argv[2]);
    { ha_format fmt=HA_FORMAT_INIT(48000,2,pcm); sc.capture_format=fmt; sc.playback_format=fmt; }
    sc.quantum_frames=Q; sc.capture_slots=32; sc.playback_slots=32; sc.reference_slots=64;
    REQUIRE(ha_stream_open(context,&sc,&stream)==HA_OK);
    for(j=0;j<3;++j) SDK(IAudioClient_Start(taps[j].client));
    REQUIRE(ha_stream_start(stream)==HA_PENDING); await_state(HA_RUNNING);
    printf("diagnostic=constant-s16-grid levels=8 phase_ms=1500 settle_ms=600 hail_pcm=%u oracle_tolerance=0 qualification=0\n",pcm);
    for(phase=0;phase<sizeof(levels)/sizeof(levels[0]);++phase) {
        observation o[5]; uint64_t start=ticks(); memset(o,0,sizeof(o));
        while(ticks()-start<frequency*3/2) { unsigned measured=ticks()-start>=frequency*3/5;
            progress(); hail_io(&o[4],&o[0],levels[phase],measured);
            for(j=0;j<3;++j) drain_tap(&taps[j],&o[j+1],levels[phase],measured); Sleep(1);
        }
        for(j=0;j<5;++j) { REQUIRE(o[j].frames>24000); printf("level=%d stage=%s frames=%llu mismatches=%llu nonzero=%llu max_error_s16_units=%.17g flags=%llu\n",
            levels[phase],names[j],(unsigned long long)o[j].frames,(unsigned long long)o[j].mismatches,(unsigned long long)o[j].nonzero,o[j].max_error,(unsigned long long)o[j].flags); }
    }
    REQUIRE(ha_stream_stop(stream)==HA_PENDING); await_state(HA_STOPPED);
    for(j=0;j<3;++j) { SDK(IAudioClient_Stop(taps[j].client)); taps[j].capture->lpVtbl->Release(taps[j].capture); IAudioClient_Release(taps[j].client); }
    REQUIRE(ha_stream_close_begin(stream)==HA_PENDING); deadline=ticks()+5*frequency;
    do { progress(); r=ha_stream_close_finish(&stream); REQUIRE(r==HA_OK || r==HA_PENDING); REQUIRE(ticks()<deadline); Sleep(1); } while(r==HA_PENDING);
    deadline=ticks()+5*frequency;
    do { r=ha_context_destroy(&context); REQUIRE(r==HA_OK || r==HA_PENDING); REQUIRE(ticks()<deadline); Sleep(1); } while(r==HA_PENDING);
    IMMDeviceEnumerator_Release(e); CoUninitialize(); puts("COMPLETE diagnostic only; mismatches remain failures of transparency, not accepted conversion"); return 0;
}
