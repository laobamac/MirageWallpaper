//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#include "VRVideoLayout.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static void Check(bool condition, const char* message) {
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

int main() {
    const CGSize source = CGSizeMake(1920, 1080);
    const CGRect portrait = CGRectMake(0, 0, 1080, 1920);
    for (double x : { 0.0, 0.25, 0.5, 0.75, 1.0 }) {
        const auto layout = VRCalculateVideoLayout(source, portrait, 0, x, 1, false);
        Check(layout.canMoveX && !layout.canMoveY, "portrait crop exposes only X");
        Check(fabs(layout.frame.origin.x + (3413.333333333333 - 1080) * x) < 0.000001,
              "horizontal crop selects the requested source region");
        Check(CGRectGetMinX(layout.frame) <= 0 && CGRectGetMaxX(layout.frame) >= 1080 - 0.000001,
              "edge positions cover the target");
    }
    for (bool flipped : { false, true }) {
        const auto top = VRCalculateVideoLayout(portrait.size, CGRectMake(0, 0, 1920, 1080), 0, 0.5, 0, flipped);
        const auto bottom = VRCalculateVideoLayout(portrait.size, CGRectMake(0, 0, 1920, 1080), 0, 0.5, 1, flipped);
        Check(top.canMoveY && !top.canMoveX, "landscape crop exposes only Y");
        Check(fabs((flipped ? top : bottom).frame.origin.y) < 0.000001,
              "vertical edge selection respects the host coordinate system");
    }
    for (int mode : { 1, 2 }) {
        const auto a = VRCalculateVideoLayout(source, portrait, mode, 0, 0, false);
        const auto b = VRCalculateVideoLayout(source, portrait, mode, 1, 1, false);
        Check(CGRectEqualToRect(a.frame, b.frame) && !a.canMoveX && !a.canMoveY,
              "contain and stretch ignore saved crop positions");
    }
    const auto empty = VRCalculateVideoLayout(CGSizeZero, portrait, 0, 1, 1, false);
    Check(CGRectEqualToRect(empty.frame, portrait) && !empty.canMoveX && !empty.canMoveY,
          "unavailable video dimensions retain valid bounds");
    Check(VRClampPosition(NAN) == 0.5 && VRClampPosition(INFINITY) == 0.5,
          "non-finite positions recover to center");
    puts("VideoLayoutRegression: ok");
    return 0;
}
