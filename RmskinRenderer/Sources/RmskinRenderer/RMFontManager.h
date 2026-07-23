#pragma once

// RMFontManager — registers bundled skin fonts and resolves font faces.
//
// Rainmeter skins ship .ttf/.otf files in @Resources/Fonts and reference them
// by FontFace name (e.g. "nasalization rg"). We register those files with
// CoreText (CTFontManagerRegisterFontsForURL) so the names become resolvable,
// then map a FontFace + size + style to a CTFont/NSFont.

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface RMFontManager : NSObject

+ (instancetype)shared;

// Register every font file under the given directory (recursively). Safe to
// call repeatedly; already-registered files are skipped.
- (void)registerFontsInDirectory:(NSString *)directory;

// Resolve a font. face may be a family name (registered or system). bold/italic
// apply synthetic traits when the face lacks a dedicated variant.
- (NSFont *)fontWithFace:(nullable NSString *)face
                    size:(CGFloat)size
                    bold:(BOOL)bold
                  italic:(BOOL)italic;

@end

NS_ASSUME_NONNULL_END
