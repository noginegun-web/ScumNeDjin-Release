#include "managed_host_api.h"
#include "path_utils.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string TrimAscii(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::string ConfigValue(const std::wstring& baseDir, const std::string& key, const std::string& fallback) {
    const auto config = nedjin::ReadTextFile(nedjin::JoinPath(nedjin::JoinPath(baseDir, L"ScumNeDjin"), L"nedjin.ini"));
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

std::wstring ResolveConfiguredPath(const std::wstring& baseDir, const std::string& value, const std::wstring& fallbackRelative) {
    auto configured = nedjin::Utf8ToWide(value.empty() ? nedjin::WideToUtf8(fallbackRelative) : value);
    const bool absolute =
        configured.rfind(L"\\\\", 0) == 0 ||
        configured.rfind(L"/", 0) == 0 ||
        (configured.size() >= 3 && configured[1] == L':' && (configured[2] == L'\\' || configured[2] == L'/'));
    return absolute ? configured : nedjin::JoinPath(baseDir, configured);
}

std::wstring RuntimeRoot(const std::wstring& baseDir) {
    return ResolveConfiguredPath(baseDir, ConfigValue(baseDir, "runtime_data_dir", R"(..\..\Saved\ScumNeDjin)"), L"..\\..\\Saved\\ScumNeDjin");
}

int CopyJsonResult(const std::string& json, char* buffer, int capacity) {
    const int required = static_cast<int>(json.size());
    if (!buffer || capacity <= 0) {
        return required;
    }

    const int copyCount = std::min(required, capacity - 1);
    if (copyCount > 0) {
        memcpy(buffer, json.data(), static_cast<size_t>(copyCount));
    }
    buffer[copyCount] = '\0';
    return required;
}

bool IsSafeStateFileName(const std::string& name) {
    if (name.empty() || name.size() > 160) {
        return false;
    }
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos || name.find("..") != std::string::npos) {
        return false;
    }
    return name.ends_with(".json") &&
        std::all_of(name.begin(), name.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
        });
}

bool IsSafeModuleName(const std::string& name) {
    if (name.empty() || name.size() > 96) {
        return false;
    }
    if (name.find("..") != std::string::npos) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
    });
}

bool IsSafeCommandType(const std::string& name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    if (name.find("..") != std::string::npos) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '-' || ch == '_';
    });
}

bool IsAllowedManagedCommandType(const std::string& name) {
    return name == "host-api-smoke";
}

bool IsSafeIdempotencyKey(const std::string& key) {
    if (key.empty() || key.size() > 128) {
        return false;
    }
    if (key.find("..") != std::string::npos) {
        return false;
    }
    return std::all_of(key.begin(), key.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
    });
}

std::string NormalizeModuleNameForFile(std::string name) {
    for (auto& ch : name) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_')) {
            ch = '_';
        }
    }
    return name;
}

bool LooksLikeJsonObject(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    const auto last = text.find_last_not_of(" \t\r\n");
    return first != std::string::npos && last != std::string::npos && text[first] == '{' && text[last] == '}';
}

std::string RemoveJsonWhitespaceOutsideStrings(const std::string& text) {
    std::string compact;
    compact.reserve(text.size());
    bool inString = false;
    bool escape = false;
    for (char ch : text) {
        if (inString) {
            compact.push_back(ch);
            if (escape) {
                escape = false;
            }
            else if (ch == '\\') {
                escape = true;
            }
            else if (ch == '"') {
                inString = false;
            }
            continue;
        }

        if (ch == '"') {
            inString = true;
            compact.push_back(ch);
            continue;
        }

        if (!std::isspace(static_cast<unsigned char>(ch))) {
            compact.push_back(ch);
        }
    }
    return compact;
}

bool ManagedCommandPayloadAllowed(const std::string& commandType, const std::string& json) {
    if (commandType != "host-api-smoke") {
        return false;
    }

    const auto compact = RemoveJsonWhitespaceOutsideStrings(json);
    return compact.find("\"dryRun\":true") != std::string::npos;
}

struct ReflectedBindingAllowlistRow {
    const char* className;
    const char* propertyName;
    const char* declaredOn;
    const char* domain;
};

