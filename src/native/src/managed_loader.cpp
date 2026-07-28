#include "managed_loader.h"
#include "path_utils.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using hostfxr_handle = void*;
using hostfxr_initialize_for_runtime_config_fn = int32_t(__cdecl*)(const wchar_t*, const void*, hostfxr_handle*);
using hostfxr_get_runtime_delegate_fn = int32_t(__cdecl*)(hostfxr_handle, int32_t, void**);
using hostfxr_close_fn = int32_t(__cdecl*)(hostfxr_handle);
using load_assembly_and_get_function_pointer_fn = int32_t(__cdecl*)(
    const wchar_t*,
    const wchar_t*,
    const wchar_t*,
    const wchar_t*,
    void*,
    void**);
using unmanaged_probe_fn = int32_t(__cdecl*)(uint8_t*, int32_t);

constexpr int32_t hdt_load_assembly_and_get_function_pointer = 5;
const wchar_t* const unmanagedCallersOnlyMethod = reinterpret_cast<const wchar_t*>(-1);

std::mutex g_loaderMutex;
unmanaged_probe_fn g_cachedProbe = nullptr;
std::wstring g_cachedAssemblyPath;
std::wstring g_cachedTypeName;
std::wstring g_cachedMethodName;

std::string TrimAscii(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::string ConfigValue(const std::wstring& baseDir, const std::string& key, const std::string& fallback) {
    const auto config = warden::ReadTextFile(warden::JoinPath(warden::JoinPath(baseDir, L"ScumNeDjin"), L"nedjin.ini"));
    const auto needle = key + "=";
    auto pos = config.find(needle);
    if (pos == std::string::npos) return fallback;
    pos += needle.size();
    auto end = config.find_first_of("\r\n", pos);
    auto value = config.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    value = TrimAscii(value);
    return value.empty() ? fallback : value;
}

bool ConfigBool(const std::wstring& baseDir, const std::string& key, bool fallback) {
    auto value = ConfigValue(baseDir, key, fallback ? "true" : "false");
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool IsAbsolutePath(const std::wstring& path) {
    return path.rfind(L"\\\\", 0) == 0 ||
        path.rfind(L"/", 0) == 0 ||
        (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/'));
}

std::wstring ResolveConfiguredPath(const std::wstring& baseDir, const std::string& value, const std::wstring& fallbackRelative) {
    auto configured = warden::Utf8ToWide(value);
    if (configured.empty()) {
        configured = fallbackRelative;
    }
    return IsAbsolutePath(configured) ? configured : warden::JoinPath(baseDir, configured);
}

std::wstring RuntimeRoot(const std::wstring& baseDir) {
    return ResolveConfiguredPath(baseDir, ConfigValue(baseDir, "runtime_data_dir", R"(..\..\Saved\ScumNeDjin)"), L"..\\..\\Saved\\ScumNeDjin");
}

std::wstring StatusPath(const std::wstring& baseDir) {
    return warden::JoinPath(warden::JoinPath(RuntimeRoot(baseDir), L"state"), L"managed-loader-status.json");
}

std::wstring BridgeStatusPath(const std::wstring& baseDir) {
    return warden::JoinPath(warden::JoinPath(baseDir, L"nedjin_bridge"), L"managed_loader.json");
}

void WriteStatus(const std::wstring& baseDir, const std::string& status, const std::string& message, const std::string& extraJson = "") {
    std::ostringstream ss;
    ss << "{\"utc\":\"" << warden::UtcIsoNow()
       << "\",\"enabled\":" << (ConfigBool(baseDir, "managed_loader_enabled", false) ? "true" : "false")
       << ",\"status\":\"" << warden::JsonEscape(status)
       << "\",\"message\":\"" << warden::JsonEscape(message) << "\"";
    if (!extraJson.empty()) {
        ss << "," << extraJson;
    }
    ss << "}";
    const auto json = ss.str();
    warden::WriteTextFile(StatusPath(baseDir), json);
    warden::WriteTextFile(BridgeStatusPath(baseDir), json);
}

bool TryGetProc(HMODULE module, const char* name, FARPROC& proc, std::string& error) {
    proc = GetProcAddress(module, name);
    if (!proc) {
        error = std::string("missing export: ") + name;
        return false;
    }
    return true;
}

std::wstring FindLatestHostFxr() {
    const fs::path root = L"C:\\Program Files\\dotnet\\host\\fxr";
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        return {};
    }

    std::vector<fs::path> versions;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!ec && entry.is_directory()) {
            versions.push_back(entry.path());
        }
    }
    if (versions.empty()) {
        return {};
    }

    std::sort(versions.begin(), versions.end(), [](const fs::path& left, const fs::path& right) {
        return left.filename().wstring() < right.filename().wstring();
    });

    const auto candidate = versions.back() / L"hostfxr.dll";
    return warden::FileExists(candidate.wstring()) ? candidate.wstring() : L"";
}

