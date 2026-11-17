#import <AVFoundation/AVFoundation.h>
#include <mach/mach_time.h>
#include <CoreAudio/HostTime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Read-only SDK latency probe. No audio opens, permission requests or TCC edits.
 * Usage: hail_coreaudio_permission_cost [warm_iterations], default 1000. */
int main(int argc,char **argv)
{
    unsigned iterations=1000;
    if(argc>2) return 2;
    if(argc==2) {
        char *end=NULL; unsigned long value=strtoul(argv[1],&end,10);
        if(!end || *end || !value || value>10000) return 2;
        iterations=(unsigned)value;
    }
    @autoreleasepool {
        uint64_t sum=0,max=0,min=UINT64_MAX,cold=0; long status=0;
        for(unsigned i=0;i<=iterations;++i) {
            uint64_t start=AudioConvertHostTimeToNanos(mach_absolute_time());
            status=(long)[AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
            uint64_t elapsed=AudioConvertHostTimeToNanos(mach_absolute_time())-start;
            if(!i) cold=elapsed;
            else { sum+=elapsed; if(elapsed>max) max=elapsed; if(elapsed<min) min=elapsed; }
        }
        printf("status=%ld warm_iterations=%u cold_ns=%llu warm_mean_ns=%llu warm_min_ns=%llu warm_max_ns=%llu\n",
            status,iterations,(unsigned long long)cold,(unsigned long long)(sum/iterations),
            (unsigned long long)min,(unsigned long long)max);
    }
    return 0;
}
