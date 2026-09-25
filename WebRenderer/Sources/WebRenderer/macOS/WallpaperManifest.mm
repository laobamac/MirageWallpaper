#import "WallpaperManifest.h"

static NSString *const kWRManifestErrorDomain = @"WebRenderer.Manifest";

enum {
    WRManifestErrorOpenFailed = 1,
    WRManifestErrorInvalidJSON,
    WRManifestErrorMissingType,
    WRManifestErrorWrongType,
};

@implementation WRManifest {
    NSString *_workshopDir;
    NSString *_entryHTML;
    NSURL    *_entryURL;
    NSString *_title;
    NSString *_preview;
    NSDictionary *_userProperties;
    NSString *_userPropertiesJSON;
}

+ (instancetype)loadFromDirectory:(NSString *)dir error:(NSError **)error {
    NSString *pjPath = [dir stringByAppendingPathComponent:@"project.json"];
    NSData *data = [NSData dataWithContentsOfFile:pjPath
                                          options:NSDataReadingMappedIfSafe
                                            error:error];
    if (data == nil) {
        if (error != NULL) {
            *error = [NSError errorWithDomain:kWRManifestErrorDomain
                                          code:WRManifestErrorOpenFailed
                                      userInfo:@{
                NSLocalizedDescriptionKey: [NSString stringWithFormat:@"cannot open %@", pjPath],
                NSUnderlyingErrorKey: (*error ?: [NSNull null]),
            }];
        }
        return nil;
    }

    NSError *parseErr = nil;
    NSDictionary *root = [NSJSONSerialization JSONObjectWithData:data
                                                         options:(NSJSONReadingMutableContainers |
                                                                  NSJSONReadingFragmentsAllowed)
                                                           error:&parseErr];
    if (![root isKindOfClass:[NSDictionary class]]) {
        if (error != NULL) {
            *error = [NSError errorWithDomain:kWRManifestErrorDomain
                                          code:WRManifestErrorInvalidJSON
                                      userInfo:@{
                NSLocalizedDescriptionKey: [NSString stringWithFormat:@"invalid JSON in %@", pjPath],
                NSUnderlyingErrorKey: (parseErr ?: [NSNull null]),
            }];
        }
        return nil;
    }

    // type must be "web" (case-folded — corpus has both "web" and "Web").
    id typeVal = root[@"type"];
    if (![typeVal isKindOfClass:[NSString class]]) {
        if (error != NULL) {
            *error = [NSError errorWithDomain:kWRManifestErrorDomain
                                          code:WRManifestErrorMissingType
                                      userInfo:@{
                NSLocalizedDescriptionKey: [NSString stringWithFormat:@"%@ missing a string \"type\" field", pjPath]
            }];
        }
        return nil;
    }
    if (![[typeVal lowercaseString] isEqualToString:@"web"]) {
        if (error != NULL) {
            *error = [NSError errorWithDomain:kWRManifestErrorDomain
                                          code:WRManifestErrorWrongType
                                      userInfo:@{
                NSLocalizedDescriptionKey: [NSString stringWithFormat:@"%@ has type=\"%@\", expected \"web\"", pjPath, typeVal]
            }];
        }
        return nil;
    }

    WRManifest *m = [WRManifest new];
    m->_workshopDir = [dir copy];

    id fileVal = root[@"file"];
    m->_entryHTML = ([fileVal isKindOfClass:[NSString class]] && [fileVal length])
                        ? [fileVal copy] : @"index.html";
    m->_entryURL = [NSURL fileURLWithPath:[m->_workshopDir stringByAppendingPathComponent:m->_entryHTML]];

    id titleVal = root[@"title"];
    m->_title = ([titleVal isKindOfClass:[NSString class]] && [titleVal length])
                    ? [titleVal copy] : @"Wallpaper";

    id previewVal = root[@"preview"];
    m->_preview = ([previewVal isKindOfClass:[NSString class]] && [previewVal length])
                      ? [previewVal copy] : nil;

    id general = root[@"general"];
    if ([general isKindOfClass:[NSDictionary class]]) {
        id props = ((NSDictionary *)general)[@"properties"];
        if ([props isKindOfClass:[NSDictionary class]]) {
            m->_userProperties = [props copy];
        }
    }

    if (m->_userProperties != nil) {
        NSData *jd = [NSJSONSerialization dataWithJSONObject:m->_userProperties options:0 error:nil];
        m->_userPropertiesJSON = jd ? [[NSString alloc] initWithData:jd encoding:NSUTF8StringEncoding] : @"{}";
    } else {
        m->_userPropertiesJSON = @"{}";
    }

    return m;
}

- (NSString *)workshopDir        { return _workshopDir; }
- (NSString *)entryHTML          { return _entryHTML; }
- (NSURL *)entryURL              { return _entryURL; }
- (NSString *)title              { return _title; }
- (NSString *)preview            { return _preview; }
- (NSDictionary *)userProperties { return _userProperties; }
- (NSString *)userPropertiesJSON { return _userPropertiesJSON; }

@end
