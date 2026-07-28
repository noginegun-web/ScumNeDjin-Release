#include "http_server.h"
#include "managed_loader.h"
#include "path_utils.h"
#include "real_version.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace {

std::string ConfigValue(const std::wstring& baseDir, const std::string& key, const std::string& fallback) {
    const auto config = nedjin::ReadTextFile(nedjin::JoinPath(nedjin::JoinPath(baseDir, L"ScumNeDjin"), L"nedjin.ini"));
    const auto needle = key + "=";
    auto pos = config.find(needle);
    if (pos == std::string::npos) return fallback;
    pos += needle.size();
    auto end = config.find_first_of("\r\n", pos);
    auto value = config.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value.empty() ? fallback : value;
}

bool ConfigBool(const std::wstring& baseDir, const std::string& key, bool fallback) {
    auto value = ConfigValue(baseDir, key, fallback ? "true" : "false");
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

int ConfigInt(const std::wstring& baseDir, const std::string& key, int fallback, int minValue, int maxValue) {
    const auto value = ConfigValue(baseDir, key, std::to_string(fallback));
    const int parsed = std::atoi(value.c_str());
    if (parsed < minValue) return minValue;
    if (parsed > maxValue) return maxValue;
    return parsed;
}

std::wstring ResolveConfiguredUe4ssPath(const std::wstring& baseDir) {
    auto configured = nedjin::Utf8ToWide(ConfigValue(baseDir, "ue4ss_path", "UE4SS.dll"));
    if (configured.empty()) {
        configured = L"UE4SS.dll";
    }
    const bool absolute =
        configured.rfind(L"\\\\", 0) == 0 ||
        configured.rfind(L"/", 0) == 0 ||
        (configured.size() >= 3 && configured[1] == L':' && (configured[2] == L'\\' || configured[2] == L'/'));
    return absolute ? configured : nedjin::JoinPath(baseDir, configured);
}

void Bootstrap() {
    const auto baseDir = nedjin::ModuleDirectory();
    nedjin::LoadRealVersion();

    nedjin::EnsureDirectory(nedjin::JoinPath(baseDir, L"nedjin_bridge"));
    nedjin::WriteTextFile(nedjin::JoinPath(nedjin::JoinPath(baseDir, L"nedjin_bridge"), L"native_loader.json"),
        std::string("{\"utc\":\"") + nedjin::UtcIsoNow() + "\",\"loader\":\"ScumNeDjin version proxy\"}");

    nedjin::StartHttpServer(baseDir);

    const bool managedAfterUe4ss = ConfigBool(baseDir, "managed_loader_after_ue4ss", true);
    if (!managedAfterUe4ss) {
        nedjin::RunManagedLoaderProbeAsync(baseDir);
    }

    const auto ue4ssPath = ResolveConfiguredUe4ssPath(baseDir);
    const bool loadUe4ss = ConfigBool(baseDir, "load_ue4ss", false);
    if (loadUe4ss && nedjin::FileExists(ue4ssPath)) {
        const int delaySeconds = ConfigInt(baseDir, "ue4ss_delay_seconds", 90, 0, 600);
        if (delaySeconds > 0) {
            Sleep(static_cast<DWORD>(delaySeconds) * 1000U);
        }
        auto mod = LoadLibraryW(ue4ssPath.c_str());
        nedjin::WriteTextFile(nedjin::JoinPath(nedjin::JoinPath(baseDir, L"nedjin_bridge"), L"ue4ss_load.json"),
            std::string("{\"utc\":\"") + nedjin::UtcIsoNow() + "\",\"loaded\":" + (mod ? "true" : "false") +
            ",\"delaySeconds\":" + std::to_string(delaySeconds) +
            ",\"path\":\"" + nedjin::JsonEscape(nedjin::WideToUtf8(ue4ssPath)) + "\"}");
    } else {
        const auto reason = loadUe4ss ? "UE4SS DLL is missing" : "disabled by nedjin.ini";
        nedjin::WriteTextFile(nedjin::JoinPath(nedjin::JoinPath(baseDir, L"nedjin_bridge"), L"ue4ss_load.json"),
            std::string("{\"utc\":\"") + nedjin::UtcIsoNow() +
            "\",\"loaded\":false,\"skipped\":true,\"reason\":\"" + reason +
            "\",\"path\":\"" + nedjin::JsonEscape(nedjin::WideToUtf8(ue4ssPath)) + "\"}");
    }

    if (managedAfterUe4ss) {
        nedjin::RunManagedLoaderProbeAsync(baseDir);
    }
}

DWORD WINAPI BootstrapThread(LPVOID) {
    Bootstrap();
    return 0;
}

}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, BootstrapThread, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
