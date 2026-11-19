/* Device-free SDK seams. All fake interfaces are used on this test thread. */
#define COBJMACROS
#define CONST_VTABLE
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
static BOOL signal_ok = TRUE;
static DWORD thread_state = WAIT_TIMEOUT;
static BOOL WINAPI test_signal(HANDLE h) { (void)h; return signal_ok; }
static DWORD WINAPI test_wait(HANDLE h, DWORD ms) { (void)h; (void)ms; return thread_state; }
#define SetEvent test_signal
#define WaitForSingleObject test_wait
#define hail_wasapi_backend hail_test_wasapi_buffers_backend
#include "../src/backend_wasapi.c"
#undef hail_wasapi_backend
#undef WaitForSingleObject
#undef SetEvent
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)
static UINT32 packet_frames, packet_flags, packets, released, padding;
static HRESULT get_result, release_result, starts[2], stops[2], resets[2];
static unsigned start_calls[2], stop_calls[2], reset_calls[2];
static BYTE *packet_data;
static IAudioClient clients[2];
static HRESULT STDMETHODCALLTYPE next_packet(IAudioCaptureClient *c, UINT32 *n)
{ (void)c; *n = packets ? 4u : 0u; return S_OK; }
static HRESULT STDMETHODCALLTYPE capture_buffer(IAudioCaptureClient *c, BYTE **p, UINT32 *n, DWORD *flags, UINT64 *pos, UINT64 *qpc)
{ (void)c; --packets; *p=packet_data; *n=packet_frames; *flags=packet_flags; *pos=100; *qpc=200; return get_result; }
static HRESULT STDMETHODCALLTYPE capture_release(IAudioCaptureClient *c, UINT32 n)
{ (void)c; CHECK(n==packet_frames); ++released; return release_result; }
static HRESULT STDMETHODCALLTYPE get_padding(IAudioClient *c, UINT32 *n)
{ (void)c; *n=padding; return S_OK; }
static HRESULT STDMETHODCALLTYPE render_buffer(IAudioRenderClient *c, UINT32 n, BYTE **p)
{ (void)c; (void)n; *p=packet_data; return get_result; }
static HRESULT STDMETHODCALLTYPE render_release(IAudioRenderClient *c, UINT32 n, DWORD flags)
{ (void)c; (void)n; (void)flags; ++released; return release_result; }
static HRESULT STDMETHODCALLTYPE start_client(IAudioClient *c)
{ unsigned i=(unsigned)(c-clients); ++start_calls[i]; return starts[i]; }
static HRESULT STDMETHODCALLTYPE stop_client(IAudioClient *c)
{ unsigned i=(unsigned)(c-clients); ++stop_calls[i]; return stops[i]; }
static HRESULT STDMETHODCALLTYPE reset_client(IAudioClient *c)
{ unsigned i=(unsigned)(c-clients); ++reset_calls[i]; return resets[i]; }
static const IAudioCaptureClientVtbl capture_ops = { .GetBuffer=capture_buffer, .ReleaseBuffer=capture_release, .GetNextPacketSize=next_packet };
static const IAudioRenderClientVtbl render_ops = { .GetBuffer=render_buffer, .ReleaseBuffer=render_release };
static const IAudioClientVtbl client_ops = { .GetCurrentPadding=get_padding, .Start=start_client, .Stop=stop_client, .Reset=reset_client };
static IAudioCaptureClient capture_client = { &capture_ops };
static IAudioRenderClient render_client = { &render_ops };
static ha_stream stream;
static wa_stream adapter;
static unsigned char silence[2u * HA_MAX_CHANNELS * sizeof(float)];
static _Alignas(16) unsigned char storage[2048];
static void setup(void)
{
    ha_format f=HA_FORMAT_INIT(48000,2,HA_PCM_S16);
    memset(&stream,0,sizeof(stream)); memset(&adapter,0,sizeof(adapter));
    memset(storage,0xcc,sizeof(storage));
    stream.backend_state=&adapter; stream.info.capture_format=f; stream.info.playback_format=f;
    stream.info.quantum_frames=2; stream.concurrent=1;
    stream.capture.storage=storage; stream.capture.slots=4; stream.capture.quantum=2;
    stream.capture.bytes_per_frame=4; stream.capture.stride=(uint32_t)hail_align(sizeof(hail_slot)+8u);
    atomic_init(&stream.capture.read_index,0); atomic_init(&stream.capture.write_index,0);
    stream.playback.storage=storage+1024; stream.playback.slots=4; stream.playback.quantum=2;
    stream.playback.bytes_per_frame=4; stream.playback.stride=stream.capture.stride;
    atomic_init(&stream.playback.read_index,0); atomic_init(&stream.playback.write_index,0);
    atomic_init(&stream.state,HA_RUNNING); hail_rt_init(&stream); hail_rt_open(&stream,0); hail_rt_open(&stream,1);
    adapter.owner=&stream; adapter.capture=&capture_client; adapter.render=&render_client;
    adapter.silence=silence;
    adapter.client[0]=&clients[0]; adapter.client[1]=&clients[1]; adapter.frames[0]=4; adapter.frames[1]=4;
    adapter.capture_engine_rate=48000;
    clients[0].lpVtbl=&client_ops; clients[1].lpVtbl=&client_ops;
    atomic_init(&adapter.command,0); atomic_init(&adapter.done,0);
    packet_frames=4; packet_flags=0; packets=1; released=0; padding=0; packet_data=NULL;
    get_result=S_OK; release_result=S_OK;
    memset(starts,0,sizeof(starts)); memset(stops,0,sizeof(stops)); memset(resets,0,sizeof(resets));
    memset(start_calls,0,sizeof(start_calls)); memset(stop_calls,0,sizeof(stop_calls)); memset(reset_calls,0,sizeof(reset_calls));
    signal_ok=TRUE; thread_state=WAIT_TIMEOUT;
}
static void notification_test(void)
{
    ha_context c={0}; wa_context p={0}; hail_device devices[HA_MAX_DEVICES],replacement;
    ha_stream *streams[1]={&stream}; unsigned i;
    setup(); c.backend_state=&p; c.backend=&hail_test_wasapi_buffers_backend;
    c.devices=devices; c.max_devices=HA_MAX_DEVICES; c.streams=streams; c.max_streams=1; c.next_id=100;
    c.device_count=1; stream.context=&c; stream.info.capture_device_id=7;
    CHECK(hail_device_record(&devices[0],HA_CAPTURE,"fixture","fixture",0)==HA_OK);
    devices[0].info.runtime_id=7; p.owner=&c; p.notification.owner=&p;
    atomic_init(&c.registry_dirty,0); atomic_init(&p.lost_removals,0);
    for(i=0;i<16;++i) atomic_init(&p.removals[i].state,0);
    CHECK(notify_removal(&p.notification.iface,L"fixture")==S_OK);
    CHECK(c.device_count==1 && atomic_load(&stream.state)==HA_RUNNING && atomic_load(&c.registry_dirty));
    wa_context_poll(&c); CHECK(!c.device_count && atomic_load(&stream.state)==HA_FAILED && stream.status.terminal_result==HA_DEVICE_LOST);
    CHECK(hail_device_record(&replacement,HA_CAPTURE,"fixture","fixture",0)==HA_OK);
    CHECK(hail_registry_publish(&c,&replacement,1,"fixture",NULL)==HA_OK && c.devices[0].info.runtime_id!=7);
    for(i=0;i<17;++i) CHECK(notify_removal(&p.notification.iface,L"unknown")==S_OK);
    CHECK(atomic_load(&p.lost_removals)); wa_context_poll(&c); CHECK(!c.device_count);
}
static void multichannel_test(void)
{
    float input[2u*HA_MAX_CHANNELS], output[2u*HA_MAX_CHANNELS];
    ha_block_info b=HA_INIT(ha_block_info);
    ha_stream_info info=HA_INIT(ha_stream_info); WAVEFORMATEXTENSIBLE wave; size_t bytes;
    const uint32_t counts[]={1,2,4,HA_MAX_CHANNELS};
    for(unsigned k=0;k<sizeof(counts)/sizeof(counts[0]);++k) {
        uint32_t channels=counts[k]; ha_format f=HA_FORMAT_INIT(48000,channels,HA_PCM_F32);
        f.channel_layout=HA_LAYOUT_DISCRETE;
        setup(); stream.info.capture_format=f;
        stream.capture.bytes_per_frame=channels*4u; stream.capture.slots=2;
        stream.capture.stride=(uint32_t)hail_align(sizeof(hail_slot)+2u*channels*4u);
        info.capture_format=f; info.capture_slots=2; info.quantum_frames=2;
        CHECK(wa_extra_bytes(&info,&bytes)==HA_OK && bytes==2u*channels*4u);
        wave_format(&f,&wave); CHECK(!wave.dwChannelMask && wave.Format.nChannels==channels);
        for(unsigned i=0;i<2u*channels;++i) input[i]=(float)i/128.0f;
        packet_data=(BYTE *)(void *)input; packet_frames=2; wa_capture(&adapter);
        CHECK(!atomic_load(&stream.rt[0].error));
        CHECK(hail_ring_read(&stream.capture,output,2,&b)==2 && !memcmp(input,output,bytes));
        packets=1; packet_flags=AUDCLNT_BUFFERFLAGS_SILENT; wa_capture(&adapter);
        CHECK(hail_ring_read(&stream.capture,output,2,&b)==2);
        for(unsigned i=0;i<2u*channels;++i) CHECK(output[i]==0);
    }
}
static void fractional_capture_test(void)
{
    /* Values observed in the failing SDK capture lease, in S16 units.
     * Fractional samples are valid F32 and must reach the caller bit-exact.
     * This checks provenance, not transparency of the Windows mixer/driver. */
    const float input[] = {3062.0f/32768.0f, -3062.125f/32768.0f,
        4095.75f/32768.0f, -4096.0f/32768.0f,
        8191.0f/32768.0f, -8190.875f/32768.0f, 0.0f, -0.0f};
    float output[8]; ha_block_info b = HA_INIT(ha_block_info);
    setup(); stream.info.capture_format.sample_format = HA_PCM_F32;
    stream.capture.bytes_per_frame = 8;
    stream.capture.stride = (uint32_t)hail_align(sizeof(hail_slot) + 16u);
    packet_data = (BYTE *)(const void *)input;
    wa_capture(&adapter);
    CHECK(released == 1 && !atomic_load(&stream.rt[0].error));
    CHECK(hail_ring_read(&stream.capture, output, 2, &b) == 2);
    CHECK(hail_ring_read(&stream.capture, output + 4, 2, &b) == 2);
    CHECK(!memcmp(input, output, sizeof(input)));
}
int main(void)
{
    ha_block_info b=HA_INIT(ha_block_info); int16_t pcm[4];
    int16_t input[8]={100,-100,101,-101,102,-102,103,-103};
    setup(); packet_data=(BYTE *)(void *)input; wa_capture(&adapter);
    CHECK(released==1 && !atomic_load(&stream.rt[0].error));
    CHECK(hail_ring_read(&stream.capture,pcm,2,&b)==2 && !memcmp(pcm,input,sizeof(pcm)));
    CHECK(hail_ring_read(&stream.capture,pcm,2,&b)==2 && !memcmp(pcm,input+4,sizeof(pcm)));
    setup(); CHECK(hail_ring_push(&stream.playback,input,4,&b)==4); packet_data=(BYTE *)(void *)pcm; wa_render(&adapter);
    CHECK(released==2 && !atomic_load(&stream.rt[1].error) && !memcmp(pcm,input+4,sizeof(pcm)));
    setup(); CHECK(hail_ring_push(&stream.playback,input,2,&b)==2); packet_data=(BYTE *)(void *)pcm; wa_render(&adapter);
    CHECK(released==2 && stream.rt[1].current.underrun==2 && !pcm[0] && !pcm[3]);
    setup(); packet_flags=AUDCLNT_BUFFERFLAGS_SILENT; wa_capture(&adapter);
    CHECK(released==1 && !atomic_load(&stream.rt[0].error));
    CHECK(hail_ring_read(&stream.capture,pcm,2,&b)==2 && (b.flags & HA_BLOCK_SILENCE));
    CHECK((b.flags & (HA_TIME_HOST_VALID|HA_TIME_DEVICE_VALID))==(HA_TIME_HOST_VALID|HA_TIME_DEVICE_VALID));
    CHECK(b.device_position==100 && b.host_time_ns==20000 && pcm[0]==0 && pcm[3]==0);
    CHECK(hail_ring_read(&stream.capture,pcm,2,&b)==2 && b.device_position==102 && (b.flags & HA_BLOCK_SILENCE));
    setup(); stream.info.capture_format.sample_rate=16000; packet_flags=AUDCLNT_BUFFERFLAGS_SILENT; wa_capture(&adapter);
    CHECK(hail_ring_read(&stream.capture,pcm,2,&b)==2 && !(b.flags&HA_TIME_DEVICE_VALID));
    CHECK((b.flags&(HA_TIME_HOST_VALID|HA_TIME_ESTIMATED))==(HA_TIME_HOST_VALID|HA_TIME_ESTIMATED));
    CHECK(!b.device_position && b.frame_time_denominator==16000);
    setup(); get_result=AUDCLNT_S_BUFFER_EMPTY; packet_frames=0; wa_capture(&adapter);
    CHECK(!released && !atomic_load(&stream.rt[0].error));
    setup(); packet_frames=5; packet_flags=AUDCLNT_BUFFERFLAGS_SILENT; wa_capture(&adapter);
    CHECK(released==1 && atomic_load(&stream.rt[0].error)==HA_STREAM_FAILED && !atomic_load(&stream.capture.write_index));
    setup(); wa_capture(&adapter); CHECK(released==1 && atomic_load(&stream.rt[0].error)==HA_STREAM_FAILED);
    setup(); packet_flags=AUDCLNT_BUFFERFLAGS_SILENT|AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR|AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
    wa_capture(&adapter); CHECK(hail_ring_read(&stream.capture,pcm,2,&b)==2);
    CHECK(!(b.flags & (HA_TIME_HOST_VALID|HA_TIME_DEVICE_VALID)) && (b.flags & HA_BLOCK_UNKNOWN_GAP));
    setup(); get_result=AUDCLNT_E_DEVICE_INVALIDATED; wa_capture(&adapter);
    CHECK(!released && atomic_load(&stream.rt[0].error)==HA_DEVICE_LOST);
    setup(); packet_flags=AUDCLNT_BUFFERFLAGS_SILENT; release_result=E_FAIL; wa_capture(&adapter);
    CHECK(released==1 && atomic_load(&stream.rt[0].error)==HA_STREAM_FAILED);
    setup(); wa_render(&adapter); CHECK(released==1 && atomic_load(&stream.rt[1].error)==HA_STREAM_FAILED);
    setup(); padding=5; wa_render(&adapter); CHECK(!released && atomic_load(&stream.rt[1].error)==HA_STREAM_FAILED);
    setup(); starts[1]=E_ACCESSDENIED;
    CHECK(wa_command(&adapter,WA_START)==E_ACCESSDENIED && start_calls[0]==1 && start_calls[1]==1 && stop_calls[0]==1);
    CHECK(!adapter.started[0] && !adapter.started[1]);
    setup(); adapter.started[0]=adapter.started[1]=1; stops[0]=E_FAIL;
    CHECK(wa_command(&adapter,WA_STOP)==E_FAIL && adapter.started[0] && !adapter.started[1] && !reset_calls[0]);
    setup(); adapter.started[0]=adapter.started[1]=1;
    CHECK(wa_command(&adapter,WA_FLUSH)==S_OK && adapter.started[0] && !stop_calls[0] && !reset_calls[0] && start_calls[1]==1);
    setup(); adapter.started[0]=adapter.started[1]=1; starts[1]=E_FAIL;
    CHECK(wa_command(&adapter,WA_FLUSH)==E_FAIL && adapter.started[0] && !adapter.started[1] && !stop_calls[0]);
    setup(); starts[1]=E_ACCESSDENIED; stops[0]=E_FAIL;
    CHECK(wa_command(&adapter,WA_START)==E_ACCESSDENIED && adapter.started[0]);
    setup(); adapter.pending=WA_START; adapter.result=HA_PERMISSION_DENIED; adapter.native_result=E_ACCESSDENIED; atomic_store(&adapter.done,1);
    CHECK(wa_progress(&stream,WA_STOP)==HA_PERMISSION_DENIED && !adapter.pending && stream.status.diagnostic.native_code==E_ACCESSDENIED);
    setup(); adapter.pending=WA_START; CHECK(wa_progress(&stream,WA_STOP)==HA_PENDING && adapter.pending==WA_START);
    atomic_store(&adapter.done,1); adapter.result=HA_OK;
    CHECK(wa_progress(&stream,WA_STOP)==HA_PENDING && adapter.pending==WA_STOP && atomic_load(&adapter.command)==WA_STOP);
    setup(); signal_ok=FALSE; CHECK(wa_progress(&stream,WA_START)==HA_STREAM_FAILED);
    setup(); thread_state=WAIT_OBJECT_0; CHECK(wa_progress(&stream,WA_STOP)==HA_STREAM_FAILED);
    { wa_context p={0}; CHECK(wa_context_request(&p,WA_REFRESH)==HA_STREAM_FAILED);
      p.reply=CreateEventW(NULL,FALSE,FALSE,NULL); CHECK(p.reply); signal_ok=FALSE;
      CHECK(wa_context_request(&p,WA_REFRESH)==HA_STREAM_FAILED); CHECK(CloseHandle(p.reply)); }
    notification_test(); multichannel_test(); fractional_capture_test();
    puts("WASAPI packet leases, flags, bounds, rollback and command supersession passed");
    return 0;
}
