#include "ue4ss_abi_shim.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    constexpr int object_flag_class_default_object = 0x10;

    auto find_object_guarded(const wchar_t* class_name, const wchar_t* object_path, int flags) -> RC::Unreal::UObject*
    {
        __try
        {
            return RC::Unreal::UObjectGlobals::FindObject(class_name, object_path, 0, flags);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    auto narrow(const std::wstring& value) -> std::string
    {
        if (value.empty()) return {};

        const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (needed <= 0) return {};

        std::string result(static_cast<std::size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), needed, nullptr, nullptr);
        return result;
    }

    auto json_escape(const std::string& value) -> std::string
    {
        std::string result;
        result.reserve(value.size() + 8);
        for (const char ch : value)
        {
            switch (ch)
            {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\r': result += "\\r"; break;
            case '\n': result += "\\n"; break;
            case '\t': result += "\\t"; break;
            default: result.push_back(ch); break;
            }
        }
        return result;
    }

    auto module_dir() -> fs::path
    {
        HMODULE module{};
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&module_dir),
            &module);

        wchar_t buffer[MAX_PATH]{};
        const DWORD copied = GetModuleFileNameW(module, buffer, MAX_PATH);
        if (copied == 0) return {};

        return fs::path(std::wstring(buffer, copied)).parent_path();
    }

    auto win64_dir() -> fs::path
    {
        const auto dir = module_dir();
        return dir.parent_path().parent_path().parent_path();
    }

    auto state_dir() -> fs::path
    {
        const auto win64 = win64_dir();
        return win64.parent_path().parent_path() / L"Saved" / L"ScumNeDjin" / L"state";
    }

    auto status_path() -> fs::path
    {
        return state_dir() / L"managed" / L"ue4ss-cppmod-bridge-status.json";
    }

    auto inline_command_path() -> fs::path
    {
        return state_dir() / L"native-trader-inline-command.json";
    }

    auto inline_result_path() -> fs::path
    {
        return state_dir() / L"native-trader-inline-result.json";
    }

    auto first_property_result_path() -> fs::path
    {
        return state_dir() / L"managed" / L"first-reflected-property-probe-result.json";
    }

    auto utc_now() -> std::string
    {
        SYSTEMTIME st{};
        GetSystemTime(&st);
        std::ostringstream ss;
        ss << std::setfill('0') << std::setw(4) << st.wYear << '-'
           << std::setw(2) << st.wMonth << '-'
           << std::setw(2) << st.wDay << 'T'
           << std::setw(2) << st.wHour << ':'
           << std::setw(2) << st.wMinute << ':'
           << std::setw(2) << st.wSecond << 'Z';
        return ss.str();
    }

    auto regex_value(const std::string& text, const char* key) -> std::string
    {
        const std::regex pattern(std::string("\\\"") + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
        std::smatch match;
        if (std::regex_search(text, match, pattern) && match.size() > 1) return match[1].str();
        return {};
    }

    auto first_regex_value(const std::string& text, std::initializer_list<const char*> keys) -> std::string
    {
        for (const auto* key : keys)
        {
            auto value = regex_value(text, key);
            if (!value.empty()) return value;
        }
        return {};
    }

    auto lower_copy(std::string value) -> std::string
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }

    auto widen(const std::string& value) -> std::wstring
    {
        if (value.empty()) return {};
        const int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
        if (needed <= 0) return {};
        std::wstring result(static_cast<std::size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), needed);
        return result;
    }

    auto object_path_from_full_name(const std::string& full_name_value) -> std::string
    {
        const auto space = full_name_value.find(' ');
        if (space != std::string::npos && space + 1 < full_name_value.size())
        {
            return full_name_value.substr(space + 1);
        }
        return full_name_value;
    }

    auto parse_uobject_address(const std::string& value) -> std::uintptr_t
    {
        auto text = value;
        text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char ch) { return std::isspace(ch) != 0; }), text.end());
        if (text.empty()) return 0;

        int base = 10;
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        {
            text = text.substr(2);
            base = 16;
        }

        try
        {
            return static_cast<std::uintptr_t>(std::stoull(text, nullptr, base));
        }
        catch (...)
        {
            return 0;
        }
    }

    auto object_full_name_matches(const std::string& candidate_full_name, const std::string& requested_full_name) -> bool
    {
        if (candidate_full_name.empty() || requested_full_name.empty()) return false;
        if (candidate_full_name == requested_full_name) return true;
        return object_path_from_full_name(candidate_full_name) == object_path_from_full_name(requested_full_name);
    }

    auto class_token_from_full_name(const std::string& full_name_value) -> std::string
    {
        const auto space = full_name_value.find(' ');
        if (space == std::string::npos || space == 0) return {};
        return full_name_value.substr(0, space);
    }

    void push_unique_class_search_name(std::vector<std::wstring>& values, const std::wstring& value)
    {
        if (value.empty()) return;
        if (std::find(values.begin(), values.end(), value) == values.end())
        {
            values.push_back(value);
        }
    }

    auto build_class_search_names(std::initializer_list<const wchar_t*> class_names, const std::string& requested_full_name) -> std::vector<std::wstring>
    {
        std::vector<std::wstring> values;
        push_unique_class_search_name(values, widen(class_token_from_full_name(requested_full_name)));
        for (const auto* class_name : class_names)
        {
            push_unique_class_search_name(values, class_name == nullptr ? std::wstring{} : std::wstring(class_name));
        }
        return values;
    }

    auto static_find_object_exact_path(const wchar_t* object_path, std::string& route) -> RC::Unreal::UObject*
    {
        // StaticFindObject-compatible exact-path probe through UE4SS exported FindObject.
        for (const wchar_t* candidate_path : {object_path, L"CoreUObject.Object", L"Object", L"Class /Script/CoreUObject.Object"})
        {
            for (const wchar_t* class_name : {L"Class", L"UClass", L"Object"})
            {
                auto* object = find_object_guarded(class_name, candidate_path, 0);
                route += route.empty() ? "" : ";";
                route += "FindObject(" + narrow(class_name) + "," + narrow(candidate_path) + ")=" + (object != nullptr ? "1" : "0");
                if (object != nullptr) return object;
            }
        }

        return nullptr;
    }

    auto safe_full_name(RC::Unreal::UObject* object) -> std::string
    {
        if (object == nullptr) return {};
        return narrow(object->GetFullName());
    }

    auto committed_readable_address(std::uintptr_t address, std::size_t size = sizeof(std::uintptr_t)) -> bool
    {
        if (address == 0 || size == 0) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if ((mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & PAGE_NOACCESS) != 0) return false;
        const auto begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto end = begin + mbi.RegionSize;
        return address >= begin && address + size >= address && address + size <= end;
    }

    auto safe_full_name_noexcept(RC::Unreal::UObject* object) -> std::string
    {
        if (object == nullptr) return {};
        if (!committed_readable_address(reinterpret_cast<std::uintptr_t>(object))) return "<GetFullName unreadable>";
        return "<uobject-readable>";
    }

    auto safe_class_private(RC::Unreal::UObject* object) -> RC::Unreal::UClass*
    {
        if (object == nullptr) return nullptr;
        __try
        {
            return object->GetClassPrivate();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    auto object_class_full_name_noexcept(RC::Unreal::UObject* object) -> std::string
    {
        if (object == nullptr) return {};
        auto* object_class = safe_class_private(object);
        if (object_class == nullptr) return "<GetClass exception>";
        return safe_full_name_noexcept(object_class);
    }

    auto object_property_noexcept_raw(RC::Unreal::UObject* object, const wchar_t* property_name, bool& missing, bool& exception) -> RC::Unreal::UObject*
    {
        missing = false;
        exception = false;
        if (object == nullptr) return nullptr;
        __try
        {
            auto* value = static_cast<RC::Unreal::UObject**>(object->GetValuePtrByPropertyNameInChain(property_name));
            if (value == nullptr)
            {
                missing = true;
                return nullptr;
            }
            return *value;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            exception = true;
            return nullptr;
        }
    }

    auto property_value_ptr_noexcept_raw(RC::Unreal::UObject* object, const wchar_t* property_name, bool& missing, bool& exception) -> void*
    {
        missing = false;
        exception = false;
        if (object == nullptr) return nullptr;
        __try
        {
            auto* value = object->GetValuePtrByPropertyNameInChain(property_name);
            if (value == nullptr)
            {
                missing = true;
                return nullptr;
            }
            return value;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            exception = true;
            return nullptr;
        }
    }

    void append_json_string_array(std::ostringstream& ss, const std::vector<std::string>& values)
    {
        ss << "[";
        for (std::size_t index = 0; index < values.size(); ++index)
        {
            if (index > 0) ss << ",";
            ss << "\"" << json_escape(values[index]) << "\"";
        }
        ss << "]";
    }

    auto find_object_by_full_name_exact(std::initializer_list<const wchar_t*> class_names, const std::string& requested_full_name, std::vector<std::string>& attempts, const char* label) -> RC::Unreal::UObject*
    {
        if (requested_full_name.empty())
        {
            attempts.push_back(std::string(label) + "=empty");
            return nullptr;
        }

        const auto path = object_path_from_full_name(requested_full_name);
        const auto path_w = widen(path);
        const auto search_class_names = build_class_search_names(class_names, requested_full_name);
        for (const auto& class_name : search_class_names)
        {
            if (path_w.empty()) continue;
            auto* object = find_object_guarded(class_name.c_str(), path_w.c_str(), object_flag_class_default_object);
            attempts.push_back(std::string(label) + ".FindObject(" + narrow(class_name) + "," + path + ")=" + (object != nullptr ? "1" : "0"));
            if (object != nullptr) return object;
        }

        attempts.push_back(std::string(label) + ".leaf-lookup=blocked-by-policy");
        return nullptr;
    }

    auto object_from_lua_address_guarded(
        const std::string& address_text,
        const std::string& requested_full_name,
        std::vector<std::string>& attempts,
        const char* label) -> RC::Unreal::UObject*
    {
        (void)address_text;
        (void)requested_full_name;
        attempts.push_back(std::string(label) + ".address-fallback=blocked-by-policy");
        return nullptr;
    }

    void write_status(bool unreal_ready, bool core_object_found, const std::string& core_object_name, const std::string& route)
    {
        const auto path = status_path();
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);

        std::ostringstream json;
        json << "{"
             << "\"ok\":" << (unreal_ready && core_object_found ? "true" : "false")
             << ",\"schema\":\"scum-nedjin-ue4ss-cppmod-bridge-status-v1\""
             << ",\"module\":\"ScumNedjinManagedBridgeMod\""
             << ",\"version\":\"0.1.0-local-readonly\""
             << ",\"policy\":{"
             << "\"readOnly\":true"
             << ",\"exactStaticObjectPathOnly\":true"
             << ",\"runtimeObjectScan\":false"
             << ",\"findAll\":false"
             << ",\"hookInstall\":false"
             << ",\"ueMutation\":false"
             << ",\"commandDispatch\":false"
             << ",\"scumDbWrite\":false"
             << "}"
             << ",\"unrealReady\":" << (unreal_ready ? "true" : "false")
             << ",\"coreObjectFound\":" << (core_object_found ? "true" : "false")
             << ",\"coreObjectName\":\"" << json_escape(core_object_name) << "\""
             << ",\"allowedNativeProbe\":\"StaticFindObject(/Script/CoreUObject.Object)\""
             << ",\"route\":\"" << json_escape(route) << "\""
             << ",\"statusPath\":\"" << json_escape(narrow(path.wstring())) << "\""
             << "}";

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << json.str();
    }

    void write_inline_result(const std::string& id, bool ok, const std::string& message, const std::string& data)
    {
        const auto path = inline_result_path();
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);

        std::ostringstream json;
        json << "{"
             << "\"id\":\"" << json_escape(id) << "\""
             << ",\"ok\":" << (ok ? "true" : "false")
             << ",\"source\":\"ScumNedjinManagedBridgeMod\""
             << ",\"message\":\"" << json_escape(message) << "\""
             << ",\"data\":" << (data.empty() ? "{}" : data)
             << "}";

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << json.str();
    }

    auto first_reflected_property_probe(const std::string& command_text) -> std::pair<bool, std::string>
    {
        const auto started_at = utc_now();
        const auto chest_full_name = first_regex_value(command_text, {"chestFullName", "targetFullName", "objectPath"});
        const auto chest_class = first_regex_value(command_text, {"chestClass", "targetClass"});
        const auto chest_address = first_regex_value(command_text, {"chestAddress", "targetAddress", "objectAddress"});
        const auto component_full_name = first_regex_value(command_text, {"componentFullName", "nameableComponentFullName", "componentPath"});
        const auto component_class = first_regex_value(command_text, {"componentClass", "nameableComponentClass"});
        const auto component_address = first_regex_value(command_text, {"componentAddress", "nameableComponentAddress"});

        std::vector<std::string> attempts;
        auto* chest = find_object_by_full_name_exact(
            {L"ChestItem", L"AChestItem", L"Item", L"AItem", L"Actor", L"Object", L"UObject"},
            chest_full_name,
            attempts,
            "chest");
        if (chest == nullptr)
        {
            chest = object_from_lua_address_guarded(chest_address, chest_full_name, attempts, "chest");
        }

        RC::Unreal::UObject* component = nullptr;
        bool component_missing = false;
        bool component_exception = false;
        if (!component_full_name.empty())
        {
            component = find_object_by_full_name_exact(
                {L"NameableItemComponent", L"UNameableItemComponent", L"ItemComponent", L"UItemComponent", L"Object", L"UObject"},
                component_full_name,
                attempts,
                "component");
        }
        if (component == nullptr)
        {
            component = object_from_lua_address_guarded(component_address, component_full_name, attempts, "component");
        }
        if (component == nullptr && chest != nullptr)
        {
            component = object_property_noexcept_raw(chest, L"_nameableItemComponent", component_missing, component_exception);
            attempts.push_back(std::string("chest._nameableItemComponent=") +
                (component_exception ? "exception" : (component_missing ? "missing" : safe_full_name_noexcept(component))));
        }

        const auto chest_resolved = chest != nullptr;
        const auto component_resolved = component != nullptr;
        const auto component_before = safe_full_name_noexcept(component);
        const auto component_class_full = object_class_full_name_noexcept(component);
        const auto component_looks_valid =
            lower_copy(component_class + " " + component_class_full + " " + component_before).find("nameableitemcomponent") != std::string::npos;

        bool property_missing = false;
        bool property_exception = false;
        auto* property_ptr = property_value_ptr_noexcept_raw(component, L"_nameData", property_missing, property_exception);
        const auto property_pointer_read_ok =
            property_ptr != nullptr &&
            !property_missing &&
            !property_exception &&
            committed_readable_address(reinterpret_cast<std::uintptr_t>(property_ptr), 0x40);

        const auto component_after = safe_full_name_noexcept(component);
        const auto stale_guard_ok =
            component_resolved &&
            !component_before.empty() &&
            component_before == component_after &&
            component_looks_valid;

        const auto bridge_status_path = status_path();
        std::ifstream bridge_file(bridge_status_path, std::ios::binary);
        std::string bridge_text;
        if (bridge_file)
        {
            std::ostringstream bridge_stream;
            bridge_stream << bridge_file.rdbuf();
            bridge_text = bridge_stream.str();
        }

        const auto bridge_available = !bridge_text.empty();
        const auto bridge_ok = bridge_text.find("\"ok\":true") != std::string::npos;
        const auto unreal_ready = bridge_text.find("\"unrealReady\":true") != std::string::npos;
        const auto core_object_found = bridge_text.find("\"coreObjectFound\":true") != std::string::npos;
        const auto ok = bridge_available && bridge_ok && unreal_ready && core_object_found && chest_resolved &&
            component_resolved && stale_guard_ok && property_pointer_read_ok;

        const auto completed_at = utc_now();
        const auto result_path = first_property_result_path();
        std::error_code ec;
        fs::create_directories(result_path.parent_path(), ec);

        std::ostringstream proof;
        proof << "{"
              << "\"ok\":" << (ok ? "true" : "false")
              << ",\"schema\":\"scum-managed-first-reflected-property-probe-result-v1\""
              << ",\"domain\":\"inventory-loot-chests\""
              << ",\"className\":\"UNameableItemComponent\""
              << ",\"propertyName\":\"_nameData\""
              << ",\"propertyType\":\"FItemNameReplicationData\""
              << ",\"operation\":\"reflected-property-read\""
              << ",\"nativeApi\":\"UObject::GetValuePtrByPropertyNameInChain\""
              << ",\"objectPath\":\"" << json_escape(safe_full_name_noexcept(chest)) << "\""
              << ",\"componentPath\":\"" << json_escape(component_before) << "\""
              << ",\"propertyValueKind\":\"FItemNameReplicationData\""
              << ",\"startedAtUtc\":\"" << json_escape(started_at) << "\""
              << ",\"completedAtUtc\":\"" << json_escape(completed_at) << "\""
              << ",\"bridgeAvailable\":" << (bridge_available ? "true" : "false")
              << ",\"unrealReady\":" << (unreal_ready ? "true" : "false")
              << ",\"coreObjectFound\":" << (core_object_found ? "true" : "false")
              << ",\"dryRunAllowlisted\":true"
              << ",\"propertyPointerReadOk\":" << (property_pointer_read_ok ? "true" : "false")
              << ",\"staleObjectGuardApplied\":" << (stale_guard_ok ? "true" : "false")
              << ",\"readOnly\":true"
              << ",\"runtimeObjectScan\":false"
              << ",\"findAll\":false"
              << ",\"loadAsset\":false"
              << ",\"hookInstall\":false"
              << ",\"ueMutation\":false"
              << ",\"commandDispatch\":false"
              << ",\"scumDbWrite\":false"
              << ",\"fallbackMutation\":false"
              << ",\"objectCount\":" << (component_resolved ? 1 : 0)
              << ",\"logCriticalMarkers\":0"
              << ",\"chestResolved\":" << (chest_resolved ? "true" : "false")
              << ",\"componentResolved\":" << (component_resolved ? "true" : "false")
              << ",\"componentLooksValid\":" << (component_looks_valid ? "true" : "false")
              << ",\"propertyMissing\":" << (property_missing ? "true" : "false")
              << ",\"propertyException\":" << (property_exception ? "true" : "false")
              << ",\"componentException\":" << (component_exception ? "true" : "false")
              << ",\"routeAttempts\":";
        append_json_string_array(proof, attempts);
        proof << "}";

        std::ofstream proof_file(result_path, std::ios::binary | std::ios::trunc);
        if (proof_file) proof_file << proof.str();

        std::ostringstream data;
        data << "{\"ok\":" << (ok ? "true" : "false")
             << ",\"stage\":\"base-loot-first-reflected-property-probe\""
             << ",\"resultPath\":\"" << json_escape(narrow(result_path.wstring())) << "\""
             << ",\"chestResolved\":" << (chest_resolved ? "true" : "false")
             << ",\"componentResolved\":" << (component_resolved ? "true" : "false")
             << ",\"propertyPointerReadOk\":" << (property_pointer_read_ok ? "true" : "false")
             << ",\"staleObjectGuardApplied\":" << (stale_guard_ok ? "true" : "false")
             << "}";
        return {ok, data.str()};
    }

    void process_inline_command_once()
    {
        const auto command_path = inline_command_path();
        std::ifstream file(command_path, std::ios::binary);
        if (!file) return;

        std::ostringstream command_stream;
        command_stream << file.rdbuf();
        const auto text = command_stream.str();
        if (text.empty()) return;

        const auto id = first_regex_value(text, {"id"});
        const auto command = first_regex_value(text, {"command"});
        if (command != "base_loot_first_reflected_property_probe" && command != "base_loot_nameable_reflected_probe")
        {
            return;
        }

        std::error_code ec;
        fs::remove(command_path, ec);

        const auto [ok, data] = first_reflected_property_probe(text);
        write_inline_result(id.empty() ? "base-loot-first-reflected-property-probe" : id,
            ok,
            ok ? "Base loot reflected property probe ok" : "Base loot reflected property probe failed",
            data);
    }
}

