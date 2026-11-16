#define COBJMACROS
#define CONST_VTABLE
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "hail_internal.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <propidl.h>

/* SDK interface IDs are private constants rather than INITGUID definitions,
 * which would add unrelated COM globals to a statically linked Hail archive. */
#define IID_IAudioClock hail_iid_clock
#define IID_IAudioCaptureClient hail_iid_capture
#define IID_IAudioRenderClient hail_iid_render
#define IID_IAudioClient hail_iid_client
#define IID_IMMDeviceEnumerator hail_iid_enumerator
#define IID_IMMEndpoint hail_iid_endpoint
#define IID_IMMNotificationClient hail_iid_notification
#define CLSID_MMDeviceEnumerator hail_clsid_enumerator
static const GUID hail_iid_clock = {0xcd63314f,0x3fba,0x4a1b,{0x81,0x2c,0xef,0x96,0x35,0x87,0x28,0xe7}};
static const GUID hail_iid_capture = {0xc8adbd64,0xe71e,0x48a0,{0xa4,0xde,0x18,0x5c,0x39,0x5c,0xd3,0x17}};
static const GUID hail_iid_render = {0xf294acfc,0x3146,0x4483,{0xa7,0xbf,0xad,0xdc,0xa7,0xc2,0x60,0xe2}};
static const GUID hail_iid_client = {0x1cb9ad4c,0xdbfa,0x4c32,{0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}};
static const GUID hail_iid_enumerator = {0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
static const GUID hail_iid_endpoint = {0x1be09788,0x6894,0x4089,{0x85,0x86,0x9a,0x2a,0x6c,0x26,0x5a,0xc5}};
static const GUID hail_iid_notification = {0x7991eec9,0x7e89,0x4d85,{0x83,0x90,0x6c,0x70,0x3c,0xec,0x60,0xc0}};
static const GUID hail_clsid_enumerator = {0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};

#define WA_REFRESH 1u
#define WA_PROBE 2u
#define WA_EXIT 3u
#define WA_START 4u
#define WA_STOP 5u
#define WA_FLUSH 6u
#define WA_RETIRE 7u

typedef struct wa_context wa_context;
typedef struct wa_notification {
    IMMNotificationClient iface;
    LONG refs, active;
    wa_context *owner;
} wa_notification;
struct wa_context {
    ha_context *owner;
    HANDLE thread, request, reply;
    atomic_uint command, done, live;
    struct { atomic_uint state; char key[1024]; } removals[16];
    atomic_uint lost_removals;
    ha_result result;
    IMMDeviceEnumerator *enumerator;
    wa_notification notification;
    uint32_t registered, count;
    hail_device devices[HA_MAX_DEVICES];
    char defaults[2][1024], probe_key[1024];
    ha_format probe_format;
    uint32_t probe_negotiation;
    ha_probe_info probe_info;
};
typedef struct wa_stream {
    ha_stream *owner;
    HANDLE thread, request, reply, audio[2];
    atomic_uint command, done;
    uint32_t pending, started[2], frames[2];
    ha_result result;
    WCHAR keys[2][1024];
    IAudioClient *client[2];
    IAudioCaptureClient *capture;
    IAudioRenderClient *render;
    IAudioClock *clock;
    unsigned char silence[HA_MAX_QUANTUM * 8u];
} wa_stream;

static ha_result wa_result(HRESULT hr)
{
    if (hr == E_ACCESSDENIED) return HA_PERMISSION_DENIED;
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) return HA_DEVICE_LOST;
    if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) return HA_FORMAT_UNSUPPORTED;
    if (hr == E_OUTOFMEMORY) return HA_RESOURCE_EXHAUSTED;
    return SUCCEEDED(hr) ? HA_OK : HA_STREAM_FAILED;
}
static int utf8(char *out, int capacity, const WCHAR *in)
{
    int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, in, -1, NULL, 0, NULL, NULL);
    if (required <= 0 || required > capacity) return 0;
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, in, -1, out, capacity, NULL, NULL) == required;
}
static int wide(WCHAR *out, int capacity, const char *in)
{
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in, -1, out, capacity) > 0;
}
static void wave_format(const ha_format *f, WAVEFORMATEXTENSIBLE *w)
{
    static const GUID pcm = {1,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
    static const GUID floating = {3,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
    memset(w, 0, sizeof(*w));
    w->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; w->Format.nChannels = (WORD)f->channels;
    w->Format.nSamplesPerSec = f->sample_rate; w->Format.wBitsPerSample = f->sample_format == HA_PCM_S16 ? 16 : 32;
    w->Format.nBlockAlign = (WORD)(f->channels * w->Format.wBitsPerSample / 8u);
    w->Format.nAvgBytesPerSec = w->Format.nSamplesPerSec * w->Format.nBlockAlign;
    w->Format.cbSize = 22; w->Samples.wValidBitsPerSample = w->Format.wBitsPerSample;
    w->dwChannelMask = f->channels == 1 ? SPEAKER_FRONT_CENTER : SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    w->SubFormat = f->sample_format == HA_PCM_S16 ? pcm : floating;
}
static void engine_format(const WAVEFORMATEX *w, ha_probe_info *out)
{
    ha_format f = HA_INIT(ha_format);
    uint32_t format = 0;
    if (w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && w->wBitsPerSample == 32) format = HA_PCM_F32;
    if (w->wFormatTag == WAVE_FORMAT_PCM && w->wBitsPerSample == 16) format = HA_PCM_S16;
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE *ex = (const WAVEFORMATEXTENSIBLE *)(const void *)w;
        if (ex->SubFormat.Data1 == 3 && w->wBitsPerSample == 32) format = HA_PCM_F32;
        if (ex->SubFormat.Data1 == 1 && w->wBitsPerSample == 16) format = HA_PCM_S16;
    }
    if (!format) return;
    f.sample_rate = w->nSamplesPerSec; f.channels = w->nChannels; f.sample_format = format;
    f.storage = HA_INTERLEAVED; f.channel_layout = f.channels == 1 ? HA_LAYOUT_MONO : f.channels == 2 ? HA_LAYOUT_STEREO : 0;
    out->backend_format = f; out->backend_format_valid = 1;
}

static HRESULT STDMETHODCALLTYPE notify_query(IMMNotificationClient *iface, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IMMNotificationClient)) return E_NOINTERFACE;
    *out = iface; IMMNotificationClient_AddRef(iface); return S_OK;
}
static ULONG STDMETHODCALLTYPE notify_add(IMMNotificationClient *iface)
{ return (ULONG)InterlockedIncrement(&((wa_notification *)iface)->refs); }
static ULONG STDMETHODCALLTYPE notify_release(IMMNotificationClient *iface)
{ return (ULONG)InterlockedDecrement(&((wa_notification *)iface)->refs); }
static HRESULT notify_dirty(IMMNotificationClient *iface)
{
    wa_notification *n = (wa_notification *)iface;
    InterlockedIncrement(&n->active);
    atomic_store_explicit(&n->owner->owner->registry_dirty, 1, memory_order_release);
    InterlockedDecrement(&n->active); return S_OK;
}
static HRESULT notify_removal(IMMNotificationClient *iface, LPCWSTR id)
{
    wa_notification *n = (wa_notification *)iface;
    wa_context *p = n->owner;
    unsigned i;
    InterlockedIncrement(&n->active);
    for (i = 0; i < 16; ++i) {
        unsigned expected = 0;
        if (!atomic_compare_exchange_strong_explicit(&p->removals[i].state, &expected, 1,
                memory_order_acquire, memory_order_relaxed)) continue;
        if (!id || !utf8(p->removals[i].key, sizeof(p->removals[i].key), id)) {
            atomic_store_explicit(&p->lost_removals, 1, memory_order_release);
            atomic_store_explicit(&p->removals[i].state, 0, memory_order_release);
        } else atomic_store_explicit(&p->removals[i].state, 2, memory_order_release);
        break;
    }
    if (i == 16) atomic_store_explicit(&p->lost_removals, 1, memory_order_release);
    atomic_store_explicit(&p->owner->registry_dirty, 1, memory_order_release);
    InterlockedDecrement(&n->active); return S_OK;
}
static HRESULT STDMETHODCALLTYPE notify_state(IMMNotificationClient *i, LPCWSTR id, DWORD state)
{ return state == DEVICE_STATE_ACTIVE ? notify_dirty(i) : notify_removal(i, id); }
static HRESULT STDMETHODCALLTYPE notify_added(IMMNotificationClient *i, LPCWSTR id)
{ (void)id; return notify_dirty(i); }
static HRESULT STDMETHODCALLTYPE notify_removed(IMMNotificationClient *i, LPCWSTR id)
{ return notify_removal(i, id); }
static HRESULT STDMETHODCALLTYPE notify_default(IMMNotificationClient *i, EDataFlow flow, ERole role, LPCWSTR id)
{ (void)flow; (void)role; (void)id; return notify_dirty(i); }
static HRESULT STDMETHODCALLTYPE notify_property(IMMNotificationClient *i, LPCWSTR id, const PROPERTYKEY key)
{ (void)id; (void)key; return notify_dirty(i); }
static const IMMNotificationClientVtbl notification_vtable = {
    notify_query, notify_add, notify_release, notify_state, notify_added, notify_removed, notify_default, notify_property
};

