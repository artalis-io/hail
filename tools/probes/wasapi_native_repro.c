/* Standalone WASAPI/VB-CABLE marker-flush reproducer. No Hail header/library.
 * Build: clang-cl /nologo /W4 /WX /std:c11 wasapi_native_repro.c
 *        /link ole32.lib uuid.lib
 * Explicit synthetic cable endpoints only; no PCM files or device mutations. */
#define COBJMACROS
#define CONST_VTABLE
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define RATE 48000u
#define QUANTUM 256u
#define MAX_FRAMES 8192u
#define EXAMPLES 8u
#define NS UINT64_C(1000000000)
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL line=%d check=%s\n",__LINE__,#x); fflush(NULL); ExitProcess(2); } } while(0)
#define SDK(x) do { HRESULT result=(x); if(FAILED(result)) { fprintf(stderr,"SDK line=%d hr=0x%08lx\n",__LINE__,(unsigned long)result); fflush(NULL); ExitProcess(2); } } while(0)
static const GUID cls={0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
static const GUID en={0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
static const GUID ep={0x1be09788,0x6894,0x4089,{0x85,0x86,0x9a,0x2a,0x6c,0x26,0x5a,0xc5}};
static const GUID cl={0x1cb9ad4c,0xdbfa,0x4c32,{0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}};
static const GUID cap={0xc8adbd64,0xe71e,0x48a0,{0xa4,0xde,0x18,0x5c,0x39,0x5c,0xd3,0x17}};
static const GUID ren={0xf294acfc,0x3146,0x4483,{0xa7,0xbf,0xad,0xdc,0xa7,0xc2,0x60,0xe2}};
static const GUID floating={3,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
typedef struct leg {
    IAudioClient *client; IAudioCaptureClient *capture; IAudioRenderClient *render;
    HANDLE event; UINT32 capacity; unsigned started;
} leg;
typedef struct example { uint64_t position,host_ns; double left,right; DWORD flags; } example;
typedef struct observation {
    uint64_t frames,fractional,asymmetric,nonfinite,zeros,bad,verified[3],discontinuities,timestamp_errors;
    unsigned examples,seen,last_marker,last_code; example sample[EXAMPLES];
} observation;
static leg legs[2];
static uint64_t frequency;
static observation render_stats,capture_stats[4];
static uint64_t ticks(void) { LARGE_INTEGER t; CHECK(QueryPerformanceCounter(&t)); return (uint64_t)t.QuadPart; }
static uint64_t milliseconds(unsigned value) { return (uint64_t)value*frequency/1000u; }
static uint64_t elapsed_ns(uint64_t start) {
    uint64_t elapsed=ticks()-start,seconds=elapsed/frequency;
    CHECK(seconds<UINT64_MAX/NS);
    return seconds*NS+(elapsed%frequency)*NS/frequency;
}
static unsigned number(const char *s,unsigned limit) {
    char *end; unsigned long n;
    CHECK(s && s[0]>='0' && s[0]<='9'); n=strtoul(s,&end,10);
    CHECK(!*end && n<=limit); return (unsigned)n;
}
static void open_leg(IMMDeviceEnumerator *enumerator,leg *l,const char *key,unsigned playback)
{
    WCHAR wide[2048]; IMMDevice *device=NULL; IMMEndpoint *endpoint=NULL; EDataFlow flow=eAll;
    WAVEFORMATEX *mix=NULL; WAVEFORMATEXTENSIBLE w;
    CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,key,-1,wide,2048)>0);
    SDK(IMMDeviceEnumerator_GetDevice(enumerator,wide,&device));
    SDK(IMMDevice_QueryInterface(device,&ep,(void **)&endpoint)); SDK(IMMEndpoint_GetDataFlow(endpoint,&flow));
    CHECK(flow==(playback?eRender:eCapture)); IMMEndpoint_Release(endpoint);
    SDK(IMMDevice_Activate(device,&cl,CLSCTX_ALL,NULL,(void **)&l->client)); IMMDevice_Release(device);
    SDK(IAudioClient_GetMixFormat(l->client,&mix)); CHECK(mix && mix->nSamplesPerSec==RATE && mix->nChannels==2);
    printf("mix direction=%s rate=%lu channels=%u bits=%u\n",playback?"render":"capture",(unsigned long)mix->nSamplesPerSec,mix->nChannels,mix->wBitsPerSample); CoTaskMemFree(mix);
    memset(&w,0,sizeof(w)); w.Format.wFormatTag=WAVE_FORMAT_EXTENSIBLE; w.Format.nChannels=2; w.Format.nSamplesPerSec=RATE;
    w.Format.wBitsPerSample=32; w.Format.nBlockAlign=8; w.Format.nAvgBytesPerSec=RATE*8u; w.Format.cbSize=22;
    w.Samples.wValidBitsPerSample=32; w.dwChannelMask=3; w.SubFormat=floating;
    l->event=CreateEventW(NULL,FALSE,FALSE,NULL); CHECK(l->event);
    SDK(IAudioClient_Initialize(l->client,AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK|AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,200000,0,&w.Format,NULL));
    SDK(IAudioClient_SetEventHandle(l->client,l->event)); SDK(IAudioClient_GetBufferSize(l->client,&l->capacity));
    CHECK(l->capacity && l->capacity<=MAX_FRAMES);
    SDK(IAudioClient_GetService(l->client,playback?&ren:&cap,playback?(void **)&l->render:(void **)&l->capture));
    printf("native direction=%s requested_buffer_100ns=200000 capacity=%u flags=event_autoconvert_src_quality\n",playback?"render":"capture",l->capacity);
}
static void inspect(observation *s,float left,float right,uint64_t position,uint64_t host,DWORD flags,unsigned oracle)
{
    double l=(double)left*32768.0,r=(double)right*32768.0; unsigned fractional,value,marker,code;
    ++s->frames;
    if(!isfinite(l) || !isfinite(r)) { ++s->nonfinite; ++s->bad; return; }
    fractional=l!=floor(l) || r!=floor(r); s->fractional+=fractional; s->asymmetric+=r!=-l;
    if(fractional && s->examples<EXAMPLES) {
        example *e=&s->sample[s->examples++]; e->position=position; e->host_ns=host; e->left=l; e->right=r; e->flags=flags;
    }
    if(l==0 && r==0) { ++s->zeros; return; }
    if(!oracle) return;
    /* Independent decode of the predeclared 11-bit marker code; no tolerance. */
    if(fractional || l<2048 || l>=6144 || r!=-l) { ++s->bad; return; }
    value=(unsigned)l; marker=value/2048u; code=value%2048u;
    if(s->seen && (marker<s->last_marker || (marker==s->last_marker && code!=(s->last_code+1u)%2048u))) ++s->bad;
    s->seen=1; s->last_marker=marker; s->last_code=code; ++s->verified[marker];
}
static void capture(observation *s,unsigned oracle)
{
    unsigned attempt;
    for(attempt=0;attempt<64;++attempt) {
        BYTE *data=NULL; UINT32 next=0,n=0,i; DWORD flags=0; UINT64 position=0,qpc=0; HRESULT hr;
        SDK(IAudioCaptureClient_GetNextPacketSize(legs[0].capture,&next)); if(!next) break;
        hr=IAudioCaptureClient_GetBuffer(legs[0].capture,&data,&n,&flags,&position,&qpc); SDK(hr);
        if(hr==AUDCLNT_S_BUFFER_EMPTY) break;
        CHECK(n && n<=legs[0].capacity && (data || (flags&AUDCLNT_BUFFERFLAGS_SILENT)));
        CHECK(position<=UINT64_MAX-n && qpc<=UINT64_MAX/100u-NS);
        if(s) {
            const float *f=(const float *)data;
            s->discontinuities+=(flags&AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)!=0;
            s->timestamp_errors+=(flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)!=0;
            for(i=0;i<n;++i) {
                float l=(flags&AUDCLNT_BUFFERFLAGS_SILENT)?0:f[i*2u],r=(flags&AUDCLNT_BUFFERFLAGS_SILENT)?0:f[i*2u+1u];
                inspect(s,l,r,position+i,qpc*100u+(uint64_t)i*NS/RATE,flags,oracle);
            }
        }
        SDK(IAudioCaptureClient_ReleaseBuffer(legs[0].capture,n));
    }
}
static void render(unsigned marker,uint32_t *serial)
{
    UINT32 padding=0,available; SDK(IAudioClient_GetCurrentPadding(legs[1].client,&padding)); CHECK(padding<=legs[1].capacity);
    available=legs[1].capacity-padding;
    while(available) {
        BYTE *data=NULL; float *f; UINT32 n=available>QUANTUM?QUANTUM:available,i;
        SDK(IAudioRenderClient_GetBuffer(legs[1].render,n,&data)); CHECK(data); f=(float *)data;
        for(i=0;i<n;++i) {
            unsigned code=marker?marker*2048u+((*serial+i)%2048u):0;
            f[i*2u]=(float)code/32768.0f; f[i*2u+1u]=-f[i*2u];
            inspect(&render_stats,f[i*2u],f[i*2u+1u],*serial+i,0,0,0);
        }
        SDK(IAudioRenderClient_ReleaseBuffer(legs[1].render,n,0));
        if(marker) *serial+=n; available-=n;
    }
}
static void wait_audio(void)
{
    HANDLE handles[2]={legs[0].event,legs[1].event}; DWORD result=WaitForMultipleObjects(2,handles,FALSE,1);
    CHECK(result==WAIT_TIMEOUT || result==WAIT_OBJECT_0 || result==WAIT_OBJECT_0+1u);
}
static void run(unsigned ms,unsigned marker,uint32_t *serial,observation *s,unsigned oracle)
{
    uint64_t start=ticks(),duration=milliseconds(ms);
    while(ticks()-start<duration) { capture(s,oracle); render(marker,serial); wait_audio(); }
}
static void start_both(void) { unsigned i; for(i=0;i<2;++i) { SDK(IAudioClient_Start(legs[i].client)); legs[i].started=1; } }
static void stop_reset_both(void) {
    unsigned i; for(i=0;i<2;++i) { if(legs[i].started) { SDK(IAudioClient_Stop(legs[i].client)); legs[i].started=0; } SDK(IAudioClient_Reset(legs[i].client)); }
}
static void report(const char *name,const observation *s)
{
    unsigned i;
    printf("stage=%s frames=%llu fractional=%llu asymmetric=%llu nonfinite=%llu zeros=%llu bad=%llu verified_A=%llu verified_B=%llu discontinuities=%llu timestamp_errors=%llu\n",name,
        (unsigned long long)s->frames,(unsigned long long)s->fractional,(unsigned long long)s->asymmetric,(unsigned long long)s->nonfinite,(unsigned long long)s->zeros,
        (unsigned long long)s->bad,(unsigned long long)s->verified[1],(unsigned long long)s->verified[2],(unsigned long long)s->discontinuities,(unsigned long long)s->timestamp_errors);
    for(i=0;i<s->examples;++i) { const example *e=&s->sample[i]; printf("example stage=%s position=%llu host_ns=%llu left_s16=%.17g right_s16=%.17g flags=%lu\n",name,
        (unsigned long long)e->position,(unsigned long long)e->host_ns,e->left,e->right,(unsigned long)e->flags); }
}
static int self_test(void)
{
    observation s; memset(&s,0,sizeof(s));
    inspect(&s,0,0,0,0,0,1); CHECK(s.zeros==1 && !s.verified[1]);
    inspect(&s,0.0625f,-0.0625f,0,0,0,1); inspect(&s,2049.0f/32768.0f,-2049.0f/32768.0f,1,0,0,1); CHECK(!s.bad && s.verified[1]==2);
    inspect(&s,2051.0f/32768.0f,-2051.0f/32768.0f,2,0,0,1); CHECK(s.bad==1);
    inspect(&s,0.0625f,0.0625f,3,0,0,1); CHECK(s.bad==2 && s.asymmetric==1);
    inspect(&s,2048.5f/32768.0f,-2048.5f/32768.0f,4,0,0,1); CHECK(s.bad==3 && s.fractional==1);
    inspect(&s,NAN,0,5,0,0,1); CHECK(s.bad==4 && s.nonfinite==1);
    inspect(&s,0.125f,-0.125f,6,0,0,1); inspect(&s,0.0625f,-0.0625f,7,0,0,1); CHECK(s.bad==5);
    puts("PASS native-only oracle rejects zeros-only evidence, skips, channels, fractional/nonfinite samples and regressions"); return 0;
}
int main(int argc,char **argv)
{
    IMMDeviceEnumerator *enumerator=NULL; LARGE_INTEGER f; unsigned unprimed=0,delay=0,ack_delay=0,i; uint32_t serial=0;
    uint64_t begin,stop_ns,reset_ns,start_ns; observation condition; unsigned failed;
    if(argc==2 && !strcmp(argv[1],"--self-test")) return self_test();
    CHECK(argc==6); unprimed=number(argv[3],1); delay=number(argv[4],1000); ack_delay=number(argv[5],1000);
    CHECK(QueryPerformanceFrequency(&f) && f.QuadPart>0); frequency=(uint64_t)f.QuadPart; CHECK(frequency<=UINT64_MAX/NS);
    CHECK(setvbuf(stdout,NULL,_IONBF,0)==0); SDK(CoInitializeEx(NULL,COINIT_MULTITHREADED));
    SDK(CoCreateInstance(&cls,NULL,CLSCTX_ALL,&en,(void **)&enumerator));
    open_leg(enumerator,&legs[0],argv[1],0); open_leg(enumerator,&legs[1],argv[2],1); IMMDeviceEnumerator_Release(enumerator);
    printf("reproducer=native-only hail_linked=0 rate=48000 channels=2 pcm=f32 quantum=256 unprimed=%u requested_delay_ms=%u requested_ack_delay_ms=%u sample_tolerance=0\n",unprimed,delay,ack_delay);
    start_both(); run(600,0,&serial,NULL,0); memset(&condition,0,sizeof(condition)); run(900,0,&serial,&condition,0);
    report("condition",&condition); CHECK(condition.frames>24000 && condition.zeros==condition.frames && !condition.nonfinite);
    stop_reset_both(); memset(&render_stats,0,sizeof(render_stats));
    if(!unprimed) render(1,&serial);
    start_both(); if(unprimed) run(60,0,&serial,&capture_stats[0],1);
    run(1000,1,&serial,&capture_stats[0],1);
    /* Keep one oracle state across nominal and flush phases, preserving sequence. */
    capture_stats[1]=capture_stats[0]; run(5000,1,&serial,&capture_stats[1],1);
    begin=ticks(); run(delay,1,&serial,&capture_stats[1],1);
    printf("flush_requested observed_additional_delay_ns=%llu\n",(unsigned long long)elapsed_ns(begin));
    begin=ticks(); SDK(IAudioClient_Stop(legs[1].client)); legs[1].started=0; stop_ns=elapsed_ns(begin);
    begin=ticks(); SDK(IAudioClient_Reset(legs[1].client)); reset_ns=elapsed_ns(begin);
    begin=ticks(); SDK(IAudioClient_Start(legs[1].client)); legs[1].started=1; start_ns=elapsed_ns(begin);
    printf("flush_stop_ns=%llu reset_ns=%llu restart_ns=%llu\n",(unsigned long long)stop_ns,(unsigned long long)reset_ns,(unsigned long long)start_ns);
    capture_stats[2]=capture_stats[1]; begin=ticks(); run(ack_delay,0,&serial,&capture_stats[2],1);
    printf("flush_observed_ack_delay_ns=%llu\n",(unsigned long long)elapsed_ns(begin)); serial=0;
    run(1000,2,&serial,&capture_stats[2],1); capture_stats[3]=capture_stats[2]; run(400,0,&serial,&capture_stats[3],1);
    stop_reset_both();
    for(i=0;i<2;++i) { if(legs[i].capture) IAudioCaptureClient_Release(legs[i].capture); if(legs[i].render) IAudioRenderClient_Release(legs[i].render);
        IAudioClient_Release(legs[i].client); CHECK(CloseHandle(legs[i].event)); }
    CoUninitialize(); report("render_buffer",&render_stats); report("capture_startup",&capture_stats[0]); report("capture_through_nominal",&capture_stats[1]);
    report("capture_through_flush",&capture_stats[2]); report("capture_final",&capture_stats[3]);
    failed=render_stats.fractional || render_stats.asymmetric || render_stats.nonfinite || capture_stats[3].bad || capture_stats[3].verified[1]<=RATE/3u || capture_stats[3].verified[2]<=RATE/3u;
    printf("%s native_only_exact_marker_check resources_released=1 no_physical_latency_claim=1\n",failed?"FAIL":"PASS");
    return failed?1:0;
}