class ScumNedjinManagedBridgeMod final : public RC::CppUserModBase
{
  public:
    ScumNedjinManagedBridgeMod()
    {
        ModName = STR("ScumNedjinManagedBridgeMod");
        ModVersion = STR("0.1.0-local-readonly");
        ModDescription = STR("Local read-only UE4SS C++ bridge surface for ScumNeDjin managed-loader research");
        ModAuthors = STR("ScumNeDjin");
        write_status(false, false, "", "constructor");
    }

    auto on_update() -> void override
    {
        process_inline_command_once();
    }
    auto on_unreal_init() -> void override
    {
        constexpr auto core_object_path = L"/Script/CoreUObject.Object";
        std::string route;
        auto* core_object = static_find_object_exact_path(core_object_path, route);
        write_status(true, core_object != nullptr, safe_full_name_noexcept(core_object), "on_unreal_init exact static object path;" + route);
    }
    auto on_ui_init() -> void override {}
    auto on_program_start() -> void override {}
    auto on_dll_load(RC::StringViewType) -> void override {}
    auto render_tab() -> void override {}
    auto on_cpp_mods_loaded() -> void override {}
    auto on_lua_start(RC::StringViewType, RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua&, std::vector<RC::LuaMadeSimple::Lua*>&) -> void override {}
    auto on_lua_start(RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua&, std::vector<RC::LuaMadeSimple::Lua*>&) -> void override {}
    auto on_lua_stop(RC::StringViewType, RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua&, std::vector<RC::LuaMadeSimple::Lua*>&) -> void override {}
    auto on_lua_stop(RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua&, std::vector<RC::LuaMadeSimple::Lua*>&) -> void override {}
    auto on_lua_start(RC::StringViewType, RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua*) -> void override {}
    auto on_lua_start(RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua*) -> void override {}
    auto on_lua_stop(RC::StringViewType, RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua*) -> void override {}
    auto on_lua_stop(RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua&, RC::LuaMadeSimple::Lua*) -> void override {}
};

extern "C"
{
    __declspec(dllexport) RC::CppUserModBase* start_mod()
    {
        return new ScumNedjinManagedBridgeMod();
    }

    __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