static ha_result wa_enumerate(wa_context *p)
{
    static const PROPERTYKEY friendly = {{0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}},14};
    IMMDeviceCollection *collection = NULL;
    UINT count = 0, i; HRESULT hr;
    p->count = 0; p->defaults[0][0] = 0; p->defaults[1][0] = 0;
    hr = IMMDeviceEnumerator_EnumAudioEndpoints(p->enumerator, eAll, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) return wa_result(hr);
    hr = IMMDeviceCollection_GetCount(collection, &count);
    if (SUCCEEDED(hr) && count > p->owner->max_devices) hr = E_OUTOFMEMORY;
    for (i = 0; SUCCEEDED(hr) && i < count; ++i) {
        IMMDevice *device = NULL; IMMEndpoint *endpoint = NULL; IPropertyStore *properties = NULL;
        LPWSTR id = NULL; EDataFlow flow; char key[1024], label[1024]; PROPVARIANT value;
        PropVariantInit(&value);
        hr = IMMDeviceCollection_Item(collection, i, &device);
        if (SUCCEEDED(hr)) hr = IMMDevice_GetId(device, &id);
        if (SUCCEEDED(hr)) hr = IMMDevice_QueryInterface(device, &IID_IMMEndpoint, (void **)&endpoint);
        if (SUCCEEDED(hr)) hr = IMMEndpoint_GetDataFlow(endpoint, &flow);
        if (SUCCEEDED(hr) && !utf8(key, sizeof(key), id)) hr = E_INVALIDARG;
        if (SUCCEEDED(hr)) {
            memcpy(label, key, strlen(key) + 1u);
            if (SUCCEEDED(IMMDevice_OpenPropertyStore(device, STGM_READ, &properties)) &&
                SUCCEEDED(IPropertyStore_GetValue(properties, &friendly, &value)) && value.vt == VT_LPWSTR)
                (void)utf8(label, sizeof(label), value.pwszVal);
            if (hail_device_record(&p->devices[p->count], flow == eCapture ? HA_CAPTURE : HA_PLAYBACK, key, label, 0) != HA_OK)
                hr = E_OUTOFMEMORY;
            else { hail_device_persistent(&p->devices[p->count]); ++p->count; }
        }
        PropVariantClear(&value);
        if (properties) IPropertyStore_Release(properties);
        if (endpoint) IMMEndpoint_Release(endpoint);
        if (id) CoTaskMemFree(id);
        if (device) IMMDevice_Release(device);
    }
    IMMDeviceCollection_Release(collection);
    if (FAILED(hr)) return wa_result(hr);
    for (i = 0; i < 2; ++i) {
        IMMDevice *device = NULL; LPWSTR id = NULL;
        if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(p->enumerator, i ? eRender : eCapture, eMultimedia, &device))) {
            if (SUCCEEDED(IMMDevice_GetId(device, &id))) { (void)utf8(p->defaults[i], sizeof(p->defaults[i]), id); CoTaskMemFree(id); }
            IMMDevice_Release(device);
        }
    }
    return HA_OK;
}

