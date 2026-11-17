#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <hail.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#import <AVFoundation/AVFoundation.h>
/* Run only in the separately signed denial host. Request authorization through
 * ordinary OS controls, then exercise native denial; never reset/bypass TCC. */
extern int hail_test_authorize(void);
int main(int argc,char **argv)
{
    ha_context_config cc=HA_INIT(ha_context_config); ha_context *c=NULL; ha_stream *s=NULL;
    ha_device_info d[HA_MAX_DEVICES]; ha_stream_config sc=HA_INIT(ha_stream_config);
    ha_device_selector selector=HA_INIT(ha_device_selector); ha_format f=HA_FORMAT_INIT(48000,1,HA_PCM_F32);
    ha_result r; uint32_t i,n; int denied=0;
    setvbuf(stdout,NULL,_IOLBF,0);
    if(argc!=2) return 2;
    printf("microphone_authorization_status=%ld\n",(long)[AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]);
    if(hail_test_authorize()) { puts("FAIL host is authorized; denial precondition unavailable"); return 1; }
    puts("host_authorization_denied=1");
    cc.backend=HA_BACKEND_COREAUDIO; r=ha_context_create(&cc,&c);
    printf("context=%s\n",ha_result_string(r)); if(r==HA_PERMISSION_DENIED) return 0; if(r!=HA_OK) return 1;
    for(i=0;i<HA_MAX_DEVICES;++i) { ha_device_info value=HA_INIT(ha_device_info); d[i]=value; }
    r=ha_context_enumerate(c,d,HA_MAX_DEVICES,&n); if(r!=HA_OK) return 1;
    for(i=0;i<n;++i) if(d[i].direction==HA_CAPTURE && !strcmp(d[i].name,argv[1])) break;
    if(i==n) { puts("input_missing_not_permission_result"); goto done; }
    selector.kind=HA_SELECT_RUNTIME_ID; selector.runtime_id=d[i].runtime_id;
    sc.capture_device=selector; sc.capture_format=f; sc.directions=HA_CAPTURE; sc.negotiation=HA_EXACT_APPLICATION;
    r=ha_stream_open(c,&sc,&s); printf("open=%s\n",ha_result_string(r)); denied=r==HA_PERMISSION_DENIED;
    if(r==HA_OK) {
        r=ha_stream_start(s); printf("start=%s\n",ha_result_string(r));
        for(i=0;i<500 && r==HA_PENDING;++i) {
            ha_stream_status status=HA_INIT(ha_stream_status); struct timespec t={0,2000000};
            (void)ha_context_poll(c,NULL,0,&n); (void)ha_stream_get_status(s,&status);
            if(status.state==HA_FAILED) { r=status.terminal_result; printf("terminal=%s native=%lld\n",ha_result_string(r),(long long)status.diagnostic.native_code); denied=r==HA_PERMISSION_DENIED; break; }
            /* Authorization/native errors may arrive after asynchronous start.
             * Keep servicing for a full second; RUNNING alone is not evidence
             * of permission or a reason to stop observing errors. */
            if(status.state==HA_RUNNING && i==499) puts("unexpected_running_after_permission_window");
            nanosleep(&t,NULL);
        }
        (void)ha_stream_close_begin(s);
        while(s) { (void)ha_context_poll(c,NULL,0,&n); if(ha_stream_close_finish(&s)==HA_OK) break; struct timespec t={0,2000000}; nanosleep(&t,NULL); }
    }
 done:
    while(c) { r=ha_context_destroy(&c); if(r!=HA_OK && r!=HA_PENDING) return 1; }
    puts(denied?"PASS explicit native permission denial":"FAIL no explicit permission denial observed"); return denied?0:1;
}
