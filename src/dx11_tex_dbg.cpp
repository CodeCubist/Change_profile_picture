#include <thread>
#include <cstring>
#include <Windows.h>
#include <d3d11.h>
#include <gdiplus.h>
#include <MinHook.h>

#if defined(_MSC_VER) || (defined(__clang__) && defined(_WIN32))
	// MSVC / MSVC Clang
    #pragma data_seg(".shared")
	wchar_t target_process_name[64] = {0};
	int state = 0;
    #pragma data_seg()
    #pragma comment(linker, "/SECTION:.shared,RWS")
#else
    // GCC / MinGW
    wchar_t target_process_name[64] __attribute__((section(".shared"), shared)) = {0};
    int state __attribute__((section(".shared"), shared)) = 0;
#endif

bool is_hooked = false;
uint8_t* __restrict debug_texture_data = nullptr;
unsigned texture_size = 0;

HRESULT(WINAPI *origin_func_Map)(ID3D11DeviceContext*,ID3D11Resource*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*) = nullptr;


bool varify_process_name() {		
	wchar_t process_name[MAX_PATH], *process_base_name;

	if (!GetModuleFileNameW(nullptr, process_name, MAX_PATH))
		return false;

	if (!(process_base_name = wcsrchr(process_name, L'\\')))
		return false;

	if (_wcsicmp(process_base_name + 1, target_process_name) == 0)
		return true;

	return false;
}


bool fetch_dbgtex_via_pipe() {
    constexpr char pipe_name[] = "\\\\.\\pipe\\D3D11_TexDbg_SharedBuffer";

    HANDLE pipe = CreateFileA(pipe_name, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);

    if (pipe == INVALID_HANDLE_VALUE) 
        return false;

    DWORD bytes_read;
    ReadFile(pipe, &texture_size, sizeof(texture_size), &bytes_read, nullptr);

    debug_texture_data = new uint8_t[texture_size * texture_size * 4];
    ReadFile(pipe, debug_texture_data, texture_size * texture_size * 4, &bytes_read, nullptr);

    CloseHandle(pipe);
    return true;
}


HRESULT WINAPI hooked_Map(ID3D11DeviceContext* _this, ID3D11Resource *pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE *pMappedResource) {
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

	if (desc.Usage == D3D11_USAGE_STAGING && 
		desc.Width == texture_size && desc.Height == texture_size && 
		desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS &&
		pMappedResource->RowPitch == texture_size * 4
	) {
		std::memcpy(pMappedResource->pData, debug_texture_data, texture_size * texture_size * 4);
	}

    return hr;
}


void hook() {
	D3D_FEATURE_LEVEL levels[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 1;
	sd.OutputWindow = GetForegroundWindow();
	sd.Windowed = true;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	
	ID3D11Device* d3d_device = nullptr;
	IDXGISwapChain* d3d_swap_chain = nullptr;
	ID3D11DeviceContext* d3d_context = nullptr;

	HRESULT hr = D3D11CreateDeviceAndSwapChain(0, D3D_DRIVER_TYPE_HARDWARE, 0, 0, levels, 2, D3D11_SDK_VERSION, &sd, &d3d_swap_chain, &d3d_device, 0, &d3d_context);
	if(FAILED(hr)) {
		MessageBoxA(nullptr, "Error in hook(): D3D11CreateDeviceAndSwapChain", "Error", MB_OK | MB_ICONERROR);
		return;
	}

	if (!fetch_dbgtex_via_pipe()) {
        MessageBoxA(nullptr, "Error in hook(): Faild to fetch Texture data", "Error", MB_OK | MB_ICONERROR);
		return;
    }

	auto context_vtable_ptr = (void***)d3d_context;
	auto context_vtable = *context_vtable_ptr;

	auto func_map = context_vtable[14];

	MH_Initialize();
	MH_CreateHook(func_map, (void*)hooked_Map, (void**)&origin_func_Map);
	MH_EnableHook(MH_ALL_HOOKS);

	d3d_device->Release();
	d3d_swap_chain->Release();
	d3d_context->Release();

	is_hooked = true;
}


void unhook() {
	MH_DisableHook(MH_ALL_HOOKS);
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	MH_Uninitialize();
	delete[] debug_texture_data;
	is_hooked = false;
}



BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason_for_call, LPVOID lpReserved) {
	if (reason_for_call == DLL_PROCESS_ATTACH) {
        if (state == 0) {
            state = 1;
            return true;
        }
		
        if (state == 1 && varify_process_name()) {
			state = 2;
			std::thread(hook).detach();
			return true;
		}

		return false;
	}

	else if (reason_for_call == DLL_PROCESS_DETACH && is_hooked) {
		if (lpReserved == nullptr)
			unhook();
		state = 1;
	}
		
    return true;
}


extern "C" {
	__declspec(dllexport) void set_target(const char* process_name) {
		if (process_name) {
			MultiByteToWideChar(CP_UTF8, 0, process_name, -1, target_process_name, 64);
			target_process_name[63] = L'\0';
		}
    }

	__declspec(dllexport) LRESULT CALLBACK hook_proc(int code, WPARAM wParam, LPARAM lParam) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
}