static ha_result wa_probe_worker(wa_context *p)
{
    IMMDevice *device = NULL; IAudioClient *client = NULL; WAVEFORMATEX *mix = NULL;
    WCHAR key[1024]; HRESULT hr;
    memset(&p->probe_info, 0, sizeof(p->probe_info));
    p->probe_info.struct_size = sizeof(p->probe_info); p->probe_info.struct_version = HA_STRUCT_VERSION;
    if (p->probe_negotiation == HA_EXACT_END_TO_END) return HA_UNSUPPORTED;
    if (!wide(key, 1024, p->probe_key)) return HA_INVALID_ARGUMENT;
    hr = IMMDeviceEnumerator_GetDevice(p->enumerator, key, &device);
    if (SUCCEEDED(hr)) hr = IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetMixFormat(client, &mix);
    if (SUCCEEDED(hr)) {
        p->probe_info.application_format = p->probe_format;
        p->probe_info.conversion = HA_CONVERSION_NATIVE;
        p->probe_info.capabilities = HA_FEATURE_NATIVE_FLUSH | HA_FEATURE_RENDER_REFERENCE;
        engine_format(mix, &p->probe_info);
    }
    if (mix) CoTaskMemFree(mix);
    if (client) IAudioClient_Release(client);
    if (device) IMMDevice_Release(device);
    return wa_result(hr);
}

