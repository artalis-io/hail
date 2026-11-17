#import <AVFoundation/AVFoundation.h>
#include <mach/mach_time.h>
#include <CoreAudio/HostTime.h>
#include <stdio.h>
int main(void) { @autoreleasepool { unsigned i; uint64_t sum=0,max=0,min=UINT64_MAX,cold=0; long status=0;
for(i=0;i<1001;++i) { uint64_t start=AudioConvertHostTimeToNanos(mach_absolute_time()); status=(long)[AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]; uint64_t elapsed=AudioConvertHostTimeToNanos(mach_absolute_time())-start;
if(!i) cold=elapsed; else {sum+=elapsed;if(elapsed>max)max=elapsed;if(elapsed<min)min=elapsed;} }
printf("status=%ld cold_ns=%llu warm_mean_ns=%llu warm_min_ns=%llu warm_max_ns=%llu\n",status,(unsigned long long)cold,(unsigned long long)(sum/1000),(unsigned long long)min,(unsigned long long)max); } }
