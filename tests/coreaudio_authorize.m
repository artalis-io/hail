#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#include <stdio.h>

/* Host authorization belongs to this opt-in test, never to the library. */
int hail_test_authorize(void)
{
    @autoreleasepool {
        AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
        fprintf(stderr, "microphone_authorization_before=%ld\n", (long)status);
        if (status == AVAuthorizationStatusNotDetermined) {
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
                (void)granted;
                dispatch_semaphore_signal(done);
            }];
            dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
            dispatch_release(done);
            status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
        }
        fprintf(stderr, "microphone_authorization_after=%ld\n", (long)status);
        return status == AVAuthorizationStatusAuthorized;
    }
}
