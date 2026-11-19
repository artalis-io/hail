/* Test-only observers. Included after CHECK/now_ns and the Hail test globals.
 * All SDK clients, rings and statistics belong exclusively to the I/O owner.
 * Never uses production worker-owned COM interfaces. */
#define TRACE_RING 65536u
#define TRACE_PHASES 5u
#define TRACE_EXAMPLES 8u
enum { TRACE_REFERENCE, TRACE_HAIL, TRACE_NATIVE, TRACE_RENDER, TRACE_STAGES };
typedef struct trace_example {
    uint64_t observed_ns,host_ns,position; double left,right; uint32_t flags;
} trace_example;
typedef struct trace_stats {
    uint64_t frames,fractional,asymmetric,nonfinite;
    unsigned examples; trace_example example[TRACE_EXAMPLES];
} trace_stats;
typedef struct trace_cell { uint64_t position,host_ns; float left,right; unsigned valid; } trace_cell;
typedef struct trace_tap { IAudioClient *client; IAudioCaptureClient *capture; UINT32 capacity; } trace_tap;
static unsigned marker_trace,marker_taps;
static const char *trace_capture_key,*trace_playback_key;
static trace_stats trace_statistics[TRACE_STAGES][TRACE_PHASES];
static trace_cell trace_native_ring[TRACE_RING],trace_hail_ring[TRACE_RING];
static uint64_t trace_pairs,trace_pair_mismatches,trace_clock_rejections;
static trace_tap trace_taps[2];
static const char *trace_names[]={"hail_reference","hail_capture","native_cable_f32","native_render_loopback_f32"};
static void trace_observe(unsigned stage,double left,double right,uint64_t host,uint64_t position,uint32_t flags)
{
    unsigned p=atomic_load(&phase),fractional;
    trace_stats *s;
    if(!marker_trace) return;
    CHECK(stage<TRACE_STAGES && p<TRACE_PHASES); s=&trace_statistics[stage][p]; ++s->frames;
    left*=32768.0; right*=32768.0;
    if(!isfinite(left) || !isfinite(right)) { ++s->nonfinite; return; }
    fractional=left!=floor(left) || right!=floor(right);
    s->fractional+=fractional; s->asymmetric+=right!=-left;
    if(fractional && s->examples<TRACE_EXAMPLES) {
        trace_example *e=&s->example[s->examples++]; e->observed_ns=now_ns(); e->host_ns=host; e->position=position;
        e->left=left; e->right=right; e->flags=flags;
    }
}
static void trace_pair(unsigned native,uint64_t position,uint64_t host,float left,float right)
{
    trace_cell *own=native?trace_native_ring:trace_hail_ring,*other=native?trace_hail_ring:trace_native_ring;
    trace_cell *a=&own[position&(TRACE_RING-1u)],*b=&other[position&(TRACE_RING-1u)]; uint64_t delta;
    a->position=position; a->host_ns=host; a->left=left; a->right=right; a->valid=1;
    if(!b->valid || b->position!=position) return;
    delta=host>b->host_ns?host-b->host_ns:b->host_ns-host;
    /* Timestamp alignment only: SDK correlations have 100 ns granularity.
     * Numerical sample comparison below is exact, with zero tolerance. */
    if(delta>100u) { ++trace_clock_rejections; return; }
    ++trace_pairs; if(left!=b->left || right!=b->right) ++trace_pair_mismatches;
}
static void trace_open_tap(trace_tap *t,const char *key,unsigned loopback)
{
    static const GUID cls={0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
    static const GUID en={0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
    static const GUID cl={0x1cb9ad4c,0xdbfa,0x4c32,{0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}};
    static const GUID cap={0xc8adbd64,0xe71e,0x48a0,{0xa4,0xde,0x18,0x5c,0x39,0x5c,0xd3,0x17}};
    static const GUID floating={3,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
    IMMDeviceEnumerator *e=NULL; IMMDevice *d=NULL; WCHAR wide[HA_DEVICE_KEY_CAPACITY]; WAVEFORMATEXTENSIBLE w;
    CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,key,-1,wide,HA_DEVICE_KEY_CAPACITY)>0);
    CHECK(SUCCEEDED(CoCreateInstance(&cls,NULL,CLSCTX_ALL,&en,(void **)&e)));
    CHECK(SUCCEEDED(IMMDeviceEnumerator_GetDevice(e,wide,&d)));
    CHECK(SUCCEEDED(IMMDevice_Activate(d,&cl,CLSCTX_ALL,NULL,(void **)&t->client)));
    memset(&w,0,sizeof(w)); w.Format.wFormatTag=WAVE_FORMAT_EXTENSIBLE; w.Format.nChannels=2; w.Format.nSamplesPerSec=48000;
    w.Format.wBitsPerSample=32; w.Format.nBlockAlign=8; w.Format.nAvgBytesPerSec=384000; w.Format.cbSize=22;
    w.Samples.wValidBitsPerSample=32; w.dwChannelMask=3; w.SubFormat=floating;
    CHECK(SUCCEEDED(IAudioClient_Initialize(t->client,AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY|(loopback?AUDCLNT_STREAMFLAGS_LOOPBACK:0),200000,0,&w.Format,NULL)));
    CHECK(SUCCEEDED(IAudioClient_GetBufferSize(t->client,&t->capacity)) && t->capacity && t->capacity<=HA_MAX_QUANTUM);
    CHECK(SUCCEEDED(IAudioClient_GetService(t->client,&cap,(void **)&t->capture)));
    CHECK(SUCCEEDED(IAudioClient_Start(t->client))); IMMDevice_Release(d); IMMDeviceEnumerator_Release(e);
}
static void trace_start(void)
{
    if(!marker_taps) return;
    CHECK(SUCCEEDED(CoInitializeEx(NULL,COINIT_MULTITHREADED)));
    trace_open_tap(&trace_taps[0],trace_capture_key,0);
    if(marker_taps==2) trace_open_tap(&trace_taps[1],trace_playback_key,1);
}
static void trace_drain(void)
{
    unsigned leg,attempt;
    for(leg=0;leg<marker_taps;++leg) for(attempt=0;attempt<64;++attempt) {
        trace_tap *t=&trace_taps[leg]; BYTE *data=NULL; UINT32 next=0,n=0,i; DWORD flags=0; UINT64 position=0,qpc=0;
        CHECK(SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(t->capture,&next))); if(!next) break;
        CHECK(SUCCEEDED(IAudioCaptureClient_GetBuffer(t->capture,&data,&n,&flags,&position,&qpc)));
        CHECK(n && n<=t->capacity && (data || (flags&AUDCLNT_BUFFERFLAGS_SILENT)) && position<=UINT64_MAX-n && qpc<=UINT64_MAX/100u-NS);
        for(i=0;i<n;++i) {
            const float *f=(const float *)data; float left=(flags&AUDCLNT_BUFFERFLAGS_SILENT)?0:f[i*2u],right=(flags&AUDCLNT_BUFFERFLAGS_SILENT)?0:f[i*2u+1u];
            uint64_t host=qpc*100u+(uint64_t)i*NS/48000u;
            trace_observe(leg?TRACE_RENDER:TRACE_NATIVE,left,right,host,position+i,flags);
            if(!leg && !(flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) trace_pair(1,position+i,host,left,right);
        }
        CHECK(SUCCEEDED(IAudioCaptureClient_ReleaseBuffer(t->capture,n)));
    }
}
static void trace_stop(void)
{
    unsigned i; if(!marker_taps) return;
    trace_drain();
    for(i=0;i<marker_taps;++i) { CHECK(SUCCEEDED(IAudioClient_Stop(trace_taps[i].client))); IAudioCaptureClient_Release(trace_taps[i].capture); IAudioClient_Release(trace_taps[i].client); memset(&trace_taps[i],0,sizeof(trace_taps[i])); }
    CoUninitialize();
}
static void trace_report(void)
{
    unsigned stage,p,i; if(!marker_trace) return;
    printf("marker_trace taps=%u paired_frames=%llu paired_mismatches=%llu clock_alignment_rejections=%llu static_ring_bytes=%llu\n",marker_taps,
        (unsigned long long)trace_pairs,(unsigned long long)trace_pair_mismatches,(unsigned long long)trace_clock_rejections,
        (unsigned long long)(sizeof(trace_native_ring)+sizeof(trace_hail_ring)));
    for(stage=0;stage<TRACE_STAGES;++stage) for(p=0;p<TRACE_PHASES;++p) {
        trace_stats *s=&trace_statistics[stage][p];
        printf("marker_stage=%s phase=%u frames=%llu fractional=%llu asymmetric=%llu nonfinite=%llu\n",trace_names[stage],p,
            (unsigned long long)s->frames,(unsigned long long)s->fractional,(unsigned long long)s->asymmetric,(unsigned long long)s->nonfinite);
        for(i=0;i<s->examples;++i) { trace_example *e=&s->example[i];
            printf("marker_example stage=%s phase=%u observed_ns=%llu host_ns=%llu position=%llu left_s16=%.17g right_s16=%.17g flags=%u\n",trace_names[stage],p,
                (unsigned long long)e->observed_ns,(unsigned long long)e->host_ns,(unsigned long long)e->position,e->left,e->right,e->flags);
        }
    }
}
static int trace_self_test(void)
{
    marker_trace=1; atomic_store(&phase,2);
    trace_observe(TRACE_NATIVE,0.0625,-0.0625,0,0,0); CHECK(!trace_statistics[TRACE_NATIVE][2].fractional);
    trace_observe(TRACE_NATIVE,2048.5/32768.0,-2048.5/32768.0,0,0,0); CHECK(trace_statistics[TRACE_NATIVE][2].fractional==1);
    trace_pair(1,17,1000,0.0625f,-0.0625f); trace_pair(0,17,1100,0.0625f,-0.0625f); CHECK(trace_pairs==1 && !trace_pair_mismatches);
    trace_pair(1,18,1000,0.0625f,-0.0625f); trace_pair(0,18,1000,0.125f,-0.125f); CHECK(trace_pair_mismatches==1);
    trace_pair(1,19,1000,0,0); trace_pair(0,19,1101,0,0); CHECK(trace_pairs==2 && trace_clock_rejections==1);
    puts("PASS marker trace distinguishes fractional PCM, exact pairs and unrelated clock correlations"); return 0;
}