static DWORD WINAPI wa_context_worker(void *user)
{
    wa_context *p = user;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int initialized = SUCCEEDED(hr);
    if (initialized) hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&p->enumerator);
    if (SUCCEEDED(hr)) {
        ha_result r = wa_enumerate(p);
        if (r != HA_OK) hr = r == HA_RESOURCE_EXHAUSTED ? E_OUTOFMEMORY : E_FAIL;
    }
    if (SUCCEEDED(hr)) {
        p->notification.iface.lpVtbl = &notification_vtable; p->notification.refs = 1; p->notification.owner = p;
        hr = IMMDeviceEnumerator_RegisterEndpointNotificationCallback(p->enumerator, &p->notification.iface);
        if (SUCCEEDED(hr)) p->registered = 1;
    }
    atomic_store_explicit(&p->live, SUCCEEDED(hr), memory_order_release);
    p->result = wa_result(hr);
    atomic_store_explicit(&p->done, 1, memory_order_release); SetEvent(p->reply);
    if (SUCCEEDED(hr)) for (;;) {
        uint32_t command;
        (void)WaitForSingleObject(p->request, INFINITE);
        command = atomic_exchange_explicit(&p->command, 0, memory_order_acquire);
        if (command == WA_EXIT) {
            HRESULT unregistered = S_OK;
            if (p->registered) {
                unregistered = IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(p->enumerator, &p->notification.iface);
                if (SUCCEEDED(unregistered) || unregistered == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) p->registered = 0;
            }
            p->result = p->registered || InterlockedCompareExchange(&p->notification.active, 0, 0) ||
                InterlockedCompareExchange(&p->notification.refs, 0, 0) > 1 ? HA_PENDING : HA_OK;
            if (p->result == HA_OK) atomic_store_explicit(&p->live, 0, memory_order_release);
            atomic_store_explicit(&p->done, 1, memory_order_release); SetEvent(p->reply);
            if (p->result == HA_OK) break;
            continue;
        }
        p->result = command == WA_REFRESH ? wa_enumerate(p) : wa_probe_worker(p);
        atomic_store_explicit(&p->done, 1, memory_order_release); SetEvent(p->reply);
    }
    if (p->enumerator) { IMMDeviceEnumerator_Release(p->enumerator); p->enumerator = NULL; }
    if (initialized) CoUninitialize();
    return 0;
}
static ha_result wa_context_request(wa_context *p, uint32_t command)
{
    ResetEvent(p->reply); atomic_store_explicit(&p->done, 0, memory_order_relaxed);
    atomic_store_explicit(&p->command, command, memory_order_release); SetEvent(p->request);
    for (;;) {
        DWORD wait = WaitForSingleObject(p->reply, INFINITE);
        if (wait != WAIT_OBJECT_0) return HA_STREAM_FAILED;
        if (atomic_load_explicit(&p->done, memory_order_acquire)) return p->result;
    }
}
/* Bounded MPSC notification slots: native notifications never touch the registry.
 * Losing removal identities conservatively invalidates all old incarnations. */
