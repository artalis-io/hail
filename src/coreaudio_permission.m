#import <AVFoundation/AVFoundation.h>
#include <hail.h>

/* Source-private control-path query. Never requests access or changes privacy
 * state. AUHAL can open/start while TCC denies capture, so its OSStatus alone
 * does not establish permission. Undetermined is not a grant: hosts still
 * authorize physical capture; virtual fixtures may not require a prompt. */
__attribute__((visibility("hidden")))
ha_result hail_coreaudio_capture_permission(void)
{
    @autoreleasepool {
        AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
        return status == AVAuthorizationStatusDenied || status == AVAuthorizationStatusRestricted
            ? HA_PERMISSION_DENIED : HA_OK;
    }
}
