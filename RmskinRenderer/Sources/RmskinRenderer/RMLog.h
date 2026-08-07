#pragma once

// Lightweight logging helpers for RmskinRenderer. Errors/warnings go to stderr
// (captured by Mirage's RendererController.terminationHandler); verbose parse
// traces are compiled out unless RM_VERBOSE is defined.

#import <Foundation/Foundation.h>

#define RMLogError(fmt, ...)   NSLog(@"[Rmskin][error] " fmt, ##__VA_ARGS__)
#define RMLogWarn(fmt, ...)    NSLog(@"[Rmskin][warn] " fmt, ##__VA_ARGS__)

// Always emit debug traces in the renderer host (these go to stderr and are
// captured by Mirage for diagnosis); the volume is small for typical themes.
#define RMLogDebug(fmt, ...)   NSLog(@"[Rmskin][debug] " fmt, ##__VA_ARGS__)
