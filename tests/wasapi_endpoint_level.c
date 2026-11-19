/* Qualification-only capture-level helper. No audio clients or PCM storage. */
#define COBJMACROS
#define CONST_VTABLE
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static const GUID cls={0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
static const GUID en={0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
static const GUID ep={0x1be09788,0x6894,0x4089,{0x85,0x86,0x9a,0x2a,0x6c,0x26,0x5a,0xc5}};
static const GUID vol={0x5cdf2c82,0x841e,0x4546,{0x97,0x22,0x0c,0xf7,0x40,0x78,0x22,0x9a}};
static void json_key(const char *key)
{
    const unsigned char *p=(const unsigned char *)key; putchar('"');
    for(;*p;++p) {
        if(*p=='"' || *p=='\\') { putchar('\\'); putchar(*p); }
        else if(*p<32) printf("\\u%04x",*p);
        else putchar(*p);
    }
    putchar('"');
}
static int defaults(void)
{
    IMMDeviceEnumerator *enumerator=NULL; HRESULT hr; unsigned flow,role; int failed=0;
    hr=CoInitializeEx(NULL,COINIT_MULTITHREADED); if(FAILED(hr)) return 1;
    hr=CoCreateInstance(&cls,NULL,CLSCTX_ALL,&en,(void **)&enumerator);
    if(FAILED(hr)) { CoUninitialize(); return 1; }
    for(flow=0;flow<2;++flow) for(role=0;role<3;++role) {
        IMMDevice *device=NULL; LPWSTR id=NULL; char key[2048];
        hr=IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator,flow?eCapture:eRender,(ERole)role,&device);
        if(SUCCEEDED(hr)) hr=IMMDevice_GetId(device,&id);
        if(SUCCEEDED(hr) && (!id || !WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,id,-1,key,sizeof(key),NULL,NULL))) hr=E_FAIL;
        printf("{\"direction\":%u,\"role\":%u,\"key\":",flow?1u:2u,role);
        if(SUCCEEDED(hr)) json_key(key); else printf("null");
        printf(",\"native_code\":%ld}\n",(long)hr);
        if(FAILED(hr) && hr!=HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) failed=1;
        if(id) CoTaskMemFree(id);
        if(device) IMMDevice_Release(device);
    }
    IMMDeviceEnumerator_Release(enumerator); CoUninitialize(); return failed;
}
int main(int argc,char **argv)
{
    IMMDeviceEnumerator *enumerator=NULL; IMMDevice *device=NULL; IMMEndpoint *endpoint=NULL;
    IAudioEndpointVolume *volume=NULL; WCHAR key[512]; HRESULT hr; EDataFlow direction=eAll;
    float gain=0,target=0; BOOL muted=FALSE; char *end=NULL; unsigned setting=0;
    if(argc==2 && !strcmp(argv[1],"--defaults")) return defaults();
    if(argc!=2 && argc!=4) return 2;
    if(argc==4) {
        double requested=strtod(argv[2],&end);
        if(!end || end==argv[2] || *end || !isfinite(requested) || requested<0 || requested>1 ||
           (strcmp(argv[3],"0") && strcmp(argv[3],"1"))) return 2;
        target=(float)requested; setting=1;
    }
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,argv[1],-1,key,512)) return 2;
    hr=CoInitializeEx(NULL,COINIT_MULTITHREADED); if(FAILED(hr)) return 1;
    hr=CoCreateInstance(&cls,NULL,CLSCTX_ALL,&en,(void **)&enumerator);
    if(SUCCEEDED(hr)) hr=IMMDeviceEnumerator_GetDevice(enumerator,key,&device);
    if(SUCCEEDED(hr)) hr=IMMDevice_QueryInterface(device,&ep,(void **)&endpoint);
    if(SUCCEEDED(hr)) hr=IMMEndpoint_GetDataFlow(endpoint,&direction);
    if(SUCCEEDED(hr) && direction!=eCapture) hr=E_INVALIDARG;
    if(SUCCEEDED(hr)) hr=IMMDevice_Activate(device,&vol,CLSCTX_ALL,NULL,(void **)&volume);
    if(SUCCEEDED(hr) && setting) hr=IAudioEndpointVolume_SetMasterVolumeLevelScalar(volume,target,NULL);
    if(SUCCEEDED(hr) && setting) hr=IAudioEndpointVolume_SetMute(volume,argv[3][0]=='1',NULL);
    if(SUCCEEDED(hr)) hr=IAudioEndpointVolume_GetMasterVolumeLevelScalar(volume,&gain);
    if(SUCCEEDED(hr)) hr=IAudioEndpointVolume_GetMute(volume,&muted);
    if(SUCCEEDED(hr)) printf("{\"gain\":%.9g,\"muted\":%s}\n",(double)gain,muted?"true":"false");
    else fprintf(stderr,"capture level query/set failed HRESULT=0x%08lx\n",(unsigned long)hr);
    if(volume) IAudioEndpointVolume_Release(volume);
    if(endpoint) IMMEndpoint_Release(endpoint);
    if(device) IMMDevice_Release(device);
    if(enumerator) IMMDeviceEnumerator_Release(enumerator);
    CoUninitialize(); return FAILED(hr)?1:0;
}
