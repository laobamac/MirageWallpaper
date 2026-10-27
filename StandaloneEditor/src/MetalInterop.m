// Copyright © 2026 王孝慈. All rights reserved.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <pthread.h>
static PFN_vkGetInstanceProcAddr originalInstance;
static PFN_vkGetDeviceProcAddr originalDevice;
static PFN_vkSetDebugUtilsObjectNameEXT originalName;
static PFN_vkDestroyImage originalDestroyImage;
static void (*getTexture)(VkImage,id<MTLTexture>*);
static VkResult (*setTexture)(VkImage,id<MTLTexture>);
static NSMutableDictionary<NSString*,id<MTLTexture>> *exports;
static NSMutableDictionary<NSNumber*,NSString*> *imageKeys;
static NSLock *lock;
extern void *MirageAudioFunctions(void *original);
static VkResult setName(VkDevice device,const VkDebugUtilsObjectNameInfoEXT *info){
    const char *name=info->pObjectName;
    if(info->objectType==VK_OBJECT_TYPE_IMAGE && name && (!strncmp(name,"MirageExport:",13)||!strncmp(name,"MirageImport:",13))) {
        @autoreleasepool {
            NSString *key=[NSString stringWithUTF8String:name+13];
            [lock lock];
            VkResult result=VK_ERROR_INITIALIZATION_FAILED;
            if(name[6]=='E') {
                id<MTLTexture> texture=nil;getTexture((VkImage)info->objectHandle,&texture);
                if(texture){exports[key]=texture;imageKeys[@(info->objectHandle)]=key;result=VK_SUCCESS;}
            } else {
                id<MTLTexture> texture=exports[key];
                if(texture)result=setTexture((VkImage)info->objectHandle,texture);
            }
            if(result!=VK_SUCCESS)fprintf(stderr,"[Mirage Metal interop] texture sharing failed: %d\n",result);
            [lock unlock];return result;
        }
    }
    return originalName(device,info);
}
static void destroyImage(VkDevice device,VkImage image,const VkAllocationCallbacks *allocator){
    [lock lock];NSString *key=imageKeys[@((uint64_t)image)];
    if(key){[exports removeObjectForKey:key];[imageKeys removeObjectForKey:@((uint64_t)image)];}
    [lock unlock];originalDestroyImage(device,image,allocator);
}
static PFN_vkVoidFunction replace(const char *name,PFN_vkVoidFunction original){
    if(!strcmp(name,"vkSetDebugUtilsObjectNameEXT")){originalName=(void*)original;return (PFN_vkVoidFunction)setName;}
    if(!strcmp(name,"vkDestroyImage")){originalDestroyImage=(void*)original;return (PFN_vkVoidFunction)destroyImage;}
    return original;
}
static PFN_vkVoidFunction deviceProc(VkDevice device,const char *name){return replace(name,originalDevice(device,name));}
static PFN_vkVoidFunction instanceProc(VkInstance instance,const char *name){
    PFN_vkVoidFunction original=originalInstance(instance,name);
    if(!strcmp(name,"vkGetDeviceProcAddr")){originalDevice=(void*)original;return (PFN_vkVoidFunction)deviceProc;}
    return replace(name,original);
}
static void *lookup(void *handle,const char *name){
    void *original=dlsym(handle,name);
    if(!strcmp(name,"__wine_unix_call_funcs"))return MirageAudioFunctions(original);
    if(!strcmp(name,"vkGetInstanceProcAddr")||!strcmp(name,"vk_icdGetInstanceProcAddr")){
        Dl_info info={0};dladdr(original,&info);
        if(!info.dli_fname||!strstr(info.dli_fname,"MoltenVK"))return original;
        originalInstance=original;
        getTexture=dlsym(handle,"vkGetMTLTextureMVK");setTexture=dlsym(handle,"vkSetMTLTextureMVK");
        if(!getTexture||!setTexture)return original;
        static dispatch_once_t once;dispatch_once(&once,^{
            exports=[NSMutableDictionary new];imageKeys=[NSMutableDictionary new];lock=[NSLock new];
            fprintf(stderr,"[Mirage Metal interop] GPU texture sharing enabled\n");
        });
        return instanceProc;
    }
    return original;
}
__attribute__((used)) static struct {const void *replacement;const void *original;} interpose[] __attribute__((section("__DATA,__interpose")))={{lookup,dlsym}};