static void wa_context_poll(ha_context *c)
{
    wa_context *p = c->backend_state;
    unsigned i;
    if (atomic_exchange_explicit(&p->lost_removals, 0, memory_order_acquire)) {
        while (c->device_count) {
            char key[1024];
            memcpy(key, c->devices[0].key, sizeof(key));
            hail_registry_remove(c, key, 0, HA_CAPTURE | HA_PLAYBACK);
        }
    }
    for (i = 0; i < 16; ++i) if (atomic_load_explicit(&p->removals[i].state, memory_order_acquire) == 2) {
        hail_registry_remove(c, p->removals[i].key, 0, HA_CAPTURE | HA_PLAYBACK);
        atomic_store_explicit(&p->removals[i].state, 0, memory_order_release);
    }
}
static ha_result wa_refresh(ha_context *c)
{
    wa_context *p = c->backend_state; ha_result r = wa_context_request(p, WA_REFRESH);
    wa_context_poll(c);
    return r == HA_OK ? hail_registry_publish(c, p->devices, p->count, p->defaults[0], p->defaults[1]) : r;
}
static ha_result wa_context_init(ha_context *c)
{
    wa_context *p = c->backend_state;
    unsigned i;
    atomic_init(&p->lost_removals, 0);
    for (i = 0; i < 16; ++i) atomic_init(&p->removals[i].state, 0);
    p->owner = c; atomic_init(&p->command, 0); atomic_init(&p->done, 0); atomic_init(&p->live, 0);
    p->request = CreateEventW(NULL, FALSE, FALSE, NULL); p->reply = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!p->request || !p->reply) return HA_RESOURCE_EXHAUSTED;
    p->thread = CreateThread(NULL, 0, wa_context_worker, p, 0, NULL);
    if (!p->thread) return HA_RESOURCE_EXHAUSTED;
    if (WaitForSingleObject(p->reply, INFINITE) != WAIT_OBJECT_0 || !atomic_load_explicit(&p->done, memory_order_acquire)) return HA_BACKEND_INIT_FAILED;
    return p->result == HA_OK ? hail_registry_publish(c, p->devices, p->count, p->defaults[0], p->defaults[1]) : p->result;
}
static ha_result wa_context_free(ha_context *c)
{
    wa_context *p = c->backend_state;
    if (p->thread) {
        if (atomic_load_explicit(&p->live, memory_order_acquire)) {
            ha_result r = wa_context_request(p, WA_EXIT); if (r != HA_OK) return r;
        }
        WaitForSingleObject(p->thread, INFINITE); CloseHandle(p->thread);
    }
    if (p->request) CloseHandle(p->request);
    if (p->reply) CloseHandle(p->reply);
    memset(p, 0, sizeof(*p));
    return HA_OK;
}
static ha_result wa_probe(ha_context *c, uint64_t id, const ha_format *f, uint32_t negotiation, ha_probe_info *out)
{
    wa_context *p = c->backend_state; hail_device *d = hail_device_find(c, id); ha_result r;
    if (!d) return HA_DEVICE_UNAVAILABLE;
    memcpy(p->probe_key, d->key, strlen(d->key) + 1u); p->probe_format = *f; p->probe_negotiation = negotiation;
    r = wa_context_request(p, WA_PROBE);
    if (r == HA_OK) *out = p->probe_info;
    return r;
}