const ReflectedBindingAllowlistRow* FindReflectedBindingAllowlistRow(const std::string& className, const std::string& propertyName) {
    static constexpr ReflectedBindingAllowlistRow rows[] = {
        {"UInventoryComponent", "_repHasEntries", "UInventoryComponent", "inventory-loot-chests"},
        {"UGridInventoryComponent", "_repHasEntries", "UInventoryComponent", "inventory-loot-chests"},
        {"UChestInventoryComponent", "_repHasEntries", "UInventoryComponent", "inventory-loot-chests"},
        {"AChestItem", "_inventory", "AChestItem", "inventory-loot-chests"},
        {"AChestItem", "_nameableItemComponent", "AChestItem", "inventory-loot-chests"},
        {"UNameableItemComponent", "_nameData", "UNameableItemComponent", "inventory-loot-chests"},
        {"UBaseBuildingItemComponent", "_itemClass", "UBaseBuildingItemComponent", "inventory-loot-chests"},
        {"UBaseBuildingItemComponent", "_shouldDestroyAssociatedItemWhenMovedOutOfFlag", "UBaseBuildingItemComponent", "inventory-loot-chests"},
        {"UBaseBuildingItemComponent", "_shouldDestroyElementWhenMovedOutOfFlag", "UBaseBuildingItemComponent", "inventory-loot-chests"},
        {"UBP_Base_Improvised_Wooden_Chest_C", "_itemClass", "UBaseBuildingItemComponent", "inventory-loot-chests"},
    };

    for (const auto& row : rows) {
        if (className == row.className && propertyName == row.propertyName) {
            return &row;
        }
    }
    return nullptr;
}

std::string FileStatusJson(const std::wstring& path) {
    std::error_code ec;
    const bool exists = fs::is_regular_file(path, ec);
    uintmax_t size = 0;
    if (exists) {
        ec.clear();
        size = fs::file_size(path, ec);
        if (ec) {
            size = 0;
        }
    }

    std::ostringstream ss;
    ss << "{\"exists\":" << (exists ? "true" : "false")
       << ",\"sizeBytes\":" << size << "}";
    return ss.str();
}

std::string LoadedModuleJson(const wchar_t* moduleName) {
    HMODULE module = GetModuleHandleW(moduleName);
    std::wstring path;
    if (module) {
        wchar_t buffer[MAX_PATH]{};
        const DWORD copied = GetModuleFileNameW(module, buffer, MAX_PATH);
        if (copied > 0) {
            path.assign(buffer, copied);
        }
    }

    std::ostringstream ss;
    ss << "{\"loaded\":" << (module ? "true" : "false")
       << ",\"path\":\"" << nedjin::JsonEscape(nedjin::WideToUtf8(path)) << "\"}";
    return ss.str();
}

std::string ModulePath(HMODULE module) {
    if (!module) {
        return "";
    }

    wchar_t buffer[MAX_PATH]{};
    const DWORD copied = GetModuleFileNameW(module, buffer, MAX_PATH);
    return copied > 0 ? nedjin::WideToUtf8(std::wstring(buffer, copied)) : "";
}

std::string PeExportSummaryJson(HMODULE module) {
    if (!module) {
        return "{\"available\":false,\"error\":\"module-not-loaded\",\"exportCount\":0,\"sampleNames\":[]}";
    }

    auto* base = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return "{\"available\":false,\"error\":\"bad-dos-signature\",\"exportCount\":0,\"sampleNames\":[]}";
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return "{\"available\":false,\"error\":\"bad-nt-signature\",\"exportCount\":0,\"sampleNames\":[]}";
    }

    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (directory.VirtualAddress == 0 || directory.Size == 0) {
        return "{\"available\":true,\"exportCount\":0,\"sampleNames\":[]}";
    }

    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const DWORD nameCount = exports->NumberOfNames;
    std::vector<std::string> sampleNames;
    const DWORD sampleCount = std::min<DWORD>(nameCount, 64);
    sampleNames.reserve(sampleCount);
    for (DWORD i = 0; i < sampleCount; ++i) {
        const char* name = reinterpret_cast<const char*>(base + names[i]);
        if (name && *name) {
            sampleNames.emplace_back(name);
        }
    }

    std::ostringstream ss;
    ss << "{\"available\":true"
       << ",\"exportCount\":" << nameCount
       << ",\"ordinalBase\":" << exports->Base
       << ",\"sampleNames\":[";
    for (size_t i = 0; i < sampleNames.size(); ++i) {
        if (i > 0) {
            ss << ",";
        }
        ss << "\"" << nedjin::JsonEscape(sampleNames[i]) << "\"";
    }
    ss << "]}";
    return ss.str();
}

