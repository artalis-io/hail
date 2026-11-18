#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <stdio.h>
#include <stdlib.h>
#define OK(x) do { HRESULT h=(x); if(FAILED(h)) { fprintf(stderr,"line=%d HRESULT=%08lx\n",__LINE__,(unsigned long)h); exit(1); } } while(0)
int main(int argc,char **argv) {
 if(argc!=2 && argc!=3) return 2;
 if(argc==3 && (strcmp(argv[2],"--unity") || strcmp(argv[1],"{0.0.0.00000000}.{266bf420-96a2-47b5-a831-f6a6dbc0ae03}"))) return 2;
 OK(CoInitializeEx(NULL,COINIT_MULTITHREADED));
 IMMDeviceEnumerator *e=nullptr; IMMDevice *d=nullptr; IAudioClient *c=nullptr;
 IAudioEndpointVolume *v=nullptr; IAudioSessionManager2 *m=nullptr; IAudioSessionEnumerator *s=nullptr;
 WCHAR key[2048]; if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,argv[1],-1,key,2048)) return 2;
 OK(CoCreateInstance(__uuidof(MMDeviceEnumerator),NULL,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),(void**)&e));
 OK(e->GetDevice(key,&d)); OK(d->Activate(__uuidof(IAudioClient),CLSCTX_ALL,NULL,(void**)&c));
 WAVEFORMATEX *w=nullptr; OK(c->GetMixFormat(&w));
 printf("{\"key\":\"%s\",\"rate\":%lu,\"channels\":%u,\"bits\":%u,\"tag\":%u,",argv[1],w->nSamplesPerSec,w->nChannels,w->wBitsPerSample,w->wFormatTag);
 OK(d->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,NULL,(void**)&v));
 float gain=0; BOOL mute=FALSE; UINT n=0;
 OK(v->GetChannelCount(&n));
 if(argc==3) { OK(v->SetMasterVolumeLevelScalar(1,NULL)); for(UINT i=0;i<n;++i) OK(v->SetChannelVolumeLevelScalar(i,1,NULL)); OK(v->SetMute(FALSE,NULL)); }
 OK(v->GetMasterVolumeLevelScalar(&gain)); OK(v->GetMute(&mute));
 printf("\"master_gain\":%.9g,\"muted\":%s,\"channel_gains\":[",gain,mute?"true":"false");
 for(UINT i=0;i<n;++i) { OK(v->GetChannelVolumeLevelScalar(i,&gain)); printf("%s%.9g",i?",":"",gain); }
 printf("],\"sessions\":[");
 OK(d->Activate(__uuidof(IAudioSessionManager2),CLSCTX_ALL,NULL,(void**)&m)); OK(m->GetSessionEnumerator(&s));
 int count=0; OK(s->GetCount(&count));
 for(int i=0;i<count;++i) { IAudioSessionControl *sc=nullptr; IAudioSessionControl2 *sc2=nullptr; AudioSessionState state; DWORD pid=0;
  OK(s->GetSession(i,&sc)); OK(sc->GetState(&state)); OK(sc->QueryInterface(__uuidof(IAudioSessionControl2),(void**)&sc2)); OK(sc2->GetProcessId(&pid));
  printf("%s{\"pid\":%lu,\"state\":%d}",i?",":"",pid,(int)state); sc2->Release(); sc->Release(); }
 printf("]}\n"); s->Release(); m->Release(); v->Release(); CoTaskMemFree(w); c->Release(); d->Release(); e->Release(); CoUninitialize(); return 0;
}
