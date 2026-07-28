#include "real_version.h"

#include <string>

using BOOL = int;
using DWORD = unsigned long;
using UINT = unsigned int;
using PUINT = UINT*;
using LPDWORD = DWORD*;
using LPVOID = void*;
using LPCVOID = const void*;
using LPCSTR = const char*;
using LPCWSTR = const wchar_t*;
using LPSTR = char*;
using LPWSTR = wchar_t*;
using HMODULE = void*;
using FARPROC = void (*)();

#ifndef FALSE
#define FALSE 0
#endif

#ifndef WINAPI
#define WINAPI __stdcall
#endif

extern "C" __declspec(dllimport) UINT WINAPI GetSystemDirectoryW(LPWSTR lpBuffer, UINT uSize);
extern "C" __declspec(dllimport) HMODULE WINAPI LoadLibraryW(LPCWSTR lpLibFileName);
extern "C" __declspec(dllimport) FARPROC WINAPI GetProcAddress(HMODULE hModule, LPCSTR lpProcName);

namespace warden {

namespace {
HMODULE g_realVersion{};
}

void LoadRealVersion() {
    if (g_realVersion) {
        return;
    }
    wchar_t systemDir[260]{};
    GetSystemDirectoryW(systemDir, 260);
    std::wstring path = systemDir;
    path += L"\\version.dll";
    g_realVersion = LoadLibraryW(path.c_str());
}

void* RealVersionModule() {
    LoadRealVersion();
    return g_realVersion;
}

}

template <typename Fn>
static Fn RealProc(const char* name) {
    auto mod = static_cast<HMODULE>(warden::RealVersionModule());
    return mod ? reinterpret_cast<Fn>(GetProcAddress(mod, name)) : nullptr;
}

extern "C" {

__declspec(dllexport) BOOL WINAPI GetFileVersionInfoA(LPCSTR a, DWORD b, DWORD c, LPVOID d) {
    using Fn = BOOL(WINAPI*)(LPCSTR, DWORD, DWORD, LPVOID);
    auto fn = RealProc<Fn>("GetFileVersionInfoA");
    return fn ? fn(a, b, c, d) : FALSE;
}

__declspec(dllexport) BOOL WINAPI GetFileVersionInfoW(LPCWSTR a, DWORD b, DWORD c, LPVOID d) {
    using Fn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    auto fn = RealProc<Fn>("GetFileVersionInfoW");
    return fn ? fn(a, b, c, d) : FALSE;
}

__declspec(dllexport) BOOL WINAPI GetFileVersionInfoExA(DWORD flags, LPCSTR a, DWORD b, DWORD c, LPVOID d) {
    using Fn = BOOL(WINAPI*)(DWORD, LPCSTR, DWORD, DWORD, LPVOID);
    auto fn = RealProc<Fn>("GetFileVersionInfoExA");
    return fn ? fn(flags, a, b, c, d) : FALSE;
}

__declspec(dllexport) BOOL WINAPI GetFileVersionInfoExW(DWORD flags, LPCWSTR a, DWORD b, DWORD c, LPVOID d) {
    using Fn = BOOL(WINAPI*)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
    auto fn = RealProc<Fn>("GetFileVersionInfoExW");
    return fn ? fn(flags, a, b, c, d) : FALSE;
}

__declspec(dllexport) DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR a, LPDWORD b) {
    using Fn = DWORD(WINAPI*)(LPCSTR, LPDWORD);
    auto fn = RealProc<Fn>("GetFileVersionInfoSizeA");
    return fn ? fn(a, b) : 0;
}

__declspec(dllexport) DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR a, LPDWORD b) {
    using Fn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    auto fn = RealProc<Fn>("GetFileVersionInfoSizeW");
    return fn ? fn(a, b) : 0;
}

__declspec(dllexport) DWORD WINAPI GetFileVersionInfoSizeExA(DWORD flags, LPCSTR a, LPDWORD b) {
    using Fn = DWORD(WINAPI*)(DWORD, LPCSTR, LPDWORD);
    auto fn = RealProc<Fn>("GetFileVersionInfoSizeExA");
    return fn ? fn(flags, a, b) : 0;
}

__declspec(dllexport) DWORD WINAPI GetFileVersionInfoSizeExW(DWORD flags, LPCWSTR a, LPDWORD b) {
    using Fn = DWORD(WINAPI*)(DWORD, LPCWSTR, LPDWORD);
    auto fn = RealProc<Fn>("GetFileVersionInfoSizeExW");
    return fn ? fn(flags, a, b) : 0;
}

__declspec(dllexport) BOOL WINAPI VerQueryValueA(LPCVOID a, LPCSTR b, LPVOID* c, PUINT d) {
    using Fn = BOOL(WINAPI*)(LPCVOID, LPCSTR, LPVOID*, PUINT);
    auto fn = RealProc<Fn>("VerQueryValueA");
    return fn ? fn(a, b, c, d) : FALSE;
}

__declspec(dllexport) BOOL WINAPI VerQueryValueW(LPCVOID a, LPCWSTR b, LPVOID* c, PUINT d) {
    using Fn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    auto fn = RealProc<Fn>("VerQueryValueW");
    return fn ? fn(a, b, c, d) : FALSE;
}

__declspec(dllexport) DWORD WINAPI VerLanguageNameA(DWORD a, LPSTR b, DWORD c) {
    using Fn = DWORD(WINAPI*)(DWORD, LPSTR, DWORD);
    auto fn = RealProc<Fn>("VerLanguageNameA");
    return fn ? fn(a, b, c) : 0;
}

__declspec(dllexport) DWORD WINAPI VerLanguageNameW(DWORD a, LPWSTR b, DWORD c) {
    using Fn = DWORD(WINAPI*)(DWORD, LPWSTR, DWORD);
    auto fn = RealProc<Fn>("VerLanguageNameW");
    return fn ? fn(a, b, c) : 0;
}

__declspec(dllexport) DWORD WINAPI VerFindFileA(DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPSTR e, PUINT f, LPSTR g, PUINT h) {
    using Fn = DWORD(WINAPI*)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT, LPSTR, PUINT);
    auto fn = RealProc<Fn>("VerFindFileA");
    return fn ? fn(a, b, c, d, e, f, g, h) : 0;
}

__declspec(dllexport) DWORD WINAPI VerFindFileW(DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPWSTR e, PUINT f, LPWSTR g, PUINT h) {
    using Fn = DWORD(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT, LPWSTR, PUINT);
    auto fn = RealProc<Fn>("VerFindFileW");
    return fn ? fn(a, b, c, d, e, f, g, h) : 0;
}

__declspec(dllexport) DWORD WINAPI VerInstallFileA(DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPCSTR e, LPCSTR f, LPSTR g, PUINT h) {
    using Fn = DWORD(WINAPI*)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT);
    auto fn = RealProc<Fn>("VerInstallFileA");
    return fn ? fn(a, b, c, d, e, f, g, h) : 0;
}

__declspec(dllexport) DWORD WINAPI VerInstallFileW(DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPCWSTR e, LPCWSTR f, LPWSTR g, PUINT h) {
    using Fn = DWORD(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT);
    auto fn = RealProc<Fn>("VerInstallFileW");
    return fn ? fn(a, b, c, d, e, f, g, h) : 0;
}

}