std::wstring HostFxrPath(const std::wstring& baseDir) {
    auto configured = ConfigValue(baseDir, "managed_loader_hostfxr_path", "");
    if (!configured.empty()) {
        return ResolveConfiguredPath(baseDir, configured, L"");
    }
    return FindLatestHostFxr();
}

DWORD WINAPI ManagedLoaderProbeThread(LPVOID param) {
    std::unique_ptr<std::wstring> baseDir(static_cast<std::wstring*>(param));
    if (!baseDir) {
        return 0;
    }

    try {
        warden::RunManagedLoaderProbe(*baseDir);
    }
    catch (...) {
        WriteStatus(*baseDir, "error", "managed loader probe threw an unhandled native exception",
            "\"stage\":\"native-exception\"");
    }

    return 0;
}

}

namespace warden {

void RunManagedLoaderProbeAsync(const std::wstring& baseDir) {
    std::wstring* threadBaseDir = nullptr;
    try {
        threadBaseDir = new std::wstring(baseDir);
    }
    catch (...) {
        WriteStatus(baseDir, "error", "managed loader async allocation failed",
            "\"stage\":\"async-start\"");
        return;
    }

    HANDLE thread = CreateThread(nullptr, 0, ManagedLoaderProbeThread, threadBaseDir, 0, nullptr);
    if (!thread) {
        delete threadBaseDir;
        WriteStatus(baseDir, "error", "managed loader async thread creation failed",
            "\"stage\":\"async-start\"");
        return;
    }

    CloseHandle(thread);
}

void RunManagedLoaderProbe(const std::wstring& baseDir) {
    if (!ConfigBool(baseDir, "managed_loader_enabled", false)) {
        WriteStatus(baseDir, "skipped", "managed loader disabled by nedjin.ini");
        return;
    }

    if (!ConfigBool(baseDir, "managed_loader_allow_in_process_probe", false)) {
        WriteStatus(baseDir, "blocked", "managed loader is enabled but in-process probe is blocked by safety flag",
            "\"stage\":\"safety-gate\",\"requiredFlag\":\"managed_loader_allow_in_process_probe\"");
        return;
    }

    const auto hostfxrPath = HostFxrPath(baseDir);
    const auto runtimeConfigPath = ResolveConfiguredPath(
        baseDir,
        ConfigValue(baseDir, "managed_loader_runtime_config", R"(ScumNeDjin\managed\ScumManagedProbe.runtimeconfig.json)"),
        L"ScumNeDjin\\managed\\ScumManagedProbe.runtimeconfig.json");
    const auto assemblyPath = ResolveConfiguredPath(
        baseDir,
        ConfigValue(baseDir, "managed_loader_assembly", R"(ScumNeDjin\managed\ScumManagedProbe.dll)"),
        L"ScumNeDjin\\managed\\ScumManagedProbe.dll");
    const auto typeName = Utf8ToWide(ConfigValue(baseDir, "managed_loader_type", "ScumManagedProbe.EntryPoints, ScumManagedProbe"));
    const auto methodName = Utf8ToWide(ConfigValue(baseDir, "managed_loader_method", "Probe"));
    const auto markerDir = ResolveConfiguredPath(
        baseDir,
        ConfigValue(baseDir, "managed_loader_marker_dir", R"(..\..\Saved\ScumNeDjin\state)"),
        L"..\\..\\Saved\\ScumNeDjin\\state");

    WriteStatus(baseDir, "starting", "managed loader in-process probe starting",
        "\"stage\":\"starting\",\"runtimeConfig\":\"" + JsonEscape(WideToUtf8(runtimeConfigPath)) +
        "\",\"assembly\":\"" + JsonEscape(WideToUtf8(assemblyPath)) + "\"");

    if (hostfxrPath.empty() || !FileExists(hostfxrPath)) {
        WriteStatus(baseDir, "error", "hostfxr.dll not found", "\"stage\":\"hostfxr-path\"");
        return;
    }
    if (!FileExists(runtimeConfigPath) || !FileExists(assemblyPath)) {
        WriteStatus(baseDir, "error", "managed runtimeconfig or assembly is missing",
            "\"stage\":\"managed-files\",\"runtimeConfig\":\"" + JsonEscape(WideToUtf8(runtimeConfigPath)) +
            "\",\"assembly\":\"" + JsonEscape(WideToUtf8(assemblyPath)) + "\"");
        return;
    }

    EnsureDirectory(markerDir);
    auto managedRuntimeDir = fs::path(assemblyPath).parent_path().wstring();
    auto probeArgsUtf8 = WideToUtf8(markerDir) + "\n" + WideToUtf8(managedRuntimeDir);
    auto markerDirBytes = std::vector<uint8_t>(probeArgsUtf8.begin(), probeArgsUtf8.end());

    unmanaged_probe_fn probe = nullptr;
    bool usedCachedProbe = false;
    {
        std::lock_guard<std::mutex> guard(g_loaderMutex);
        if (g_cachedProbe) {
            if (g_cachedAssemblyPath != assemblyPath || g_cachedTypeName != typeName || g_cachedMethodName != methodName) {
                WriteStatus(baseDir, "blocked", "managed runtime already initialized for a different entrypoint; restart SCUMServer to change managed payload",
                    "\"stage\":\"entrypoint-change-blocked\",\"cachedAssembly\":\"" + JsonEscape(WideToUtf8(g_cachedAssemblyPath)) +
                    "\",\"requestedAssembly\":\"" + JsonEscape(WideToUtf8(assemblyPath)) + "\"");
                return;
            }

            probe = g_cachedProbe;
            usedCachedProbe = true;
        }
        else {
            HMODULE hostfxr = LoadLibraryW(hostfxrPath.c_str());
            if (!hostfxr) {
                WriteStatus(baseDir, "error", "LoadLibraryW(hostfxr) failed", "\"stage\":\"load-hostfxr\"");
                return;
            }

            FARPROC initProc{};
            FARPROC delegateProc{};
            FARPROC closeProc{};
            std::string procError;
            if (!TryGetProc(hostfxr, "hostfxr_initialize_for_runtime_config", initProc, procError) ||
                !TryGetProc(hostfxr, "hostfxr_get_runtime_delegate", delegateProc, procError) ||
                !TryGetProc(hostfxr, "hostfxr_close", closeProc, procError)) {
                WriteStatus(baseDir, "error", procError, "\"stage\":\"exports\"");
                return;
            }

            const auto init = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(initProc);
            const auto getDelegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(delegateProc);
            const auto close = reinterpret_cast<hostfxr_close_fn>(closeProc);

            hostfxr_handle context{};
            const int32_t initRc = init(runtimeConfigPath.c_str(), nullptr, &context);
            if (initRc != 0 || !context) {
                WriteStatus(baseDir, "error", "hostfxr_initialize_for_runtime_config failed",
                    "\"stage\":\"initialize\",\"hresult\":" + std::to_string(initRc));
                return;
            }

            void* loadAssemblyRaw{};
            const int32_t delegateRc = getDelegate(context, hdt_load_assembly_and_get_function_pointer, &loadAssemblyRaw);
            close(context);
            if (delegateRc != 0 || !loadAssemblyRaw) {
                WriteStatus(baseDir, "error", "hostfxr_get_runtime_delegate failed",
                    "\"stage\":\"delegate\",\"hresult\":" + std::to_string(delegateRc));
                return;
            }

            const auto loadAssembly = reinterpret_cast<load_assembly_and_get_function_pointer_fn>(loadAssemblyRaw);
            void* probeRaw{};
            const int32_t loadRc = loadAssembly(
                assemblyPath.c_str(),
                typeName.c_str(),
                methodName.c_str(),
                unmanagedCallersOnlyMethod,
                nullptr,
                &probeRaw);
            if (loadRc != 0 || !probeRaw) {
                WriteStatus(baseDir, "error", "load_assembly_and_get_function_pointer failed",
                    "\"stage\":\"load-managed-method\",\"hresult\":" + std::to_string(loadRc));
                return;
            }

            probe = reinterpret_cast<unmanaged_probe_fn>(probeRaw);
            g_cachedProbe = probe;
            g_cachedAssemblyPath = assemblyPath;
            g_cachedTypeName = typeName;
            g_cachedMethodName = methodName;
        }
    }

    const int32_t result = probe(markerDirBytes.data(), static_cast<int32_t>(markerDirBytes.size()));
    if (result != 20260702) {
        WriteStatus(baseDir, "error", "managed probe returned unexpected value",
            "\"stage\":\"probe\",\"result\":" + std::to_string(result));
        return;
    }

    WriteStatus(baseDir, "ok", "managed loader probe completed",
        "\"stage\":\"complete\",\"result\":" + std::to_string(result) +
        ",\"cachedEntrypoint\":" + (usedCachedProbe ? "true" : "false") +
        ",\"hostfxr\":\"" + JsonEscape(WideToUtf8(hostfxrPath)) +
        "\",\"assembly\":\"" + JsonEscape(WideToUtf8(assemblyPath)) + "\"");
}

}
