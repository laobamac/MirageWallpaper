// Copyright © 2026 王孝慈. All rights reserved.
// DXVK 1.10.3 / MoltenVK bridge for the isolated editor. Each renderer keeps
// its own D3D11 device and context; only the underlying Metal texture is shared.
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <vulkan/vulkan.h>

static HMODULE realModule;
static CRITICAL_SECTION lock;
static uintptr_t nextHandle=0x4d000000;
static HRESULT (WINAPI *realCreate)(IDXGIAdapter*,D3D_DRIVER_TYPE,HMODULE,UINT,const D3D_FEATURE_LEVEL*,UINT,UINT,ID3D11Device**,D3D_FEATURE_LEVEL*,ID3D11DeviceContext**);
static HRESULT (WINAPI *realTexture)(ID3D11Device*,const D3D11_TEXTURE2D_DESC*,const D3D11_SUBRESOURCE_DATA*,ID3D11Texture2D**);
static HRESULT (WINAPI *realOpen)(ID3D11Device*,HANDLE,REFIID,void**);
static HRESULT (WINAPI *realGetHandle)(IDXGIResource*,HANDLE*);
static ULONG (WINAPI *realRelease)(ID3D11Texture2D*);
static void (WINAPI *realClear)(ID3D11DeviceContext*,ID3D11RenderTargetView*,const FLOAT*);
typedef struct SharedTexture { ID3D11Texture2D *texture; IDXGIResource *resource; HANDLE handle; struct SharedTexture *next; } SharedTexture;
static SharedTexture *textures;

static void patch(void **entry, void *replacement, void **original) {
    if(*entry==replacement)return;
    DWORD protection;VirtualProtect(entry,sizeof(void*),PAGE_READWRITE,&protection);
    *original=*entry;*entry=replacement;
    VirtualProtect(entry,sizeof(void*),protection,&protection);
}
static const GUID IID_VkSurface={0x5546cf8c,0x77e7,0x4341,{0xb0,0x5d,0x8d,0x4d,0x50,0x00,0xe7,0x7d}};
static const GUID IID_VkDevice={0xe2ef5fa5,0xdc21,0x4af7,{0x90,0xc4,0xf6,0x7e,0xf6,0xa0,0x93,0x23}};
static void WINAPI clearRenderTarget(ID3D11DeviceContext *context,ID3D11RenderTargetView *view,const FLOAT *color) {
    realClear(context,view,color);
    if(!view)return;
    ID3D11Resource *resource=NULL;ID3D11Texture2D *texture=NULL;
    ID3D11RenderTargetView_GetResource(view,&resource);
    if(!resource)return;
    HRESULT hr=ID3D11Resource_QueryInterface(resource,&IID_ID3D11Texture2D,(void**)&texture);
    ID3D11Resource_Release(resource);
    if(FAILED(hr))return;
    BOOL shared=FALSE;
    EnterCriticalSection(&lock);
    for(SharedTexture *t=textures;t;t=t->next)if(t->texture==texture){shared=TRUE;break;}
    LeaveCriticalSection(&lock);
    if(shared) {
        void *surface=NULL,*interop=NULL;
        hr=ID3D11Texture2D_QueryInterface(texture,&IID_VkSurface,&surface);
        if(SUCCEEDED(hr)) {
            VkImageLayout layout;
            hr=((HRESULT (STDMETHODCALLTYPE*)(void*,VkImage*,VkImageLayout*,VkImageCreateInfo*))(*(void***)surface)[4])(surface,NULL,&layout,NULL);
            if(SUCCEEDED(hr))hr=((HRESULT (STDMETHODCALLTYPE*)(void*,void**))(*(void***)surface)[3])(surface,&interop);
            if(SUCCEEDED(hr)) {
                D3D11_TEXTURE2D_DESC desc;ID3D11Texture2D_GetDesc(texture,&desc);
                VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,desc.MipLevels,0,desc.ArraySize};
                // The bridge removes DXGI's native sharing flag, so DXVK would
                // otherwise defer this clear past Flush. A same-layout interop
                // transition materializes it without a CPU/GPU wait per frame.
                ((void (STDMETHODCALLTYPE*)(void*,void*,const VkImageSubresourceRange*,VkImageLayout,VkImageLayout))(*(void***)interop)[5])(interop,surface,&range,layout,layout);
                IUnknown_Release((IUnknown*)interop);
            }
            IUnknown_Release((IUnknown*)surface);
        }
    }
    ID3D11Texture2D_Release(texture);
}