uintmax_t DirectoryFileCount(const std::wstring& path) {
    std::error_code ec;
    if (!fs::is_directory(path, ec)) {
        return 0;
    }

    uintmax_t count = 0;
    for (const auto& entry : fs::directory_iterator(path, ec)) {
        if (ec) {
            return count;
        }
        std::error_code fileEc;
        if (entry.is_regular_file(fileEc) && !fileEc) {
            ++count;
        }
    }
    return count;
}

std::string ReadJsonObjectOrNull(const std::wstring& path, size_t maxBytes) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return "null";
    }
    ec.clear();
    const auto size = fs::file_size(path, ec);
    if (ec || size == 0 || size > maxBytes) {
        return "null";
    }
    const auto text = nedjin::ReadTextFile(path);
    return LooksLikeJsonObject(text) ? text : "null";
}

std::wstring ManagedStateRoot(const std::wstring& baseDir) {
    return nedjin::JoinPath(nedjin::JoinPath(RuntimeRoot(baseDir), L"state"), L"managed");
}

std::string GeneratedIdempotencyKey() {
    std::ostringstream ss;
    ss << "auto-" << GetCurrentProcessId() << "-" << GetTickCount64();
    return ss.str();
}

}

extern "C" {

__declspec(dllexport) int __stdcall ScumNedjin_GetHostInfoJson(char* buffer, int capacity) {
    const auto baseDir = nedjin::ModuleDirectory();
    const auto runtimeRoot = RuntimeRoot(baseDir);
    const auto stateRoot = nedjin::JoinPath(runtimeRoot, L"state");
    const auto managedDir = nedjin::JoinPath(nedjin::JoinPath(baseDir, L"ScumNeDjin"), L"managed");

    std::ostringstream ss;
    ss << "{\"ok\":true"
       << ",\"api\":\"ScumNedjin.ManagedHostApi\""
       << ",\"apiVersion\":2"
       << ",\"allowedCommandTypes\":[\"host-api-smoke\"]"
       << ",\"gameCommandDispatchEnabled\":false"
       << ",\"ueMutationDispatchEnabled\":false"
       << ",\"scumDbWriteEnabled\":false"
       << ",\"utc\":\"" << nedjin::UtcIsoNow() << "\""
       << ",\"baseDir\":\"" << nedjin::JsonEscape(nedjin::WideToUtf8(baseDir)) << "\""
       << ",\"runtimeRoot\":\"" << nedjin::JsonEscape(nedjin::WideToUtf8(runtimeRoot)) << "\""
       << ",\"stateRoot\":\"" << nedjin::JsonEscape(nedjin::WideToUtf8(stateRoot)) << "\""
       << ",\"managedDir\":\"" << nedjin::JsonEscape(nedjin::WideToUtf8(managedDir)) << "\""
       << ",\"managedLoaderEnabled\":" << (ConfigBool(baseDir, "managed_loader_enabled", false) ? "true" : "false")
       << ",\"managedLoaderAllowInProcessProbe\":" << (ConfigBool(baseDir, "managed_loader_allow_in_process_probe", false) ? "true" : "false")
       << ",\"statusFiles\":{"
       << "\"nativeHttp\":" << FileStatusJson(nedjin::JoinPath(stateRoot, L"native_http.json"))
       << ",\"managedLoader\":" << FileStatusJson(nedjin::JoinPath(stateRoot, L"managed-loader-status.json"))
       << ",\"bridgeHeartbeat\":" << FileStatusJson(nedjin::JoinPath(nedjin::JoinPath(baseDir, L"nedjin_bridge"), L"heartbeat.json"))
       << "}}";

    return CopyJsonResult(ss.str(), buffer, capacity);
}

__declspec(dllexport) int __stdcall ScumNedjin_GetRuntimeSnapshotJson(char* buffer, int capacity) {
    const auto baseDir = nedjin::ModuleDirectory();
    const auto runtimeRoot = RuntimeRoot(baseDir);
    const auto stateRoot = nedjin::JoinPath(runtimeRoot, L"state");
    const auto managedRoot = ManagedStateRoot(baseDir);
    const auto commandRoot = nedjin::JoinPath(managedRoot, L"commands");
    const auto bridgeRoot = nedjin::JoinPath(baseDir, L"nedjin_bridge");

    const auto pendingRoot = nedjin::JoinPath(commandRoot, L"pending");
    const auto doneRoot = nedjin::JoinPath(commandRoot, L"done");
    const auto failedRoot = nedjin::JoinPath(commandRoot, L"failed");

    std::ostringstream ss;
    ss << "{\"ok\":true"
       << ",\"schema\":\"scum-nedjin-managed-runtime-snapshot-v1\""
       << ",\"utc\":\"" << nedjin::UtcIsoNow() << "\""
       << ",\"baseDir\":\"" << nedjin::JsonEscape(nedjin::WideToUtf8(baseDir)) << "\""
       << ",\"runtimeRoot\":\"" << nedjin::JsonEscape(nedjin::WideToUtf8(runtimeRoot)) << "\""
       << ",\"stateRoot\":\"" << nedjin::JsonEscape(nedjin::WideToUtf8(stateRoot)) << "\""
       << ",\"bridge\":{"
       << "\"heartbeat\":" << ReadJsonObjectOrNull(nedjin::JoinPath(bridgeRoot, L"heartbeat.json"), 64 * 1024)
       << ",\"result\":" << FileStatusJson(nedjin::JoinPath(bridgeRoot, L"result.json"))
       << ",\"command\":" << FileStatusJson(nedjin::JoinPath(bridgeRoot, L"cmd.json"))
       << "}"
       << ",\"nativeHttp\":" << ReadJsonObjectOrNull(nedjin::JoinPath(stateRoot, L"native_http.json"), 64 * 1024)
       << ",\"managedLoader\":" << ReadJsonObjectOrNull(nedjin::JoinPath(stateRoot, L"managed-loader-status.json"), 64 * 1024)
       << ",\"managedQueue\":{"
       << "\"pending\":" << DirectoryFileCount(pendingRoot)
       << ",\"done\":" << DirectoryFileCount(doneRoot)
       << ",\"failed\":" << DirectoryFileCount(failedRoot)
       << ",\"dispatcherEnabled\":false"
       << "}"
       << "}";

    return CopyJsonResult(ss.str(), buffer, capacity);
}

__declspec(dllexport) int __stdcall ScumNedjin_GetReadOnlyRuntimeProbeJson(char* buffer, int capacity) {
    const auto baseDir = nedjin::ModuleDirectory();
    const auto runtimeRoot = RuntimeRoot(baseDir);
    const auto stateRoot = nedjin::JoinPath(runtimeRoot, L"state");
    const auto managedDir = nedjin::JoinPath(nedjin::JoinPath(baseDir, L"ScumNeDjin"), L"managed");
    const auto bridgeRoot = nedjin::JoinPath(baseDir, L"nedjin_bridge");

    wchar_t exeBuffer[MAX_PATH]{};
    const DWORD exeCopied = GetModuleFileNameW(nullptr, exeBuffer, MAX_PATH);
    const std::wstring exePath = exeCopied > 0 ? std::wstring(exeBuffer, exeCopied) : L"";

    std::ostringstream ss;
    ss << "{\"ok\":true"
       << ",\"schema\":\"scum-nedjin-managed-readonly-runtime-probe-v1\""
       << ",\"utc\":\"" << nedjin::UtcIsoNow() << "\""
       << ",\"policy\":{"
       << "\"readOnly\":true"
       << ",\"staticDumpSelected\":true"
       << ",\"runtimeObjectScan\":false"
       << ",\"ueMutation\":false"
       << ",\"scumDbWrite\":false"
       << ",\"commandDispatch\":false"
       << "}"
       << ",\"process\":{"
       << "\"id\":" << GetCurrentProcessId()
       << ",\"exe\":\"" << nedjin::JsonEscape(nedjin::WideToUtf8(exePath)) << "\""
       << "}"
       << ",\"modules\":{"
       << "\"version\":" << LoadedModuleJson(L"version.dll")
       << ",\"ue4ss\":" << LoadedModuleJson(L"UE4SS.dll")
       << ",\"xinput\":" << LoadedModuleJson(L"xinput1_3.dll")
       << ",\"hostfxr\":" << LoadedModuleJson(L"hostfxr.dll")
       << ",\"coreclr\":" << LoadedModuleJson(L"coreclr.dll")
       << "}"
       << ",\"paths\":{"
       << "\"baseDir\":\"" << nedjin::JsonEscape(nedjin::WideToUtf8(baseDir)) << "\""
       << ",\"runtimeRoot\":\"" << nedjin::JsonEscape(nedjin::WideToUtf8(runtimeRoot)) << "\""
       << ",\"stateRoot\":\"" << nedjin::JsonEscape(nedjin::WideToUtf8(stateRoot)) << "\""
       << ",\"managedDir\":\"" << nedjin::JsonEscape(nedjin::WideToUtf8(managedDir)) << "\""
       << "}"
       << ",\"files\":{"
       << "\"bridgeHeartbeat\":" << FileStatusJson(nedjin::JoinPath(bridgeRoot, L"heartbeat.json"))
       << ",\"nativeHttp\":" << FileStatusJson(nedjin::JoinPath(stateRoot, L"native_http.json"))
       << ",\"managedLoader\":" << FileStatusJson(nedjin::JoinPath(stateRoot, L"managed-loader-status.json"))
       << ",\"managedModules\":" << FileStatusJson(nedjin::JoinPath(managedDir, L"managed-modules.json"))
       << ",\"sdkCatalog\":" << FileStatusJson(nedjin::JoinPath(managedDir, L"scum-managed-sdk-catalog-2026-07-03.json"))
       << "}}";

    return CopyJsonResult(ss.str(), buffer, capacity);
}

__declspec(dllexport) int __stdcall ScumNedjin_GetUe4ssSurfaceJson(char* buffer, int capacity) {
    const auto baseDir = nedjin::ModuleDirectory();
    const auto bridgeRoot = nedjin::JoinPath(baseDir, L"nedjin_bridge");
    const HMODULE ue4ss = GetModuleHandleW(L"UE4SS.dll");
    const HMODULE version = GetModuleHandleW(L"version.dll");

    std::ostringstream ss;
    ss << "{\"ok\":true"
       << ",\"schema\":\"scum-nedjin-managed-ue4ss-surface-v1\""
       << ",\"utc\":\"" << nedjin::UtcIsoNow() << "\""
       << ",\"policy\":{"
       << "\"readOnly\":true"
       << ",\"moduleHeaderOnly\":true"
       << ",\"runtimeObjectScan\":false"
       << ",\"ueMutation\":false"
       << ",\"scumDbWrite\":false"
       << ",\"commandDispatch\":false"
       << ",\"hookInstall\":false"
       << ",\"allowedOperations\":[\"module-loaded-state\",\"pe-export-summary\",\"diagnostic-file-status\"]"
       << ",\"blockedOperations\":[\"runtime-object-scan\",\"find-all\",\"load-asset\",\"hook-install\",\"ue-mutation\",\"command-dispatch\",\"scum-db-write\"]"
       << "}";

    std::ostringstream result;
    result << ss.str()
       << ",\"modules\":{"
       << "\"version\":{\"loaded\":" << (version ? "true" : "false")
       << ",\"path\":\"" << nedjin::JsonEscape(ModulePath(version)) << "\"}"
       << ",\"ue4ss\":{\"loaded\":" << (ue4ss ? "true" : "false")
       << ",\"path\":\"" << nedjin::JsonEscape(ModulePath(ue4ss)) << "\"}"
       << "}"
       << ",\"ue4ssExports\":" << PeExportSummaryJson(ue4ss)
       << ",\"files\":{"
       << "\"ue4ssLoad\":" << ReadJsonObjectOrNull(nedjin::JoinPath(bridgeRoot, L"ue4ss_load.json"), 64 * 1024)
       << ",\"ue4ssLog\":" << FileStatusJson(nedjin::JoinPath(baseDir, L"UE4SS.log"))
       << ",\"bridgeHeartbeat\":" << FileStatusJson(nedjin::JoinPath(bridgeRoot, L"heartbeat.json"))
       << "}}";

    return CopyJsonResult(result.str(), buffer, capacity);
}

__declspec(dllexport) int __stdcall ScumNedjin_GetReflectedBindingDryRunJson(const char* classNameUtf8, const char* propertyNameUtf8, char* buffer, int capacity) {
    const std::string className = classNameUtf8 ? classNameUtf8 : "";
    const std::string propertyName = propertyNameUtf8 ? propertyNameUtf8 : "";
    const auto* row = FindReflectedBindingAllowlistRow(className, propertyName);
    const HMODULE ue4ss = GetModuleHandleW(L"UE4SS.dll");

    std::ostringstream ss;
    ss << "{\"ok\":" << (row ? "true" : "false")
       << ",\"schema\":\"scum-nedjin-managed-reflected-binding-dryrun-v1\""
       << ",\"utc\":\"" << nedjin::UtcIsoNow() << "\""
       << ",\"requested\":{"
       << "\"className\":\"" << nedjin::JsonEscape(className) << "\""
       << ",\"propertyName\":\"" << nedjin::JsonEscape(propertyName) << "\""
       << "}"
       << ",\"policy\":{"
       << "\"dryRun\":true"
       << ",\"readOnly\":true"
       << ",\"runtimeObjectScan\":false"
       << ",\"findAll\":false"
       << ",\"loadAsset\":false"
       << ",\"hookInstall\":false"
       << ",\"ueMutation\":false"
       << ",\"commandDispatch\":false"
       << ",\"scumDbWrite\":false"
       << ",\"runtimeDereference\":false"
       << "}"
       << ",\"ue4ssLoaded\":" << (ue4ss ? "true" : "false")
       << ",\"allowlisted\":" << (row ? "true" : "false");
    if (row) {
        ss << ",\"binding\":{"
           << "\"domain\":\"" << row->domain << "\""
           << ",\"className\":\"" << row->className << "\""
           << ",\"propertyName\":\"" << row->propertyName << "\""
           << ",\"declaredOn\":\"" << row->declaredOn << "\""
           << ",\"inherited\":" << (std::string(row->className) == row->declaredOn ? "false" : "true")
           << "}";
    }
    else {
        ss << ",\"error\":\"reflected-binding-not-allowlisted\"";
    }
    ss << "}";

    return CopyJsonResult(ss.str(), buffer, capacity);
}

__declspec(dllexport) int __stdcall ScumNedjin_ReadStateFileJson(const char* fileNameUtf8, char* buffer, int capacity) {
    const std::string fileName = fileNameUtf8 ? fileNameUtf8 : "";
    if (!IsSafeStateFileName(fileName)) {
        return CopyJsonResult("{\"ok\":false,\"error\":\"invalid-state-file-name\"}", buffer, capacity);
    }

    const auto baseDir = nedjin::ModuleDirectory();
    const auto stateRoot = nedjin::JoinPath(RuntimeRoot(baseDir), L"state");
    const auto path = nedjin::JoinPath(stateRoot, nedjin::Utf8ToWide(fileName));
    if (!nedjin::FileExists(path)) {
        return CopyJsonResult("{\"ok\":false,\"error\":\"state-file-not-found\"}", buffer, capacity);
    }

    const auto text = nedjin::ReadTextFile(path);
    if (text.empty()) {
        return CopyJsonResult("{\"ok\":false,\"error\":\"state-file-empty\"}", buffer, capacity);
    }

    return CopyJsonResult(text, buffer, capacity);
}

__declspec(dllexport) int __stdcall ScumNedjin_WriteManagedStatusJson(const char* moduleNameUtf8, const char* jsonUtf8, char* buffer, int capacity) {
    if (!buffer || capacity <= 0) {
        return 4096;
    }

    const std::string moduleName = moduleNameUtf8 ? moduleNameUtf8 : "";
    const std::string json = jsonUtf8 ? jsonUtf8 : "";
    if (!IsSafeModuleName(moduleName)) {
        return CopyJsonResult("{\"ok\":false,\"error\":\"invalid-module-name\"}", buffer, capacity);
    }
    if (json.empty() || json.size() > 256 * 1024 || !LooksLikeJsonObject(json)) {
        return CopyJsonResult("{\"ok\":false,\"error\":\"invalid-json-payload\"}", buffer, capacity);
    }

    const auto baseDir = nedjin::ModuleDirectory();
    const auto managedStateRoot = ManagedStateRoot(baseDir);
    const auto safeModuleName = NormalizeModuleNameForFile(moduleName);
    const auto path = nedjin::JoinPath(managedStateRoot, nedjin::Utf8ToWide(safeModuleName + ".status.json"));
    if (!nedjin::WriteTextFile(path, json)) {
        return CopyJsonResult("{\"ok\":false,\"error\":\"write-failed\"}", buffer, capacity);
    }

    std::ostringstream ss;
    ss << "{\"ok\":true"
       << ",\"module\":\"" << nedjin::JsonEscape(moduleName) << "\""
       << ",\"path\":\"" << nedjin::JsonEscape(nedjin::WideToUtf8(path)) << "\""
       << ",\"sizeBytes\":" << json.size()
       << ",\"utc\":\"" << nedjin::UtcIsoNow() << "\"}";
    return CopyJsonResult(ss.str(), buffer, capacity);
}

__declspec(dllexport) int __stdcall ScumNedjin_AppendManagedEventJson(const char* moduleNameUtf8, const char* jsonUtf8, char* buffer, int capacity) {
    if (!buffer || capacity <= 0) {
        return 4096;
    }

    const std::string moduleName = moduleNameUtf8 ? moduleNameUtf8 : "";
    const std::string json = jsonUtf8 ? jsonUtf8 : "";
    if (!IsSafeModuleName(moduleName)) {
        return CopyJsonResult("{\"ok\":false,\"error\":\"invalid-module-name\"}", buffer, capacity);
    }
    if (json.empty() || json.size() > 64 * 1024 || !LooksLikeJsonObject(json)) {
        return CopyJsonResult("{\"ok\":false,\"error\":\"invalid-json-payload\"}", buffer, capacity);
    }

    const auto baseDir = nedjin::ModuleDirectory();
    const auto managedStateRoot = ManagedStateRoot(baseDir);
    nedjin::EnsureDirectory(managedStateRoot);

    const auto safeModuleName = NormalizeModuleNameForFile(moduleName);
    const auto path = nedjin::JoinPath(managedStateRoot, nedjin::Utf8ToWide(safeModuleName + ".events.jsonl"));
    std::ofstream file(fs::path(path), std::ios::binary | std::ios::app);
    if (!file) {
        return CopyJsonResult("{\"ok\":false,\"error\":\"append-open-failed\"}", buffer, capacity);
    }
    file << "{\"utc\":\"" << nedjin::UtcIsoNow()
         << "\",\"module\":\"" << nedjin::JsonEscape(moduleName)
         << "\",\"event\":" << json << "}\n";
    file.flush();
    if (!file.good()) {
        return CopyJsonResult("{\"ok\":false,\"error\":\"append-write-failed\"}", buffer, capacity);
    }

    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    std::ostringstream ss;
    ss << "{\"ok\":true"
       << ",\"module\":\"" << nedjin::JsonEscape(moduleName) << "\""
       << ",\"path\":\"" << nedjin::JsonEscape(nedjin::WideToUtf8(path)) << "\""
       << ",\"sizeBytes\":" << (ec ? 0 : size)
       << ",\"utc\":\"" << nedjin::UtcIsoNow() << "\"}";
    return CopyJsonResult(ss.str(), buffer, capacity);
}

__declspec(dllexport) int __stdcall ScumNedjin_EnqueueManagedCommandJson(const char* moduleNameUtf8, const char* commandTypeUtf8, const char* idempotencyKeyUtf8, const char* jsonUtf8, char* buffer, int capacity) {
    if (!buffer || capacity <= 0) {
        return 4096;
    }

    const std::string moduleName = moduleNameUtf8 ? moduleNameUtf8 : "";
    const std::string commandType = commandTypeUtf8 ? commandTypeUtf8 : "";
    const std::string requestedKey = idempotencyKeyUtf8 ? idempotencyKeyUtf8 : "";
    const std::string idempotencyKey = requestedKey.empty() ? GeneratedIdempotencyKey() : requestedKey;
    const std::string json = jsonUtf8 ? jsonUtf8 : "";

    if (!IsSafeModuleName(moduleName)) {
        return CopyJsonResult("{\"ok\":false,\"error\":\"invalid-module-name\"}", buffer, capacity);
    }
    if (!IsSafeCommandType(commandType)) {
        return CopyJsonResult("{\"ok\":false,\"error\":\"invalid-command-type\"}", buffer, capacity);
    }
    if (!IsAllowedManagedCommandType(commandType)) {
        return CopyJsonResult("{\"ok\":false,\"error\":\"command-type-not-allowed\"}", buffer, capacity);
    }
    if (!IsSafeIdempotencyKey(idempotencyKey)) {
        return CopyJsonResult("{\"ok\":false,\"error\":\"invalid-idempotency-key\"}", buffer, capacity);
    }
    if (json.empty() || json.size() > 128 * 1024 || !LooksLikeJsonObject(json)) {
        return CopyJsonResult("{\"ok\":false,\"error\":\"invalid-json-payload\"}", buffer, capacity);
    }
    if (!ManagedCommandPayloadAllowed(commandType, json)) {
        return CopyJsonResult("{\"ok\":false,\"error\":\"command-payload-not-allowed\"}", buffer, capacity);
    }

    const auto baseDir = nedjin::ModuleDirectory();
    const auto safeModuleName = NormalizeModuleNameForFile(moduleName);
    const auto pendingRoot = nedjin::JoinPath(nedjin::JoinPath(ManagedStateRoot(baseDir), L"commands"), L"pending");
    const auto fileName = safeModuleName + "--" + commandType + "--" + idempotencyKey + ".json";
    const auto path = nedjin::JoinPath(pendingRoot, nedjin::Utf8ToWide(fileName));

    if (nedjin::FileExists(path)) {
        std::ostringstream existing;
        existing << "{\"ok\":true"
                 << ",\"status\":\"existing\""
                 << ",\"module\":\"" << nedjin::JsonEscape(moduleName) << "\""
                 << ",\"commandType\":\"" << nedjin::JsonEscape(commandType) << "\""
                 << ",\"idempotencyKey\":\"" << nedjin::JsonEscape(idempotencyKey) << "\""
                 << ",\"path\":\"" << nedjin::JsonEscape(nedjin::WideToUtf8(path)) << "\""
                 << ",\"utc\":\"" << nedjin::UtcIsoNow() << "\"}";
        return CopyJsonResult(existing.str(), buffer, capacity);
    }

    std::ostringstream body;
    body << "{\"schema\":\"scum-nedjin-managed-command-v1\""
         << ",\"status\":\"queued\""
         << ",\"utc\":\"" << nedjin::UtcIsoNow() << "\""
         << ",\"module\":\"" << nedjin::JsonEscape(moduleName) << "\""
         << ",\"commandType\":\"" << nedjin::JsonEscape(commandType) << "\""
         << ",\"idempotencyKey\":\"" << nedjin::JsonEscape(idempotencyKey) << "\""
         << ",\"payload\":" << json << "}";

    if (!nedjin::WriteTextFile(path, body.str())) {
        return CopyJsonResult("{\"ok\":false,\"error\":\"enqueue-write-failed\"}", buffer, capacity);
    }

    std::ostringstream result;
    result << "{\"ok\":true"
           << ",\"status\":\"queued\""
           << ",\"module\":\"" << nedjin::JsonEscape(moduleName) << "\""
           << ",\"commandType\":\"" << nedjin::JsonEscape(commandType) << "\""
           << ",\"idempotencyKey\":\"" << nedjin::JsonEscape(idempotencyKey) << "\""
           << ",\"path\":\"" << nedjin::JsonEscape(nedjin::WideToUtf8(path)) << "\""
           << ",\"sizeBytes\":" << body.str().size()
           << ",\"utc\":\"" << nedjin::UtcIsoNow() << "\"}";
    return CopyJsonResult(result.str(), buffer, capacity);
}

}