static void wa_clients_free(wa_stream *a)
{
    uint32_t i;
    if (a->clock) { IAudioClock_Release(a->clock); a->clock = NULL; }
    if (a->capture) { IAudioCaptureClient_Release(a->capture); a->capture = NULL; }
    if (a->render) { IAudioRenderClient_Release(a->render); a->render = NULL; }
    for (i = 0; i < 2; ++i) if (a->client[i]) { IAudioClient_Stop(a->client[i]); IAudioClient_Release(a->client[i]); a->client[i] = NULL; }
}
static HRESULT wa_clients_open(wa_stream *a)
{
    IMMDeviceEnumerator *enumerator = NULL;
    uint32_t i; HRESULT hr;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&enumerator);
    for (i = 0; SUCCEEDED(hr) && i < 2; ++i) if (a->keys[i][0]) {
        IMMDevice *device = NULL; WAVEFORMATEXTENSIBLE w;
        const ha_format *f = i ? &a->owner->info.playback_format : &a->owner->info.capture_format;
        wave_format(f, &w);
        hr = IMMDeviceEnumerator_GetDevice(enumerator, a->keys[i], &device);
        if (SUCCEEDED(hr)) hr = IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&a->client[i]);
        if (device) IMMDevice_Release(device);
        if (SUCCEEDED(hr)) hr = IAudioClient_Initialize(a->client[i], AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            200000, 0, &w.Format, NULL);
        if (SUCCEEDED(hr)) hr = IAudioClient_SetEventHandle(a->client[i], a->audio[i]);
        if (SUCCEEDED(hr)) hr = IAudioClient_GetBufferSize(a->client[i], &a->frames[i]);
        if (SUCCEEDED(hr) && a->frames[i] > HA_MAX_QUANTUM) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
        if (SUCCEEDED(hr)) hr = IAudioClient_GetService(a->client[i], i ? &IID_IAudioRenderClient : &IID_IAudioCaptureClient,
                                                       i ? (void **)&a->render : (void **)&a->capture);
        if (SUCCEEDED(hr) && i) (void)IAudioClient_GetService(a->client[i], &IID_IAudioClock, (void **)&a->clock);
    }
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    return hr;
}
static void wa_capture(wa_stream *a)
{
    ha_stream *s = a->owner; uint32_t packet, iterations = 0; HRESULT hr = S_OK;
    if (!a->capture || !hail_rt_enter(s, 0)) return;
    while (iterations++ < 64u && SUCCEEDED(hr = IAudioCaptureClient_GetNextPacketSize(a->capture, &packet)) && packet) {
        BYTE *data = NULL; UINT32 frames = 0; DWORD flags = 0; UINT64 pos = 0, qpc = 0;
        hr = IAudioCaptureClient_GetBuffer(a->capture, &data, &frames, &flags, &pos, &qpc);
        if (FAILED(hr)) break;
        if (frames > HA_MAX_QUANTUM || (!data && !(flags & AUDCLNT_BUFFERFLAGS_SILENT))) {
            IAudioCaptureClient_ReleaseBuffer(a->capture, frames); hr = E_FAIL; break;
        }
        {
            uint32_t offset = 0;
            while (offset < frames) {
                uint32_t n = frames - offset, valid = flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR ? 0u : HA_TIME_DEVICE_VALID;
                uint64_t host = 0;
                ha_result r;
                if (n > s->info.quantum_frames) n = s->info.quantum_frames;
                if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && qpc <= UINT64_MAX / 100u) {
                    uint64_t delta = (uint64_t)offset * UINT64_C(1000000000) / s->info.capture_format.sample_rate;
                    if (qpc * 100u <= UINT64_MAX - delta) { host = qpc * 100u + delta; valid |= HA_TIME_HOST_VALID; }
                }
                if (!offset && (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)) valid |= HA_BLOCK_UNKNOWN_GAP;
                r = hail_native_capture(s, flags & AUDCLNT_BUFFERFLAGS_SILENT ? a->silence : data + (size_t)offset * s->capture.bytes_per_frame,
                                        n, host, pos <= UINT64_MAX - offset ? pos + offset : 0, UINT64_MAX,
                                        pos <= UINT64_MAX - offset ? valid : valid & ~HA_TIME_DEVICE_VALID);
                if (r != HA_OK) { hail_rt_error(s, 0, r, 0); break; }
                offset += n;
            }
        }
        hr = IAudioCaptureClient_ReleaseBuffer(a->capture, frames);
        if (atomic_load_explicit(&s->rt[0].error, memory_order_relaxed)) break;
    }
    if (FAILED(hr)) hail_rt_error(s, 0, wa_result(hr), hr);
    hail_rt_leave(s, 0);
}
static void wa_render(wa_stream *a)
{
    ha_stream *s = a->owner; UINT32 padding = 0, available; HRESULT hr;
    if (!a->render || !hail_rt_enter(s, 1)) return;
    hr = IAudioClient_GetCurrentPadding(a->client[1], &padding);
    if (SUCCEEDED(hr) && padding > a->frames[1]) hr = E_FAIL;
    available = SUCCEEDED(hr) ? a->frames[1] - padding : 0;
    while (available && SUCCEEDED(hr)) {
        BYTE *buffer = NULL; uint32_t n = available > s->info.quantum_frames ? s->info.quantum_frames : available;
        UINT64 pos = 0, qpc = 0, frequency = 0; uint64_t host = 0; uint32_t flags = 0;
        hr = IAudioRenderClient_GetBuffer(a->render, n, &buffer); if (FAILED(hr)) break;
        if (a->clock && SUCCEEDED(IAudioClock_GetPosition(a->clock, &pos, &qpc)) && qpc <= UINT64_MAX / 100u &&
            SUCCEEDED(IAudioClock_GetFrequency(a->clock, &frequency))) {
            uint64_t delta = (uint64_t)padding * UINT64_C(1000000000) / s->info.playback_format.sample_rate;
            if (qpc * 100u <= UINT64_MAX - delta) { host = qpc * 100u + delta; flags = HA_TIME_HOST_VALID | HA_TIME_ESTIMATED; }
        }
        if (hail_native_render(s, buffer, n, host, 0, UINT64_MAX, flags) != HA_OK) {
            IAudioRenderClient_ReleaseBuffer(a->render, n, AUDCLNT_BUFFERFLAGS_SILENT); hr = E_FAIL; break;
        }
        hr = IAudioRenderClient_ReleaseBuffer(a->render, n, 0); available -= n; padding += n;
    }
    if (FAILED(hr)) hail_rt_error(s, 1, wa_result(hr), hr);
    hail_rt_leave(s, 1);
}
static HRESULT wa_command(wa_stream *a, uint32_t command)
{
    uint32_t i; HRESULT result = S_OK;
    if (command == WA_FLUSH) {
        int running = a->started[1] != 0;
        if (running) result = IAudioClient_Stop(a->client[1]);
        if (SUCCEEDED(result)) result = IAudioClient_Reset(a->client[1]);
        if (SUCCEEDED(result) && running) result = IAudioClient_Start(a->client[1]);
        return result;
    }
    for (i = 0; i < 2; ++i) if (a->client[i]) {
        HRESULT hr = S_OK;
        if (command == WA_START && !a->started[i]) {
            hr = IAudioClient_Start(a->client[i]); if (SUCCEEDED(hr)) a->started[i] = 1;
        } else if (command != WA_START) {
            if (a->started[i]) hr = IAudioClient_Stop(a->client[i]);
            a->started[i] = 0;
            if (SUCCEEDED(hr)) hr = IAudioClient_Reset(a->client[i]);
        }
        if (FAILED(hr)) result = hr;
    }
    if (FAILED(result) && command == WA_START) for (i = 0; i < 2; ++i) if (a->client[i] && a->started[i]) {
        (void)IAudioClient_Stop(a->client[i]); a->started[i] = 0;
    }
    return result;
}
static DWORD WINAPI wa_stream_worker(void *user)
{
    wa_stream *a = user; HANDLE handles[3];
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int initialized = SUCCEEDED(hr);
    if (initialized) hr = wa_clients_open(a);
    a->result = wa_result(hr);
    atomic_store_explicit(&a->done, 1, memory_order_release); SetEvent(a->reply);
    handles[0] = a->request; handles[1] = a->audio[0]; handles[2] = a->audio[1];
    if (SUCCEEDED(hr)) for (;;) {
        DWORD event = WaitForMultipleObjects(3, handles, FALSE, INFINITE);
        if (event == WAIT_OBJECT_0) {
            uint32_t command = atomic_exchange_explicit(&a->command, 0, memory_order_acquire);
            if (command == WA_EXIT) break;
            if (command == WA_RETIRE) { wa_clients_free(a); a->result = HA_OK; }
            else a->result = wa_result(wa_command(a, command));
            atomic_store_explicit(&a->done, 1, memory_order_release); SetEvent(a->reply);
        } else if (event == WAIT_OBJECT_0 + 1u) wa_capture(a);
        else if (event == WAIT_OBJECT_0 + 2u) wa_render(a);
        else { hail_rt_error(a->owner, 0, HA_STREAM_FAILED, GetLastError()); break; }
    }
    wa_clients_free(a); if (initialized) CoUninitialize(); return 0;
}
static void wa_stream_free(ha_stream *s)
{
    wa_stream *a = s->backend_state; uint32_t i;
    if (a->thread) {
        DWORD code = 0;
        if (GetExitCodeThread(a->thread, &code) && code == STILL_ACTIVE) {
            atomic_store_explicit(&a->command, WA_EXIT, memory_order_release); SetEvent(a->request);
        }
        WaitForSingleObject(a->thread, INFINITE); CloseHandle(a->thread);
    }
    if (a->request) CloseHandle(a->request);
    if (a->reply) CloseHandle(a->reply);
    for (i = 0; i < 2; ++i) if (a->audio[i]) CloseHandle(a->audio[i]);
    memset(a, 0, sizeof(*a));
}
static ha_result wa_open(ha_stream *s)
{
    wa_stream *a = s->backend_state; uint32_t i; ha_result r;
    a->owner = s; atomic_init(&a->command, 0); atomic_init(&a->done, 0);
    a->request = CreateEventW(NULL, FALSE, FALSE, NULL); a->reply = CreateEventW(NULL, FALSE, FALSE, NULL);
    for (i = 0; i < 2; ++i) {
        uint64_t id = i ? s->info.playback_device_id : s->info.capture_device_id;
        a->audio[i] = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (id) {
            hail_device *d = hail_device_find(s->context, id);
            if (!d || !wide(a->keys[i], 1024, d->key)) { wa_stream_free(s); return HA_DEVICE_UNAVAILABLE; }
        }
    }
    if (!a->request || !a->reply || !a->audio[0] || !a->audio[1]) { wa_stream_free(s); return HA_RESOURCE_EXHAUSTED; }
    a->thread = CreateThread(NULL, 0, wa_stream_worker, a, 0, NULL);
    if (!a->thread) { wa_stream_free(s); return HA_RESOURCE_EXHAUSTED; }
    WaitForSingleObject(a->reply, INFINITE);
    r = atomic_load_explicit(&a->done, memory_order_acquire) ? a->result : HA_STREAM_FAILED;
    if (r != HA_OK) wa_stream_free(s);
    return r;
}
static ha_result wa_request(ha_stream *s, uint32_t command)
{
    wa_stream *a = s->backend_state;
    if (a->pending) {
        if (!atomic_load_explicit(&a->done, memory_order_acquire)) return HA_PENDING;
        a->pending = 0;
        if (atomic_load_explicit(&a->command, memory_order_relaxed) == command) return a->result;
        /* The worker consumes command to zero. Track completion by pending ID. */
    }
    a->pending = command;
    atomic_store_explicit(&a->done, 0, memory_order_relaxed);
    atomic_store_explicit(&a->command, command, memory_order_release); SetEvent(a->request);
    return HA_PENDING;
}
static ha_result wa_progress(ha_stream *s, uint32_t command)
{
    wa_stream *a = s->backend_state;
    DWORD code = STILL_ACTIVE;
    if (GetExitCodeThread(a->thread, &code) && code != STILL_ACTIVE)
        return command == WA_STOP || command == WA_RETIRE ? HA_OK : HA_STREAM_FAILED;
    if (a->pending == command && atomic_load_explicit(&a->done, memory_order_acquire)) {
        a->pending = 0; return a->result;
    }
    return wa_request(s, command);
}
static ha_result wa_start(ha_stream *s) { return wa_progress(s, WA_START); }
static ha_result wa_stop(ha_stream *s) { return wa_progress(s, WA_STOP); }
static ha_result wa_flush(ha_stream *s) { return wa_progress(s, WA_FLUSH); }
static ha_result wa_retire(ha_stream *s) { return wa_progress(s, WA_RETIRE); }
const hail_backend_ops hail_wasapi_backend = {
    .id = HA_BACKEND_WASAPI, .features = HA_FEATURE_WASAPI | HA_FEATURE_RENDER_REFERENCE | HA_FEATURE_NATIVE_FLUSH,
    .context_init = wa_context_init, .probe = wa_probe, .open = wa_open,
    .start = wa_start, .stop = wa_stop, .flush = wa_flush, .retire = wa_retire,
    .context_bytes = sizeof(wa_context), .stream_bytes = sizeof(wa_stream),
    .refresh = wa_refresh, .context_poll = wa_context_poll, .context_free = wa_context_free, .stream_free = wa_stream_free
};
