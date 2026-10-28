// Copyright © 2026 王孝慈. All rights reserved.
// Two independent devices must see the same Metal texture through the D3D11 bridge.
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>

#define CHECK(call) do { HRESULT hr = (call); if (FAILED(hr)) { \
    fprintf(stderr, "%s failed: 0x%08lx\n", #call, (unsigned long)hr); return 1; } } while (0)

static int complete(ID3D11Device *device, ID3D11DeviceContext *context) {
    D3D11_QUERY_DESC desc = {D3D11_QUERY_EVENT, 0};
    ID3D11Query *query = NULL;
    CHECK(ID3D11Device_CreateQuery(device, &desc, &query));
    ID3D11DeviceContext_End(context, (ID3D11Asynchronous *)query);
    ID3D11DeviceContext_Flush(context);
    ULONGLONG start = GetTickCount64();
    HRESULT result;
    while ((result = ID3D11DeviceContext_GetData(context, (ID3D11Asynchronous *)query, NULL, 0, 0)) == S_FALSE) {
        if (GetTickCount64() - start > 15000) { ID3D11Query_Release(query); return 1; }
        Sleep(1);
    }
    ID3D11Query_Release(query);
    return FAILED(result);
}

static int readback(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *texture,
                    const BYTE expected[4], const char *label) {
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D *staging = NULL;
    ID3D11Texture2D_GetDesc(texture, &desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, (ID3D11Resource *)texture);
    if (complete(device, context)) return 3;
    D3D11_MAPPED_SUBRESOURCE mapped;
    CHECK(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped));
    for (UINT row = 0; row < desc.Height; ++row) {
        const BYTE *pixel = (const BYTE *)mapped.pData + row * mapped.RowPitch;
        for (UINT column = 0; column < desc.Width; ++column, pixel += 4) {
            for (UINT channel = 0; channel < 4; ++channel) {
                int difference = (int)pixel[channel] - expected[channel];
                if (difference < -1 || difference > 1) {
                    fprintf(stderr, "%s pixel mismatch: %u,%u,%u,%u\n", label, pixel[0], pixel[1], pixel[2], pixel[3]);
                    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
                    ID3D11Texture2D_Release(staging);
                    return 4;
                }
            }
        }
    }
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);
    return 0;
}

int main(void) {
    ID3D11Device *writer = NULL, *reader = NULL;
    ID3D11DeviceContext *writeContext = NULL, *readContext = NULL;
    CHECK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &writer, NULL, &writeContext));
    CHECK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &reader, NULL, &readContext));
    // Repeat to exercise texture destruction and handle reuse during preview resizing.
    for (UINT iteration = 0; iteration < 12; ++iteration) {
        D3D11_TEXTURE2D_DESC desc = {32 + iteration, 16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
            {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED};
        ID3D11Texture2D *exported = NULL, *imported = NULL;
        ID3D11RenderTargetView *view = NULL;
        IDXGIResource *resource = NULL;
        CHECK(ID3D11Device_CreateTexture2D(writer, &desc, NULL, &exported));
        CHECK(ID3D11Device_CreateRenderTargetView(writer, (ID3D11Resource *)exported, NULL, &view));
        const FLOAT color[4] = {1, 0.25f, 0, 1};
        const BYTE expected[4] = {255, 64, 0, 255};
        ID3D11DeviceContext_ClearRenderTargetView(writeContext, view, color);
        if (complete(writer, writeContext)) return 2;
        CHECK(ID3D11Texture2D_QueryInterface(exported, &IID_IDXGIResource, (void **)&resource));
        HANDLE handle = NULL;
        CHECK(IDXGIResource_GetSharedHandle(resource, &handle));
        CHECK(ID3D11Device_OpenSharedResource(reader, handle, &IID_ID3D11Texture2D, (void **)&imported));
        if (readback(reader, readContext, imported, expected, "initial frame")) return 4;
        const FLOAT nextColor[4] = {0, 0.5f, 1, 1};
        const BYTE nextExpected[4] = {0, 128, 255, 255};
        ID3D11DeviceContext_ClearRenderTargetView(writeContext, view, nextColor);
        if (complete(writer, writeContext)) return 2;
        if (readback(reader, readContext, imported, nextExpected, "updated frame")) return 4;
        IDXGIResource_Release(resource); ID3D11RenderTargetView_Release(view); ID3D11Texture2D_Release(exported);
        // The imported texture must retain its backing after the producer exits.
        if (readback(reader, readContext, imported, nextExpected, "released producer")) return 4;
        ID3D11Texture2D_Release(imported);
    }
    ID3D11DeviceContext_Release(readContext); ID3D11DeviceContext_Release(writeContext);
    ID3D11Device_Release(reader); ID3D11Device_Release(writer);
    puts("MIRAGE_D3D11_SHARED_TEXTURE_OK");
    return 0;
}
