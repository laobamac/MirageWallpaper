//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#pragma once

#include <CoreGraphics/CoreGraphics.h>
#include <math.h>
#include <stdbool.h>

typedef struct {
    CGRect frame;
    bool canMoveX;
    bool canMoveY;
} VRVideoLayout;

static inline double VRClampPosition(double value) {
    return isfinite(value) ? fmin(fmax(value, 0.0), 1.0) : 0.5;
}

static inline VRVideoLayout VRCalculateVideoLayout(CGSize source, CGRect bounds, int fillMode,
                                                    double x, double y, bool flipped) {
    VRVideoLayout result = { bounds, false, false };
    if (!isfinite(source.width) || !isfinite(source.height) ||
        !isfinite(bounds.size.width) || !isfinite(bounds.size.height) ||
        source.width <= 0 || source.height <= 0 ||
        bounds.size.width <= 0 || bounds.size.height <= 0 || fillMode == 2) return result;
    const bool cover = fillMode != 1;
    const double sx = bounds.size.width / source.width;
    const double sy = bounds.size.height / source.height;
    const double scale = cover ? fmax(sx, sy) : fmin(sx, sy);
    const CGSize size = CGSizeMake(source.width * scale, source.height * scale);
    x = cover ? VRClampPosition(x) : 0.5;
    y = cover ? VRClampPosition(y) : 0.5;
    result.frame = CGRectMake(bounds.origin.x + (bounds.size.width - size.width) * x,
                              bounds.origin.y + (bounds.size.height - size.height) * (flipped ? y : 1.0 - y),
                              size.width, size.height);
    result.canMoveX = cover && size.width - bounds.size.width > bounds.size.width * 0.000001;
    result.canMoveY = cover && size.height - bounds.size.height > bounds.size.height * 0.000001;
    return result;
}
