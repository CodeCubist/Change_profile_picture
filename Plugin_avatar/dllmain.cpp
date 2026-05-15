#include <Windows.h>
#include <d3d11.h>
#include <gdiplus.h>
#include "MinHook/MinHook.h"
#include <vector>
#include <unordered_map>
#include <thread>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "MinHook/libMinHook.x64.lib")

HMODULE g_hModule = nullptr;
bool g_is_hooked = false;
ULONG_PTR g_gdiplus_token;
std::unordered_map<uint64_t, std::vector<uint8_t>> debug_textures;
std::unordered_map<uint64_t, bool> notified_sizes;

HRESULT(WINAPI* origin_func_Map)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*) = nullptr;

void init_console() {
    AllocConsole();
    FILE* fDummy;
    freopen_s(&fDummy, "CONOUT$", "w", stdout);
    freopen_s(&fDummy, "CONIN$", "r", stdin);
    SetConsoleTitleW(L"Avatar Plugin Status Console");
    printf("[Info] Console allocated successfully.\n");
}

void free_console() {
    printf("[Info] Freeing console and exiting...\n");
    FreeConsole();
}

HRESULT WINAPI hooked_Map(ID3D11DeviceContext* _this, ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource) {
    HRESULT hr = origin_func_Map(_this, pResource, Subresource, MapType, MapFlags, pMappedResource);

    if (FAILED(hr))
        return hr;

    if (MapType != D3D11_MAP_READ && MapType != D3D11_MAP_READ_WRITE)
        return hr;

    ID3D11Texture2D* d3d_texture = nullptr;
    if (FAILED(pResource->QueryInterface(IID_ID3D11Texture2D, (void**)&d3d_texture)))
        return hr;

    D3D11_TEXTURE2D_DESC desc;
    d3d_texture->GetDesc(&desc);
    d3d_texture->Release();

    if (desc.Usage == D3D11_USAGE_STAGING && desc.Width == desc.Height) {
        if (desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {

            if (auto it = debug_textures.find(desc.Width); it != debug_textures.end()) {
                uint8_t* __restrict dst = static_cast<uint8_t*>(pMappedResource->pData);
                const uint8_t* __restrict src = it->second.data();
                unsigned row_size = desc.Width * 4;

                for (unsigned y = 0; y < desc.Height; y++)
                    std::memcpy(dst + y * pMappedResource->RowPitch, src + y * row_size, row_size);

                if (!notified_sizes[desc.Width]) {
                    printf("[Success] Replaced texture at runtime. Size: %ux%u\n", desc.Width, desc.Height);
                    notified_sizes[desc.Width] = true;
                }
            }
        }
    }

    return hr;
}

bool get_image_path(wchar_t* out_path, size_t max_len) {
    if (!GetModuleFileNameW(g_hModule, out_path, (DWORD)max_len))
        return false;

    wchar_t* last_slash = wcsrchr(out_path, L'\\');
    if (last_slash) {
        *(last_slash + 1) = L'\0';
    }
    
    wcscat_s(out_path, max_len, L"avatar.png");
    return true;
}

bool load_target_texture() {
    printf("[Process] Initializing GDI+...\n");
    Gdiplus::GdiplusStartupInput gdiplus_startup_input;
    if (Gdiplus::GdiplusStartup(&g_gdiplus_token, &gdiplus_startup_input, nullptr) != Gdiplus::Ok) {
        printf("[Error] Failed to initialize GDI+.\n");
        return false;
    }

    wchar_t image_path[MAX_PATH] = { 0 };
    if (!get_image_path(image_path, MAX_PATH)) {
        printf("[Error] Failed to get module path.\n");
        return false;
    }

    wprintf(L"[Process] Loading image from: %s\n", image_path);
    Gdiplus::Bitmap origin_bmp(image_path);
    if (origin_bmp.GetLastStatus() != Gdiplus::Ok) {
        printf("[Error] Failed to load avatar.png. Ensure it exists in the same directory.\n");
        return false;
    }

    std::vector<int> sizes = { 1024, 512, 256, 128, 64 };
    printf("[Process] Generating target textures...\n");

    for (int texture_size : sizes) {
        Gdiplus::Bitmap bmp(texture_size, texture_size, PixelFormat32bppARGB);
        Gdiplus::Graphics graphics(&bmp);

        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.Clear(Gdiplus::Color::Transparent);

        Gdiplus::GraphicsPath path;
        path.AddEllipse(0, 0, texture_size, texture_size);
        graphics.SetClip(&path);

        graphics.TranslateTransform(0, (Gdiplus::REAL)texture_size);
        graphics.ScaleTransform(1.0f, -1.0f);
        graphics.DrawImage(&origin_bmp, 0, 0, texture_size, texture_size);

        Gdiplus::Rect rect(0, 0, texture_size, texture_size);
        Gdiplus::BitmapData bmp_data;

        if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bmp_data) != Gdiplus::Ok)
            continue;

        std::vector<uint8_t> texture(texture_size * texture_size * 4);
        const uint8_t* __restrict src = (uint8_t*)bmp_data.Scan0;
        
        for (int i = 0; i < texture_size * texture_size * 4; i += 4) {
            texture[i + 0] = src[i + 2];
            texture[i + 1] = src[i + 1];
            texture[i + 2] = src[i + 0];
            texture[i + 3] = src[i + 3];
        }

        bmp.UnlockBits(&bmp_data);
        debug_textures[texture_size] = std::move(texture);
        printf("[Process] Texture generated: %dx%d\n", texture_size, texture_size);
    }

    printf("[Success] Image loading and processing completed.\n");
    return true;
}

void init_hook() {
    if (!load_target_texture())
        return;

    printf("[Process] Initializing D3D11 Hook...\n");
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) hwnd = GetDesktopWindow();

    D3D_FEATURE_LEVEL levels[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = hwnd;
    sd.Windowed = true;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* d3d_device = nullptr;
    IDXGISwapChain* d3d_swap_chain = nullptr;
    ID3D11DeviceContext* d3d_context = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(0, D3D_DRIVER_TYPE_HARDWARE, 0, 0, levels, 2, D3D11_SDK_VERSION, &sd, &d3d_swap_chain, &d3d_device, 0, &d3d_context);
    if (FAILED(hr)) {
        printf("[Error] D3D11CreateDeviceAndSwapChain failed.\n");
        return;
    }

    auto context_vtable = *(void***)d3d_context;

    MH_Initialize();
    MH_CreateHook(context_vtable[14], (void*)hooked_Map, (void**)&origin_func_Map);
    MH_EnableHook(MH_ALL_HOOKS);

    d3d_device->Release();
    d3d_swap_chain->Release();
    d3d_context->Release();

    g_is_hooked = true;
    printf("[Success] D3D11 Hook successfully installed. Waiting for textures...\n");
}

void unhook() {
    if (g_is_hooked) {
        printf("[Process] Disabling hooks...\n");
        MH_DisableHook(MH_ALL_HOOKS);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        MH_Uninitialize();
        g_is_hooked = false;
    }
    
    debug_textures.clear();
    notified_sizes.clear();
    Gdiplus::GdiplusShutdown(g_gdiplus_token);
}

DWORD WINAPI MainThread(LPVOID lpReserved) {
    init_console();
    init_hook();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason_for_call, LPVOID lpReserved) {
    switch (reason_for_call) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            g_hModule = hModule;
            CloseHandle(CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr));
            break;

        case DLL_PROCESS_DETACH:
            unhook();
            free_console();
            break;
    }
    return TRUE;
}