//
//  ShaderOptimizer.m
//  Mirage Wallpaper
//
//  Created on 2026/10/28.
//  Copyright © 2026 王孝慈. All rights reserved.
//

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <mach-o/dyld.h>

static NSArray<NSTextCheckingResult *> *matches(NSString *source, NSString *pattern) {
    NSRegularExpression *expression = [NSRegularExpression regularExpressionWithPattern:pattern options:0 error:nil];
    return [expression matchesInString:source options:0 range:NSMakeRange(0, source.length)];
}

NSString *MirageOptimizeShader(NSString *source) {
    if (![source containsString:@"thread spvUnsafeArray<float, 64>& x0, thread spvUnsafeArray<float, 64>& x1"])
        return source;
    if (![source containsString:@"struct cb1_t\n{\n    float4 m[32];\n};"] ||
        matches(source, @"\\bps_main\\s*\\(").count != 2) return source;
    NSArray *functions = matches(source, @"void ps_main\\([^\\n]+constant cb1_t& cb1[^\\n]+\\)\\n\\{\\n");
    if (functions.count != 1) return source;
    NSRange header = [functions[0] range];
    NSUInteger start = NSMaxRange(header), end = start, depth = 1;
    for (; end < source.length && depth; ++end) {
        unichar character = [source characterAtIndex:end];
        if (character == '{') ++depth;
        else if (character == '}') --depth;
    }
    if (depth) return source;
    NSMutableString *copies = [NSMutableString new];
    for (unsigned array = 0; array < 2; ++array)
        for (unsigned index = 0; index < 64; ++index)
            [copies appendFormat:@"    x%u[%u] = cb1.m[%u].%c;\n", array, index,
                array * 16 + index / 4, "xyzw"[index % 4]];
    NSString *body = [source substringWithRange:NSMakeRange(start, end - start)];
    if (![body hasPrefix:copies]) return source;
    body = [body substringFromIndex:copies.length];
    NSString *pattern = @"if \\(uint\\((as_type<int>\\(r[0-9]+\\.[xyzw]\\))\\) < 64u\\)\\n    \\{\\n        _[0-9]+ = (x[01])\\[\\1\\];\\n    \\}";
    NSArray<NSTextCheckingResult *> *reads = matches(body, pattern);
    if (!reads.count || matches(body, @"\\bx[01]\\b").count != reads.count) return source;
    NSString *entry = [source substringFromIndex:end];
    if (matches(entry, @"\\bx[01]\\b").count != 4 ||
        ![entry containsString:@"    spvUnsafeArray<float, 64> x0;\n    spvUnsafeArray<float, 64> x1;\n"] ||
        ![entry containsString:@", x0, x1, r0, r1);"]) return source;
    NSMutableString *changed = [body mutableCopy];
    for (NSTextCheckingResult *read in reads.reverseObjectEnumerator) {
        NSString *index = [body substringWithRange:[read rangeAtIndex:1]];
        NSString *array = [body substringWithRange:[read rangeAtIndex:2]];
        NSString *before = [NSString stringWithFormat:@"%@[%@]", array, index];
        NSString *after = [NSString stringWithFormat:@"cb1.m[%u + (uint(%@) >> 2)][uint(%@) & 3u]",
            [array isEqualToString:@"x1"] ? 16 : 0, index, index];
        NSString *block = [[body substringWithRange:read.range] stringByReplacingOccurrencesOfString:before withString:after];
        [changed replaceCharactersInRange:read.range withString:block];
    }
    NSMutableString *result = [source mutableCopy];
    [result replaceCharactersInRange:NSMakeRange(start, end - start) withString:changed];
    [result insertString:@"#define MIRAGE_SPECTRUM_COMPILER_VERSION 1\n" atIndex:0];
    return result;
}

typedef id<MTLLibrary> (*CompileSync)(id, SEL, NSString *, MTLCompileOptions *, NSError **) NS_RETURNS_RETAINED;
typedef void (*CompileAsync)(id, SEL, NSString *, MTLCompileOptions *, MTLNewLibraryCompletionHandler);
static NSMutableDictionary<NSString *, NSValue *> *implementations;

static IMP implementation(id device, SEL selector) {
    for (Class cls = object_getClass(device); cls; cls = class_getSuperclass(cls)) {
        NSString *key = [NSString stringWithFormat:@"%s/%s", class_getName(cls), sel_getName(selector)];
        NSValue *value = implementations[key];
        if (value) return value.pointerValue;
    }
    return NULL;
}

static id<MTLLibrary> compileSync(id device, SEL selector, NSString *source, MTLCompileOptions *options,
                                 NSError **error) NS_RETURNS_RETAINED {
    CompileSync original = (CompileSync)implementation(device, selector);
    NSString *optimized = MirageOptimizeShader(source);
    id<MTLLibrary> library = original(device, selector, optimized, options, error);
    if (!library && optimized != source) {
        fprintf(stderr, "[Mirage shader] optimized compilation failed; using original source\n");
        if (error) *error = nil;
        library = original(device, selector, source, options, error);
    }
    return library;
}

static void compileAsync(id device, SEL selector, NSString *source, MTLCompileOptions *options,
                         MTLNewLibraryCompletionHandler handler) {
    CompileAsync original = (CompileAsync)implementation(device, selector);
    NSString *optimized = MirageOptimizeShader(source);
    if (optimized == source) { original(device, selector, source, options, handler); return; }
    original(device, selector, optimized, options, ^(id<MTLLibrary> library, NSError *error) {
        if (library) handler(library, error);
        else {
            fprintf(stderr, "[Mirage shader] optimized compilation failed; using original source\n");
            original(device, selector, source, options, handler);
        }
    });
}

__attribute__((constructor)) static void initializeShaderOptimizer(void) {
    char path[4096]; uint32_t capacity = sizeof(path);
    if (_NSGetExecutablePath(path, &capacity) || !strstr(path, "Mirage WE Editor.app/")) return;
    @autoreleasepool {
        implementations = [NSMutableDictionary new];
        SEL selectors[] = {@selector(newLibraryWithSource:options:error:),
                           @selector(newLibraryWithSource:options:completionHandler:)};
        IMP replacements[] = {(IMP)compileSync, (IMP)compileAsync};
        for (id<MTLDevice> device in MTLCopyAllDevices()) {
            Class cls = object_getClass(device);
            for (unsigned index = 0; index < 2; ++index) {
                Method method = class_getInstanceMethod(cls, selectors[index]);
                if (!method) continue;
                IMP original = method_getImplementation(method);
                if (original == replacements[index]) continue;
                NSString *key = [NSString stringWithFormat:@"%s/%s", class_getName(cls), sel_getName(selectors[index])];
                implementations[key] = [NSValue valueWithPointer:original];
                class_replaceMethod(cls, selectors[index], replacements[index], method_getTypeEncoding(method));
            }
        }
    }
}
