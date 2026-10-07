//
//  BakeSupport.h
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#pragma once
#include <stdint.h>

#ifdef __OBJC__
#import <AVFoundation/AVFoundation.h>
#import <CoreImage/CoreImage.h>
NSDictionary *MBReadRequest(int argc, const char **argv);
BOOL MBValidateRequest(NSDictionary *request);
void MBEvent(NSString *event, NSDictionary *values);
BOOL MBVerify(NSString *path, NSInteger width, NSInteger height, double duration);
BOOL MBCancelled(void);
@interface MBBakeWriter : NSObject
@property(nonatomic, readonly) NSString *failure;
- (instancetype)initWithRequest:(NSDictionary *)request;
- (BOOL)appendRGBA:(const uint8_t *)bytes width:(uint32_t)width height:(uint32_t)height frame:(int64_t)frame;
- (BOOL)appendImage:(CIImage *)image frame:(int64_t)frame;
- (BOOL)appendAudio:(const float *)samples frames:(uint32_t)frames at:(int64_t)offset;
- (BOOL)finish;
- (void)cancel;
@end
#endif

#ifdef __cplusplus
extern "C" {
#endif
typedef struct MBBakeSceneOptions {
    const char *source;
    const char *assets;
    const char *cache;
    const char *properties;
    const char *storage;
    const char *fill;
    uint32_t width, height, fps, seed, frames, warmup;
    double speed, position_x, position_y, volume;
} MBBakeSceneOptions;
int MBRunScene(const MBBakeSceneOptions *options, void *writer);
int MBWriteFrame(void *writer, const uint8_t *rgba, uint32_t width, uint32_t height, int64_t frame);
int MBWriteAudio(void *writer, const float *samples, uint32_t count, int64_t offset);
int MBShouldStop(void);
void MBProgress(uint32_t frame, uint32_t count);
void MBWarmup(uint32_t frame, uint32_t count);
#ifdef __cplusplus
}
#endif
