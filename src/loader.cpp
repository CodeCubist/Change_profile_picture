#include <exception>
#include <format>
#include <filesystem>
#include <span>
#include <Windows.h>
#include <gdiplus.h>
#include <commdlg.h>

import console;
import cmdline;

namespace ansi = console::ansi;

template <class Func, class T> struct scope_exit {
    Func func;
    T& data;
    ~scope_exit() { func(data); }
};


auto load_texture(const wchar_t* file_path, const int texture_size) -> std::vector<uint8_t> {
    Gdiplus::GdiplusStartupInput gdiplus_startup_input;
    ULONG_PTR gdiplus_token;

    Gdiplus::GdiplusStartup(&gdiplus_token, &gdiplus_startup_input, nullptr);
    scope_exit auto_shutdown_gdi(Gdiplus::GdiplusShutdown, gdiplus_token);

    auto origin_texture = Gdiplus::Bitmap(file_path);
    if (origin_texture.GetLastStatus() != Gdiplus::Ok)
        throw std::runtime_error("Failed to Load Image File");

    auto texture = Gdiplus::Bitmap(texture_size, texture_size, PixelFormat32bppARGB);
    auto graphics = Gdiplus::Graphics(&texture);

    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.Clear(Gdiplus::Color::Transparent);

    Gdiplus::GraphicsPath path;
    path.AddEllipse(0, 0, texture_size, texture_size);
    graphics.SetClip(&path);

    graphics.TranslateTransform(0, (Gdiplus::REAL)texture_size);
    graphics.ScaleTransform(1.0f, -1.0f);
    graphics.DrawImage(&origin_texture, 0, 0, texture_size, texture_size);

    auto rect = Gdiplus::Rect(0, 0, texture_size, texture_size);
    Gdiplus::BitmapData bmp_data;

    if (texture.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bmp_data) != Gdiplus::Ok)
        throw std::runtime_error("Failed to Lock bits");
    
    std::vector<uint8_t> result(texture_size * texture_size * 4);
    const uint8_t* __restrict src = (uint8_t*)bmp_data.Scan0;
    for (int i = 0; i < texture_size * texture_size * 4; i += 4) {
        result[i + 0] = src[i + 2];
        result[i + 1] = src[i + 1];
        result[i + 2] = src[i + 0];
        result[i + 3] = src[i + 3];
    }

    texture.UnlockBits(&bmp_data);
    return result;
}


template <class Func>
auto get_module_func(HMODULE module, const char* func_name) -> Func {
    if(auto func = (Func)(void*)GetProcAddress(module, func_name); !func)
        throw std::runtime_error(std::format("Cannot find function '{}' in module", func_name));
    else return func;
}


auto open_file_picker(const wchar_t* filter, const std::filesystem::path& initial_dir = L"") -> std::wstring {
    OPENFILENAMEW ofn = {};
    wchar_t file_path[MAX_PATH] = {0};
    
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = file_path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrInitialDir = initial_dir.c_str();
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn) == true)
        return std::wstring(ofn.lpstrFile);

    return L"";
}


void start_texdbg_pipe(const std::span<uint8_t> data, int texture_size) {
    constexpr char pipe_name[] = "\\\\.\\pipe\\D3D11_TexDbg_SharedBuffer";

    HANDLE pipe = CreateNamedPipeA(pipe_name, PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_WAIT, 1, data.size() + 4, 0, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE)
        throw std::runtime_error(std::format("Failed to Create Named Pipe '{}{}{}'", ansi::blue, pipe_name, ansi::reset));

    scope_exit auto_close_pipe(CloseHandle, pipe);

    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
        throw std::runtime_error(std::format("Failed to Connect to Named Pipe '{}{}{}'", ansi::blue, pipe_name, ansi::reset));

    DWORD written;
    WriteFile(pipe, &texture_size, sizeof(texture_size), &written, nullptr);
    WriteFile(pipe, data.data(), data.size(), &written, nullptr);
}


auto main(int argc, char* argv[]) -> int try {
    console::init();
    auto args = cmdline::parse(argc, argv);

    int texture_size = args["--texture_size"] | args["-s"] | 512;
    std::string module_path = args["--module"] | args["-m"] | "dx11_texture_debugger.dll";
    std::string target_process_name = args["--target"] | args["-t"] | "";

    console::info("DirectX 11 Texture Debugger {}v1.0.0{}", ansi::blue, ansi::reset);
    console::info("Target: '{}{}{}'", ansi::blue, target_process_name, ansi::reset);

    HMODULE module = LoadLibraryA(module_path.c_str());
    if(!module)
        throw std::runtime_error(std::format("Failed to Load '{}{}{}'", ansi::blue, module_path, ansi::reset));

    scope_exit auto_free_module(FreeLibrary, module);
    console::info("Successfully Load '{}{}{}'", ansi::blue, module_path, ansi::reset);

    std::wstring texture_file = open_file_picker(L"Image Files\0*.jpg;*.jpeg;*.png;*.bmp\0All Files\0*.*\0");
    std::vector<uint8_t> texture = load_texture(texture_file.c_str(), texture_size);
    console::info("Successfully Load Debug Texture Image");

    auto func_hook_proc = get_module_func<HOOKPROC>(module, "hook_proc");
    auto func_set_target = get_module_func<void(*)(const char*)>(module, "set_target");

    func_set_target(target_process_name.c_str());
    
    HHOOK hook = SetWindowsHookExW(WH_CBT, func_hook_proc, module, 0);
    if(!hook)
        throw std::runtime_error("Unable to Set Windows Hook");

    scope_exit auto_unhook(UnhookWindowsHookEx, hook);
    console::info("Successfully Set Windows Hook");
    
    console::info("Waiting for the Texture data to be taken by Target...");
    start_texdbg_pipe(texture, texture_size);
    console::info("Texture data was Successfully taken by Target");

    console::info("Press {}Enter{} to Exit...", ansi::blue, ansi::reset);
    std::getchar();
    return 0;
}

catch(const std::exception& e) {
    console::println("{}[Error]{} {}", ansi::red, ansi::reset, e.what());
    system("pause");
}