static VkResult markTexture(ID3D11Texture2D *texture,HANDLE handle,const char *mode) {
    void *surface=NULL,*interop=NULL;ID3D11Device *device=NULL;
    HRESULT hr=ID3D11Texture2D_QueryInterface(texture,&IID_VkSurface,&surface);
    if(FAILED(hr))return VK_ERROR_INITIALIZATION_FAILED;
    VkImage image=VK_NULL_HANDLE;VkDevice vkDevice=VK_NULL_HANDLE;
    hr=((HRESULT (STDMETHODCALLTYPE*)(void*,VkImage*,VkImageLayout*,VkImageCreateInfo*))(*(void***)surface)[4])(surface,&image,NULL,NULL);
    ID3D11Texture2D_GetDevice(texture,&device);
    if(SUCCEEDED(hr))hr=ID3D11Device_QueryInterface(device,&IID_VkDevice,&interop);
    VkResult result=VK_ERROR_INITIALIZATION_FAILED;
    if(SUCCEEDED(hr)) {
        ((void (STDMETHODCALLTYPE*)(void*,VkInstance*,VkPhysicalDevice*,VkDevice*))(*(void***)interop)[3])(interop,NULL,NULL,&vkDevice);
        HMODULE vulkan=GetModuleHandleW(L"vulkan-1.dll");
        PFN_vkGetDeviceProcAddr get=(void*)GetProcAddress(vulkan,"vkGetDeviceProcAddr");
        PFN_vkSetDebugUtilsObjectNameEXT set=get?(void*)get(vkDevice,"vkSetDebugUtilsObjectNameEXT"):NULL;
        char name[80];snprintf(name,sizeof(name),"Mirage%s:%llx",mode,(unsigned long long)(uintptr_t)handle);
        VkDebugUtilsObjectNameInfoEXT info={VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,NULL,VK_OBJECT_TYPE_IMAGE,(uint64_t)image,name};
        if(set && !strcmp(mode,"Import")) {
            // DXVK queues a clear when creating the destination image. Finish
            // that initialization before replacing its Metal backing, or the
            // queued clear can erase the producer's already-rendered pixels.
            PFN_vkQueueWaitIdle wait=(void*)get(vkDevice,"vkQueueWaitIdle");
            VkQueue queue=VK_NULL_HANDLE;
            ((void (STDMETHODCALLTYPE*)(void*,VkQueue*,uint32_t*))(*(void***)interop)[4])(interop,&queue,NULL);
            if(wait && queue) {
                ((void (STDMETHODCALLTYPE*)(void*))(*(void***)interop)[6])(interop);
                ((void (STDMETHODCALLTYPE*)(void*))(*(void***)interop)[7])(interop);
                result=wait(queue);
                if(result==VK_SUCCESS)result=set(vkDevice,&info);
                ((void (STDMETHODCALLTYPE*)(void*))(*(void***)interop)[8])(interop);
            }
        } else if(set)result=set(vkDevice,&info);
        else fprintf(stderr,"[Mirage interop] debug marker unavailable\n");
        IUnknown_Release((IUnknown*)interop);
    }
    IUnknown_Release((IUnknown*)surface);ID3D11Device_Release(device);return result;
}
static HRESULT WINAPI getHandle(IDXGIResource *resource,HANDLE *handle) {
    EnterCriticalSection(&lock);
    for(SharedTexture *t=textures;t;t=t->next)if(t->resource==resource){
        if(handle)*handle=t->handle;
        LeaveCriticalSection(&lock);return handle?S_OK:E_INVALIDARG;
    }
    LeaveCriticalSection(&lock);return realGetHandle(resource,handle);
}
static ULONG WINAPI releaseTexture(ID3D11Texture2D *texture) {
    // Destruction may acquire DXVK locks. Never hold our registry lock across
    // it, or a render thread releasing a resource can invert the lock order.
    HANDLE generation=NULL;
    EnterCriticalSection(&lock);
    for(SharedTexture *t=textures;t;t=t->next)if(t->texture==texture){generation=t->handle;break;}
    LeaveCriticalSection(&lock);
    ULONG refs=realRelease(texture);
    if(!refs && generation) {
        EnterCriticalSection(&lock);
        SharedTexture **slot=&textures;
        while(*slot){
            SharedTexture *t=*slot;
            if(t->texture==texture && t->handle==generation){*slot=t->next;HeapFree(GetProcessHeap(),0,t);break;}
            slot=&t->next;
        }
        LeaveCriticalSection(&lock);
    }
    return refs;
}
static HRESULT WINAPI createTexture(ID3D11Device *device,const D3D11_TEXTURE2D_DESC *desc,const D3D11_SUBRESOURCE_DATA *initial,ID3D11Texture2D **out) {
    if(!desc || !(desc->MiscFlags&D3D11_RESOURCE_MISC_SHARED))return realTexture(device,desc,initial,out);
    if(desc->MiscFlags&(D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX|D3D11_RESOURCE_MISC_SHARED_NTHANDLE))return realTexture(device,desc,initial,out);
    D3D11_TEXTURE2D_DESC local=*desc;local.MiscFlags&=~D3D11_RESOURCE_MISC_SHARED;
    HRESULT result=realTexture(device,&local,initial,out);
    if(FAILED(result)||!out||!*out)return result;
    IDXGIResource *resource=NULL;
    if(FAILED(ID3D11Texture2D_QueryInterface(*out,&IID_IDXGIResource,(void**)&resource)))return result;
    SharedTexture *t=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*t));
    if(!t){IDXGIResource_Release(resource);ID3D11Texture2D_Release(*out);*out=NULL;return E_OUTOFMEMORY;}
    t->texture=*out;t->resource=resource;
    EnterCriticalSection(&lock);
    // Opaque KMT-style IDs never go to the OS. Monotonic IDs prevent an old
    // GPU image destruction callback from erasing a newer export after resize.
    t->handle=(HANDLE)++nextHandle;
    t->next=textures;textures=t;
    patch((void**)&resource->lpVtbl->GetSharedHandle,getHandle,(void**)&realGetHandle);
    patch((void**)&(*out)->lpVtbl->Release,releaseTexture,(void**)&realRelease);
    LeaveCriticalSection(&lock);
    VkResult exported=markTexture(*out,t->handle,"Export");
    IDXGIResource_Release(resource);
    if(exported!=VK_SUCCESS){ID3D11Texture2D_Release(*out);*out=NULL;return E_FAIL;}
    return result;
}
static HRESULT WINAPI openResource(ID3D11Device *device,HANDLE handle,REFIID iid,void **out) {
    if(!out)return E_INVALIDARG;
    *out=NULL;
    ID3D11Texture2D *source=NULL;
    EnterCriticalSection(&lock);
    for(SharedTexture *t=textures;t;t=t->next)if(t->handle==handle){source=t->texture;ID3D11Texture2D_AddRef(source);break;}
    LeaveCriticalSection(&lock);
    if(!source)return realOpen(device,handle,iid,out);
    D3D11_TEXTURE2D_DESC desc;ID3D11Texture2D_GetDesc(source,&desc);
    ID3D11Texture2D *imported=NULL;
    HRESULT hr=realTexture(device,&desc,NULL,&imported);
    if(SUCCEEDED(hr)) {
        VkResult native=markTexture(imported,handle,"Import");
        hr=native==VK_SUCCESS?ID3D11Texture2D_QueryInterface(imported,iid,out):E_FAIL;
        ID3D11Texture2D_Release(imported);
    }
    ID3D11Texture2D_Release(source);
    return hr;
}
HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter *adapter,D3D_DRIVER_TYPE type,HMODULE software,UINT flags,const D3D_FEATURE_LEVEL *levels,UINT count,UINT sdk,ID3D11Device **out,D3D_FEATURE_LEVEL *level,ID3D11DeviceContext **context) {
    EnterCriticalSection(&lock);
    if(!realModule){realModule=LoadLibraryW(L"mirage_dxvk.dll");realCreate=(void*)GetProcAddress(realModule,"D3D11CreateDevice");}
    LeaveCriticalSection(&lock);
    if(!realCreate)return E_FAIL;
    ID3D11Device *device=NULL;
    HRESULT hr=realCreate(adapter,type,software,flags,levels,count,sdk,&device,level,context);
    if(FAILED(hr))return hr;
    EnterCriticalSection(&lock);
    patch((void**)&device->lpVtbl->CreateTexture2D,createTexture,(void**)&realTexture);
    patch((void**)&device->lpVtbl->OpenSharedResource,openResource,(void**)&realOpen);
    ID3D11DeviceContext *immediate=NULL;ID3D11Device_GetImmediateContext(device,&immediate);
    patch((void**)&immediate->lpVtbl->ClearRenderTargetView,clearRenderTarget,(void**)&realClear);
    ID3D11DeviceContext_Release(immediate);
    LeaveCriticalSection(&lock);
    if(out)*out=device;else ID3D11Device_Release(device);
    return hr;
}
HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter *adapter,D3D_DRIVER_TYPE type,HMODULE software,UINT flags,const D3D_FEATURE_LEVEL *levels,UINT count,UINT sdk,const DXGI_SWAP_CHAIN_DESC *desc,IDXGISwapChain **swap,ID3D11Device **device,D3D_FEATURE_LEVEL *level,ID3D11DeviceContext **context) {
    ID3D11Device *created=NULL;HRESULT hr=D3D11CreateDevice(adapter,type,software,flags,levels,count,sdk,&created,level,context);
    if(FAILED(hr))return hr;
    if(swap){IDXGIDevice *dxgi=NULL;IDXGIAdapter *parent=NULL;IDXGIFactory *factory=NULL;hr=ID3D11Device_QueryInterface(created,&IID_IDXGIDevice,(void**)&dxgi);if(SUCCEEDED(hr))hr=IDXGIDevice_GetAdapter(dxgi,&parent);if(SUCCEEDED(hr))hr=IDXGIAdapter_GetParent(parent,&IID_IDXGIFactory,(void**)&factory);if(SUCCEEDED(hr))hr=IDXGIFactory_CreateSwapChain(factory,(IUnknown*)created,(DXGI_SWAP_CHAIN_DESC*)desc,swap);if(factory)IDXGIFactory_Release(factory);if(parent)IDXGIAdapter_Release(parent);if(dxgi)IDXGIDevice_Release(dxgi);}
    if(device)*device=created;else ID3D11Device_Release(created);return hr;
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,void *reserved){if(reason==DLL_PROCESS_ATTACH){InitializeCriticalSection(&lock);DisableThreadLibraryCalls(instance);}return TRUE;}
