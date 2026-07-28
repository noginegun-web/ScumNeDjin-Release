#include "SCUMTraderManager.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    constexpr auto mod_version = "0.1.142-base-loot-store-route";
    constexpr auto stocked_armory_outpost_key = "a_0";
    constexpr auto stocked_armory_tradepost_full_name = "BP_Outpost_Armory_NPCInteractionBox_C /Game/ConZ_Files/Maps/The_Island/A_0_Outpost.A_0_Outpost:PersistentLevel.BP_Outpost_Armory_NPCInteractionBox_2";
    constexpr auto stocked_armory_manager_full_name = "BP_TradeOutpostManager_C /Game/ConZ_Files/Maps/The_Island/A_0_Outpost.A_0_Outpost:PersistentLevel.BP_TradeOutpostManager_2";
    constexpr auto stocked_armory_personality_guid = "55A6C939442B571C8DA6EEA3B11F6F99";
    constexpr double max_tradepost_donor_distance_2d = 1000000.0;
    constexpr int object_flag_class_default_object = 0x10;
    constexpr std::size_t process_event_vtable_index = 0x44;
    constexpr std::uint8_t interaction_type_godmode_fill = 23;
    constexpr std::uint8_t interaction_type_store = 60;
    constexpr std::uint8_t interaction_type_trade_buy = 238;

    using ProcessEventFn = void(__fastcall*)(RC::Unreal::UObject* object, RC::Unreal::UObject* function, void* params);

    struct CachedTradeOutpostManager
    {
        RC::Unreal::UObject* object{};
        std::string full_name{};
        int refresh_count{};
    };

    std::unordered_map<std::string, CachedTradeOutpostManager> g_trade_outpost_manager_cache;

    struct CachedEditorActorIdentity
    {
        RC::Unreal::AActor* actor{};
        std::string full_name{};
        std::string class_full_name{};
        RC::Unreal::FVector last_location{};
        RC::Unreal::FRotator last_rotation{};
        bool has_location{};
        bool has_rotation{};
    };

    std::unordered_map<std::string, CachedEditorActorIdentity> g_editor_actor_identity_cache;
    std::unordered_map<std::string, std::wstring> g_cfcore_runtime_string_storage;

    struct FGuidAbi
    {
        std::uint32_t a{};
        std::uint32_t b{};
        std::uint32_t c{};
        std::uint32_t d{};
    };

    struct TArrayAbi
    {
        void* data{};
        std::int32_t count{};
        std::int32_t max{};
    };

    struct FScriptDelegateAbi
    {
        void* object{};
        std::uint64_t function_name{};
    };

    struct FAssureServerModsUpdatedParamsAbi
    {
        TArrayAbi mod_ids{};
        TArrayAbi dev_mod_ids{};
    };

    struct AssureServerModsUpdatedProcessEventParamsAbi
    {
        FAssureServerModsUpdatedParamsAbi params{};
        FScriptDelegateAbi on_progress{};
        FScriptDelegateAbi on_updated{};
        FScriptDelegateAbi on_error{};
    };

    struct AssureClientModsUpdatedProcessEventParamsAbi
    {
        TArrayAbi server_file_ids{};
        FScriptDelegateAbi on_progress{};
        FScriptDelegateAbi on_updated{};
        FScriptDelegateAbi on_error{};
    };

    struct ClientBeginByteStreamProcessEventParamsAbi
    {
        std::int32_t stream_id{};
        std::uint8_t stream_type{};
        std::uint8_t padding[3]{};
    };

    struct ClientReceiveBytesFromStreamProcessEventParamsAbi
    {
        std::int32_t stream_id{};
        std::uint8_t padding[4]{};
        TArrayAbi bytes{};
    };

    struct ClientEndByteStreamProcessEventParamsAbi
    {
        std::int32_t stream_id{};
    };

    static_assert(sizeof(FScriptDelegateAbi) == 0x10);
    static_assert(sizeof(FAssureServerModsUpdatedParamsAbi) == 0x20);
    static_assert(sizeof(AssureServerModsUpdatedProcessEventParamsAbi) == 0x50);
    static_assert(sizeof(AssureClientModsUpdatedProcessEventParamsAbi) == 0x40);
    static_assert(offsetof(ClientBeginByteStreamProcessEventParamsAbi, stream_id) == 0x0);
    static_assert(offsetof(ClientBeginByteStreamProcessEventParamsAbi, stream_type) == 0x4);
    static_assert(sizeof(ClientBeginByteStreamProcessEventParamsAbi) == 0x8);
    static_assert(offsetof(ClientReceiveBytesFromStreamProcessEventParamsAbi, stream_id) == 0x0);
    static_assert(offsetof(ClientReceiveBytesFromStreamProcessEventParamsAbi, bytes) == 0x8);
    static_assert(sizeof(ClientReceiveBytesFromStreamProcessEventParamsAbi) == 0x18);
    static_assert(offsetof(ClientEndByteStreamProcessEventParamsAbi, stream_id) == 0x0);
    static_assert(sizeof(ClientEndByteStreamProcessEventParamsAbi) == 0x4);

    struct FModInfoAbi
    {
        RC::Unreal::UObject* mod_manifest_class{};
    };

    static_assert(sizeof(FModInfoAbi) == sizeof(void*));

    struct FUObjectItemAbi
    {
        std::uintptr_t object{};
        std::int32_t flags{};
        std::int32_t cluster_root_index{};
        std::int32_t serial_number{};
        std::int32_t padding{};
    };

    struct TUObjectArrayAbi
    {
        std::uintptr_t objects{};
        std::uint8_t pad8[0x8]{};
        std::int32_t max_elements{};
        std::int32_t num_elements{};
        std::int32_t max_chunks{};
        std::int32_t num_chunks{};
    };

    static_assert(sizeof(FUObjectItemAbi) == 0x18);
    static_assert(sizeof(TUObjectArrayAbi) == 0x20);

    struct alignas(16) FTraderMarkerAbi
    {
        std::uint8_t sedentary_npc_class[0x28]{};
        std::uint8_t padding_to_spawn_transform[0x08]{};
        RC::Unreal::FTransform spawn_transform{};
        bool should_raycast_spawn_position{};
        std::uint8_t padding_to_personality[0x07]{};
        RC::Unreal::UObject* trader_personality{};
        RC::Unreal::FTransform purchased_tradeables_spawn_transform{};
        RC::Unreal::FTransform depot_spawn_transform{};
    };

    static_assert(offsetof(FTraderMarkerAbi, spawn_transform) == 0x30);
    static_assert(offsetof(FTraderMarkerAbi, should_raycast_spawn_position) == 0x60);
    static_assert(offsetof(FTraderMarkerAbi, trader_personality) == 0x68);
    static_assert(offsetof(FTraderMarkerAbi, purchased_tradeables_spawn_transform) == 0x70);
    static_assert(offsetof(FTraderMarkerAbi, depot_spawn_transform) == 0xA0);
    static_assert(sizeof(FTraderMarkerAbi) == 0xD0);

    struct FPendingTraderPersonalityDataHelperStructAbi
    {
        RC::Unreal::UObject* personality{};
    };

    static_assert(sizeof(FPendingTraderPersonalityDataHelperStructAbi) == sizeof(void*));

    struct alignas(16) FTraderLocationMarkerAbi
    {
        std::int32_t marker_type{};
        std::uint8_t padding_to_transform[0x0C]{};
        RC::Unreal::FTransform transform{};
    };

    static_assert(offsetof(FTraderLocationMarkerAbi, transform) == 0x10);
    static_assert(sizeof(FTraderLocationMarkerAbi) == 0x40);

    struct alignas(16) FSedentaryNPCMarkerAbi
    {
        std::uint8_t sedentary_npc_class[0x28]{};
        std::uint8_t padding_to_spawn_transform[0x08]{};
        RC::Unreal::FTransform spawn_transform{};
        bool should_raycast_spawn_position{};
    };

    static_assert(offsetof(FSedentaryNPCMarkerAbi, spawn_transform) == 0x30);
    static_assert(offsetof(FSedentaryNPCMarkerAbi, should_raycast_spawn_position) == 0x60);
    static_assert(sizeof(FSedentaryNPCMarkerAbi) == 0x70);

    struct FTextAbi
    {
        std::uint8_t bytes[0x18]{};
    };

    struct FAdminCommandArgumentDescriptionAbi
    {
        FTextAbi name{};
        FTextAbi description{};
        bool show_completion_values_in_help_text{};
        std::uint8_t padding[0x07]{};
        RC::Unreal::UObject* data{};
    };

    static_assert(offsetof(FAdminCommandArgumentDescriptionAbi, data) == 0x38);
    static_assert(sizeof(FAdminCommandArgumentDescriptionAbi) == 0x40);

    struct FInteractionDataAbi
    {
        bool modifier_pressed{};
        std::uint8_t pad_modifier[0x07]{};
        std::int64_t integer_data{};
        RC::Unreal::UObject* pointer_data{};
        bool bool_data{};
        std::uint8_t pad_bool[0x07]{};
        FTextAbi text_data{};
        RC::Unreal::FVector interaction_location{};
        RC::Unreal::FVector interaction_normal{};
        RC::Unreal::FVector vector_data{};
        std::int64_t base_element_id{};
    };

    static_assert(offsetof(FInteractionDataAbi, integer_data) == 0x08);
    static_assert(offsetof(FInteractionDataAbi, pointer_data) == 0x10);
    static_assert(offsetof(FInteractionDataAbi, bool_data) == 0x18);
    static_assert(offsetof(FInteractionDataAbi, text_data) == 0x20);
    static_assert(offsetof(FInteractionDataAbi, interaction_location) == 0x38);
    static_assert(offsetof(FInteractionDataAbi, interaction_normal) == 0x44);
    static_assert(offsetof(FInteractionDataAbi, vector_data) == 0x50);
    static_assert(offsetof(FInteractionDataAbi, base_element_id) == 0x60);
    static_assert(sizeof(FInteractionDataAbi) == 0x68);

    struct PrisonerClientInteractParams
    {
        RC::Unreal::UObject* interactable{};
        std::uint8_t interaction_type{};
        std::uint8_t pad_interaction_type[0x07]{};
        FInteractionDataAbi interaction_data{};
    };

    static_assert(offsetof(PrisonerClientInteractParams, interaction_type) == 0x08);
    static_assert(offsetof(PrisonerClientInteractParams, interaction_data) == 0x10);
    static_assert(sizeof(PrisonerClientInteractParams) == 0x78);

    struct InteractableInterfaceInteractParams
    {
        RC::Unreal::UObject* user_controller{};
        std::uint8_t interaction_type{};
        std::uint8_t pad_interaction_type[0x07]{};
        FInteractionDataAbi interaction_data{};
    };

    static_assert(offsetof(InteractableInterfaceInteractParams, interaction_type) == 0x08);
    static_assert(offsetof(InteractableInterfaceInteractParams, interaction_data) == 0x10);
    static_assert(sizeof(InteractableInterfaceInteractParams) == 0x78);

    struct PlayerRpcInteractWithObjectOnServerParams
    {
        RC::Unreal::UObject* interactable{};
        RC::Unreal::UObject* user_controller{};
        std::uint8_t interaction_type{};
        std::uint8_t pad_interaction_type[0x07]{};
        FInteractionDataAbi interaction_data{};
    };

    static_assert(offsetof(PlayerRpcInteractWithObjectOnServerParams, user_controller) == 0x08);
    static_assert(offsetof(PlayerRpcInteractWithObjectOnServerParams, interaction_type) == 0x10);
    static_assert(offsetof(PlayerRpcInteractWithObjectOnServerParams, interaction_data) == 0x18);
    static_assert(sizeof(PlayerRpcInteractWithObjectOnServerParams) == 0x80);

    struct PlayerRpcInteractItemWithItemOnServerParams
    {
        RC::Unreal::UObject* item{};
        RC::Unreal::UObject* player_controller{};
        RC::Unreal::UObject* other_item{};
        std::uint8_t interaction_type{};
        std::uint8_t pad_interaction_type[0x07]{};
        FInteractionDataAbi interaction_data{};
    };

    static_assert(offsetof(PlayerRpcInteractItemWithItemOnServerParams, player_controller) == 0x08);
    static_assert(offsetof(PlayerRpcInteractItemWithItemOnServerParams, other_item) == 0x10);
    static_assert(offsetof(PlayerRpcInteractItemWithItemOnServerParams, interaction_type) == 0x18);
    static_assert(offsetof(PlayerRpcInteractItemWithItemOnServerParams, interaction_data) == 0x20);
    static_assert(sizeof(PlayerRpcInteractItemWithItemOnServerParams) == 0x88);

    struct PlaceableServerPlaceParams
    {
        RC::Unreal::FVector location{};
        RC::Unreal::FRotator rotation{};
    };

    static_assert(offsetof(PlaceableServerPlaceParams, rotation) == 0x0C);
    static_assert(sizeof(PlaceableServerPlaceParams) == 0x18);

    struct GetAllActorsOfClassParams
    {
        RC::Unreal::UObject* world_context_object{};
        RC::Unreal::UObject* actor_class{};
        TArrayAbi out_actors{};
    };

    static_assert(offsetof(GetAllActorsOfClassParams, actor_class) == 0x08);
    static_assert(offsetof(GetAllActorsOfClassParams, out_actors) == 0x10);
    static_assert(sizeof(GetAllActorsOfClassParams) == 0x20);

    struct FHitResultAbi
    {
        std::uint8_t bytes[0x98]{};
    };

    static_assert(sizeof(FHitResultAbi) == 0x98);

    struct FHitResultTraceAbi
    {
        std::int32_t face_index{};
        float time{};
        float distance{};
        RC::Unreal::FVector location{};
        RC::Unreal::FVector impact_point{};
        RC::Unreal::FVector normal{};
        RC::Unreal::FVector impact_normal{};
        RC::Unreal::FVector trace_start{};
        RC::Unreal::FVector trace_end{};
        float penetration_depth{};
        std::int32_t item{};
        std::uint8_t element_index{};
        std::uint8_t blocking_hit{};
        std::uint8_t start_penetrating{};
        std::uint8_t pad_flags{};
        RC::Unreal::FWeakObjectPtr phys_material{};
        RC::Unreal::FWeakObjectPtr actor{};
        RC::Unreal::FWeakObjectPtr component{};
        std::uint8_t bone_name[0x08]{};
        std::uint8_t my_bone_name[0x08]{};
    };

    static_assert(offsetof(FHitResultTraceAbi, location) == 0x0C);
    static_assert(offsetof(FHitResultTraceAbi, trace_start) == 0x3C);
    static_assert(offsetof(FHitResultTraceAbi, actor) == 0x68);
    static_assert(offsetof(FHitResultTraceAbi, component) == 0x70);
    static_assert(sizeof(FHitResultTraceAbi) == 0x88);

    struct FLinearColorAbi
    {
        float r{};
        float g{};
        float b{};
        float a{1.0f};
    };

    static_assert(sizeof(FLinearColorAbi) == 0x10);

    struct LineTraceSingleParams
    {
        RC::Unreal::UObject* world_context_object{};
        RC::Unreal::FVector start{};
        RC::Unreal::FVector end{};
        std::uint8_t trace_channel{};
        bool trace_complex{};
        std::uint8_t pad_trace_complex[0x06]{};
        TArrayAbi actors_to_ignore{};
        std::uint8_t draw_debug_type{};
        std::uint8_t pad_draw_debug_type[0x03]{};
        FHitResultTraceAbi out_hit{};
        bool ignore_self{};
        std::uint8_t pad_ignore_self[0x03]{};
        FLinearColorAbi trace_color{1.0f, 0.0f, 0.0f, 1.0f};
        FLinearColorAbi trace_hit_color{0.0f, 1.0f, 0.0f, 1.0f};
        float draw_time{};
        bool return_value{};
        std::uint8_t pad_return_value[0x03]{};
    };

    static_assert(offsetof(LineTraceSingleParams, start) == 0x08);
    static_assert(offsetof(LineTraceSingleParams, end) == 0x14);
    static_assert(offsetof(LineTraceSingleParams, trace_channel) == 0x20);
    static_assert(offsetof(LineTraceSingleParams, actors_to_ignore) == 0x28);
    static_assert(offsetof(LineTraceSingleParams, draw_debug_type) == 0x38);
    static_assert(offsetof(LineTraceSingleParams, out_hit) == 0x3C);
    static_assert(offsetof(LineTraceSingleParams, ignore_self) == 0xC4);
    static_assert(offsetof(LineTraceSingleParams, trace_color) == 0xC8);
    static_assert(offsetof(LineTraceSingleParams, trace_hit_color) == 0xD8);
    static_assert(offsetof(LineTraceSingleParams, draw_time) == 0xE8);
    static_assert(offsetof(LineTraceSingleParams, return_value) == 0xEC);
    static_assert(sizeof(LineTraceSingleParams) == 0xF0);

    struct SphereTraceSingleParams
    {
        RC::Unreal::UObject* world_context_object{};
        RC::Unreal::FVector start{};
        RC::Unreal::FVector end{};
        float radius{};
        std::uint8_t trace_channel{};
        bool trace_complex{};
        std::uint8_t pad_trace_complex[0x02]{};
        TArrayAbi actors_to_ignore{};
        std::uint8_t draw_debug_type{};
        std::uint8_t pad_draw_debug_type[0x03]{};
        FHitResultTraceAbi out_hit{};
        bool ignore_self{};
        std::uint8_t pad_ignore_self[0x03]{};
        FLinearColorAbi trace_color{1.0f, 0.0f, 0.0f, 1.0f};
        FLinearColorAbi trace_hit_color{0.0f, 1.0f, 0.0f, 1.0f};
        float draw_time{};
        bool return_value{};
        std::uint8_t pad_return_value[0x03]{};
    };

    static_assert(offsetof(SphereTraceSingleParams, radius) == 0x20);
    static_assert(offsetof(SphereTraceSingleParams, trace_channel) == 0x24);
    static_assert(offsetof(SphereTraceSingleParams, actors_to_ignore) == 0x28);
    static_assert(offsetof(SphereTraceSingleParams, out_hit) == 0x3C);
    static_assert(offsetof(SphereTraceSingleParams, ignore_self) == 0xC4);
    static_assert(offsetof(SphereTraceSingleParams, return_value) == 0xEC);
    static_assert(sizeof(SphereTraceSingleParams) == 0xF0);

    struct ActorSetLocationAndRotationParams
    {
        RC::Unreal::FVector new_location{};
        RC::Unreal::FRotator new_rotation{};
        bool sweep{};
        std::uint8_t pad_sweep[0x03]{};
        FHitResultAbi sweep_hit_result{};
        bool teleport{};
        bool return_value{};
        std::uint8_t pad_return[0x02]{};
    };

    static_assert(offsetof(ActorSetLocationAndRotationParams, new_rotation) == 0x0C);
    static_assert(offsetof(ActorSetLocationAndRotationParams, sweep) == 0x18);
    static_assert(offsetof(ActorSetLocationAndRotationParams, sweep_hit_result) == 0x1C);
    static_assert(offsetof(ActorSetLocationAndRotationParams, teleport) == 0xB4);
    static_assert(offsetof(ActorSetLocationAndRotationParams, return_value) == 0xB5);
    static_assert(sizeof(ActorSetLocationAndRotationParams) == 0xB8);

    struct alignas(16) ActorSetTransformParams
    {
        RC::Unreal::FTransform new_transform{};
        bool sweep{};
        std::uint8_t pad_sweep[0x03]{};
        FHitResultAbi sweep_hit_result{};
        bool teleport{};
        bool return_value{};
        std::uint8_t pad_return[0x02]{};
    };

    static_assert(offsetof(ActorSetTransformParams, sweep) == 0x30);
    static_assert(offsetof(ActorSetTransformParams, sweep_hit_result) == 0x34);
    static_assert(offsetof(ActorSetTransformParams, teleport) == 0xCC);
    static_assert(offsetof(ActorSetTransformParams, return_value) == 0xCD);
    static_assert(sizeof(ActorSetTransformParams) == 0xD0);

    struct SceneComponentSetWorldLocationAndRotationParams
    {
        RC::Unreal::FVector new_location{};
        RC::Unreal::FRotator new_rotation{};
        bool sweep{};
        std::uint8_t pad_sweep[0x03]{};
        FHitResultAbi sweep_hit_result{};
        bool teleport{};
        std::uint8_t pad_return[0x03]{};
    };

    static_assert(offsetof(SceneComponentSetWorldLocationAndRotationParams, new_rotation) == 0x0C);
    static_assert(offsetof(SceneComponentSetWorldLocationAndRotationParams, sweep) == 0x18);
    static_assert(offsetof(SceneComponentSetWorldLocationAndRotationParams, sweep_hit_result) == 0x1C);
    static_assert(offsetof(SceneComponentSetWorldLocationAndRotationParams, teleport) == 0xB4);
    static_assert(sizeof(SceneComponentSetWorldLocationAndRotationParams) == 0xB8);

    auto narrow(const std::wstring& value) -> std::string
    {
        if (value.empty()) return {};
        const int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (needed <= 0) return {};
        std::string out(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), needed, nullptr, nullptr);
        return out;
    }

    auto widen(const std::string& value) -> std::wstring
    {
        if (value.empty()) return {};
        const int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
        if (needed <= 0) return {};
        std::wstring out(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), needed);
        return out;
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

    auto json_escape(const std::string& value) -> std::string
    {
        std::string out;
        out.reserve(value.size() + 16);
        for (const auto ch : value)
        {
            switch (ch)
            {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20)
                {
                    std::ostringstream ss;
                    ss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(ch));
                    out += ss.str();
                }
                else
                {
                    out += ch;
                }
            }
        }
        return out;
    }

    auto regex_value(const std::string& text, const char* key) -> std::string
    {
        const std::regex pattern(std::string("\\\"") + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
        std::smatch match;
        if (std::regex_search(text, match, pattern) && match.size() > 1) return match[1].str();
        return {};
    }

    auto regex_number(const std::string& text, const char* key, double fallback) -> double
    {
        const std::regex pattern(std::string("\\\"") + key + "\\\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)");
        std::smatch match;
        if (!std::regex_search(text, match, pattern) || match.size() <= 1) return fallback;
        try
        {
            return std::stod(match[1].str());
        }
        catch (...)
        {
            return fallback;
        }
    }

    auto regex_number_value(const std::string& text, const char* key, double& value) -> bool
    {
        const std::regex pattern(std::string("\\\"") + key + "\\\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)");
        std::smatch match;
        if (!std::regex_search(text, match, pattern) || match.size() <= 1) return false;
        try
        {
            value = std::stod(match[1].str());
            return std::isfinite(value);
        }
        catch (...)
        {
            return false;
        }
    }

    auto lower_copy(std::string value) -> std::string
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }

    auto regex_bool(const std::string& text, const char* key, bool fallback) -> bool
    {
        const std::regex pattern(std::string("\\\"") + key + "\\\"\\s*:\\s*(true|false|1|0|\\\"true\\\"|\\\"false\\\"|\\\"1\\\"|\\\"0\\\")", std::regex_constants::icase);
        std::smatch match;
        if (!std::regex_search(text, match, pattern) || match.size() <= 1) return fallback;
        auto value = lower_copy(match[1].str());
        value.erase(std::remove(value.begin(), value.end(), '"'), value.end());
        if (value == "true" || value == "1") return true;
        if (value == "false" || value == "0") return false;
        return fallback;
    }

    auto contains_any(const std::string& haystack, std::initializer_list<const char*> needles) -> bool
    {
        for (const auto* needle : needles)
        {
            if (haystack.find(needle) != std::string::npos) return true;
        }
        return false;
    }

    auto hex_address(std::uintptr_t value) -> std::string
    {
        std::ostringstream ss;
        ss << "0x" << std::hex << value;
        return ss.str();
    }

    auto guid_to_string(const FGuidAbi* guid) -> std::string
    {
        if (guid == nullptr) return {};
        std::ostringstream ss;
        ss << std::uppercase << std::hex << std::setfill('0')
           << std::setw(8) << guid->a
           << std::setw(8) << guid->b
           << std::setw(8) << guid->c
           << std::setw(8) << guid->d;
        return ss.str();
    }

    auto is_reasonable_array(const TArrayAbi* array) -> bool
    {
        if (array == nullptr) return false;
        if (array->count < 0 || array->max < 0) return false;
        if (array->count > array->max) return false;
        return array->count < 10000;
    }

    auto full_name(RC::Unreal::UObject* object) -> std::string
    {
        if (object == nullptr) return {};
        return narrow(object->GetFullName(nullptr));
    }

    auto committed_readable_address(std::uintptr_t address, std::size_t size = sizeof(std::uintptr_t)) -> bool;

    auto safe_full_name(RC::Unreal::UObject* object) -> std::string
    {
        if (object == nullptr) return {};
        if (!committed_readable_address(reinterpret_cast<std::uintptr_t>(object)))
        {
            return "<GetFullName unreadable>";
        }
        try
        {
            return full_name(object);
        }
        catch (...)
        {
            return "<GetFullName exception>";
        }
    }

    template <typename T>
    auto safe_read(std::uintptr_t address, T& value) -> bool
    {
        if (address == 0) return false;
        __try
        {
            std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    template <typename T>
    auto safe_write(std::uintptr_t address, const T& value) -> bool
    {
        if (address == 0) return false;
        __try
        {
            std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    auto safe_copy_from_address(void* destination, std::uintptr_t source, std::size_t size) -> bool
    {
        if (destination == nullptr || source == 0 || size == 0) return false;
        __try
        {
            std::memcpy(destination, reinterpret_cast<const void*>(source), size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    auto safe_read_pointer(std::uintptr_t address, std::uintptr_t& value) -> bool
    {
        return safe_read(address, value);
    }

    auto committed_readable_address(std::uintptr_t address, std::size_t size) -> bool
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

    auto executable_address(std::uintptr_t address) -> bool
    {
        if (address == 0) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if ((mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & PAGE_NOACCESS) != 0) return false;
        const auto protect = mbi.Protect & 0xff;
        return protect == PAGE_EXECUTE ||
               protect == PAGE_EXECUTE_READ ||
               protect == PAGE_EXECUTE_READWRITE ||
               protect == PAGE_EXECUTE_WRITECOPY;
    }

    auto capture_exception_code(EXCEPTION_POINTERS* exception_info, unsigned long& exception_code) -> int
    {
        exception_code = 0;
        if (exception_info != nullptr && exception_info->ExceptionRecord != nullptr)
        {
            exception_code = exception_info->ExceptionRecord->ExceptionCode;
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }

    auto process_event_from_vtable(RC::Unreal::UObject* object, std::string& detail) -> ProcessEventFn
    {
        detail.clear();
        const auto object_address = reinterpret_cast<std::uintptr_t>(object);
        if (object_address == 0)
        {
            detail = "object-null";
            return nullptr;
        }
        if (!committed_readable_address(object_address))
        {
            detail = "object-unreadable:" + hex_address(object_address);
            return nullptr;
        }

        std::uintptr_t vtable{};
        if (!safe_read_pointer(object_address, vtable) ||
            !committed_readable_address(vtable, (process_event_vtable_index + 1) * sizeof(std::uintptr_t)))
        {
            detail = "vtable-unreadable:" + hex_address(vtable);
            return nullptr;
        }

        std::uintptr_t slot_value{};
        const auto slot_address = vtable + (process_event_vtable_index * sizeof(std::uintptr_t));
        if (!safe_read_pointer(slot_address, slot_value) || slot_value == 0)
        {
            detail = "slot-unreadable:" + hex_address(slot_address);
            return nullptr;
        }
        if (!executable_address(slot_value))
        {
            detail = "slot-not-executable:" + hex_address(slot_value);
            return nullptr;
        }

        std::ostringstream ss;
        ss << "object=" << hex_address(object_address)
           << ",vtable=" << hex_address(vtable)
           << ",slot=" << std::dec << process_event_vtable_index
           << ",processEvent=" << hex_address(slot_value);
        detail = ss.str();
        return reinterpret_cast<ProcessEventFn>(slot_value);
    }

    auto process_event_from_ue4ss_log(const fs::path& win64_dir, std::string& detail) -> ProcessEventFn
    {
        detail.clear();
        const auto path = win64_dir / L"UE4SS.log";
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            detail = "ue4ss-log-open-failed";
            return nullptr;
        }

        std::string line;
        std::string last_address_text;
        while (std::getline(file, line))
        {
            const auto pos = line.find("ProcessEvent address 0x");
            if (pos == std::string::npos) continue;
            last_address_text = line.substr(pos + std::string("ProcessEvent address ").size());
        }

        if (last_address_text.empty())
        {
            detail = "ue4ss-log-processevent-address-missing";
            return nullptr;
        }

        std::uintptr_t address{};
        std::istringstream stream(last_address_text);
        stream >> std::hex >> address;
        if (address == 0)
        {
            detail = "ue4ss-log-processevent-address-parse-failed:" + last_address_text;
            return nullptr;
        }
        if (!executable_address(address))
        {
            detail = "ue4ss-log-processevent-not-executable:" + hex_address(address);
            return nullptr;
        }

        detail = "ue4ss-log-process-event=" + hex_address(address);
        return reinterpret_cast<ProcessEventFn>(address);
    }

    auto actor_location_noexcept(RC::Unreal::UObject* object, RC::Unreal::FVector& location) -> bool
    {
        __try
        {
            location = static_cast<RC::Unreal::AActor*>(object)->K2_GetActorLocation();
            return std::isfinite(location.x) &&
                   std::isfinite(location.y) &&
                   std::isfinite(location.z) &&
                   std::abs(static_cast<double>(location.x)) < 100000000.0 &&
                   std::abs(static_cast<double>(location.y)) < 100000000.0 &&
                   std::abs(static_cast<double>(location.z)) < 100000000.0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    auto actor_rotation_noexcept(RC::Unreal::UObject* object, RC::Unreal::FRotator& rotation) -> bool
    {
        __try
        {
            rotation = static_cast<RC::Unreal::AActor*>(object)->K2_GetActorRotation();
            return std::isfinite(rotation.pitch) &&
                   std::isfinite(rotation.yaw) &&
                   std::isfinite(rotation.roll);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    auto safe_class_private(RC::Unreal::UObject* object) -> RC::Unreal::UClass*;

    auto object_class_full_name_noexcept(RC::Unreal::UObject* object) -> std::string
    {
        if (object == nullptr) return {};
        auto* object_class = safe_class_private(object);
        if (object_class == nullptr) return "<GetClass exception>";
        return safe_full_name(object_class);
    }

    auto invoke_process_event_noexcept(
        ProcessEventFn process_event,
        RC::Unreal::UObject* object,
        RC::Unreal::UObject* function,
        void* params,
        unsigned long& exception_code) -> bool
    {
        exception_code = 0;
        __try
        {
            process_event(object, function, params);
            return true;
        }
        __except (capture_exception_code(GetExceptionInformation(), exception_code))
        {
            return false;
        }
    }

    auto object_leaf_from_full_name(const std::string& full_name_value) -> std::string
    {
        auto value = full_name_value;
        const auto space = value.find(' ');
        if (space != std::string::npos && space + 1 < value.size())
        {
            value = value.substr(space + 1);
        }
        const auto colon = value.find_last_of(':');
        const auto dot = value.find_last_of('.');
        const auto slash = value.find_last_of("/\\");
        const auto pos = std::max(colon == std::string::npos ? 0 : colon,
                                  std::max(dot == std::string::npos ? 0 : dot,
                                           slash == std::string::npos ? 0 : slash));
        if (pos == 0 && value.find_first_of(":.\\/") == std::string::npos) return value;
        return pos + 1 < value.size() ? value.substr(pos + 1) : value;
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

    auto class_token_from_full_name(const std::string& full_name_value) -> std::string
    {
        const auto space = full_name_value.find(' ');
        if (space == std::string::npos || space == 0) return {};
        return full_name_value.substr(0, space);
    }

    auto push_unique_class_search_name(std::vector<std::wstring>& values, const std::wstring& value) -> void
    {
        if (value.empty()) return;
        if (std::find(values.begin(), values.end(), value) == values.end())
        {
            values.push_back(value);
        }
    }

    auto build_class_search_names(
        std::initializer_list<const wchar_t*> class_names,
        const std::string& requested_full_name) -> std::vector<std::wstring>
    {
        std::vector<std::wstring> values;
        push_unique_class_search_name(values, widen(class_token_from_full_name(requested_full_name)));
        for (const auto* class_name : class_names)
        {
            push_unique_class_search_name(values, class_name == nullptr ? std::wstring{} : std::wstring(class_name));
        }
        return values;
    }

    auto object_path_matches(const std::string& candidate_full_name, const std::string& requested_path) -> bool
    {
        return !requested_path.empty() && object_path_from_full_name(candidate_full_name) == requested_path;
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

    auto find_object_by_full_name(
        std::initializer_list<const wchar_t*> class_names,
        const std::string& requested_full_name,
        std::vector<std::string>& attempts,
        const char* label,
        bool allow_leaf_lookup = true) -> RC::Unreal::UObject*
    {
        if (requested_full_name.empty())
        {
            attempts.push_back(std::string(label) + "=empty");
            return nullptr;
        }

        const auto path = object_path_from_full_name(requested_full_name);
        const auto leaf = object_leaf_from_full_name(requested_full_name);
        const auto path_w = widen(path);
        const auto leaf_w = widen(leaf);
        const auto search_class_names = build_class_search_names(class_names, requested_full_name);

        for (const auto& class_name : search_class_names)
        {
            if (!path_w.empty())
            {
                auto* object = RC::Unreal::UObjectGlobals::FindObject(class_name.c_str(), path_w.c_str(), 0, object_flag_class_default_object);
                attempts.push_back(std::string(label) + ".FindObject(" + narrow(class_name) + "," + path + ")=" + (object != nullptr ? "1" : "0"));
                if (object != nullptr) return object;
            }
        }

        if (!allow_leaf_lookup)
        {
            attempts.push_back(std::string(label) + ".FindObjects(leaf)=disabled-after-2026-06-05-actor-editor-crash");
            return nullptr;
        }

        for (const auto& class_name : search_class_names)
        {
            if (leaf_w.empty()) continue;
            std::vector<RC::Unreal::UObject*> found;
            RC::Unreal::UObjectGlobals::FindObjects(class_name.c_str(), leaf_w.c_str(), found, 0, object_flag_class_default_object, false);
            attempts.push_back(std::string(label) + ".FindObjects(" + narrow(class_name) + "," + leaf + ")=" + std::to_string(found.size()));
            for (auto* object : found)
            {
                if (object == nullptr) continue;
                const auto candidate_full = safe_full_name(object);
                if (candidate_full == requested_full_name || object_path_matches(candidate_full, path)) return object;
            }
        }

        return nullptr;
    }

    auto object_from_lua_address_guarded(
        const std::string& address_text,
        const std::string& requested_full_name,
        std::vector<std::string>& attempts,
        const char* label) -> RC::Unreal::UObject*
    {
        const auto address = parse_uobject_address(address_text);
        if (address == 0)
        {
            attempts.push_back(std::string(label) + ".address=empty");
            return nullptr;
        }

        if (!committed_readable_address(address))
        {
            attempts.push_back(std::string(label) + ".address=unreadable");
            return nullptr;
        }

        auto* object = reinterpret_cast<RC::Unreal::UObject*>(address);
        const auto full_name = safe_full_name(object);
        const auto matches = object_full_name_matches(full_name, requested_full_name);
        attempts.push_back(std::string(label) + ".address(" + address_text + ")=" + (matches ? "matched:" : "mismatch:") + full_name);
        if (!matches) return nullptr;
        return object;
    }

    auto resolve_function_by_name(
        const std::string& owner_token,
        const std::string& function_name,
        std::initializer_list<const char*> exact_paths,
        std::vector<std::string>& attempts,
        const bool allow_find_objects = true) -> RC::Unreal::UObject*
    {
        for (const auto* raw_path : exact_paths)
        {
            const auto path = widen(raw_path);
            for (const auto* class_name : {L"Function", L"UFunction", L"Object"})
            {
                auto* object = RC::Unreal::UObjectGlobals::FindObject(class_name, path.c_str(), 0, object_flag_class_default_object);
                attempts.push_back("function.FindObject(" + narrow(class_name) + "," + raw_path + ")=" + (object != nullptr ? safe_full_name(object) : "0"));
                if (object != nullptr) return object;
            }
        }

        if (!allow_find_objects)
        {
            attempts.push_back("function.FindObjects=blocked-for-exact-route");
            return nullptr;
        }

        const auto function_w = widen(function_name);
        const auto owner_lower = lower_copy(owner_token);
        const auto function_lower = lower_copy(function_name);
        for (const auto* class_name : {L"Function", L"UFunction", L"Object"})
        {
            std::vector<RC::Unreal::UObject*> found;
            RC::Unreal::UObjectGlobals::FindObjects(class_name, function_w.c_str(), found, 0, object_flag_class_default_object, false);
            attempts.push_back("function.FindObjects(" + narrow(class_name) + "," + function_name + ")=" + std::to_string(found.size()));
            for (auto* object : found)
            {
                const auto object_name = lower_copy(full_name(object));
                if (object_name.find(owner_lower) != std::string::npos &&
                    object_name.find(function_lower) != std::string::npos)
                {
                    return object;
                }
            }
        }
        return nullptr;
    }

    auto module_base() -> std::uintptr_t
    {
        return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    }

    auto module_size(std::uintptr_t base) -> std::size_t
    {
        if (base == 0) return 0;
        __try
        {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
            return nt->OptionalHeader.SizeOfImage;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    auto find_pattern(std::uintptr_t base, std::size_t size, const char* pattern, const char* mask) -> std::uintptr_t
    {
        if (base == 0 || size == 0 || pattern == nullptr || mask == nullptr) return 0;
        const auto pattern_len = std::strlen(mask);
        if (pattern_len == 0 || size < pattern_len) return 0;
        __try
        {
            const auto* bytes = reinterpret_cast<const unsigned char*>(base);
            for (std::size_t index = 0; index + pattern_len <= size; ++index)
            {
                bool matched = true;
                for (std::size_t pattern_index = 0; pattern_index < pattern_len; ++pattern_index)
                {
                    if (mask[pattern_index] != '?' && bytes[index + pattern_index] != static_cast<unsigned char>(pattern[pattern_index]))
                    {
                        matched = false;
                        break;
                    }
                }
                if (matched) return base + index;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
        return 0;
    }

    auto is_reasonable_object_array(const TUObjectArrayAbi& object_array) -> bool
    {
        if (object_array.objects == 0) return false;
        if (object_array.max_elements <= 0 || object_array.num_elements <= 0 || object_array.num_elements > object_array.max_elements) return false;
        if (object_array.max_chunks <= 0 || object_array.num_chunks <= 0 || object_array.num_chunks > object_array.max_chunks) return false;
        if (object_array.max_chunks > 0x2000 || object_array.num_chunks > 0x2000) return false;
        std::uintptr_t first_chunk{};
        return safe_read_pointer(object_array.objects, first_chunk) && first_chunk != 0;
    }

    auto read_object_array_from_gobjects(std::uintptr_t gobjects, TUObjectArrayAbi& object_array, std::uintptr_t& used_offset) -> bool
    {
        object_array = {};
        used_offset = 0;
        if (gobjects == 0) return false;

        for (const auto offset : {std::uintptr_t{0x10}, std::uintptr_t{0x0}, std::uintptr_t{0x8}, std::uintptr_t{0x18}})
        {
            TUObjectArrayAbi candidate{};
            if (!safe_read(gobjects + offset, candidate)) continue;
            if (!is_reasonable_object_array(candidate)) continue;
            object_array = candidate;
            used_offset = offset;
            return true;
        }
        return false;
    }

    auto is_probably_guobject_array(std::uintptr_t candidate) -> bool
    {
        TUObjectArrayAbi object_array{};
        std::uintptr_t used_offset{};
        return read_object_array_from_gobjects(candidate, object_array, used_offset);
    }

    auto guobject_array_from_ue4ss_log() -> std::uintptr_t
    {
        wchar_t module_path[MAX_PATH]{};
        const auto ue4ss = GetModuleHandleW(L"UE4SS.dll");
        if (ue4ss == nullptr || GetModuleFileNameW(ue4ss, module_path, MAX_PATH) == 0) return 0;

        auto log_path_w = std::wstring(module_path);
        const auto slash = log_path_w.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return 0;
        log_path_w = log_path_w.substr(0, slash + 1) + L"UE4SS.log";

        std::ifstream input(log_path_w);
        if (!input) return 0;

        std::uintptr_t last_address = 0;
        std::string line;
        const std::regex pattern(R"(Found GUObjectArray:\s*0x([0-9A-Fa-f]+))");
        while (std::getline(input, line))
        {
            std::smatch match;
            if (!std::regex_search(line, match, pattern) || match.size() < 2) continue;
            try
            {
                const auto parsed = static_cast<std::uintptr_t>(std::stoull(match[1].str(), nullptr, 16));
                if (is_probably_guobject_array(parsed)) last_address = parsed;
            }
            catch (...)
            {
            }
        }
        return last_address;
    }

    auto find_guobject_array() -> std::uintptr_t
    {
        static std::uintptr_t cached{};
        if (cached != 0 && is_probably_guobject_array(cached)) return cached;

        if (const auto from_log = guobject_array_from_ue4ss_log(); from_log != 0)
        {
            cached = from_log;
            return cached;
        }

        const auto base = module_base();
        if (base == 0) return 0;

        for (const auto rva : {std::uintptr_t{0x07071A10}, std::uintptr_t{0x0767D190}})
        {
            const auto candidate = base + rva;
            if (is_probably_guobject_array(candidate))
            {
                cached = candidate;
                return cached;
            }
        }

        const auto size = module_size(base);
        const auto pattern = "\x48\x8B\x05\x00\x00\x00\x00\x48\x8B\x0C\xC8";
        const auto mask = "xxx????xxxx";
        const auto match = find_pattern(base, size, pattern, mask);
        if (match != 0)
        {
            std::int32_t relative{};
            if (safe_read(match + 3, relative))
            {
                const auto rip_target = match + 7 + static_cast<std::intptr_t>(relative);
                std::array<std::uintptr_t, 3> candidates{
                    rip_target,
                    rip_target > 0x10 ? rip_target - 0x10 : 0,
                    0};
                std::uintptr_t dereferenced{};
                if (safe_read_pointer(rip_target, dereferenced))
                {
                    candidates[2] = dereferenced;
                }
                for (const auto candidate : candidates)
                {
                    if (candidate != 0 && is_probably_guobject_array(candidate))
                    {
                        cached = candidate;
                        return cached;
                    }
                    if (candidate > 0x10 && is_probably_guobject_array(candidate - 0x10))
                    {
                        cached = candidate - 0x10;
                        return cached;
                    }
                }
            }
        }

        return 0;
    }

    auto read_uobject_item_by_index(
        std::int32_t object_index,
        FUObjectItemAbi& item,
        std::string& detail,
        std::uintptr_t* item_address_out = nullptr,
        std::uintptr_t expected_object = 0) -> bool
    {
        if (item_address_out != nullptr) *item_address_out = 0;
        if (object_index < 0)
        {
            detail = "negative object index";
            return false;
        }

        const auto gobjects = find_guobject_array();
        if (gobjects == 0)
        {
            detail = "GUObjectArray unresolved";
            return false;
        }

        TUObjectArrayAbi object_array{};
        std::uintptr_t used_offset{};
        if (!read_object_array_from_gobjects(gobjects, object_array, used_offset))
        {
            detail = "GUObjectArray view unreadable at 0x" + [] (std::uintptr_t value) {
                std::ostringstream ss;
                ss << std::hex << value;
                return ss.str();
            }(gobjects);
            return false;
        }

        if (object_index >= object_array.num_elements)
        {
            detail = "object index outside GUObjectArray";
            return false;
        }

        struct ItemCandidate
        {
            std::string layout;
            std::uintptr_t address{};
        };

        std::vector<ItemCandidate> candidates;
        constexpr std::int32_t elements_per_chunk = 0x10000;
        const auto chunk_index = object_index / elements_per_chunk;
        const auto in_chunk_index = object_index % elements_per_chunk;
        if (chunk_index >= 0 && chunk_index < object_array.num_chunks)
        {
            std::uintptr_t chunk{};
            if (safe_read_pointer(object_array.objects + (static_cast<std::uintptr_t>(chunk_index) * sizeof(std::uintptr_t)), chunk) && chunk != 0)
            {
                candidates.push_back(ItemCandidate{
                    "chunked",
                    chunk + (static_cast<std::uintptr_t>(in_chunk_index) * sizeof(FUObjectItemAbi))});
            }
        }
        candidates.push_back(ItemCandidate{
            "direct",
            object_array.objects + (static_cast<std::uintptr_t>(object_index) * sizeof(FUObjectItemAbi))});

        std::string attempts;
        FUObjectItemAbi first_nonzero_item{};
        std::uintptr_t first_nonzero_address{};
        std::string first_nonzero_detail;
        bool has_first_nonzero = false;
        for (const auto& candidate : candidates)
        {
            FUObjectItemAbi candidate_item{};
            const auto readable = safe_read(candidate.address, candidate_item);
            std::ostringstream ss;
            ss << candidate.layout
               << ":item=" << hex_address(candidate.address)
               << (readable
                   ? (",object=" + hex_address(candidate_item.object) + ",serial=" + std::to_string(candidate_item.serial_number))
                   : ",unreadable");
            if (!attempts.empty()) attempts += "; ";
            attempts += ss.str();
            if (!readable || candidate_item.object == 0) continue;

            if (!has_first_nonzero)
            {
                first_nonzero_item = candidate_item;
                first_nonzero_address = candidate.address;
                first_nonzero_detail = ss.str();
                has_first_nonzero = true;
            }
            if (expected_object != 0 && candidate_item.object == expected_object)
            {
                item = candidate_item;
                if (item_address_out != nullptr) *item_address_out = candidate.address;
                std::ostringstream detail_ss;
                detail_ss << "gobjects=" << hex_address(gobjects)
                          << ", objArrayOffset=" << hex_address(used_offset)
                          << std::dec << ", numElements=" << object_array.num_elements
                          << ", numChunks=" << object_array.num_chunks
                          << ", chunk=" << chunk_index
                          << ", inChunk=" << in_chunk_index
                          << ", matched=" << ss.str();
                detail = detail_ss.str();
                return true;
            }
        }

        if (expected_object == 0 && has_first_nonzero)
        {
            item = first_nonzero_item;
            if (item_address_out != nullptr) *item_address_out = first_nonzero_address;
            std::ostringstream detail_ss;
            detail_ss << "gobjects=" << hex_address(gobjects)
                      << ", objArrayOffset=" << hex_address(used_offset)
                      << std::dec << ", numElements=" << object_array.num_elements
                      << ", numChunks=" << object_array.num_chunks
                      << ", fallback=" << first_nonzero_detail;
            detail = detail_ss.str();
            return true;
        }

        std::ostringstream detail_ss;
        detail_ss << "object item not found: gobjects=" << hex_address(gobjects)
                  << ", objArrayOffset=" << hex_address(used_offset)
                  << std::dec << ", numElements=" << object_array.num_elements
                  << ", numChunks=" << object_array.num_chunks
                  << ", chunk=" << chunk_index
                  << ", inChunk=" << in_chunk_index
                  << ", expected=" << hex_address(expected_object)
                  << ", attempts=[" << attempts << "]";
        detail = detail_ss.str();
        return false;
    }

    auto read_uobject_item_from_array(const TUObjectArrayAbi& object_array, std::int32_t object_index, FUObjectItemAbi& item) -> bool
    {
        item = {};
        if (object_index < 0 || object_index >= object_array.num_elements) return false;

        constexpr std::int32_t elements_per_chunk = 0x10000;
        const auto chunk_index = object_index / elements_per_chunk;
        const auto in_chunk_index = object_index % elements_per_chunk;
        if (chunk_index >= 0 && chunk_index < object_array.num_chunks)
        {
            std::uintptr_t chunk{};
            if (safe_read_pointer(object_array.objects + (static_cast<std::uintptr_t>(chunk_index) * sizeof(std::uintptr_t)), chunk) && chunk != 0)
            {
                FUObjectItemAbi candidate{};
                if (safe_read(chunk + (static_cast<std::uintptr_t>(in_chunk_index) * sizeof(FUObjectItemAbi)), candidate) && candidate.object != 0)
                {
                    item = candidate;
                    return true;
                }
            }
        }

        FUObjectItemAbi direct{};
        if (safe_read(object_array.objects + (static_cast<std::uintptr_t>(object_index) * sizeof(FUObjectItemAbi)), direct) && direct.object != 0)
        {
            item = direct;
            return true;
        }
        return false;
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

    auto actor_world_noexcept(RC::Unreal::AActor* actor) -> RC::Unreal::UWorld*
    {
        if (actor == nullptr) return nullptr;
        __try
        {
            return actor->GetWorld();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    auto looks_like_scan_relevant_class(const std::string& class_name_lower) -> bool
    {
        return contains_any(
            class_name_lower,
            {
                "class /script/coreuobject.class",
                "class /script/coreuobject.function",
                "class /script/coreuobject.scriptstruct",
                "blueprintgeneratedclass",
                "trader",
                "trade",
                "outpost",
                "economy",
                "rpcchannel",
                "datatable",
                "dataasset",
                "npcinteractionbox",
                "interactionbox"});
    }

    auto append_json_string_array(std::ostringstream& ss, const std::vector<std::string>& values) -> void;

    struct ObjectScanBucket
    {
        std::string key;
        std::vector<std::string> needles;
        std::int32_t count{};
        std::vector<std::string> samples;
    };

    auto bucket_matches(const ObjectScanBucket& bucket, const std::string& text_lower) -> bool
    {
        for (const auto& needle : bucket.needles)
        {
            if (!needle.empty() && text_lower.find(needle) != std::string::npos) return true;
        }
        return false;
    }

    auto append_scan_bucket_json(std::ostringstream& ss, const ObjectScanBucket& bucket) -> void
    {
        ss << "{\"count\":" << bucket.count << ",\"samples\":";
        append_json_string_array(ss, bucket.samples);
        ss << "}";
    }

    auto next_manual_weak_serial(std::int32_t object_index) -> LONG
    {
        static volatile LONG counter = 0x30000000;
        const auto serial = InterlockedIncrement(&counter);
        if (serial > 0) return serial;
        return static_cast<LONG>(0x10000000 + (object_index & 0x0FFFFFFF));
    }

    auto ensure_uobject_item_serial(std::uintptr_t item_address, std::int32_t object_index, FUObjectItemAbi& item, std::string& detail) -> bool
    {
        if (item.serial_number > 0)
        {
            detail = "existing serial=" + std::to_string(item.serial_number);
            return true;
        }
        if (item_address == 0)
        {
            detail = "item address is null";
            return false;
        }

        auto* serial_ptr = reinterpret_cast<volatile LONG*>(item_address + offsetof(FUObjectItemAbi, serial_number));
        const auto proposed = next_manual_weak_serial(object_index);
        const auto previous = InterlockedCompareExchange(serial_ptr, proposed, 0);
        const auto serial = previous == 0 ? proposed : previous;
        if (serial <= 0)
        {
            detail = "serial allocation failed: previous=" + std::to_string(previous) + ", proposed=" + std::to_string(proposed);
            return false;
        }

        item.serial_number = serial;
        detail = previous == 0
            ? "allocated serial=" + std::to_string(serial)
            : "raced existing serial=" + std::to_string(serial);
        return true;
    }

    auto build_weak_object_ptr(RC::Unreal::UObject* value, RC::Unreal::FWeakObjectPtr& weak, std::string& detail) -> bool
    {
        weak.object_index = -1;
        weak.object_serial_number = 0;
        if (value == nullptr)
        {
            detail = "value is null";
            return false;
        }

        std::int32_t object_index = -1;
        if (!safe_read(reinterpret_cast<std::uintptr_t>(value) + 0x0C, object_index) || object_index < 0)
        {
            detail = "UObject.InternalIndex unreadable";
            return false;
        }

        FUObjectItemAbi item{};
        std::uintptr_t item_address{};
        std::string item_detail;
        if (!read_uobject_item_by_index(object_index, item, item_detail, &item_address, reinterpret_cast<std::uintptr_t>(value)))
        {
            detail = "UObject item unresolved: " + item_detail;
            return false;
        }
        if (item.object != reinterpret_cast<std::uintptr_t>(value))
        {
            std::ostringstream ss;
            ss << "UObject item mismatch: " << item_detail
               << ", expected=0x" << std::hex << reinterpret_cast<std::uintptr_t>(value)
               << ", got=0x" << item.object;
            detail = ss.str();
            return false;
        }

        std::string serial_detail;
        if (!ensure_uobject_item_serial(item_address, object_index, item, serial_detail))
        {
            detail = "UObject item serial unresolved: " + item_detail + ", " + serial_detail;
            return false;
        }

        weak.object_index = object_index;
        weak.object_serial_number = item.serial_number;
        detail = "manual weak ok: " + item_detail + ", " + serial_detail;
        return true;
    }

    auto object_property(RC::Unreal::UObject* object, const wchar_t* property_name) -> RC::Unreal::UObject*
    {
        if (object == nullptr) return nullptr;
        auto* value = static_cast<RC::Unreal::UObject**>(object->GetValuePtrByPropertyNameInChain(property_name));
        return value == nullptr ? nullptr : *value;
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

    auto object_property_noexcept(RC::Unreal::UObject* object, const wchar_t* property_name, std::vector<std::string>& details) -> RC::Unreal::UObject*
    {
        if (object == nullptr)
        {
            details.push_back("property " + narrow(property_name) + "=skipped:object-null");
            return nullptr;
        }
        bool missing = false;
        bool exception = false;
        auto* result = object_property_noexcept_raw(object, property_name, missing, exception);
        if (exception)
        {
            details.push_back("property " + narrow(property_name) + "=exception");
            return nullptr;
        }
        if (missing)
        {
            details.push_back("property " + narrow(property_name) + "=missing");
            return nullptr;
        }
        details.push_back("property " + narrow(property_name) + "=" + safe_full_name(result));
        return result;
    }

    auto string_property(RC::Unreal::UObject* object, const wchar_t* property_name) -> std::string
    {
        if (object == nullptr) return {};
        auto* value = static_cast<const TArrayAbi*>(object->GetValuePtrByPropertyNameInChain(property_name));
        if (!is_reasonable_array(value) || value->data == nullptr || value->count <= 0) return {};
        constexpr std::int32_t max_chars = 4096;
        const auto count = std::min(value->count, max_chars);
        const auto bytes = static_cast<std::size_t>(count) * sizeof(wchar_t);
        if (!committed_readable_address(reinterpret_cast<std::uintptr_t>(value->data), bytes)) return "<unreadable>";
        const auto* chars = static_cast<const wchar_t*>(value->data);
        std::int32_t length = 0;
        while (length < count && chars[length] != L'\0') ++length;
        return narrow(std::wstring(chars, chars + length));
    }

    auto set_object_property(RC::Unreal::UObject* object, const wchar_t* property_name, RC::Unreal::UObject* value, std::string& detail) -> bool
    {
        if (object == nullptr)
        {
            detail = "object is null";
            return false;
        }
        auto* property = static_cast<RC::Unreal::UObject**>(object->GetValuePtrByPropertyNameInChain(property_name));
        if (property == nullptr)
        {
            detail = "property missing: " + narrow(property_name);
            return false;
        }
        *property = value;
        detail = "property set: " + narrow(property_name);
        return true;
    }

    auto vector_property(RC::Unreal::UObject* object, const wchar_t* property_name, RC::Unreal::FVector& value) -> bool
    {
        if (object == nullptr) return false;
        auto* property = static_cast<RC::Unreal::FVector*>(object->GetValuePtrByPropertyNameInChain(property_name));
        if (property == nullptr) return false;
        value = *property;
        return true;
    }

    auto rotator_property(RC::Unreal::UObject* object, const wchar_t* property_name, RC::Unreal::FRotator& value) -> bool
    {
        if (object == nullptr) return false;
        auto* property = static_cast<RC::Unreal::FRotator*>(object->GetValuePtrByPropertyNameInChain(property_name));
        if (property == nullptr) return false;
        value = *property;
        return true;
    }

    auto weak_object_property(RC::Unreal::UObject* object, const wchar_t* property_name) -> RC::Unreal::UObject*
    {
        if (object == nullptr) return nullptr;
        auto* property = static_cast<RC::Unreal::FWeakObjectPtr*>(object->GetValuePtrByPropertyNameInChain(property_name));
        return property == nullptr ? nullptr : property->Get();
    }

    auto set_weak_object_property(RC::Unreal::UObject* object, const wchar_t* property_name, RC::Unreal::UObject* value, std::string& detail) -> bool
    {
        if (object == nullptr)
        {
            detail = "object is null";
            return false;
        }
        auto* property = static_cast<RC::Unreal::FWeakObjectPtr*>(object->GetValuePtrByPropertyNameInChain(property_name));
        if (property == nullptr)
        {
            detail = "weak property missing: " + narrow(property_name);
            return false;
        }

        auto weak_raw = [](const RC::Unreal::FWeakObjectPtr* weak) -> std::string
        {
            if (weak == nullptr) return "null";
            return "index=" + std::to_string(weak->object_index) + ",serial=" + std::to_string(weak->object_serial_number);
        };

        const auto before_raw = weak_raw(property);
        property->operator=(value);
        auto* resolved = property->Get();
        const auto direct_raw = weak_raw(property);
        std::string temp_detail = "skipped";
        std::string manual_detail = "skipped";

        if (resolved != value)
        {
            RC::Unreal::FWeakObjectPtr temp{};
            temp.object_index = -1;
            temp.object_serial_number = 0;
            temp.operator=(value);
            auto* temp_resolved = temp.Get();
            temp_detail = "tempRaw=" + weak_raw(&temp) + ", tempResolved=" + full_name(temp_resolved);
            if (temp_resolved == value)
            {
                *property = temp;
                resolved = property->Get();
            }
        }

        if (resolved != value)
        {
            RC::Unreal::FWeakObjectPtr manual{};
            if (build_weak_object_ptr(value, manual, manual_detail))
            {
                *property = manual;
                resolved = property->Get();
                if (resolved != value)
                {
                    auto verify = property->Get();
                    manual_detail += ", ue4ssGetAfterManual=" + full_name(verify);
                }
            }
        }

        detail = "weak property set: " + narrow(property_name)
            + ", beforeRaw=" + before_raw
            + ", directRaw=" + direct_raw
            + ", fallback=" + temp_detail
            + ", manual=" + manual_detail
            + ", finalRaw=" + weak_raw(property)
            + ", resolved=" + full_name(resolved);
        return resolved == value;
    }

    auto set_vector_property(RC::Unreal::UObject* object, const wchar_t* property_name, const RC::Unreal::FVector& value, std::string& detail) -> bool
    {
        if (object == nullptr)
        {
            detail = "object is null";
            return false;
        }
        auto* property = static_cast<RC::Unreal::FVector*>(object->GetValuePtrByPropertyNameInChain(property_name));
        if (property == nullptr)
        {
            detail = "property missing: " + narrow(property_name);
            return false;
        }
        *property = value;
        detail = "vector property set: " + narrow(property_name);
        return true;
    }

    auto set_rotator_property(RC::Unreal::UObject* object, const wchar_t* property_name, const RC::Unreal::FRotator& value, std::string& detail) -> bool
    {
        if (object == nullptr)
        {
            detail = "object is null";
            return false;
        }
        auto* property = static_cast<RC::Unreal::FRotator*>(object->GetValuePtrByPropertyNameInChain(property_name));
        if (property == nullptr)
        {
            detail = "property missing: " + narrow(property_name);
            return false;
        }
        *property = value;
        detail = "rotator property set: " + narrow(property_name);
        return true;
    }

    auto array_property(RC::Unreal::UObject* object, const wchar_t* property_name) -> const TArrayAbi*
    {
        if (object == nullptr) return nullptr;
        return static_cast<const TArrayAbi*>(object->GetValuePtrByPropertyNameInChain(property_name));
    }

    auto read_uobject_array_item_noexcept(const TArrayAbi* array, std::int32_t index) -> RC::Unreal::UObject*
    {
        if (!is_reasonable_array(array) || array->data == nullptr) return nullptr;
        if (index < 0 || index >= array->count) return nullptr;
        const auto address = reinterpret_cast<std::uintptr_t>(array->data) + static_cast<std::uintptr_t>(index) * sizeof(std::uintptr_t);
        std::uintptr_t raw{};
        if (!safe_read_pointer(address, raw) || raw == 0) return nullptr;
        if (!committed_readable_address(raw)) return nullptr;
        return reinterpret_cast<RC::Unreal::UObject*>(raw);
    }

    auto actor_array_quality_noexcept(const TArrayAbi& array) -> int
    {
        if (!is_reasonable_array(&array) || array.data == nullptr || array.count <= 0) return 0;
        const auto data_address = reinterpret_cast<std::uintptr_t>(array.data);
        const auto min_read_size = static_cast<std::size_t>(std::min<std::int32_t>(array.count, 1)) * sizeof(std::uintptr_t);
        if (!committed_readable_address(data_address, min_read_size)) return 0;

        const auto sample_limit = std::min<std::int32_t>(array.count, 96);
        const auto step = std::max<std::int32_t>(1, array.count / sample_limit);
        int readable_objects = 0;
        int actor_like = 0;
        int checked = 0;
        for (std::int32_t index = 0; index < array.count && checked < sample_limit; index += step)
        {
            ++checked;
            auto* object = read_uobject_array_item_noexcept(&array, index);
            if (object == nullptr) continue;
            const auto object_name = lower_copy(safe_full_name(object));
            if (object_name.rfind("class ", 0) == 0) continue;
            ++readable_objects;
            RC::Unreal::FVector location{};
            if (actor_location_noexcept(object, location))
            {
                ++actor_like;
                if (actor_like >= 3) break;
            }
        }

        if (actor_like <= 0) return 0;
        return actor_like * 100 + readable_objects;
    }

    auto read_raw_tarray_noexcept(RC::Unreal::UObject* object, std::uintptr_t offset, TArrayAbi& value) -> bool
    {
        if (object == nullptr) return false;
        const auto address = reinterpret_cast<std::uintptr_t>(object) + offset;
        if (!committed_readable_address(address, sizeof(TArrayAbi))) return false;
        return safe_read(address, value);
    }

    auto read_level_actor_array(
        RC::Unreal::UObject* level,
        const std::string& level_name,
        TArrayAbi& out_array,
        std::string& out_source,
        std::vector<std::string>& notes) -> bool
    {
        if (level == nullptr) return false;

        if (auto* actor_cluster = object_property(level, L"ActorCluster"))
        {
            if (const auto* cluster_actors = array_property(actor_cluster, L"Actors");
                is_reasonable_array(cluster_actors) && cluster_actors->data != nullptr && cluster_actors->count > 0)
            {
                out_array = *cluster_actors;
                out_source = level_name + ".ActorCluster.Actors";
                return true;
            }
        }

        if (const auto* reflected_actors = array_property(level, L"Actors");
            is_reasonable_array(reflected_actors) && reflected_actors->data != nullptr && reflected_actors->count > 0)
        {
            out_array = *reflected_actors;
            out_source = level_name + ".Actors";
            return true;
        }

        if (notes.size() < 10)
        {
            notes.push_back("Actors missing: " + level_name + " (raw ULevel actor-array scan disabled after 2026-06-04 fatal)");
        }
        return false;
    }

    auto guid_property(RC::Unreal::UObject* object, const wchar_t* property_name) -> const FGuidAbi*
    {
        if (object == nullptr) return nullptr;
        return static_cast<const FGuidAbi*>(object->GetValuePtrByPropertyNameInChain(property_name));
    }

    auto float_property(RC::Unreal::UObject* object, const wchar_t* property_name, float fallback = 0.0f) -> float
    {
        if (object == nullptr) return fallback;
        auto* value = static_cast<float*>(object->GetValuePtrByPropertyNameInChain(property_name));
        return value == nullptr ? fallback : *value;
    }

    auto set_float_property(
        RC::Unreal::UObject* object,
        const wchar_t* property_name,
        float desired,
        std::string& detail,
        bool raise_only = true) -> bool
    {
        if (object == nullptr)
        {
            detail = "object is null";
            return false;
        }
        if (!std::isfinite(desired) || desired <= 0.0f)
        {
            detail = "invalid desired value: " + std::to_string(desired);
            return false;
        }
        auto* value = static_cast<float*>(object->GetValuePtrByPropertyNameInChain(property_name));
        if (value == nullptr)
        {
            detail = "property missing: " + narrow(property_name);
            return false;
        }
        const auto before = *value;
        if (!raise_only || before < desired)
        {
            *value = desired;
        }
        const auto after = *value;
        std::ostringstream ss;
        ss << "float property " << narrow(property_name)
           << " before=" << before
           << " desired=" << desired
           << " after=" << after
           << " mode=" << (raise_only ? "raise-only" : "assign");
        detail = ss.str();
        return raise_only ? after >= std::min(before, desired) && after >= desired * 0.99f : std::abs(after - desired) <= std::max(1.0f, desired * 0.01f);
    }

    auto bool_property(RC::Unreal::UObject* object, const wchar_t* property_name, bool fallback = false) -> bool
    {
        if (object == nullptr) return fallback;
        auto* value = static_cast<std::uint8_t*>(object->GetValuePtrByPropertyNameInChain(property_name));
        return value == nullptr ? fallback : *value != 0;
    }

    auto property_address_noexcept(
        RC::Unreal::UObject* object,
        const wchar_t* property_name,
        std::uintptr_t& address,
        bool& missing) -> bool
    {
        address = 0;
        missing = false;
        if (object == nullptr)
        {
            missing = true;
            return false;
        }
        void* raw{};
        __try
        {
            raw = object->GetValuePtrByPropertyNameInChain(property_name);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        if (raw == nullptr)
        {
            missing = true;
            return false;
        }
        address = reinterpret_cast<std::uintptr_t>(raw);
        return true;
    }

    template <typename T>
    auto read_pod_property_noexcept(
        RC::Unreal::UObject* object,
        const wchar_t* property_name,
        T& value,
        bool& missing) -> bool
    {
        std::uintptr_t address{};
        if (!property_address_noexcept(object, property_name, address, missing)) return false;
        if (!committed_readable_address(address, sizeof(T))) return false;
        return safe_read(address, value);
    }

    auto string_property_noexcept(
        RC::Unreal::UObject* object,
        const wchar_t* property_name,
        std::string& value,
        bool& missing) -> bool
    {
        value.clear();
        std::uintptr_t address{};
        if (!property_address_noexcept(object, property_name, address, missing)) return false;

        TArrayAbi array{};
        if (!safe_read(address, array)) return false;
        if (!is_reasonable_array(&array) || array.data == nullptr || array.count <= 0) return true;

        constexpr std::int32_t max_chars = 4096;
        const auto count = std::min(array.count, max_chars);
        const auto bytes = static_cast<std::size_t>(count) * sizeof(wchar_t);
        if (!committed_readable_address(reinterpret_cast<std::uintptr_t>(array.data), bytes)) return false;

        const auto* chars = static_cast<const wchar_t*>(array.data);
        std::int32_t length = 0;
        while (length < count && chars[length] != L'\0') ++length;
        value = narrow(std::wstring(chars, chars + length));
        return true;
    }

    auto array_count_property_noexcept(
        RC::Unreal::UObject* object,
        const wchar_t* property_name,
        std::int32_t& count,
        std::int32_t& max,
        bool& missing) -> bool
    {
        count = -1;
        max = -1;
        std::uintptr_t address{};
        if (!property_address_noexcept(object, property_name, address, missing)) return false;

        TArrayAbi array{};
        if (!safe_read(address, array)) return false;
        if (!is_reasonable_array(&array)) return false;
        count = array.count;
        max = array.max;
        return true;
    }

    auto find_object_exact_noexcept(
        const wchar_t* class_name,
        const wchar_t* object_path,
        int required_flags,
        int banned_flags) -> RC::Unreal::UObject*
    {
        if (class_name == nullptr || object_path == nullptr) return nullptr;
        __try
        {
            return RC::Unreal::UObjectGlobals::FindObject(class_name, object_path, required_flags, banned_flags);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    auto append_probe_property_prefix(std::ostringstream& ss, const char* json_name, bool& first) -> void
    {
        if (!first) ss << ",";
        first = false;
        ss << "\"" << json_name << "\":";
    }

    auto append_probe_missing_or_unreadable(std::ostringstream& ss, bool missing, bool readable, const std::string& value_json) -> void
    {
        ss << "{\"present\":" << (!missing ? "true" : "false")
           << ",\"readable\":" << (readable ? "true" : "false");
        if (readable)
        {
            ss << ",\"value\":" << value_json;
        }
        ss << "}";
    }

    auto append_probe_bool_property(
        std::ostringstream& ss,
        const char* json_name,
        RC::Unreal::UObject* object,
        const wchar_t* property_name,
        bool& first) -> void
    {
        append_probe_property_prefix(ss, json_name, first);
        bool missing = false;
        std::uint8_t raw{};
        const auto readable = read_pod_property_noexcept(object, property_name, raw, missing);
        append_probe_missing_or_unreadable(ss, missing, readable, raw != 0 ? "true" : "false");
    }

    auto append_probe_int32_property(
        std::ostringstream& ss,
        const char* json_name,
        RC::Unreal::UObject* object,
        const wchar_t* property_name,
        bool& first) -> void
    {
        append_probe_property_prefix(ss, json_name, first);
        bool missing = false;
        std::int32_t raw{};
        const auto readable = read_pod_property_noexcept(object, property_name, raw, missing);
        append_probe_missing_or_unreadable(ss, missing, readable, std::to_string(raw));
    }

    auto append_probe_uint8_property(
        std::ostringstream& ss,
        const char* json_name,
        RC::Unreal::UObject* object,
        const wchar_t* property_name,
        bool& first) -> void
    {
        append_probe_property_prefix(ss, json_name, first);
        bool missing = false;
        std::uint8_t raw{};
        const auto readable = read_pod_property_noexcept(object, property_name, raw, missing);
        append_probe_missing_or_unreadable(ss, missing, readable, std::to_string(static_cast<int>(raw)));
    }

    auto append_probe_float_property(
        std::ostringstream& ss,
        const char* json_name,
        RC::Unreal::UObject* object,
        const wchar_t* property_name,
        bool& first) -> void
    {
        append_probe_property_prefix(ss, json_name, first);
        bool missing = false;
        float raw{};
        const auto readable = read_pod_property_noexcept(object, property_name, raw, missing);
        std::ostringstream value;
        value << raw;
        append_probe_missing_or_unreadable(ss, missing, readable, value.str());
    }

    auto append_probe_string_property(
        std::ostringstream& ss,
        const char* json_name,
        RC::Unreal::UObject* object,
        const wchar_t* property_name,
        bool& first) -> void
    {
        append_probe_property_prefix(ss, json_name, first);
        bool missing = false;
        std::string raw;
        const auto readable = string_property_noexcept(object, property_name, raw, missing);
        append_probe_missing_or_unreadable(ss, missing, readable, "\"" + json_escape(raw) + "\"");
    }

    auto append_probe_array_count_property(
        std::ostringstream& ss,
        const char* json_name,
        RC::Unreal::UObject* object,
        const wchar_t* property_name,
        bool& first) -> void
    {
        append_probe_property_prefix(ss, json_name, first);
        bool missing = false;
        std::int32_t count = -1;
        std::int32_t max = -1;
        const auto readable = array_count_property_noexcept(object, property_name, count, max, missing);
        ss << "{\"present\":" << (!missing ? "true" : "false")
           << ",\"readable\":" << (readable ? "true" : "false");
        if (readable)
        {
            ss << ",\"count\":" << count << ",\"max\":" << max;
        }
        ss << "}";
    }

    auto append_probe_presence_property(
        std::ostringstream& ss,
        const char* json_name,
        RC::Unreal::UObject* object,
        const wchar_t* property_name,
        bool& first) -> void
    {
        append_probe_property_prefix(ss, json_name, first);
        bool missing = false;
        std::uintptr_t address{};
        const auto readable = property_address_noexcept(object, property_name, address, missing) && committed_readable_address(address);
        ss << "{\"present\":" << (!missing ? "true" : "false")
           << ",\"readable\":" << (readable ? "true" : "false") << "}";
    }

    auto append_admin_argument_samples(
        std::ostringstream& ss,
        RC::Unreal::UObject* object,
        const wchar_t* property_name,
        std::size_t max_items) -> void
    {
        ss << "[";
        bool missing = false;
        std::uintptr_t address{};
        if (!property_address_noexcept(object, property_name, address, missing) || !committed_readable_address(address, sizeof(TArrayAbi)))
        {
            ss << "]";
            return;
        }

        TArrayAbi array{};
        if (!safe_read(address, array) || !is_reasonable_array(&array) || array.data == nullptr)
        {
            ss << "]";
            return;
        }

        const auto count = std::min<std::size_t>(static_cast<std::size_t>(array.count), max_items);
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto item_address = reinterpret_cast<std::uintptr_t>(array.data) + (index * sizeof(FAdminCommandArgumentDescriptionAbi));
            if (!committed_readable_address(item_address, sizeof(FAdminCommandArgumentDescriptionAbi))) continue;

            FAdminCommandArgumentDescriptionAbi item{};
            if (!safe_read(item_address, item)) continue;

            if (index > 0) ss << ",";
            ss << "{\"index\":" << index
               << ",\"showCompletionValuesInHelpText\":" << (item.show_completion_values_in_help_text ? "true" : "false")
               << ",\"dataFullName\":\"" << json_escape(safe_full_name(item.data)) << "\""
               << ",\"dataClassFullName\":\"" << json_escape(object_class_full_name_noexcept(item.data)) << "\"";

            bool default_missing = false;
            std::string default_value;
            const auto default_readable = string_property_noexcept(item.data, L"_defaultValue", default_value, default_missing);
            ss << ",\"defaultValue\":{\"present\":" << (!default_missing ? "true" : "false")
               << ",\"readable\":" << (default_readable ? "true" : "false");
            if (default_readable) ss << ",\"value\":\"" << json_escape(default_value) << "\"";
            ss << "}";

            bool case_missing = false;
            std::uint8_t case_raw{};
            const auto case_readable = read_pod_property_noexcept(item.data, L"_caseSensitive", case_raw, case_missing);
            ss << ",\"caseSensitive\":{\"present\":" << (!case_missing ? "true" : "false")
               << ",\"readable\":" << (case_readable ? "true" : "false");
            if (case_readable) ss << ",\"value\":" << (case_raw != 0 ? "true" : "false");
            ss << "}}";
        }

        ss << "]";
    }

    auto set_bool_property(
        RC::Unreal::UObject* object,
        const wchar_t* property_name,
        bool desired,
        std::string& detail) -> bool
    {
        if (object == nullptr)
        {
            detail = "object is null";
            return false;
        }
        auto* value = static_cast<std::uint8_t*>(object->GetValuePtrByPropertyNameInChain(property_name));
        if (value == nullptr)
        {
            detail = "property missing: " + narrow(property_name);
            return false;
        }
        const auto before = *value;
        *value = desired ? 1 : 0;
        const auto after = *value;
        std::ostringstream ss;
        ss << "bool property " << narrow(property_name)
           << " before=" << static_cast<int>(before)
           << " desired=" << (desired ? "true" : "false")
           << " after=" << static_cast<int>(after);
        detail = ss.str();
        return (*value != 0) == desired;
    }

    class ScopedActorReplicationDefaults
    {
      public:
        ScopedActorReplicationDefaults(
            RC::Unreal::UClass* actor_class,
            std::vector<std::string>& details,
            bool enabled) :
            m_details(&details)
        {
            if (!enabled)
            {
                details.push_back("spawn-replication-defaults=skipped:not-requested");
                return;
            }
            if (actor_class == nullptr)
            {
                details.push_back("spawn-replication-defaults=skipped:null-class");
                return;
            }

            m_cdo = object_property(static_cast<RC::Unreal::UObject*>(actor_class), L"ClassDefaultObject");
            details.push_back("spawn-replication-defaults.ClassDefaultObject=" + safe_full_name(m_cdo));
            if (m_cdo == nullptr)
            {
                return;
            }

            set_bool_default(L"bReplicates", true);
            set_bool_default(L"bReplicateMovement", true);
            set_bool_default(L"bAlwaysRelevant", true);
            set_bool_default(L"bOnlyRelevantToOwner", false);
            set_bool_default(L"bNetUseOwnerRelevancy", false);
            set_bool_default(L"bNetLoadOnClient", true);
        }

        ScopedActorReplicationDefaults(const ScopedActorReplicationDefaults&) = delete;
        auto operator=(const ScopedActorReplicationDefaults&) -> ScopedActorReplicationDefaults& = delete;

        ~ScopedActorReplicationDefaults()
        {
            if (m_cdo == nullptr || m_details == nullptr) return;
            for (const auto& snapshot : m_snapshots)
            {
                auto* value = static_cast<std::uint8_t*>(m_cdo->GetValuePtrByPropertyNameInChain(snapshot.name));
                if (value == nullptr)
                {
                    m_details->push_back("spawn-replication-defaults.restore." + narrow(snapshot.name) + "=missing");
                    continue;
                }
                *value = snapshot.before ? 1 : 0;
                m_details->push_back(
                    "spawn-replication-defaults.restore." + narrow(snapshot.name) +
                    "=" + (snapshot.before ? "true" : "false"));
            }
        }

      private:
        struct BoolSnapshot
        {
            const wchar_t* name{};
            bool before{};
        };

        auto set_bool_default(const wchar_t* property_name, bool desired) -> void
        {
            auto* value = static_cast<std::uint8_t*>(m_cdo->GetValuePtrByPropertyNameInChain(property_name));
            if (value == nullptr)
            {
                m_details->push_back("spawn-replication-defaults." + narrow(property_name) + "=missing");
                return;
            }

            const auto before = *value != 0;
            m_snapshots.push_back({property_name, before});
            *value = desired ? 1 : 0;
            m_details->push_back(
                "spawn-replication-defaults." + narrow(property_name) +
                " before=" + (before ? "true" : "false") +
                " desired=" + (desired ? "true" : "false") +
                " after=" + ((*value != 0) ? "true" : "false"));
        }

        RC::Unreal::UObject* m_cdo{};
        std::vector<BoolSnapshot> m_snapshots{};
        std::vector<std::string>* m_details{};
    };

    auto set_uint8_property(
        RC::Unreal::UObject* object,
        const wchar_t* property_name,
        std::uint8_t desired,
        std::string& detail) -> bool
    {
        if (object == nullptr)
        {
            detail = "object is null";
            return false;
        }
        auto* value = static_cast<std::uint8_t*>(object->GetValuePtrByPropertyNameInChain(property_name));
        if (value == nullptr)
        {
            detail = "property missing: " + narrow(property_name);
            return false;
        }
        const auto before = *value;
        *value = desired;
        const auto after = *value;
        std::ostringstream ss;
        ss << "uint8 property " << narrow(property_name)
           << " before=" << static_cast<int>(before)
           << " desired=" << static_cast<int>(desired)
           << " after=" << static_cast<int>(after);
        detail = ss.str();
        return after == desired;
    }

    struct BoolParam
    {
        bool value{};
    };

    struct BoolBoolParam
    {
        bool first{};
        bool second{};
    };

    struct ByteParam
    {
        std::uint8_t value{};
    };

    struct ObjectBoolReturnParam
    {
        RC::Unreal::UObject* value{};
        bool return_value{};
    };

    struct ObjectReturnParams
    {
        RC::Unreal::UObject* return_value{};
    };

    auto call_reflected_function(
        RC::Unreal::UObject* object,
        const std::string& owner_token,
        const std::string& function_name,
        std::initializer_list<const char*> exact_paths,
        void* params,
        const std::string& label,
        std::vector<std::string>& details) -> bool
    {
        if (object == nullptr)
        {
            details.push_back(label + "=skipped:object-null");
            return false;
        }

        std::vector<std::string> resolve_attempts;
        auto* function = resolve_function_by_name(owner_token, function_name, exact_paths, resolve_attempts);
        std::string process_detail;
        auto* process_event = process_event_from_vtable(object, process_detail);

        bool invoked = false;
        unsigned long exception_code = 0;
        if (function != nullptr && process_event != nullptr)
        {
            invoked = invoke_process_event_noexcept(process_event, object, function, params, exception_code);
        }

        std::ostringstream ss;
        ss << label
           << "=" << (invoked ? "ok" : "failed")
           << " object=" << safe_full_name(object)
           << " function=" << safe_full_name(function)
           << " processEvent=" << process_detail;
        if (!invoked && exception_code != 0)
        {
            ss << " exception=" << hex_address(static_cast<std::uintptr_t>(exception_code));
        }
        if (!resolve_attempts.empty())
        {
            ss << " resolve=";
            const auto count = std::min<std::size_t>(resolve_attempts.size(), 4);
            for (std::size_t index = 0; index < count; ++index)
            {
                if (index > 0) ss << " || ";
                ss << resolve_attempts[index];
            }
        }
        details.push_back(ss.str());
        return invoked;
    }

    auto call_object_return_function(
        RC::Unreal::UObject* object,
        const std::string& owner_token,
        const std::string& function_name,
        std::initializer_list<const char*> exact_paths,
        const std::string& label,
        std::vector<std::string>& details) -> RC::Unreal::UObject*
    {
        ObjectReturnParams params{};
        const auto invoked = call_reflected_function(object, owner_token, function_name, exact_paths, &params, label, details);
        if (!invoked) return nullptr;
        details.push_back(label + ".return=" + safe_full_name(params.return_value));
        return params.return_value;
    }

    auto call_actor_bool_function(
        RC::Unreal::UObject* actor,
        const std::string& function_name,
        std::initializer_list<const char*> exact_paths,
        bool value,
        const std::string& label,
        std::vector<std::string>& details) -> bool
    {
        BoolParam params{};
        params.value = value;
        return call_reflected_function(actor, "Actor", function_name, exact_paths, &params, label, details);
    }

    auto call_actor_byte_function(
        RC::Unreal::UObject* actor,
        const std::string& function_name,
        std::initializer_list<const char*> exact_paths,
        std::uint8_t value,
        const std::string& label,
        std::vector<std::string>& details) -> bool
    {
        ByteParam params{};
        params.value = value;
        return call_reflected_function(actor, "Actor", function_name, exact_paths, &params, label, details);
    }

    auto call_actor_noarg_function(
        RC::Unreal::UObject* actor,
        const std::string& function_name,
        std::initializer_list<const char*> exact_paths,
        const std::string& label,
        std::vector<std::string>& details) -> bool
    {
        return call_reflected_function(actor, "Actor", function_name, exact_paths, nullptr, label, details);
    }

    auto call_component_bool_function(
        RC::Unreal::UObject* component,
        const std::string& owner_token,
        const std::string& function_name,
        std::initializer_list<const char*> exact_paths,
        bool value,
        const std::string& label,
        std::vector<std::string>& details) -> bool
    {
        BoolParam params{};
        params.value = value;
        return call_reflected_function(component, owner_token, function_name, exact_paths, &params, label, details);
    }

    auto call_component_bool_bool_function(
        RC::Unreal::UObject* component,
        const std::string& owner_token,
        const std::string& function_name,
        std::initializer_list<const char*> exact_paths,
        bool first,
        bool second,
        const std::string& label,
        std::vector<std::string>& details) -> bool
    {
        BoolBoolParam params{};
        params.first = first;
        params.second = second;
        return call_reflected_function(component, owner_token, function_name, exact_paths, &params, label, details);
    }

    auto call_component_object_bool_return_function(
        RC::Unreal::UObject* component,
        const std::string& owner_token,
        const std::string& function_name,
        std::initializer_list<const char*> exact_paths,
        RC::Unreal::UObject* value,
        const std::string& label,
        std::vector<std::string>& details) -> bool
    {
        ObjectBoolReturnParam params{};
        params.value = value;
        const auto invoked = call_reflected_function(component, owner_token, function_name, exact_paths, &params, label, details);
        details.push_back(label + ".return=" + std::string(params.return_value ? "true" : "false"));
        return invoked;
    }

    auto wake_component_for_visibility(
        RC::Unreal::UObject* component,
        const std::string& label,
        std::vector<std::string>& details) -> void
    {
        if (component == nullptr)
        {
            details.push_back(label + "=skipped:null");
            return;
        }

        std::string detail;
        const auto mobility_set = set_uint8_property(component, L"Mobility", 2, detail);
        details.push_back(label + ".Mobility(Movable=2)=" + std::string(mobility_set ? "ok:" : "failed:") + detail);
        set_bool_property(component, L"bVisible", true, detail);
        details.push_back(label + ".bVisible=" + detail);
        set_bool_property(component, L"bHiddenInGame", false, detail);
        details.push_back(label + ".bHiddenInGame=" + detail);
        set_bool_property(component, L"bReplicates", true, detail);
        details.push_back(label + ".bReplicates=" + detail);
        set_bool_property(component, L"bAutoActivate", true, detail);
        details.push_back(label + ".bAutoActivate=" + detail);
        set_bool_property(component, L"bIsActive", true, detail);
        details.push_back(label + ".bIsActive=" + detail);
        details.push_back(label + ".wakeMode=direct-property-only-fast-no-extra-component-processevent");
    }

    auto wake_actor_for_network_trade(
        RC::Unreal::AActor* actor,
        RC::Unreal::UObject* object,
        std::vector<std::string>& details) -> void
    {
        if (actor == nullptr || object == nullptr)
        {
            details.push_back("actor-network-wake=skipped:null");
            return;
        }

        actor->SetActorHiddenInGame(false);
        actor->SetActorEnableCollision(true);
        details.push_back("SetActorHiddenInGame(false)/SetActorEnableCollision(true)=direct-ok");

        std::string detail;
        set_float_property(object, L"NetUpdateFrequency", 100.0f, detail, true);
        details.push_back("NetUpdateFrequency=" + detail);
        set_float_property(object, L"MinNetUpdateFrequency", 30.0f, detail, true);
        details.push_back("MinNetUpdateFrequency=" + detail);
        set_float_property(object, L"NetPriority", 3.0f, detail, true);
        details.push_back("NetPriority=" + detail);
        set_float_property(object, L"NetCullDistanceSquared", 4000000000000.0f, detail, true);
        details.push_back("NetCullDistanceSquared=" + detail);
        details.push_back("SetReplicates/SetReplicateMovement/SetNetDormancy/FlushNetDormancy/ForceNetUpdate=skipped:no-reflected-processevent-crashguard");
        details.push_back("actorWakeMode=direct-visibility-and-float-properties-only");
    }

    auto wake_actor_for_persistent_server_spawn(
        RC::Unreal::AActor* actor,
        RC::Unreal::UObject* object,
        std::vector<std::string>& details) -> void
    {
        if (actor == nullptr || object == nullptr)
        {
            details.push_back("persistent-spawn-wake=skipped:null");
            return;
        }

        actor->SetActorHiddenInGame(false);
        actor->SetActorEnableCollision(true);
        details.push_back("SetActorHiddenInGame(false)/SetActorEnableCollision(true)=direct-ok");

        std::string detail;
        set_bool_property(object, L"bReplicates", true, detail);
        details.push_back("bReplicates=" + detail);
        set_bool_property(object, L"bReplicateMovement", true, detail);
        details.push_back("bReplicateMovement=" + detail);
        set_bool_property(object, L"bAlwaysRelevant", true, detail);
        details.push_back("bAlwaysRelevant=" + detail);
        set_bool_property(object, L"bOnlyRelevantToOwner", false, detail);
        details.push_back("bOnlyRelevantToOwner=" + detail);
        set_bool_property(object, L"bNetUseOwnerRelevancy", false, detail);
        details.push_back("bNetUseOwnerRelevancy=" + detail);
        set_float_property(object, L"NetUpdateFrequency", 100.0f, detail, true);
        details.push_back("NetUpdateFrequency=" + detail);
        set_float_property(object, L"MinNetUpdateFrequency", 30.0f, detail, true);
        details.push_back("MinNetUpdateFrequency=" + detail);
        set_float_property(object, L"NetPriority", 3.0f, detail, true);
        details.push_back("NetPriority=" + detail);
        set_float_property(object, L"NetCullDistanceSquared", 4000000000000.0f, detail, true);
        details.push_back("NetCullDistanceSquared=" + detail);

        auto* root_component = object_property(object, L"RootComponent");
        wake_component_for_visibility(root_component, "RootComponent", details);

        call_actor_bool_function(
            object,
            "SetReplicates",
            {
                "/Script/Engine.Actor:SetReplicates",
                "Function /Script/Engine.Actor:SetReplicates",
                "/Script/Engine.Actor.SetReplicates",
                "Function /Script/Engine.Actor.SetReplicates",
            },
            true,
            "SetReplicates(true)",
            details);
        call_actor_bool_function(
            object,
            "SetReplicateMovement",
            {
                "/Script/Engine.Actor:SetReplicateMovement",
                "Function /Script/Engine.Actor:SetReplicateMovement",
                "/Script/Engine.Actor.SetReplicateMovement",
                "Function /Script/Engine.Actor.SetReplicateMovement",
            },
            true,
            "SetReplicateMovement(true)",
            details);
        call_actor_byte_function(
            object,
            "SetNetDormancy",
            {
                "/Script/Engine.Actor:SetNetDormancy",
                "Function /Script/Engine.Actor:SetNetDormancy",
                "/Script/Engine.Actor.SetNetDormancy",
                "Function /Script/Engine.Actor.SetNetDormancy",
            },
            1,
            "SetNetDormancy(DORM_Awake)",
            details);
        call_actor_noarg_function(
            object,
            "FlushNetDormancy",
            {
                "/Script/Engine.Actor:FlushNetDormancy",
                "Function /Script/Engine.Actor:FlushNetDormancy",
                "/Script/Engine.Actor.FlushNetDormancy",
                "Function /Script/Engine.Actor.FlushNetDormancy",
            },
            "FlushNetDormancy()",
            details);
        call_actor_noarg_function(
            object,
            "ForceNetUpdate",
            {
                "/Script/Engine.Actor:ForceNetUpdate",
                "Function /Script/Engine.Actor:ForceNetUpdate",
                "/Script/Engine.Actor.ForceNetUpdate",
                "Function /Script/Engine.Actor.ForceNetUpdate",
            },
            "ForceNetUpdate()",
            details);
        details.push_back("actorWakeMode=persistent-server-spawn");
    }

    auto wake_actor_for_replicated_spawn_direct_only(
        RC::Unreal::AActor* actor,
        RC::Unreal::UObject* object,
        std::vector<std::string>& details) -> void
    {
        if (actor == nullptr || object == nullptr)
        {
            details.push_back("replicated-spawn-direct-wake=skipped:null");
            return;
        }

        actor->SetActorHiddenInGame(false);
        actor->SetActorEnableCollision(true);
        details.push_back("SetActorHiddenInGame(false)/SetActorEnableCollision(true)=direct-ok");

        std::string detail;
        set_bool_property(object, L"bReplicates", true, detail);
        details.push_back("bReplicates=" + detail);
        set_bool_property(object, L"bReplicateMovement", true, detail);
        details.push_back("bReplicateMovement=" + detail);
        set_bool_property(object, L"bAlwaysRelevant", true, detail);
        details.push_back("bAlwaysRelevant=" + detail);
        set_bool_property(object, L"bOnlyRelevantToOwner", false, detail);
        details.push_back("bOnlyRelevantToOwner=" + detail);
        set_bool_property(object, L"bNetUseOwnerRelevancy", false, detail);
        details.push_back("bNetUseOwnerRelevancy=" + detail);
        set_float_property(object, L"NetUpdateFrequency", 100.0f, detail, true);
        details.push_back("NetUpdateFrequency=" + detail);
        set_float_property(object, L"MinNetUpdateFrequency", 30.0f, detail, true);
        details.push_back("MinNetUpdateFrequency=" + detail);
        set_float_property(object, L"NetPriority", 3.0f, detail, true);
        details.push_back("NetPriority=" + detail);
        set_float_property(object, L"NetCullDistanceSquared", 4000000000000.0f, detail, true);
        details.push_back("NetCullDistanceSquared=" + detail);

        auto* root_component = object_property(object, L"RootComponent");
        wake_component_for_visibility(root_component, "RootComponent", details);
        details.push_back("SetReplicates/SetReplicateMovement/SetNetDormancy/FlushNetDormancy/ForceNetUpdate=skipped:cdo-pre-spawn-direct-wake");
        details.push_back("actorWakeMode=replicated-spawn-direct-only");
    }

    auto wake_actor_for_editor_transform(
        RC::Unreal::AActor* actor,
        RC::Unreal::UObject* object,
        std::vector<std::string>& details) -> void
    {
        if (actor == nullptr || object == nullptr)
        {
            details.push_back("editor-transform-wake=skipped:null");
            return;
        }

        actor->SetActorHiddenInGame(false);
        actor->SetActorEnableCollision(true);
        details.push_back("SetActorHiddenInGame(false)/SetActorEnableCollision(true)=direct-ok");

        std::string detail;
        set_float_property(object, L"NetUpdateFrequency", 100.0f, detail, true);
        details.push_back("NetUpdateFrequency=" + detail);
        set_float_property(object, L"MinNetUpdateFrequency", 30.0f, detail, true);
        details.push_back("MinNetUpdateFrequency=" + detail);
        set_float_property(object, L"NetPriority", 3.0f, detail, true);
        details.push_back("NetPriority=" + detail);
        set_float_property(object, L"NetCullDistanceSquared", 4000000000000.0f, detail, true);
        details.push_back("NetCullDistanceSquared=" + detail);

        auto* root_component = object_property(object, L"RootComponent");
        if (root_component != nullptr)
        {
            std::string mobility_detail;
            const auto mobility_set = set_uint8_property(root_component, L"Mobility", 2, mobility_detail);
            details.push_back(
                "RootComponent.Mobility(Movable=2)=" +
                std::string(mobility_set ? "ok:" : "failed:") +
                mobility_detail);
            set_bool_property(root_component, L"bVisible", true, detail);
            details.push_back("RootComponent.bVisible=" + detail);
            set_bool_property(root_component, L"bHiddenInGame", false, detail);
            details.push_back("RootComponent.bHiddenInGame=" + detail);
        }
        else
        {
            details.push_back("RootComponent=skipped:null");
        }

        details.push_back("SetReplicates/SetReplicateMovement/SetNetDormancy/FlushNetDormancy/ForceNetUpdate=skipped:existing-map-actor-soft-wake");
        details.push_back("actorWakeMode=hektor-editor-existing-soft-no-replication");
    }

    auto wake_actor_for_editor_spawned_copy(
        RC::Unreal::AActor* actor,
        RC::Unreal::UObject* object,
        std::vector<std::string>& details) -> void
    {
        wake_actor_for_persistent_server_spawn(actor, object, details);
        details.push_back("actorWakeMode=hektor-editor-spawned-copy");
    }

    auto wake_actor_for_editor_hide(
        RC::Unreal::AActor* actor,
        RC::Unreal::UObject* object,
        std::vector<std::string>& details) -> void
    {
        if (actor == nullptr || object == nullptr)
        {
            details.push_back("editor-hide-wake=skipped:null");
            return;
        }

        actor->SetActorHiddenInGame(true);
        actor->SetActorEnableCollision(false);
        details.push_back("SetActorHiddenInGame(true)/SetActorEnableCollision(false)=direct-ok");

        std::string detail;
        set_float_property(object, L"NetUpdateFrequency", 100.0f, detail, true);
        details.push_back("NetUpdateFrequency=" + detail);
        set_float_property(object, L"NetCullDistanceSquared", 4000000000000.0f, detail, true);
        details.push_back("NetCullDistanceSquared=" + detail);
        details.push_back("SetReplicates/FlushNetDormancy/ForceNetUpdate=skipped:existing-map-actor-hide-soft-wake");
        details.push_back("actorWakeMode=hektor-editor-hide-soft");
    }

    auto copy_raw_array_property(
        RC::Unreal::UObject* destination,
        RC::Unreal::UObject* source,
        const wchar_t* property_name,
        std::size_t element_size,
        std::int32_t max_allowed_count,
        std::string& detail) -> bool;

    auto preferred_static_mesh_component(
        RC::Unreal::UObject* object,
        const std::string& label,
        std::vector<std::string>& details) -> RC::Unreal::UObject*
    {
        if (object == nullptr)
        {
            details.push_back(label + "=skipped:null-owner");
            return nullptr;
        }

        for (const auto* property_name : {L"StaticMeshComponent", L"MeshComponent", L"RootComponent"})
        {
            bool missing = false;
            bool exception = false;
            auto* component = object_property_noexcept_raw(object, property_name, missing, exception);
            details.push_back(label + "." + narrow(property_name) + "=" +
                (component != nullptr ? safe_full_name(component) : (exception ? "exception" : (missing ? "missing" : "null"))));
            if (component != nullptr) return component;
        }
        return nullptr;
    }

    auto set_static_mesh_component_mesh(
        RC::Unreal::UObject* destination_component,
        RC::Unreal::UObject* source_mesh,
        const std::string& label,
        std::vector<std::string>& details) -> bool
    {
        if (destination_component == nullptr || source_mesh == nullptr)
        {
            details.push_back(label + ".StaticMesh=failed:null-component-or-mesh");
            return false;
        }

        std::string detail;
        const auto direct_set = set_object_property(destination_component, L"StaticMesh", source_mesh, detail);
        details.push_back(label + ".StaticMeshDirect=" + std::string(direct_set ? "ok:" : "failed:") + detail + " source=" + safe_full_name(source_mesh));

        const auto reflected_set = call_component_object_bool_return_function(
            destination_component,
            "StaticMeshComponent",
            "SetStaticMesh",
            {
                "/Script/Engine.StaticMeshComponent:SetStaticMesh",
                "Function /Script/Engine.StaticMeshComponent:SetStaticMesh",
                "/Script/Engine.StaticMeshComponent.SetStaticMesh",
                "Function /Script/Engine.StaticMeshComponent.SetStaticMesh",
            },
            source_mesh,
            label + ".SetStaticMesh(NewMesh)",
            details);
        return direct_set || reflected_set;
    }

    auto copy_static_mesh_component_visuals(
        RC::Unreal::UObject* source_component,
        RC::Unreal::UObject* destination_component,
        RC::Unreal::UObject* explicit_mesh,
        const std::string& label,
        std::vector<std::string>& details) -> bool
    {
        details.push_back(label + ".sourceComponent=" + safe_full_name(source_component));
        details.push_back(label + ".destinationComponent=" + safe_full_name(destination_component));
        details.push_back(label + ".explicitMesh=" + safe_full_name(explicit_mesh));
        if (destination_component == nullptr)
        {
            details.push_back(label + "=failed:destination-component-missing");
            return false;
        }

        auto* source_mesh = explicit_mesh;
        if (source_mesh == nullptr && source_component != nullptr)
        {
            source_mesh = object_property(source_component, L"StaticMesh");
        }

        const auto mesh_set = set_static_mesh_component_mesh(destination_component, source_mesh, label, details);
        if (!mesh_set)
        {
            details.push_back(label + "=failed:mesh-not-set");
            return false;
        }

        if (source_component != nullptr)
        {
            std::string materials_detail;
            const auto materials_copied = copy_raw_array_property(
                destination_component,
                source_component,
                L"OverrideMaterials",
                sizeof(RC::Unreal::UObject*),
                256,
                materials_detail);
            details.push_back(label + ".OverrideMaterials=" + std::string(materials_copied ? "ok:" : "failed:") + materials_detail);

            RC::Unreal::FVector source_scale{};
            if (vector_property(source_component, L"RelativeScale3D", source_scale))
            {
                std::string scale_detail;
                const auto scale_set = set_vector_property(destination_component, L"RelativeScale3D", source_scale, scale_detail);
                details.push_back(label + ".RelativeScale3D=" + std::string(scale_set ? "ok:" : "failed:") + scale_detail);
            }
            else
            {
                details.push_back(label + ".RelativeScale3D=skipped:source-missing");
            }

            RC::Unreal::FVector source_relative_location{};
            if (vector_property(source_component, L"RelativeLocation", source_relative_location))
            {
                std::string location_detail;
                const auto location_set = set_vector_property(destination_component, L"RelativeLocation", source_relative_location, location_detail);
                details.push_back(label + ".RelativeLocation=" + std::string(location_set ? "ok:" : "failed:") + location_detail);
            }
            else
            {
                details.push_back(label + ".RelativeLocation=skipped:source-missing");
            }

            RC::Unreal::FRotator source_relative_rotation{};
            if (rotator_property(source_component, L"RelativeRotation", source_relative_rotation))
            {
                std::string rotation_detail;
                const auto rotation_set = set_rotator_property(destination_component, L"RelativeRotation", source_relative_rotation, rotation_detail);
                details.push_back(label + ".RelativeRotation=" + std::string(rotation_set ? "ok:" : "failed:") + rotation_detail);
            }
            else
            {
                details.push_back(label + ".RelativeRotation=skipped:source-missing");
            }
        }
        else
        {
            details.push_back(label + ".componentVisualCopy=mesh-only:no-source-component");
        }

        wake_component_for_visibility(destination_component, label + ".CopiedMeshComponent", details);
        return true;
    }

    auto copy_static_mesh_actor_visuals(
        RC::Unreal::UObject* source_actor,
        RC::Unreal::UObject* destination_actor,
        std::vector<std::string>& details) -> void
    {
        if (source_actor == nullptr || destination_actor == nullptr)
        {
            details.push_back("copyVisuals=skipped:null-actor");
            return;
        }

        auto* source_component = preferred_static_mesh_component(source_actor, "copyVisuals.source", details);
        auto* destination_component = preferred_static_mesh_component(destination_actor, "copyVisuals.destination", details);
        copy_static_mesh_component_visuals(source_component, destination_component, nullptr, "copyVisuals", details);
    }

    auto package_path_without_object_name(const std::string& full_name_value) -> std::string
    {
        auto value = object_path_from_full_name(full_name_value);
        const auto dot = value.find_last_of('.');
        if (dot != std::string::npos && dot > 0)
        {
            return value.substr(0, dot);
        }
        return value;
    }

    auto find_static_mesh_asset_for_visual_proxy(
        const std::string& source_mesh_full_name,
        std::vector<std::string>& attempts) -> RC::Unreal::UObject*
    {
        if (source_mesh_full_name.empty())
        {
            attempts.push_back("visualProxy.sourceMesh=empty");
            return nullptr;
        }

        auto* exact = find_object_by_full_name(
            {L"StaticMesh"},
            source_mesh_full_name,
            attempts,
            "visualProxy.sourceMesh",
            false);
        if (exact != nullptr) return exact;

        const auto leaf = object_leaf_from_full_name(source_mesh_full_name);
        if (leaf.empty())
        {
            attempts.push_back("visualProxy.sourceMesh.leaf=empty");
            return nullptr;
        }

        std::vector<RC::Unreal::UObject*> found;
        RC::Unreal::UObjectGlobals::FindObjects(
            L"StaticMesh",
            widen(leaf).c_str(),
            found,
            0,
            object_flag_class_default_object,
            false);
        attempts.push_back("visualProxy.sourceMesh.FindObjects(StaticMesh," + leaf + ")=" + std::to_string(found.size()));

        const auto requested_package_lower = lower_copy(package_path_without_object_name(source_mesh_full_name));
        const auto requested_path_lower = lower_copy(object_path_from_full_name(source_mesh_full_name));
        const auto requested_leaf_lower = lower_copy(leaf);
        RC::Unreal::UObject* fallback{};
        for (auto* object : found)
        {
            if (object == nullptr) continue;
            const auto full = safe_full_name(object);
            const auto lower = lower_copy(full);
            if (fallback == nullptr) fallback = object;
            attempts.push_back("visualProxy.sourceMesh.candidate=" + full);
            if (!requested_path_lower.empty() && lower.find(requested_path_lower) != std::string::npos) return object;
            if (!requested_package_lower.empty() && lower.find(requested_package_lower) != std::string::npos) return object;
            if (!requested_leaf_lower.empty() && lower.find(requested_leaf_lower) != std::string::npos && found.size() == 1) return object;
        }

        if (found.size() == 1 && fallback != nullptr)
        {
            attempts.push_back("visualProxy.sourceMesh.leafFallbackSingle=" + safe_full_name(fallback));
            return fallback;
        }
        return nullptr;
    }

    auto append_json_string_array(std::ostringstream& ss, const std::vector<std::string>& values) -> void
    {
        ss << "[";
        for (size_t index = 0; index < values.size(); ++index)
        {
            if (index > 0) ss << ",";
            ss << "\"" << json_escape(values[index]) << "\"";
        }
        ss << "]";
    }

    auto append_json_int64_array(std::ostringstream& ss, const std::vector<std::int64_t>& values) -> void
    {
        ss << "[";
        for (size_t index = 0; index < values.size(); ++index)
        {
            if (index > 0) ss << ",";
            ss << values[index];
        }
        ss << "]";
    }

    auto parse_int64_text(const std::string& text, std::int64_t& value) -> bool
    {
        value = 0;
        auto clean = text;
        clean.erase(std::remove_if(clean.begin(), clean.end(), [](unsigned char ch) { return std::isspace(ch) != 0; }), clean.end());
        if (clean.empty()) return false;
        try
        {
            std::size_t consumed = 0;
            const auto parsed = std::stoll(clean, &consumed, 10);
            if (consumed != clean.size()) return false;
            value = parsed;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    auto regex_int64_array_values(const std::string& text, const char* key) -> std::vector<std::int64_t>
    {
        std::vector<std::int64_t> values;
        if (key == nullptr || key[0] == '\0') return values;
        const std::regex array_pattern(std::string("\\\"") + key + "\\\"\\s*:\\s*\\[([^\\]]*)\\]");
        std::smatch array_match;
        if (std::regex_search(text, array_match, array_pattern) && array_match.size() > 1)
        {
            const auto body = array_match[1].str();
            const std::regex number_pattern(R"((-?[0-9]+))");
            for (auto it = std::sregex_iterator(body.begin(), body.end(), number_pattern); it != std::sregex_iterator(); ++it)
            {
                std::int64_t parsed{};
                if (parse_int64_text((*it)[1].str(), parsed)) values.push_back(parsed);
            }
        }
        return values;
    }

    auto first_regex_int64_list(
        const std::string& text,
        std::initializer_list<const char*> array_keys,
        std::initializer_list<const char*> single_keys,
        std::int64_t fallback_if_empty = 0) -> std::vector<std::int64_t>
    {
        for (const auto* key : array_keys)
        {
            auto values = regex_int64_array_values(text, key);
            if (!values.empty()) return values;
        }

        for (const auto* key : single_keys)
        {
            std::int64_t parsed{};
            const auto raw = regex_value(text, key);
            if (!raw.empty() && parse_int64_text(raw, parsed)) return {parsed};
            double number{};
            if (regex_number_value(text, key, number) && std::isfinite(number)) return {static_cast<std::int64_t>(number)};
        }

        if (fallback_if_empty > 0) return {fallback_if_empty};
        return {};
    }

    auto append_transform_json(std::ostringstream& ss, const RC::Unreal::FTransform& transform) -> void
    {
        ss << "{\"location\":{\"x\":" << transform.translation.x
           << ",\"y\":" << transform.translation.y
           << ",\"z\":" << transform.translation.z
           << "},\"rotationQuat\":{\"x\":" << transform.rotation.x
           << ",\"y\":" << transform.rotation.y
           << ",\"z\":" << transform.rotation.z
           << ",\"w\":" << transform.rotation.w
           << "},\"scale\":{\"x\":" << transform.scale3d.x
           << ",\"y\":" << transform.scale3d.y
           << ",\"z\":" << transform.scale3d.z << "}}";
    }

    auto transform_location_is_probably_world(const RC::Unreal::FTransform& transform) -> bool
    {
        return std::abs(transform.translation.x) > 20000.0f || std::abs(transform.translation.y) > 20000.0f;
    }

    auto normalize_local_transform_if_needed(RC::Unreal::FTransform& transform, const RC::Unreal::FVector& actor_location) -> bool
    {
        if (!transform_location_is_probably_world(transform)) return false;
        transform.translation.x -= actor_location.x;
        transform.translation.y -= actor_location.y;
        transform.translation.z -= actor_location.z;
        return true;
    }

    auto append_vector_json(std::ostringstream& ss, const RC::Unreal::FVector& vector) -> void
    {
        ss << "{\"x\":" << vector.x << ",\"y\":" << vector.y << ",\"z\":" << vector.z << "}";
    }

    auto make_vector(double x, double y, double z) -> RC::Unreal::FVector
    {
        RC::Unreal::FVector vector{};
        vector.x = static_cast<float>(x);
        vector.y = static_cast<float>(y);
        vector.z = static_cast<float>(z);
        return vector;
    }

    auto normalize_yaw(double yaw) -> double
    {
        while (yaw > 180.0) yaw -= 360.0;
        while (yaw < -180.0) yaw += 360.0;
        return yaw;
    }

    auto yaw_forward_2d(double yaw_degrees) -> std::pair<double, double>
    {
        constexpr auto pi = 3.14159265358979323846;
        const auto radians = yaw_degrees * pi / 180.0;
        return {std::cos(radians), std::sin(radians)};
    }

    auto location_in_front_of_player(
        double x,
        double y,
        double z,
        double yaw_degrees,
        double forward_cm,
        double z_offset_cm) -> RC::Unreal::FVector
    {
        const auto [forward_x, forward_y] = yaw_forward_2d(yaw_degrees);
        return make_vector(x + forward_x * forward_cm, y + forward_y * forward_cm, z + z_offset_cm);
    }

    auto vector_distance_3d(const RC::Unreal::FVector& a, const RC::Unreal::FVector& b) -> double
    {
        const auto dx = static_cast<double>(a.x) - static_cast<double>(b.x);
        const auto dy = static_cast<double>(a.y) - static_cast<double>(b.y);
        const auto dz = static_cast<double>(a.z) - static_cast<double>(b.z);
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    auto force_actor_world_location(
        RC::Unreal::AActor* actor,
        const RC::Unreal::FVector& desired,
        double yaw,
        RC::Unreal::FVector& actual,
        std::string& detail) -> bool
    {
        if (actor == nullptr)
        {
            detail = "actor is null";
            actual = RC::Unreal::FVector{};
            return false;
        }

        {
            RC::Unreal::FVector before{};
            if (!actor_location_noexcept(static_cast<RC::Unreal::UObject*>(actor), before))
            {
                detail = "K2_GetActorLocation failed before placement; postSpawnMove=blocked";
                actual = RC::Unreal::FVector{};
                return false;
            }

            // K2_TeleportTo / reflected K2_SetActorTransform crashed SCUMServer for spawned
            // ATrader on 2026-06-01. Rely on SpawnActor's initial transform and fail closed
            // if the actor was born somewhere else.
            actual = before;
            const auto distance = vector_distance_3d(actual, desired);
            std::ostringstream ss;
            ss << "before=";
            append_vector_json(ss, before);
            ss << " desired=";
            append_vector_json(ss, desired);
            ss << " actual=";
            append_vector_json(ss, actual);
            ss << " teleported=skipped"
               << " distance3d=" << distance
               << " directFallback=[postSpawnMove=blocked:unsafe-k2-teleport-and-reflected-placement]";
            detail = ss.str();
            return distance <= 250.0;
        }

        const auto before = actor->K2_GetActorLocation();
        RC::Unreal::FRotator rotation{};
        rotation.pitch = 0.0f;
        rotation.yaw = static_cast<float>(yaw);
        rotation.roll = 0.0f;

        actor->SetActorEnableCollision(false);
        const auto teleported = actor->K2_TeleportTo(desired, rotation);
        actor->SetActorEnableCollision(true);

        actual = actor->K2_GetActorLocation();
        auto distance = vector_distance_3d(actual, desired);
        std::vector<std::string> direct_details;
        bool direct_location_ok = false;
        bool direct_rotation_ok = false;

        if (distance > 250.0)
        {
            constexpr auto pi = 3.14159265358979323846;
            const auto half_yaw = static_cast<float>((yaw * pi / 180.0) * 0.5);
            ActorSetTransformParams reflected_params{};
            reflected_params.new_transform.rotation.x = 0.0f;
            reflected_params.new_transform.rotation.y = 0.0f;
            reflected_params.new_transform.rotation.z = std::sin(half_yaw);
            reflected_params.new_transform.rotation.w = std::cos(half_yaw);
            reflected_params.new_transform.translation = desired;
            reflected_params.new_transform.scale3d.x = 1.0f;
            reflected_params.new_transform.scale3d.y = 1.0f;
            reflected_params.new_transform.scale3d.z = 1.0f;
            reflected_params.sweep = false;
            reflected_params.teleport = true;
            std::vector<std::string> reflected_details;

            actor->SetActorEnableCollision(false);
            const auto reflected_invoked = call_reflected_function(
                static_cast<RC::Unreal::UObject*>(actor),
                "Actor",
                "K2_SetActorTransform",
                {
                    "/Script/Engine.Actor:K2_SetActorTransform",
                    "Function /Script/Engine.Actor:K2_SetActorTransform",
                    "/Script/Engine.Actor.K2_SetActorTransform",
                    "Function /Script/Engine.Actor.K2_SetActorTransform",
                },
                &reflected_params,
                "K2_SetActorTransform",
                reflected_details);
            actor->SetActorEnableCollision(true);

            const auto after_reflected = actor->K2_GetActorLocation();
            const auto after_reflected_distance = vector_distance_3d(after_reflected, desired);
            direct_details.push_back(
                "reflectedTransform invoked=" + std::string(reflected_invoked ? "true" : "false") +
                " return=" + std::string(reflected_params.return_value ? "true" : "false") +
                " distance3d=" + std::to_string(after_reflected_distance));
            for (const auto& reflected_detail : reflected_details)
            {
                direct_details.push_back(reflected_detail);
            }
            if (reflected_invoked && after_reflected_distance <= 250.0)
            {
                actual = after_reflected;
                distance = after_reflected_distance;
                direct_details.push_back("reflectedTransformVerified=K2_GetActorLocation");
            }
        }

        if (distance > 250.0)
        {
            ActorSetLocationAndRotationParams reflected_params{};
            reflected_params.new_location = desired;
            reflected_params.new_rotation = rotation;
            reflected_params.sweep = false;
            reflected_params.teleport = true;
            std::vector<std::string> reflected_details;

            actor->SetActorEnableCollision(false);
            const auto reflected_invoked = call_reflected_function(
                static_cast<RC::Unreal::UObject*>(actor),
                "Actor",
                "K2_SetActorLocationAndRotation",
                {
                    "/Script/Engine.Actor:K2_SetActorLocationAndRotation",
                    "Function /Script/Engine.Actor:K2_SetActorLocationAndRotation",
                    "/Script/Engine.Actor.K2_SetActorLocationAndRotation",
                    "Function /Script/Engine.Actor.K2_SetActorLocationAndRotation",
                },
                &reflected_params,
                "K2_SetActorLocationAndRotation",
                reflected_details);
            actor->SetActorEnableCollision(true);

            const auto after_reflected = actor->K2_GetActorLocation();
            const auto after_reflected_distance = vector_distance_3d(after_reflected, desired);
            direct_details.push_back(
                "reflectedLocationAndRotation invoked=" + std::string(reflected_invoked ? "true" : "false") +
                " return=" + std::string(reflected_params.return_value ? "true" : "false") +
                " distance3d=" + std::to_string(after_reflected_distance));
            for (const auto& reflected_detail : reflected_details)
            {
                direct_details.push_back(reflected_detail);
            }
            if (reflected_invoked && after_reflected_distance <= 250.0)
            {
                actual = after_reflected;
                distance = after_reflected_distance;
                direct_details.push_back("reflectedPlacementVerified=K2_GetActorLocation");
            }
        }

        if (distance > 250.0)
        {
            auto* root_component = object_property(static_cast<RC::Unreal::UObject*>(actor), L"RootComponent");
            SceneComponentSetWorldLocationAndRotationParams reflected_params{};
            reflected_params.new_location = desired;
            reflected_params.new_rotation = rotation;
            reflected_params.sweep = false;
            reflected_params.teleport = true;
            std::vector<std::string> reflected_details;

            actor->SetActorEnableCollision(false);
            const auto reflected_invoked = call_reflected_function(
                root_component,
                "SceneComponent",
                "K2_SetWorldLocationAndRotation",
                {
                    "/Script/Engine.SceneComponent:K2_SetWorldLocationAndRotation",
                    "Function /Script/Engine.SceneComponent:K2_SetWorldLocationAndRotation",
                    "/Script/Engine.SceneComponent.K2_SetWorldLocationAndRotation",
                    "Function /Script/Engine.SceneComponent.K2_SetWorldLocationAndRotation",
                },
                &reflected_params,
                "RootComponent.K2_SetWorldLocationAndRotation",
                reflected_details);
            actor->SetActorEnableCollision(true);

            const auto after_reflected = actor->K2_GetActorLocation();
            const auto after_reflected_distance = vector_distance_3d(after_reflected, desired);
            direct_details.push_back(
                "RootComponent.reflectedWorldLocationAndRotation invoked=" +
                std::string(reflected_invoked ? "true" : "false") +
                " distance3d=" + std::to_string(after_reflected_distance));
            for (const auto& reflected_detail : reflected_details)
            {
                direct_details.push_back(reflected_detail);
            }
            if (reflected_invoked && after_reflected_distance <= 250.0)
            {
                actual = after_reflected;
                distance = vector_distance_3d(actual, desired);
                direct_location_ok = true;
                direct_rotation_ok = true;
                direct_details.push_back("rootWorldPlacementVerified=K2_GetActorLocation");
            }
            else
            {
                actual = after_reflected;
                distance = after_reflected_distance;
            }
        }

        std::ostringstream ss;
        ss << "before=";
        append_vector_json(ss, before);
        ss << " desired=";
        append_vector_json(ss, desired);
        ss << " actual=";
        append_vector_json(ss, actual);
        ss << " teleported=" << (teleported ? "true" : "false")
           << " distance3d=" << distance;
        if (!direct_details.empty())
        {
            ss << " directFallback=[";
            for (std::size_t index = 0; index < direct_details.size(); ++index)
            {
                if (index > 0) ss << " | ";
                ss << direct_details[index];
            }
            ss << "]";
        }
        detail = ss.str();

        return distance <= 250.0;
    }

    auto append_rotator_json(std::ostringstream& ss, const RC::Unreal::FRotator& rotator) -> void
    {
        ss << "{\"pitch\":" << rotator.pitch << ",\"yaw\":" << rotator.yaw << ",\"roll\":" << rotator.roll << "}";
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

    auto first_regex_number(const std::string& text, std::initializer_list<const char*> keys, double& value) -> bool
    {
        for (const auto* key : keys)
        {
            if (regex_number_value(text, key, value)) return true;
        }
        return false;
    }

    auto parse_uintptr_value(const std::string& value, std::uintptr_t& parsed) -> bool
    {
        parsed = 0;
        auto text = value;
        text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char ch) { return std::isspace(ch) != 0; }), text.end());
        if (text.empty()) return false;
        try
        {
            std::size_t consumed = 0;
            const int base = (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0) ? 16 : 10;
            const auto value64 = std::stoull(text, &consumed, base);
            if (consumed != text.size() || value64 == 0) return false;
            parsed = static_cast<std::uintptr_t>(value64);
            return parsed != 0;
        }
        catch (...)
        {
            return false;
        }
    }

    auto first_regex_address(const std::string& text, std::initializer_list<const char*> keys, std::uintptr_t& value, std::string& raw_value) -> bool
    {
        raw_value.clear();
        for (const auto* key : keys)
        {
            auto raw = regex_value(text, key);
            if (raw.empty()) continue;
            std::uintptr_t parsed{};
            raw_value = raw;
            if (parse_uintptr_value(raw, parsed))
            {
                value = parsed;
                return true;
            }
            value = 0;
            return false;
        }
        value = 0;
        return false;
    }

    auto editor_actor_block_reason(const std::string& actor_full_name, const std::string& actor_class_full_name) -> std::string
    {
        const auto haystack = lower_copy(actor_full_name + " " + actor_class_full_name);
        if (haystack.find("default__") != std::string::npos) return "class-default-object";
        if (haystack.rfind("class ", 0) == 0 || haystack.find("/script/coreuobject.class") != std::string::npos) return "class-object";

        const std::initializer_list<std::pair<const char*, const char*>> blocked{
            {"/script/scum.trader", "trader-class"},
            {"bp_armsdealer", "trader-blueprint"},
            {"bp_banker", "trader-blueprint"},
            {"bp_barber", "trader-blueprint"},
            {"bp_bartender", "trader-blueprint"},
            {"bp_doctor", "trader-blueprint"},
            {"bp_general_goods", "trader-blueprint"},
            {"bp_harbourmaster", "trader-blueprint"},
            {"bp_mechanic", "trader-blueprint"},
            {"npcinteractionbox", "tradepost-interaction-box"},
            {"tradeoutpost", "tradepost-manager-or-route"},
            {"tradepost", "tradepost-manager-or-route"},
            {"economymanager", "economy-manager"},
            {"/script/scum.prisoner", "player-or-prisoner-class"},
            {"conzplayer", "player-or-prisoner-class"},
            {"playercontroller", "player-controller"},
            {"controller", "controller"},
            {"inventorycomponent", "inventory"},
            {"/script/scum.item", "item-class"},
            {"itementitysetup", "item-setup"},
            {"/script/scum.zombie", "creature"},
            {"/script/scum.sentry", "creature"},
            {"/script/scum.brenner", "creature"},
            {"bp_zombie", "creature-blueprint"},
            {"bp_sentry", "creature-blueprint"},
            {"bp_brenner", "creature-blueprint"},
            {"bp_razor", "creature-blueprint"},
            {"npcguard", "npc-guard"},
            {"guardedzonemanager", "npc-manager"},
        };

        for (const auto& [needle, reason] : blocked)
        {
            if (haystack.find(needle) != std::string::npos) return reason;
        }
        return {};
    }

    auto editor_actor_selection_block_reason(const std::string& actor_full_name, const std::string& actor_class_full_name) -> std::string
    {
        const auto base_reason = editor_actor_block_reason(actor_full_name, actor_class_full_name);
        if (!base_reason.empty()) return base_reason;

        const auto haystack = lower_copy(actor_full_name + " " + actor_class_full_name);
        const std::initializer_list<std::pair<const char*, const char*>> blocked{
            {"levelscriptactor", "level-script-actor"},
            {"worldsettings", "world-settings"},
            {"gamemode", "game-mode"},
            {"gamestate", "game-state"},
            {"playerstart", "player-start"},
            {"navigation", "navigation"},
            {"navmesh", "navigation"},
            {"volume", "volume-or-trigger"},
            {"trigger", "volume-or-trigger"},
            {"blockingvolume", "volume-or-trigger"},
            {"camera", "camera"},
            {"lightmass", "lighting"},
            {"skylight", "lighting"},
            {"directionallight", "lighting"},
            {"pointlight", "lighting"},
            {"spotlight", "lighting"},
            {"reflectioncapture", "reflection-capture"},
            {"postprocess", "post-process"},
            {"audio", "audio"},
            {"sound", "audio"},
            {"foliage", "foliage"},
            {"landscape", "landscape"},
            {"waterbody", "water"},
        };

        for (const auto& [needle, reason] : blocked)
        {
            if (haystack.find(needle) != std::string::npos) return reason;
        }
        return {};
    }

    struct EditorLeafResolveCandidate
    {
        RC::Unreal::UObject* object{};
        std::string full_name;
        std::string class_full_name;
        RC::Unreal::FVector location{};
        double distance_cm{std::numeric_limits<double>::infinity()};
        double client_actor_distance_cm{std::numeric_limits<double>::infinity()};
        double client_hit_distance_cm{std::numeric_limits<double>::infinity()};
        double score{std::numeric_limits<double>::infinity()};
        bool path_match{};
        bool outer_match{};
    };

    auto read_editor_vector_from_command(
        const std::string& command_text,
        std::initializer_list<const char*> x_keys,
        std::initializer_list<const char*> y_keys,
        std::initializer_list<const char*> z_keys,
        RC::Unreal::FVector& value) -> bool;
    auto outer_path_from_full_name(const std::string& full_name_value) -> std::string;
    auto normalized_path_match(const std::string& left, const std::string& right) -> bool;
    auto validated_uobject_pointer(RC::Unreal::UObject* object, std::string& detail) -> bool;

    auto is_specific_editor_class_token(const std::string& value) -> bool
    {
        const auto token = lower_copy(value);
        if (token.empty()) return false;
        if (token == "actor" || token == "aactor" || token == "object" || token == "uobject") return false;
        if (token == "class" || token == "blueprintgeneratedclass") return false;
        if (token == "staticmeshactor" || token == "skeletalmeshactor") return false;
        return true;
    }

    auto add_editor_class_search_name(std::vector<std::wstring>& class_names, const std::string& value) -> void
    {
        if (value.empty()) return;
        push_unique_class_search_name(class_names, widen(value));
    }

    auto add_editor_class_leaf_search_name(std::vector<std::wstring>& class_names, const std::string& value) -> void
    {
        const auto leaf = object_leaf_from_full_name(value);
        if (leaf.empty()) return;
        if (leaf.find("_C") == std::string::npos && leaf.find("Actor") == std::string::npos) return;
        add_editor_class_search_name(class_names, leaf);
    }

    auto editor_candidate_matches_requested_class(
        const std::string& requested_token,
        const std::string& requested_class_text,
        const std::string& candidate_full_name,
        const std::string& candidate_class_full_name) -> bool
    {
        std::vector<std::string> needles;
        if (is_specific_editor_class_token(requested_token)) needles.push_back(lower_copy(requested_token));
        const auto requested_class_leaf = object_leaf_from_full_name(requested_class_text);
        if (is_specific_editor_class_token(requested_class_leaf)) needles.push_back(lower_copy(requested_class_leaf));
        if (needles.empty()) return true;

        const auto haystack = lower_copy(candidate_full_name + " " + candidate_class_full_name);
        for (const auto& needle : needles)
        {
            if (!needle.empty() && haystack.find(needle) != std::string::npos) return true;
        }
        return false;
    }

    auto find_editor_actor_by_exact_path_index(
        const std::string& command_text,
        const std::string& requested_handle,
        std::vector<std::string>& attempts,
        const char* label) -> RC::Unreal::AActor*
    {
        const auto requested_path = object_path_from_full_name(requested_handle);
        const auto requested_leaf = object_leaf_from_full_name(requested_handle);
        if (requested_path.empty() || requested_leaf.empty())
        {
            attempts.push_back(std::string(label) + ".ExactPathIndex skipped: empty path/leaf");
            return nullptr;
        }

        if (requested_path.find(':') == std::string::npos)
        {
            attempts.push_back(std::string(label) + ".ExactPathIndex skipped: non-level object path");
            return nullptr;
        }

        const auto requested_token = class_token_from_full_name(requested_handle);
        const auto requested_class_text = first_regex_value(
            command_text,
            {"targetActorClass", "actorClassFullName", "actorClassName", "targetClass", "className"});

        std::vector<std::wstring> class_names;
        add_editor_class_search_name(class_names, requested_token);
        add_editor_class_leaf_search_name(class_names, requested_class_text);
        push_unique_class_search_name(class_names, L"Actor");
        push_unique_class_search_name(class_names, L"AActor");
        push_unique_class_search_name(class_names, L"StaticMeshActor");
        push_unique_class_search_name(class_names, L"SkeletalMeshActor");

        const auto leaf_w = widen(requested_leaf);
        std::vector<RC::Unreal::UObject*> matches;
        std::unordered_set<RC::Unreal::UObject*> seen;

        for (const auto& class_name : class_names)
        {
            std::vector<RC::Unreal::UObject*> found;
            RC::Unreal::UObjectGlobals::FindObjects(
                class_name.c_str(),
                leaf_w.c_str(),
                found,
                0,
                object_flag_class_default_object,
                false);
            attempts.push_back(
                std::string(label) + ".ExactPathIndex.FindObjects(" +
                narrow(class_name) + "," + requested_leaf + ")=" + std::to_string(found.size()));

            for (auto* object : found)
            {
                if (object == nullptr || !seen.insert(object).second) continue;

                std::string validation_detail;
                if (!validated_uobject_pointer(object, validation_detail))
                {
                    attempts.push_back(std::string(label) + ".ExactPathIndex rejected pointer-invalid detail=" + validation_detail);
                    continue;
                }

                const auto submitted_block_reason = editor_actor_block_reason(requested_handle, requested_class_text);
                if (!submitted_block_reason.empty())
                {
                    attempts.push_back(std::string(label) + ".ExactPathIndex rejected blocked-from-submitted-identity=" + submitted_block_reason + ":" + requested_handle);
                    continue;
                }

                RC::Unreal::FVector location{};
                if (!actor_location_noexcept(object, location))
                {
                    attempts.push_back(std::string(label) + ".ExactPathIndex rejected:not-aactor-or-location-failed leaf=" + requested_leaf);
                    continue;
                }

                matches.push_back(object);
            }

            if (!matches.empty())
            {
                attempts.push_back(
                    std::string(label) + ".ExactPathIndex accepted class-specific leaf bucket=" +
                    narrow(class_name) + " matches=" + std::to_string(matches.size()) +
                    "; skipped broader class buckets");
                break;
            }
        }

        if (matches.empty())
        {
            attempts.push_back(std::string(label) + ".ExactPathIndex no exact path match for " + requested_path);
            return nullptr;
        }
        if (matches.size() > 1)
        {
            attempts.push_back(std::string(label) + ".ExactPathIndex ambiguous validated leaf matches=" + std::to_string(matches.size()) + " for exact request " + requested_path);
            return nullptr;
        }

        attempts.push_back(std::string(label) + ".ExactPathIndex accepted unique validated leaf for exact request " + requested_path);
        return static_cast<RC::Unreal::AActor*>(matches.front());
    }

    auto editor_actor_identity_cache_key(const std::string& command_text, const std::string& requested_handle) -> std::string
    {
        auto key = first_regex_value(command_text, {"clientActorFullName", "actorFullName", "targetActorFullName", "targetFullName", "fullName", "objectPath", "targetObjectPath"});
        if (key.empty()) key = requested_handle;
        return key;
    }

    auto cache_editor_actor_identity(
        const std::string& key,
        RC::Unreal::AActor* actor,
        const std::string& submitted_full_name = {},
        const std::string& submitted_class_name = {}) -> void
    {
        if (key.empty() || actor == nullptr) return;
        g_editor_actor_identity_cache[key] = CachedEditorActorIdentity{
            actor,
            !submitted_full_name.empty() ? submitted_full_name : key,
            submitted_class_name,
            {},
            {},
            false,
            false};
    }

    auto update_editor_actor_identity_cache_transform(
        const std::string& command_text,
        const std::string& requested_handle,
        RC::Unreal::AActor* actor,
        const RC::Unreal::FVector& location,
        const RC::Unreal::FRotator& rotation,
        bool has_rotation) -> void
    {
        const auto key = editor_actor_identity_cache_key(command_text, requested_handle);
        if (key.empty() || actor == nullptr) return;
        auto cache_it = g_editor_actor_identity_cache.find(key);
        if (cache_it == g_editor_actor_identity_cache.end())
        {
            cache_editor_actor_identity(key, actor, requested_handle);
            cache_it = g_editor_actor_identity_cache.find(key);
            if (cache_it == g_editor_actor_identity_cache.end()) return;
        }
        if (cache_it->second.actor != actor) return;
        cache_it->second.last_location = location;
        cache_it->second.has_location = true;
        if (has_rotation)
        {
            cache_it->second.last_rotation = rotation;
            cache_it->second.has_rotation = true;
        }
    }

    auto cached_editor_actor_transform_from_command(
        const std::string& command_text,
        const std::string& requested_handle,
        RC::Unreal::AActor* actor,
        RC::Unreal::FVector& location,
        RC::Unreal::FRotator& rotation,
        bool& has_rotation,
        std::vector<std::string>& attempts) -> bool
    {
        const auto key = editor_actor_identity_cache_key(command_text, requested_handle);
        if (key.empty() || actor == nullptr) return false;
        const auto cache_it = g_editor_actor_identity_cache.find(key);
        if (cache_it == g_editor_actor_identity_cache.end()) return false;
        if (cache_it->second.actor != actor || !cache_it->second.has_location) return false;
        location = cache_it->second.last_location;
        if (cache_it->second.has_rotation)
        {
            rotation = cache_it->second.last_rotation;
            has_rotation = true;
        }
        attempts.push_back("editorActor.TransformCache fallback key=" + key);
        return true;
    }

    auto find_editor_actor_by_cached_client_identity_from_command(
        const std::string& command_text,
        const std::string& requested_handle,
        std::vector<std::string>& attempts) -> RC::Unreal::AActor*
    {
        const auto key = editor_actor_identity_cache_key(command_text, requested_handle);
        if (key.empty()) return nullptr;
        const auto cache_it = g_editor_actor_identity_cache.find(key);
        if (cache_it == g_editor_actor_identity_cache.end()) return nullptr;

        auto* actor = cache_it->second.actor;
        auto* object = static_cast<RC::Unreal::UObject*>(actor);
        std::string validation_detail;
        if (!validated_uobject_pointer(object, validation_detail))
        {
            attempts.push_back("editorActor.ClientIdentityCache stale key=" + key + " detail=" + validation_detail);
            g_editor_actor_identity_cache.erase(cache_it);
            return nullptr;
        }

        RC::Unreal::FVector location{};
        if (!actor_location_noexcept(object, location))
        {
            attempts.push_back("editorActor.ClientIdentityCache rejected location-failed key=" + key);
            g_editor_actor_identity_cache.erase(cache_it);
            return nullptr;
        }
        cache_it->second.last_location = location;
        cache_it->second.has_location = true;

        RC::Unreal::FRotator rotation{};
        if (actor_rotation_noexcept(object, rotation))
        {
            cache_it->second.last_rotation = rotation;
            cache_it->second.has_rotation = true;
        }

        const auto actual_full = !cache_it->second.full_name.empty()
            ? cache_it->second.full_name
            : (requested_handle.empty() ? key : requested_handle);
        const auto actual_class = !cache_it->second.class_full_name.empty()
            ? cache_it->second.class_full_name
            : object_class_full_name_noexcept(object);
        const auto requested_leaf = object_leaf_from_full_name(requested_handle.empty() ? key : requested_handle);
        const auto actual_leaf = object_leaf_from_full_name(actual_full);
        if (!requested_leaf.empty() && requested_leaf != actual_leaf)
        {
            attempts.push_back("editorActor.ClientIdentityCache rejected leaf-mismatch key=" + key + " expected=" + requested_leaf + " actual=" + actual_leaf);
            g_editor_actor_identity_cache.erase(cache_it);
            return nullptr;
        }

        const auto requested_token = class_token_from_full_name(requested_handle.empty() ? key : requested_handle);
        const auto requested_class_text = first_regex_value(
            command_text,
            {"targetActorClass", "actorClassFullName", "actorClassName", "targetClass", "className"});
        if (!editor_candidate_matches_requested_class(requested_token, requested_class_text, actual_full, actual_class))
        {
            attempts.push_back("editorActor.ClientIdentityCache rejected class-mismatch key=" + key + " actor=" + actual_full + " class=" + actual_class);
            g_editor_actor_identity_cache.erase(cache_it);
            return nullptr;
        }

        const auto block_reason = editor_actor_block_reason(actual_full, actual_class);
        if (!block_reason.empty())
        {
            attempts.push_back("editorActor.ClientIdentityCache rejected blocked=" + block_reason + ":" + actual_full);
            g_editor_actor_identity_cache.erase(cache_it);
            return nullptr;
        }

        attempts.push_back("editorActor.ClientIdentityCache accepted key=" + key + " actor=" + actual_full + " validation=" + validation_detail);
        return actor;
    }

    auto find_editor_actor_by_trace_component_from_command(
        const std::string& command_text,
        const std::string& requested_handle,
        std::vector<std::string>& attempts) -> RC::Unreal::AActor*
    {
        const auto component_full_name = first_regex_value(
            command_text,
            {"traceComponentFullName", "componentFullName", "targetComponentFullName", "hitComponentFullName"});
        if (component_full_name.empty())
        {
            return nullptr;
        }

        const auto component_path = object_path_from_full_name(component_full_name);
        const auto component_leaf = object_leaf_from_full_name(component_full_name);
        const auto requested_path = object_path_from_full_name(requested_handle);
        if (component_path.empty() || component_leaf.empty())
        {
            attempts.push_back("editorActor.ComponentOwner skipped: empty component path/leaf");
            return nullptr;
        }
        if (!requested_path.empty() && component_path.rfind(requested_path + ".", 0) != 0)
        {
            attempts.push_back("editorActor.ComponentOwner skipped: component path does not belong to requested actor path component=" + component_path + " requested=" + requested_path);
            return nullptr;
        }

        std::vector<std::wstring> class_names;
        add_editor_class_search_name(class_names, class_token_from_full_name(component_full_name));
        push_unique_class_search_name(class_names, L"HierarchicalInstancedStaticMeshComponent");
        push_unique_class_search_name(class_names, L"InstancedStaticMeshComponent");
        push_unique_class_search_name(class_names, L"StaticMeshComponent");
        push_unique_class_search_name(class_names, L"PrimitiveComponent");
        push_unique_class_search_name(class_names, L"SceneComponent");
        push_unique_class_search_name(class_names, L"ActorComponent");
        push_unique_class_search_name(class_names, L"Object");

        const auto component_path_w = widen(component_path);
        for (const auto& class_name : class_names)
        {
            auto* component = RC::Unreal::UObjectGlobals::FindObject(
                class_name.c_str(),
                component_path_w.c_str(),
                0,
                object_flag_class_default_object);
            attempts.push_back(
                "editorActor.ComponentOwner.FindObject(" +
                narrow(class_name) + "," + component_path + ")=" + (component != nullptr ? "1" : "0"));
            if (component == nullptr)
            {
                continue;
            }

            std::string component_validation;
            if (!validated_uobject_pointer(component, component_validation))
            {
                attempts.push_back("editorActor.ComponentOwner rejected component pointer-invalid detail=" + component_validation);
                continue;
            }

            std::vector<std::string> details;
            auto* owner = object_property_noexcept(component, L"Owner", details);
            std::string owner_validation;
            if (!validated_uobject_pointer(owner, owner_validation))
            {
                attempts.push_back("editorActor.ComponentOwner rejected owner pointer-invalid detail=" + owner_validation);
                continue;
            }

            RC::Unreal::FVector owner_location{};
            if (!actor_location_noexcept(owner, owner_location))
            {
                attempts.push_back("editorActor.ComponentOwner rejected owner location-failed component=" + component_leaf);
                continue;
            }

            const auto requested_class_text = first_regex_value(
                command_text,
                {"targetActorClass", "actorClassFullName", "actorClassName", "targetClass", "className"});
            const auto submitted_block_reason = editor_actor_block_reason(requested_handle, requested_class_text);
            if (!submitted_block_reason.empty())
            {
                attempts.push_back("editorActor.ComponentOwner rejected blocked-from-submitted-identity=" + submitted_block_reason + ":" + requested_handle);
                continue;
            }

            auto* actor = static_cast<RC::Unreal::AActor*>(owner);
            const auto key = editor_actor_identity_cache_key(command_text, requested_handle);
            cache_editor_actor_identity(key, actor, requested_handle, requested_class_text);
            attempts.push_back(
                "editorActor.ComponentOwner accepted component=" + component_path +
                " ownerFromComponent=1 validation=" + component_validation + "/" + owner_validation);
            return actor;
        }

        return nullptr;
    }

    auto find_editor_actor_by_client_identity_candidate_from_command(
        const std::string& command_text,
        const std::string& requested_handle,
        std::vector<std::string>& attempts) -> RC::Unreal::AActor*
    {
        const auto requested_leaf = object_leaf_from_full_name(requested_handle);
        if (requested_leaf.empty())
        {
            attempts.push_back("editorActor.ClientIdentity missing requested leaf");
            return nullptr;
        }

        const auto requested_path = object_path_from_full_name(requested_handle);
        attempts.push_back("editorActor.ClientIdentity.FindObjects(leaf)=disabled-after-2026-06-05-actor-editor-crash; exact FindObject(objectPath) must resolve the actor before this fallback");
        return nullptr;

        const auto replay_exact_path_allowed =
            regex_bool(command_text, "replay", false) &&
            !requested_path.empty() &&
            requested_path.find(':') != std::string::npos;

        RC::Unreal::FVector client_actor_location{};
        const auto has_client_actor_location = read_editor_vector_from_command(
            command_text,
            {"clientActorX", "targetActorX", "selectedActorX"},
            {"clientActorY", "targetActorY", "selectedActorY"},
            {"clientActorZ", "targetActorZ", "selectedActorZ"},
            client_actor_location);
        RC::Unreal::FVector client_hit_location{};
        const auto has_client_hit_location = read_editor_vector_from_command(
            command_text,
            {"clientHitX", "clientImpactX", "hitX", "impactX"},
            {"clientHitY", "clientImpactY", "hitY", "impactY"},
            {"clientHitZ", "clientImpactZ", "hitZ", "impactZ"},
            client_hit_location);

        double player_x{};
        double player_y{};
        double player_z{};
        const auto has_player_location =
            first_regex_number(command_text, {"playerX", "PlayerX"}, player_x) &&
            first_regex_number(command_text, {"playerY", "PlayerY"}, player_y) &&
            first_regex_number(command_text, {"playerZ", "PlayerZ"}, player_z);
        const auto player_location = make_vector(player_x, player_y, player_z);

        if (!has_client_actor_location && !has_client_hit_location && !has_player_location && !replay_exact_path_allowed)
        {
            attempts.push_back("editorActor.ClientIdentity refused: no client actor/hit/player location evidence");
            return nullptr;
        }

        auto requested_outer = first_regex_value(command_text, {"outerLevelPath", "targetOuterLevelPath", "clientOuterLevelPath"});
        if (requested_outer.empty()) requested_outer = outer_path_from_full_name(requested_handle);
        const auto requested_token = class_token_from_full_name(requested_handle);
        const auto requested_class_text = first_regex_value(
            command_text,
            {"targetActorClass", "actorClassFullName", "actorClassName", "targetClass", "className"});

        std::vector<std::wstring> class_names;
        add_editor_class_search_name(class_names, requested_token);
        add_editor_class_leaf_search_name(class_names, requested_class_text);
        push_unique_class_search_name(class_names, L"Actor");
        push_unique_class_search_name(class_names, L"AActor");
        push_unique_class_search_name(class_names, L"StaticMeshActor");
        push_unique_class_search_name(class_names, L"SkeletalMeshActor");

        constexpr double max_client_actor_distance_cm = 1500.0;
        constexpr double max_client_hit_distance_cm = 3500.0;
        constexpr double max_player_distance_cm = 12000.0;
        std::vector<EditorLeafResolveCandidate> candidates;
        std::unordered_set<RC::Unreal::UObject*> seen;
        const auto requested_leaf_w = widen(requested_leaf);

        for (const auto& class_name : class_names)
        {
            std::vector<RC::Unreal::UObject*> found;
            RC::Unreal::UObjectGlobals::FindObjects(class_name.c_str(), requested_leaf_w.c_str(), found, 0, object_flag_class_default_object, false);
            std::ostringstream route;
            route << "editorActor.ClientIdentity.FindObjects(" << narrow(class_name) << "," << requested_leaf << ")=" << found.size();
            if (!found.empty()) route << ":first=" << safe_full_name(found.front());
            attempts.push_back(route.str());

            for (auto* object : found)
            {
                if (object == nullptr || !seen.insert(object).second) continue;

                std::string validation_detail;
                if (!validated_uobject_pointer(object, validation_detail))
                {
                    attempts.push_back("editorActor.ClientIdentity rejected pointer-invalid detail=" + validation_detail);
                    continue;
                }

                RC::Unreal::FVector location{};
                if (!actor_location_noexcept(object, location))
                {
                    attempts.push_back("editorActor.ClientIdentity rejected location-failed");
                    continue;
                }

                const auto candidate_full = safe_full_name(object);
                const auto candidate_leaf = object_leaf_from_full_name(candidate_full);
                if (candidate_leaf != requested_leaf)
                {
                    attempts.push_back("editorActor.ClientIdentity rejected leaf-mismatch:" + candidate_full);
                    continue;
                }

                const auto candidate_class = object_class_full_name_noexcept(object);
                const auto block_reason = editor_actor_block_reason(candidate_full, candidate_class);
                if (!block_reason.empty())
                {
                    attempts.push_back("editorActor.ClientIdentity rejected blocked=" + block_reason + ":" + candidate_full);
                    continue;
                }

                if (!editor_candidate_matches_requested_class(requested_token, requested_class_text, candidate_full, candidate_class))
                {
                    attempts.push_back("editorActor.ClientIdentity rejected class-mismatch:" + candidate_full + " class=" + candidate_class);
                    continue;
                }

                EditorLeafResolveCandidate candidate{};
                candidate.object = object;
                candidate.full_name = candidate_full;
                candidate.class_full_name = candidate_class;
                candidate.location = location;
                candidate.distance_cm = has_player_location ? vector_distance_3d(location, player_location) : std::numeric_limits<double>::infinity();
                candidate.client_actor_distance_cm = has_client_actor_location ? vector_distance_3d(location, client_actor_location) : std::numeric_limits<double>::infinity();
                candidate.client_hit_distance_cm = has_client_hit_location ? vector_distance_3d(location, client_hit_location) : std::numeric_limits<double>::infinity();
                candidate.path_match = object_path_matches(candidate_full, requested_path);
                candidate.outer_match = normalized_path_match(outer_path_from_full_name(candidate_full), requested_outer);

                const auto exact_client_path_identity = candidate.path_match;
                if (!exact_client_path_identity && has_client_actor_location && candidate.client_actor_distance_cm > max_client_actor_distance_cm)
                {
                    attempts.push_back("editorActor.ClientIdentity rejected actor-distance=" + std::to_string(candidate.client_actor_distance_cm) + ":" + candidate_full);
                    continue;
                }
                if (!exact_client_path_identity && !has_client_actor_location && has_client_hit_location && candidate.client_hit_distance_cm > max_client_hit_distance_cm)
                {
                    attempts.push_back("editorActor.ClientIdentity rejected hit-distance=" + std::to_string(candidate.client_hit_distance_cm) + ":" + candidate_full);
                    continue;
                }
                if (!exact_client_path_identity && has_player_location && candidate.distance_cm > max_player_distance_cm)
                {
                    attempts.push_back("editorActor.ClientIdentity rejected player-distance=" + std::to_string(candidate.distance_cm) + ":" + candidate_full);
                    continue;
                }

                candidate.score = 0.0;
                if (exact_client_path_identity)
                {
                    // SCUM streamed levels can expose client actor/hit coordinates in local level space while
                    // the dedicated server reports world-space locations. Exact object path identity is stronger
                    // than those cross-space distances, so use distance only as a fallback discriminator.
                    candidate.score -= 1000.0;
                    attempts.push_back("editorActor.ClientIdentity exact-path identity bypassed cross-space distance guard:" + candidate_full);
                }
                else
                {
                    if (has_client_actor_location) candidate.score += candidate.client_actor_distance_cm;
                    else if (has_client_hit_location) candidate.score += candidate.client_hit_distance_cm;
                    if (has_player_location) candidate.score += candidate.distance_cm * 0.02;
                }
                if (candidate.path_match) candidate.score -= 500.0;
                if (candidate.outer_match) candidate.score -= 150.0;
                candidates.push_back(candidate);
            }
        }

        if (candidates.empty())
        {
            attempts.push_back("editorActor.ClientIdentity no strict usable candidate for leaf=" + requested_leaf);
            return nullptr;
        }

        std::sort(candidates.begin(), candidates.end(), [](const EditorLeafResolveCandidate& left, const EditorLeafResolveCandidate& right)
        {
            return left.score < right.score;
        });

        const auto& best = candidates.front();
        bool accepted = candidates.size() == 1;
        if (!accepted && candidates.size() >= 2)
        {
            const auto& second = candidates[1];
            const auto score_gap = second.score - best.score;
            const auto best_has_exact_client_identity = best.path_match && std::none_of(
                candidates.begin() + 1,
                candidates.end(),
                [](const EditorLeafResolveCandidate& candidate)
                {
                    return candidate.path_match;
                });
            const auto best_has_strong_location =
                (has_client_actor_location && best.client_actor_distance_cm <= 250.0) ||
                (!has_client_actor_location && has_client_hit_location && best.client_hit_distance_cm <= 600.0);
            accepted = best_has_exact_client_identity || (best_has_strong_location && score_gap >= 250.0);
        }

        std::ostringstream summary;
        summary << "editorActor.ClientIdentity best count=" << candidates.size()
                << " score=" << best.score
                << " actorDistance=" << best.client_actor_distance_cm
                << " hitDistance=" << best.client_hit_distance_cm
                << " playerDistance=" << best.distance_cm
                << " pathMatch=" << (best.path_match ? "true" : "false")
                << " outerMatch=" << (best.outer_match ? "true" : "false")
                << " actor=" << best.full_name;
        attempts.push_back(summary.str());

        if (!accepted)
        {
            if (candidates.size() > 1)
            {
                attempts.push_back("editorActor.ClientIdentity ambiguous strict candidates; refusing mutation");
            }
            return nullptr;
        }

        attempts.push_back("editorActor.ClientIdentity accepted strict candidate:" + best.full_name);
        cache_editor_actor_identity(editor_actor_identity_cache_key(command_text, requested_handle), static_cast<RC::Unreal::AActor*>(best.object));
        return static_cast<RC::Unreal::AActor*>(best.object);
    }

    auto find_editor_actor_by_leaf_from_command(
        const std::string& command_text,
        const std::string& requested_handle,
        std::vector<std::string>& attempts) -> RC::Unreal::AActor*
    {
        (void)command_text;
        (void)requested_handle;
        attempts.push_back("editorActor.LeafResolve disabled after 2026-06-05 live nudge crash; use exact FindObject only until a safe network/server identity route is proven");
        return nullptr;

        const auto requested_leaf = object_leaf_from_full_name(requested_handle);
        if (requested_leaf.empty()) return nullptr;

        const auto requested_path = object_path_from_full_name(requested_handle);
        const auto requested_token = class_token_from_full_name(requested_handle);
        const auto requested_class_text = first_regex_value(
            command_text,
            {"targetActorClass", "actorClassName", "targetClass", "className"});

        std::vector<std::wstring> class_names;
        add_editor_class_search_name(class_names, requested_token);
        add_editor_class_leaf_search_name(class_names, requested_class_text);
        push_unique_class_search_name(class_names, L"Actor");
        push_unique_class_search_name(class_names, L"AActor");
        push_unique_class_search_name(class_names, L"StaticMeshActor");
        push_unique_class_search_name(class_names, L"SkeletalMeshActor");
        push_unique_class_search_name(class_names, L"Object");

        double player_x{};
        double player_y{};
        double player_z{};
        const auto has_player_location =
            first_regex_number(command_text, {"playerX", "PlayerX"}, player_x) &&
            first_regex_number(command_text, {"playerY", "PlayerY"}, player_y) &&
            first_regex_number(command_text, {"playerZ", "PlayerZ"}, player_z);
        const auto player_location = make_vector(player_x, player_y, player_z);

        std::vector<EditorLeafResolveCandidate> candidates;
        std::unordered_set<RC::Unreal::UObject*> seen;
        const auto requested_leaf_w = widen(requested_leaf);

        for (const auto& class_name : class_names)
        {
            std::vector<RC::Unreal::UObject*> found;
            RC::Unreal::UObjectGlobals::FindObjects(class_name.c_str(), requested_leaf_w.c_str(), found, 0, object_flag_class_default_object, false);
            std::ostringstream route;
            route << "editorActor.LeafResolve.FindObjects(" << narrow(class_name) << "," << requested_leaf << ")=" << found.size();
            if (!found.empty()) route << ":first=" << safe_full_name(found.front());
            attempts.push_back(route.str());

            for (auto* object : found)
            {
                if (object == nullptr || !seen.insert(object).second) continue;

                RC::Unreal::FVector location{};
                if (!actor_location_noexcept(object, location))
                {
                    attempts.push_back("editorActor.LeafResolve rejected location-failed");
                    continue;
                }

                const auto candidate_full = safe_full_name(object);
                const auto candidate_class = object_class_full_name_noexcept(object);

                const auto block_reason = editor_actor_block_reason(candidate_full, candidate_class);
                if (!block_reason.empty())
                {
                    attempts.push_back("editorActor.LeafResolve rejected blocked=" + block_reason + ":" + candidate_full);
                    continue;
                }

                if (!editor_candidate_matches_requested_class(requested_token, requested_class_text, candidate_full, candidate_class))
                {
                    attempts.push_back("editorActor.LeafResolve rejected class-mismatch:" + candidate_full + " class=" + candidate_class);
                    continue;
                }

                if (object_path_matches(candidate_full, requested_path))
                {
                    attempts.push_back("editorActor.LeafResolve accepted path-match:" + candidate_full);
                    return static_cast<RC::Unreal::AActor*>(object);
                }

                EditorLeafResolveCandidate candidate{};
                candidate.object = object;
                candidate.full_name = candidate_full;
                candidate.class_full_name = candidate_class;
                candidate.location = location;
                candidate.distance_cm = has_player_location ? vector_distance_3d(location, player_location) : std::numeric_limits<double>::infinity();
                candidates.push_back(candidate);
            }
        }

        if (candidates.empty())
        {
            attempts.push_back("editorActor.LeafResolve no usable candidate for leaf=" + requested_leaf);
            return nullptr;
        }

        if (candidates.size() == 1)
        {
            attempts.push_back("editorActor.LeafResolve accepted unique leaf candidate:" + candidates.front().full_name);
            return static_cast<RC::Unreal::AActor*>(candidates.front().object);
        }

        if (has_player_location)
        {
            std::sort(candidates.begin(), candidates.end(), [](const EditorLeafResolveCandidate& left, const EditorLeafResolveCandidate& right)
            {
                return left.distance_cm < right.distance_cm;
            });
            attempts.push_back(
                "editorActor.LeafResolve accepted nearest leaf candidate count=" + std::to_string(candidates.size()) +
                " distanceCm=" + std::to_string(candidates.front().distance_cm) +
                ":" + candidates.front().full_name);
            return static_cast<RC::Unreal::AActor*>(candidates.front().object);
        }

        attempts.push_back("editorActor.LeafResolve ambiguous leaf without player location count=" + std::to_string(candidates.size()));
        return nullptr;
    }

    auto find_editor_actor_by_server_address_from_command(
        const std::string& command_text,
        std::vector<std::string>& attempts,
        std::string& requested_handle) -> RC::Unreal::AActor*
    {
        std::uintptr_t address{};
        std::string raw_address;
        const auto has_address = first_regex_address(
            command_text,
            {"serverActorAddress", "serverActorKey", "actorAddress", "actorPointerAddress", "actorKey"},
            address,
            raw_address);
        if (!has_address)
        {
            if (!raw_address.empty())
            {
                attempts.push_back("editorActor.ServerAddress invalid=" + raw_address);
            }
            return nullptr;
        }

        const auto expected_server_full = first_regex_value(command_text, {"serverActorFullName", "serverFullName"});
        const auto expected_server_class = first_regex_value(command_text, {"serverActorClassFullName", "serverActorClass"});
        requested_handle = !expected_server_full.empty() ? expected_server_full : ("serverActorAddress:" + raw_address);

        if (!committed_readable_address(address, sizeof(std::uintptr_t) + 0x10))
        {
            attempts.push_back("editorActor.ServerAddress unreadable " + hex_address(address));
            return nullptr;
        }

        auto* object = reinterpret_cast<RC::Unreal::UObject*>(address);
        std::int32_t object_index = -1;
        if (!safe_read(address + 0x0C, object_index) || object_index < 0)
        {
            attempts.push_back("editorActor.ServerAddress internal-index-unreadable " + hex_address(address));
            return nullptr;
        }

        FUObjectItemAbi item{};
        std::uintptr_t item_address{};
        std::string item_detail;
        if (!read_uobject_item_by_index(object_index, item, item_detail, &item_address, address) || item.object != address)
        {
            attempts.push_back("editorActor.ServerAddress object-item-mismatch " + hex_address(address) + " index=" + std::to_string(object_index) + " detail=" + item_detail);
            return nullptr;
        }

        auto* object_class = safe_class_private(object);
        if (object_class == nullptr)
        {
            attempts.push_back("editorActor.ServerAddress class-unreadable " + hex_address(address));
            return nullptr;
        }

        RC::Unreal::FVector location{};
        if (!actor_location_noexcept(object, location))
        {
            attempts.push_back("editorActor.ServerAddress rejected:not-aactor-or-location-failed " + hex_address(address));
            return nullptr;
        }

        const auto actual_full = safe_full_name(object);
        const auto actual_class = object_class_full_name_noexcept(object);
        if (!expected_server_full.empty() && actual_full != expected_server_full)
        {
            attempts.push_back("editorActor.ServerAddress stale-full-name expected=" + expected_server_full + " actual=" + actual_full);
            return nullptr;
        }
        if (!expected_server_class.empty() && actual_class != expected_server_class)
        {
            attempts.push_back("editorActor.ServerAddress stale-class expected=" + expected_server_class + " actual=" + actual_class);
            return nullptr;
        }

        attempts.push_back(
            "editorActor.ServerAddress accepted address=" + hex_address(address) +
            " index=" + std::to_string(object_index) +
            " item=" + hex_address(item_address) +
            " actor=" + actual_full);
        requested_handle = !actual_full.empty() ? actual_full : requested_handle;
        return static_cast<RC::Unreal::AActor*>(object);
    }

    auto find_editor_actor_from_command(
        const std::string& command_text,
        std::vector<std::string>& attempts,
        std::string& requested_handle) -> RC::Unreal::AActor*
    {
        if (auto* server_address_actor = find_editor_actor_by_server_address_from_command(command_text, attempts, requested_handle))
        {
            return server_address_actor;
        }

        requested_handle = first_regex_value(
            command_text,
            {"actorFullName", "targetActorFullName", "targetFullName", "fullName"});

        if (!requested_handle.empty())
        {
            if (auto* cached_object = find_editor_actor_by_cached_client_identity_from_command(command_text, requested_handle, attempts))
            {
                return cached_object;
            }

            auto* object = find_object_by_full_name(
                {L"Actor", L"AActor", L"StaticMeshActor", L"SkeletalMeshActor", L"Object"},
                requested_handle,
                attempts,
                "editorActor",
                false);
            RC::Unreal::FVector probe_location{};
            if (object != nullptr && actor_location_noexcept(object, probe_location))
            {
                auto* actor = static_cast<RC::Unreal::AActor*>(object);
                cache_editor_actor_identity(
                    editor_actor_identity_cache_key(command_text, requested_handle),
                    actor,
                    requested_handle,
                    first_regex_value(command_text, {"targetActorClass", "actorClassFullName", "actorClassName", "targetClass", "className"}));
                return actor;
            }
            if (object != nullptr)
            {
                attempts.push_back("editorActor.rejected:not-aactor-or-location-failed");
            }

            if (auto* component_owner = find_editor_actor_by_trace_component_from_command(command_text, requested_handle, attempts))
            {
                return component_owner;
            }

            if (auto* exact_index_actor = find_editor_actor_by_exact_path_index(command_text, requested_handle, attempts, "editorActor"))
            {
                cache_editor_actor_identity(
                    editor_actor_identity_cache_key(command_text, requested_handle),
                    exact_index_actor,
                    requested_handle,
                    first_regex_value(command_text, {"targetActorClass", "actorClassFullName", "actorClassName", "targetClass", "className"}));
                return exact_index_actor;
            }

            if (auto* identity_object = find_editor_actor_by_client_identity_candidate_from_command(command_text, requested_handle, attempts))
            {
                return identity_object;
            }
        }

        const auto object_path = first_regex_value(command_text, {"objectPath", "targetObjectPath", "path"});
        if (object_path.empty()) return nullptr;

        requested_handle = object_path;
        if (auto* cached_object = find_editor_actor_by_cached_client_identity_from_command(command_text, object_path, attempts))
        {
            return cached_object;
        }

        const auto object_path_w = widen(object_path);
        for (const auto* class_name : {L"Actor", L"AActor", L"StaticMeshActor", L"SkeletalMeshActor", L"Object"})
        {
            auto* object = RC::Unreal::UObjectGlobals::FindObject(class_name, object_path_w.c_str(), 0, object_flag_class_default_object);
            attempts.push_back(std::string("editorActor.FindObject(") + narrow(class_name) + "," + object_path + ")=" + (object != nullptr ? "1" : "0"));
            RC::Unreal::FVector probe_location{};
            if (object != nullptr && actor_location_noexcept(object, probe_location))
            {
                auto* actor = static_cast<RC::Unreal::AActor*>(object);
                cache_editor_actor_identity(
                    editor_actor_identity_cache_key(command_text, object_path),
                    actor,
                    object_path,
                    first_regex_value(command_text, {"targetActorClass", "actorClassFullName", "actorClassName", "targetClass", "className"}));
                return actor;
            }
        }

        if (auto* component_owner = find_editor_actor_by_trace_component_from_command(command_text, object_path, attempts))
        {
            return component_owner;
        }

        if (auto* exact_index_actor = find_editor_actor_by_exact_path_index(command_text, object_path, attempts, "editorActor"))
        {
            cache_editor_actor_identity(
                editor_actor_identity_cache_key(command_text, object_path),
                exact_index_actor,
                object_path,
                first_regex_value(command_text, {"targetActorClass", "actorClassFullName", "actorClassName", "targetClass", "className"}));
            return exact_index_actor;
        }

        if (auto* identity_object = find_editor_actor_by_client_identity_candidate_from_command(command_text, object_path, attempts))
        {
            return identity_object;
        }

        return nullptr;
    }

    auto editor_actor_submitted_full_name(
        const std::string& command_text,
        const std::string& requested_handle,
        RC::Unreal::UObject* actor) -> std::string
    {
        auto submitted = first_regex_value(
            command_text,
            {"actorFullName", "targetActorFullName", "targetFullName", "fullName"});
        if (!submitted.empty()) return submitted;

        submitted = first_regex_value(command_text, {"objectPath", "targetObjectPath", "path"});
        if (!submitted.empty()) return submitted;

        if (!requested_handle.empty()) return requested_handle;
        return safe_full_name(actor);
    }

    auto editor_actor_submitted_class_name(
        const std::string& command_text,
        RC::Unreal::UObject* actor) -> std::string
    {
        const auto submitted = first_regex_value(
            command_text,
            {"actorClassFullName", "targetActorClass", "actorClassName", "targetClass", "className"});
        if (!submitted.empty()) return submitted;
        return object_class_full_name_noexcept(actor);
    }

    auto actor_teleport_to_noexcept(
        RC::Unreal::AActor* actor,
        const RC::Unreal::FVector& desired_location,
        const RC::Unreal::FRotator& desired_rotation,
        bool& teleport_return,
        unsigned long& exception_code) -> bool
    {
        teleport_return = false;
        exception_code = 0;
        if (actor == nullptr) return false;

        __try
        {
            teleport_return = actor->K2_TeleportTo(desired_location, desired_rotation);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            exception_code = 0xFFFFFFFFul;
            return false;
        }
    }

    auto editor_set_actor_location_and_rotation(
        RC::Unreal::AActor* actor,
        const RC::Unreal::FVector& desired_location,
        const RC::Unreal::FRotator& desired_rotation,
        RC::Unreal::FVector& actual_location,
        RC::Unreal::FRotator& actual_rotation,
        std::vector<std::string>& details) -> bool
    {
        auto* actor_object = static_cast<RC::Unreal::UObject*>(actor);
        auto* root_component = object_property(actor_object, L"RootComponent");
        bool location_property_set = false;
        bool rotation_property_set = false;
        if (root_component != nullptr)
        {
            std::string mobility_detail;
            const auto mobility_property_set = set_uint8_property(root_component, L"Mobility", 2, mobility_detail);
            details.push_back(
                "RootComponent.Mobility(Movable=2)=" +
                std::string(mobility_property_set ? "ok:" : "failed:") +
                mobility_detail);
            std::string location_detail;
            location_property_set = set_vector_property(root_component, L"RelativeLocation", desired_location, location_detail);
            details.push_back(
                "RootComponent.RelativeLocation=" +
                std::string(location_property_set ? "ok:" : "failed:") +
                location_detail);
            std::string rotation_detail;
            rotation_property_set = set_rotator_property(root_component, L"RelativeRotation", desired_rotation, rotation_detail);
            details.push_back(
                "RootComponent.RelativeRotation=" +
                std::string(rotation_property_set ? "ok:" : "failed:") +
                rotation_detail);
        }
        else
        {
            details.push_back("RootComponent.Mobility=skipped:null-root");
        }

        auto location_ok = actor_location_noexcept(actor_object, actual_location);
        auto rotation_ok = actor_rotation_noexcept(actor_object, actual_rotation);
        if (!location_ok && location_property_set)
        {
            actual_location = desired_location;
            location_ok = true;
            details.push_back("editor.directRoot postReadback=trusted-relative-location");
        }
        if (!rotation_ok && rotation_property_set)
        {
            actual_rotation = desired_rotation;
            rotation_ok = true;
            details.push_back("editor.directRoot postReadback=trusted-relative-rotation");
        }
        const auto distance = location_ok ? vector_distance_3d(actual_location, desired_location) : std::numeric_limits<double>::infinity();
        const auto transformed = (location_property_set && rotation_property_set) || (location_ok && distance <= 5.0);
        details.push_back(
            "editor.directRoot invoked=true" +
            std::string(" locationPropertySet=") + (location_property_set ? "true" : "false") +
            " rotationPropertySet=" + std::string(rotation_property_set ? "true" : "false") +
            " locationOk=" + std::string(location_ok ? "true" : "false") +
            " rotationOk=" + std::string(rotation_ok ? "true" : "false") +
            " transformed=" + std::string(transformed ? "true" : "false") +
            " distance3d=" + std::to_string(distance));

        return transformed;
    }

    auto append_trader_marker_samples(std::ostringstream& ss, const TArrayAbi* array, std::size_t max_items) -> void
    {
        ss << "[";
        if (is_reasonable_array(array) && array->data != nullptr)
        {
            const auto* markers = static_cast<const FTraderMarkerAbi*>(array->data);
            const auto count = std::min<std::int32_t>(array->count, static_cast<std::int32_t>(max_items));
            for (std::int32_t index = 0; index < count; ++index)
            {
                const auto& marker = markers[index];
                auto* personality = marker.trader_personality;
                const auto trader_persistent_id = guid_to_string(guid_property(personality, L"TraderPersistentId"));
                if (index > 0) ss << ",";
                ss << "{\"index\":" << index
                   << ",\"shouldRaycastSpawnPosition\":" << (marker.should_raycast_spawn_position ? "true" : "false")
                   << ",\"traderPersonality\":\"" << json_escape(full_name(personality)) << "\""
                   << ",\"traderPersistentId\":\"" << json_escape(trader_persistent_id) << "\""
                   << ",\"spawnTransform\":";
                append_transform_json(ss, marker.spawn_transform);
                ss << ",\"purchasedTradeablesSpawnTransform\":";
                append_transform_json(ss, marker.purchased_tradeables_spawn_transform);
                ss << ",\"depotSpawnTransform\":";
                append_transform_json(ss, marker.depot_spawn_transform);
                ss << "}";
            }
        }
        ss << "]";
    }

    auto append_location_marker_samples(std::ostringstream& ss, const TArrayAbi* array, std::size_t max_items) -> void
    {
        ss << "[";
        if (is_reasonable_array(array) && array->data != nullptr)
        {
            const auto* markers = static_cast<const FTraderLocationMarkerAbi*>(array->data);
            const auto count = std::min<std::int32_t>(array->count, static_cast<std::int32_t>(max_items));
            for (std::int32_t index = 0; index < count; ++index)
            {
                if (index > 0) ss << ",";
                ss << "{\"index\":" << index
                   << ",\"markerType\":" << markers[index].marker_type
                   << ",\"transform\":";
                append_transform_json(ss, markers[index].transform);
                ss << "}";
            }
        }
        ss << "]";
    }

    auto object_array_samples(const TArrayAbi* array, std::size_t max_items) -> std::vector<std::string>
    {
        std::vector<std::string> samples;
        if (!is_reasonable_array(array) || array->data == nullptr) return samples;
        auto** items = static_cast<RC::Unreal::UObject**>(array->data);
        const auto count = std::min<std::int32_t>(array->count, static_cast<std::int32_t>(max_items));
        for (std::int32_t index = 0; index < count; ++index)
        {
            samples.push_back(full_name(items[index]));
        }
        return samples;
    }

    auto append_object_array_samples_json(std::ostringstream& ss, const TArrayAbi* array, std::size_t max_items) -> void
    {
        append_json_string_array(ss, object_array_samples(array, max_items));
    }

    auto append_array_count_json(std::ostringstream& ss, const TArrayAbi* array) -> void
    {
        ss << "{\"valid\":" << (is_reasonable_array(array) ? "true" : "false")
           << ",\"count\":" << (is_reasonable_array(array) ? array->count : -1)
           << ",\"max\":" << (is_reasonable_array(array) ? array->max : -1)
           << "}";
    }

    auto array_count_or_negative(const TArrayAbi* array) -> std::int32_t
    {
        return is_reasonable_array(array) ? array->count : -1;
    }

    auto append_unique_object(
        std::vector<RC::Unreal::UObject*>& result,
        std::unordered_set<RC::Unreal::UObject*>& seen,
        RC::Unreal::UObject* object) -> bool
    {
        if (object == nullptr) return false;
        if (!seen.insert(object).second) return false;
        result.push_back(object);
        return true;
    }

    auto find_all_unique(std::initializer_list<const wchar_t*> class_names, std::vector<std::string>* route_counts = nullptr) -> std::vector<RC::Unreal::UObject*>
    {
        std::vector<RC::Unreal::UObject*> result;
        std::unordered_set<RC::Unreal::UObject*> seen;
        for (const auto* class_name : class_names)
        {
            std::vector<RC::Unreal::UObject*> found;
            RC::Unreal::UObjectGlobals::FindAllOf(class_name, found);
            if (route_counts != nullptr)
            {
                std::ostringstream route;
                route << narrow(class_name) << "=" << found.size();
                route_counts->push_back(route.str());
            }
            for (auto* object : found)
            {
                append_unique_object(result, seen, object);
            }
        }
        return result;
    }

    auto find_named_objects_unique(
        std::initializer_list<const wchar_t*> class_names,
        std::initializer_list<const wchar_t*> object_names,
        std::vector<std::string>* route_counts = nullptr) -> std::vector<RC::Unreal::UObject*>
    {
        std::vector<RC::Unreal::UObject*> result;
        std::unordered_set<RC::Unreal::UObject*> seen;
        for (const auto* class_name : class_names)
        {
            for (const auto* object_name : object_names)
            {
                std::vector<RC::Unreal::UObject*> found;
                RC::Unreal::UObjectGlobals::FindObjects(class_name, object_name, found, 0, object_flag_class_default_object, false);
                if (route_counts != nullptr)
                {
                    std::ostringstream route;
                    route << "FindObjects(" << narrow(class_name) << "," << narrow(object_name) << ")=" << found.size();
                    route_counts->push_back(route.str());
                }
                for (auto* object : found)
                {
                    append_unique_object(result, seen, object);
                }
            }
        }
        return result;
    }

    auto find_path_objects_unique(
        std::initializer_list<const wchar_t*> class_names,
        std::initializer_list<const wchar_t*> object_paths,
        std::vector<std::string>* route_counts = nullptr) -> std::vector<RC::Unreal::UObject*>
    {
        std::vector<RC::Unreal::UObject*> result;
        std::unordered_set<RC::Unreal::UObject*> seen;
        for (const auto* class_name : class_names)
        {
            for (const auto* object_path : object_paths)
            {
                auto* object = RC::Unreal::UObjectGlobals::FindObject(class_name, object_path, 0, object_flag_class_default_object);
                if (route_counts != nullptr)
                {
                    std::ostringstream route;
                    route << "FindObject(" << narrow(class_name) << "," << narrow(object_path) << ")=" << (object != nullptr ? 1 : 0);
                    route_counts->push_back(route.str());
                }
                append_unique_object(result, seen, object);
            }
        }
        return result;
    }

    auto outpost_streaming_name_matches(const std::string& value) -> bool
    {
        return value.find("/a_0_outpost") != std::string::npos ||
               value.find("a_0_outpost") != std::string::npos ||
               value.find("/b_4_outpost") != std::string::npos ||
               value.find("b_4_outpost") != std::string::npos ||
               value.find("/c_2_outpost") != std::string::npos ||
               value.find("c_2_outpost") != std::string::npos ||
               value.find("/z_3_outpost") != std::string::npos ||
               value.find("z_3_outpost") != std::string::npos;
    }

    auto current_streaming_world(std::vector<std::string>* route_counts = nullptr) -> RC::Unreal::UWorld*
    {
        if (auto* economy_manager = RC::Unreal::UObjectGlobals::FindFirstOf(L"BP_EconomyManager_C"))
        {
            if (route_counts != nullptr) route_counts->push_back("streaming-world economy=BP_EconomyManager_C:" + full_name(economy_manager));
            if (auto* world = economy_manager->GetWorld()) return world;
        }
        if (auto* economy_manager = RC::Unreal::UObjectGlobals::FindFirstOf(L"ConZEconomyManager"))
        {
            if (route_counts != nullptr) route_counts->push_back("streaming-world economy=ConZEconomyManager:" + full_name(economy_manager));
            if (auto* world = economy_manager->GetWorld()) return world;
        }
        if (auto* object = RC::Unreal::UObjectGlobals::FindFirstOf(L"World"))
        {
            const auto name = lower_copy(full_name(object));
            if (route_counts != nullptr) route_counts->push_back("streaming-world FindFirstOf(World)=" + full_name(object));
            if (name.rfind("world ", 0) == 0)
            {
                return static_cast<RC::Unreal::UWorld*>(object);
            }
        }
        if (route_counts != nullptr) route_counts->push_back("streaming-world unresolved");
        return nullptr;
    }

    auto resolve_gameplay_statics_object(std::vector<std::string>* route_counts = nullptr) -> RC::Unreal::UObject*
    {
        for (const auto* class_name : {L"GameplayStatics", L"BlueprintFunctionLibrary", L"Object", L"UObject"})
        {
            for (const auto* object_path : {
                     L"/Script/Engine.Default__GameplayStatics",
                     L"Default__GameplayStatics",
                 })
            {
                auto* object = RC::Unreal::UObjectGlobals::FindObject(class_name, object_path, 0, 0);
                if (route_counts != nullptr)
                {
                    std::ostringstream route;
                    route << "GameplayStatics.FindObject(" << narrow(class_name) << "," << narrow(object_path) << ")="
                          << (object != nullptr ? full_name(object) : "0");
                    route_counts->push_back(route.str());
                }
                if (object != nullptr) return object;
            }
        }

        if (auto* object = RC::Unreal::UObjectGlobals::FindFirstOf(L"GameplayStatics"))
        {
            if (route_counts != nullptr) route_counts->push_back("GameplayStatics.FindFirstOf(GameplayStatics)=" + full_name(object));
            return object;
        }
        if (route_counts != nullptr) route_counts->push_back("GameplayStatics unresolved");
        return nullptr;
    }

    auto resolve_kismet_system_library_object(std::vector<std::string>* route_counts = nullptr) -> RC::Unreal::UObject*
    {
        for (const auto* class_name : {L"KismetSystemLibrary", L"BlueprintFunctionLibrary", L"Object", L"UObject"})
        {
            for (const auto* object_path : {
                     L"/Script/Engine.Default__KismetSystemLibrary",
                     L"Default__KismetSystemLibrary",
                 })
            {
                auto* object = RC::Unreal::UObjectGlobals::FindObject(class_name, object_path, 0, 0);
                if (route_counts != nullptr)
                {
                    std::ostringstream route;
                    route << "KismetSystemLibrary.FindObject(" << narrow(class_name) << "," << narrow(object_path) << ")="
                          << (object != nullptr ? full_name(object) : "0");
                    route_counts->push_back(route.str());
                }
                if (object != nullptr) return object;
            }
        }

        if (auto* object = RC::Unreal::UObjectGlobals::FindFirstOf(L"KismetSystemLibrary"))
        {
            if (route_counts != nullptr) route_counts->push_back("KismetSystemLibrary.FindFirstOf(KismetSystemLibrary)=" + full_name(object));
            return object;
        }
        if (route_counts != nullptr) route_counts->push_back("KismetSystemLibrary unresolved");
        return nullptr;
    }

    auto collect_actors_of_class_via_gameplay_statics(
        RC::Unreal::UClass* actor_class,
        const std::string& label,
        std::vector<std::string>* route_counts = nullptr) -> std::vector<RC::Unreal::UObject*>
    {
        std::vector<RC::Unreal::UObject*> result;
        std::unordered_set<RC::Unreal::UObject*> seen;
        std::vector<std::string> details;

        auto* world = current_streaming_world(route_counts);
        auto* gameplay_statics = resolve_gameplay_statics_object(route_counts);
        GetAllActorsOfClassParams params{};
        params.world_context_object = world;
        params.actor_class = actor_class;

        const auto invoked = call_reflected_function(
            gameplay_statics,
            "GameplayStatics",
            "GetAllActorsOfClass",
            {
                "/Script/Engine.GameplayStatics:GetAllActorsOfClass",
                "Function /Script/Engine.GameplayStatics:GetAllActorsOfClass",
                "/Script/Engine.GameplayStatics.GetAllActorsOfClass",
                "Function /Script/Engine.GameplayStatics.GetAllActorsOfClass",
            },
            &params,
            label + ".GetAllActorsOfClass",
            details);

        if (invoked && is_reasonable_array(&params.out_actors) && params.out_actors.data != nullptr)
        {
            auto** items = static_cast<RC::Unreal::UObject**>(params.out_actors.data);
            const auto count = std::min<std::int32_t>(params.out_actors.count, 4096);
            for (std::int32_t index = 0; index < count; ++index)
            {
                append_unique_object(result, seen, items[index]);
            }
        }

        if (route_counts != nullptr)
        {
            std::ostringstream route;
            route << label << ".GameplayStatics invoked=" << (invoked ? "true" : "false")
                  << " world=" << full_name(world)
                  << " class=" << full_name(actor_class)
                  << " rawCount=" << params.out_actors.count
                  << " unique=" << result.size();
            route_counts->push_back(route.str());
            for (const auto& detail : details)
            {
                route_counts->push_back(label + "." + detail);
            }
            for (std::size_t index = 0; index < std::min<std::size_t>(result.size(), 8); ++index)
            {
                route_counts->push_back(label + ".sample=" + full_name(result[index]));
            }
        }

        return result;
    }

    auto collect_world_extra_referenced_objects(std::vector<std::string>* route_counts = nullptr) -> std::vector<RC::Unreal::UObject*>
    {
        std::vector<RC::Unreal::UObject*> result;
        std::unordered_set<RC::Unreal::UObject*> seen;
        std::vector<std::string> samples;

        auto* world = current_streaming_world(route_counts);
        const auto* extra_refs = array_property(world, L"ExtraReferencedObjects");
        if (!is_reasonable_array(extra_refs) || extra_refs->data == nullptr)
        {
            if (route_counts != nullptr) route_counts->push_back("ExtraReferencedObjects missing or empty");
            return result;
        }

        auto** items = static_cast<RC::Unreal::UObject**>(extra_refs->data);
        const auto count = std::min<std::int32_t>(extra_refs->count, 4096);
        for (std::int32_t index = 0; index < count; ++index)
        {
            auto* object = items[index];
            if (append_unique_object(result, seen, object) && samples.size() < 8)
            {
                samples.push_back(full_name(object));
            }
        }

        if (route_counts != nullptr)
        {
            std::ostringstream route;
            route << "ExtraReferencedObjects count=" << extra_refs->count
                  << " unique=" << result.size();
            route_counts->push_back(route.str());
            for (const auto& sample : samples)
            {
                route_counts->push_back("ExtraReferencedObjects.sample=" + sample);
            }
        }

        return result;
    }

    auto collect_loaded_outpost_level_actors(std::vector<std::string>* route_counts = nullptr) -> std::vector<RC::Unreal::UObject*>
    {
        std::vector<RC::Unreal::UObject*> result;
        std::unordered_set<RC::Unreal::UObject*> seen;
        std::vector<std::string> notes;

        auto* world = current_streaming_world(route_counts);
        const auto* streaming_levels = array_property(world, L"StreamingLevels");
        if (!is_reasonable_array(streaming_levels) || streaming_levels->data == nullptr)
        {
            if (route_counts != nullptr) route_counts->push_back("LevelActors skipped: world StreamingLevels missing or empty");
            return result;
        }

        auto** levels = static_cast<RC::Unreal::UObject**>(streaming_levels->data);
        const auto count = std::min<std::int32_t>(streaming_levels->count, 4096);
        int matched_levels = 0;
        int actor_slots = 0;
        int selected = 0;

        auto scan_actor_array = [&](const TArrayAbi* actors, const std::string& source_name) -> void
        {
            if (!is_reasonable_array(actors) || actors->data == nullptr)
            {
                if (notes.size() < 8) notes.push_back("Actors missing: " + source_name);
                return;
            }
            const auto actor_count = std::min<std::int32_t>(actors->count, 4096);
            actor_slots += actor_count;
            for (std::int32_t actor_index = 0; actor_index < actor_count; ++actor_index)
            {
                auto* actor = read_uobject_array_item_noexcept(actors, actor_index);
                if (actor == nullptr) continue;
                const auto actor_name = lower_copy(full_name(actor));
                if (!outpost_streaming_name_matches(actor_name)) continue;
                if (append_unique_object(result, seen, actor))
                {
                    ++selected;
                    if (notes.size() < 8)
                    {
                        notes.push_back(source_name + ".Actor " + std::to_string(actor_index) + ": " + full_name(actor));
                    }
                }
            }
        };

        auto scan_level = [&](RC::Unreal::UObject* level, const std::string& level_name) -> void
        {
            if (level == nullptr) return;
            // SCUM 1.3 dump stores ULevel actors under ULevel.ActorCluster.Actors, not ULevel.Actors.
            TArrayAbi level_actors{};
            std::string actor_source;
            if (read_level_actor_array(level, level_name, level_actors, actor_source, notes))
            {
                scan_actor_array(&level_actors, actor_source);
            }
        };

        const auto* world_levels = array_property(world, L"Levels");
        if (is_reasonable_array(world_levels) && world_levels->data != nullptr)
        {
            auto** loaded_levels = static_cast<RC::Unreal::UObject**>(world_levels->data);
            const auto world_level_count = std::min<std::int32_t>(world_levels->count, 4096);
            for (std::int32_t index = 0; index < world_level_count; ++index)
            {
                auto* level = loaded_levels[index];
                const auto level_name = lower_copy(full_name(level));
                if (!outpost_streaming_name_matches(level_name)) continue;
                ++matched_levels;
                scan_level(level, full_name(level));
            }
        }

        for (std::int32_t index = 0; index < count; ++index)
        {
            auto* level_streaming = levels[index];
            if (level_streaming == nullptr) continue;
            auto* loaded_level = object_property(level_streaming, L"LoadedLevel");
            auto* pending_unload_level = object_property(level_streaming, L"PendingUnloadLevel");
            const auto combined = lower_copy(full_name(level_streaming) + " " + full_name(loaded_level) + " " + full_name(pending_unload_level));
            if (!outpost_streaming_name_matches(combined)) continue;

            ++matched_levels;
            scan_level(loaded_level, full_name(loaded_level));
            scan_level(pending_unload_level, full_name(pending_unload_level));
        }

        if (route_counts != nullptr)
        {
            std::ostringstream route;
            route << "LevelActors streamingLevels=" << streaming_levels->count
                  << " matchedOutpostLevels=" << matched_levels
                  << " actorSlots=" << actor_slots
                  << " selectedOutpostActors=" << selected;
            route_counts->push_back(route.str());
            for (const auto& note : notes)
            {
                route_counts->push_back("LevelActors." + note);
            }
        }

        return result;
    }

    struct EditorActorCandidate
    {
        RC::Unreal::AActor* actor{};
        std::string actor_full_name{};
        std::string actor_class_full_name{};
        RC::Unreal::FVector location{};
        RC::Unreal::FRotator rotation{};
        bool rotation_ok{};
        double distance_2d{};
        double distance_3d{};
        double forward_dot{};
        double score{};
    };

    auto append_editor_actor_candidate_json(std::ostringstream& ss, const EditorActorCandidate& candidate) -> void
    {
        ss << "{\"actorFullName\":\"" << json_escape(candidate.actor_full_name) << "\""
           << ",\"actorClass\":\"" << json_escape(candidate.actor_class_full_name) << "\""
           << ",\"location\":";
        append_vector_json(ss, candidate.location);
        ss << ",\"rotation\":";
        append_rotator_json(ss, candidate.rotation);
        ss << ",\"rotationOk\":" << (candidate.rotation_ok ? "true" : "false")
           << ",\"distance2dCm\":" << candidate.distance_2d
           << ",\"distance3dCm\":" << candidate.distance_3d
           << ",\"forwardDot\":" << candidate.forward_dot
           << ",\"score\":" << candidate.score
           << "}";
    }

    auto collect_editor_actor_candidates_near(
        const RC::Unreal::FVector& player_location,
        double forward_x,
        double forward_y,
        double radius_cm,
        double min_distance_cm,
        double min_forward_dot,
        bool allow_behind,
        int max_levels,
        int max_actor_slots,
        std::vector<std::string>* route_counts = nullptr) -> std::vector<EditorActorCandidate>
    {
        std::vector<EditorActorCandidate> candidates;
        if (route_counts != nullptr)
        {
            route_counts->push_back("EditorLevelActors disabled: exact client-captured actor handle required after live crashes");
        }
        (void)player_location;
        (void)forward_x;
        (void)forward_y;
        (void)radius_cm;
        (void)min_distance_cm;
        (void)min_forward_dot;
        (void)allow_behind;
        (void)max_levels;
        (void)max_actor_slots;
        return candidates;

        std::unordered_set<RC::Unreal::UObject*> seen_actors;
        std::unordered_set<RC::Unreal::UObject*> seen_levels;
        std::vector<std::string> notes;

        const auto forward_len = std::sqrt(forward_x * forward_x + forward_y * forward_y);
        if (forward_len > 0.01)
        {
            forward_x /= forward_len;
            forward_y /= forward_len;
        }
        else
        {
            forward_x = 1.0;
            forward_y = 0.0;
        }

        radius_cm = std::max(100.0, std::min(radius_cm, 6000.0));
        min_distance_cm = std::max(0.0, std::min(min_distance_cm, radius_cm));
        min_forward_dot = std::max(-1.0, std::min(min_forward_dot, 1.0));
        max_levels = std::max(1, std::min(max_levels, 512));
        max_actor_slots = std::max(1, std::min(max_actor_slots, 75000));

        int scanned_levels = 0;
        int actor_slots = 0;
        int location_reads = 0;
        int location_failures = 0;
        int radius_hits = 0;
        int blocked_hits = 0;
        bool limit_hit = false;

        auto* world = current_streaming_world(route_counts);
        if (world == nullptr)
        {
            if (route_counts != nullptr) route_counts->push_back("EditorLevelActors skipped: world unresolved");
            return candidates;
        }

        auto scan_actor_array = [&](const TArrayAbi* actors, const std::string& source_name) -> void
        {
            if (limit_hit) return;
            if (!is_reasonable_array(actors) || actors->data == nullptr)
            {
                if (notes.size() < 8) notes.push_back("Actors missing: " + source_name);
                return;
            }

            const auto actor_count = actors->count;
            for (std::int32_t actor_index = 0; actor_index < actor_count; ++actor_index)
            {
                if (actor_slots >= max_actor_slots)
                {
                    limit_hit = true;
                    return;
                }
                ++actor_slots;

                auto* actor_object = read_uobject_array_item_noexcept(actors, actor_index);
                if (actor_object == nullptr) continue;
                if (!seen_actors.insert(actor_object).second) continue;

                RC::Unreal::FVector location{};
                if (!actor_location_noexcept(actor_object, location))
                {
                    ++location_failures;
                    continue;
                }
                ++location_reads;

                const double dx = static_cast<double>(location.x) - static_cast<double>(player_location.x);
                const double dy = static_cast<double>(location.y) - static_cast<double>(player_location.y);
                const double dz = static_cast<double>(location.z) - static_cast<double>(player_location.z);
                const auto distance_2d = std::sqrt(dx * dx + dy * dy);
                const auto distance_3d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (distance_3d > radius_cm || distance_2d < min_distance_cm) continue;
                ++radius_hits;

                double dot = 0.0;
                if (distance_2d > 1.0)
                {
                    dot = (dx / distance_2d) * forward_x + (dy / distance_2d) * forward_y;
                }
                if (!allow_behind && dot < min_forward_dot) continue;

                const auto actor_full_name = full_name(actor_object);
                const auto actor_class_full_name = object_class_full_name_noexcept(actor_object);
                const auto block_reason = editor_actor_selection_block_reason(actor_full_name, actor_class_full_name);
                if (!block_reason.empty())
                {
                    ++blocked_hits;
                    if (notes.size() < 8)
                    {
                        notes.push_back("blocked " + block_reason + ": " + actor_full_name);
                    }
                    continue;
                }

                RC::Unreal::FRotator rotation{};
                const auto rotation_ok = actor_rotation_noexcept(actor_object, rotation);
                const auto straightness_penalty = (1.0 - std::max(-1.0, std::min(dot, 1.0))) * 650.0;
                const auto height_penalty = std::min(std::abs(dz), 1200.0) * 0.08;
                const auto distance_penalty = distance_2d * 0.35;
                const auto score = straightness_penalty + height_penalty + distance_penalty;

                EditorActorCandidate candidate{};
                candidate.actor = static_cast<RC::Unreal::AActor*>(actor_object);
                candidate.actor_full_name = actor_full_name;
                candidate.actor_class_full_name = actor_class_full_name;
                candidate.location = location;
                candidate.rotation = rotation;
                candidate.rotation_ok = rotation_ok;
                candidate.distance_2d = distance_2d;
                candidate.distance_3d = distance_3d;
                candidate.forward_dot = dot;
                candidate.score = score;
                candidates.push_back(std::move(candidate));
            }
        };

        auto scan_level = [&](RC::Unreal::UObject* level, const std::string& level_name) -> void
        {
            if (level == nullptr || limit_hit) return;
            if (!seen_levels.insert(level).second) return;
            if (scanned_levels >= max_levels)
            {
                limit_hit = true;
                return;
            }
            ++scanned_levels;

            TArrayAbi level_actors{};
            std::string actor_source;
            if (read_level_actor_array(level, level_name, level_actors, actor_source, notes))
            {
                scan_actor_array(&level_actors, actor_source);
            }
        };

        const auto* world_levels = array_property(world, L"Levels");
        if (is_reasonable_array(world_levels) && world_levels->data != nullptr)
        {
            auto** loaded_levels = static_cast<RC::Unreal::UObject**>(world_levels->data);
            const auto world_level_count = std::min<std::int32_t>(world_levels->count, static_cast<std::int32_t>(max_levels));
            for (std::int32_t index = 0; index < world_level_count && !limit_hit; ++index)
            {
                scan_level(loaded_levels[index], full_name(loaded_levels[index]));
            }
        }

        const auto* streaming_levels = array_property(world, L"StreamingLevels");
        if (is_reasonable_array(streaming_levels) && streaming_levels->data != nullptr)
        {
            auto** levels = static_cast<RC::Unreal::UObject**>(streaming_levels->data);
            const auto streaming_count = std::min<std::int32_t>(streaming_levels->count, 4096);
            for (std::int32_t index = 0; index < streaming_count && !limit_hit; ++index)
            {
                auto* level_streaming = levels[index];
                if (level_streaming == nullptr) continue;
                scan_level(object_property(level_streaming, L"LoadedLevel"), full_name(level_streaming) + ".LoadedLevel");
                scan_level(object_property(level_streaming, L"PendingUnloadLevel"), full_name(level_streaming) + ".PendingUnloadLevel");
            }
        }

        std::sort(candidates.begin(), candidates.end(), [](const EditorActorCandidate& left, const EditorActorCandidate& right)
        {
            return left.score < right.score;
        });
        if (candidates.size() > 64)
        {
            candidates.resize(64);
        }

        if (route_counts != nullptr)
        {
            std::ostringstream route;
            route << "EditorLevelActors levels=" << scanned_levels
                  << " actorSlots=" << actor_slots
                  << " locationReads=" << location_reads
                  << " locationFailures=" << location_failures
                  << " radiusHits=" << radius_hits
                  << " blockedHits=" << blocked_hits
                  << " candidates=" << candidates.size()
                  << " radiusCm=" << radius_cm
                  << " minForwardDot=" << min_forward_dot
                  << " allowBehind=" << (allow_behind ? "true" : "false")
                  << " limitHit=" << (limit_hit ? "true" : "false");
            route_counts->push_back(route.str());
            for (const auto& note : notes)
            {
                route_counts->push_back("EditorLevelActors." + note);
            }
        }

        return candidates;
    }

    auto read_editor_player_context_from_command(
        const std::string& command_text,
        RC::Unreal::FVector& player_location,
        double& forward_x,
        double& forward_y,
        double& yaw,
        std::vector<std::string>& attempts) -> bool
    {
        double x{};
        double y{};
        double z{};
        const auto has_x = first_regex_number(command_text, {"x", "X", "playerX", "PlayerX", "worldX", "WorldX"}, x);
        const auto has_y = first_regex_number(command_text, {"y", "Y", "playerY", "PlayerY", "worldY", "WorldY"}, y);
        const auto has_z = first_regex_number(command_text, {"z", "Z", "playerZ", "PlayerZ", "worldZ", "WorldZ"}, z);
        if (!has_x || !has_y || !has_z || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        {
            attempts.push_back("editorPlayerContext missing x/y/z");
            return false;
        }

        player_location.x = static_cast<float>(x);
        player_location.y = static_cast<float>(y);
        player_location.z = static_cast<float>(z);

        const auto has_forward_x = first_regex_number(command_text, {"forwardX", "ForwardX", "fx", "dirX"}, forward_x);
        const auto has_forward_y = first_regex_number(command_text, {"forwardY", "ForwardY", "fy", "dirY"}, forward_y);
        if (!first_regex_number(command_text, {"yaw", "Yaw", "playerYaw", "PlayerYaw"}, yaw))
        {
            yaw = 0.0;
        }

        if (!has_forward_x || !has_forward_y || !std::isfinite(forward_x) || !std::isfinite(forward_y))
        {
            const auto radians = yaw * 3.14159265358979323846 / 180.0;
            forward_x = std::cos(radians);
            forward_y = std::sin(radians);
            attempts.push_back("editorPlayerContext forward fallback from yaw");
        }

        const auto forward_len = std::sqrt(forward_x * forward_x + forward_y * forward_y);
        if (forward_len <= 0.01)
        {
            forward_x = 1.0;
            forward_y = 0.0;
            attempts.push_back("editorPlayerContext forward fallback default");
        }
        else
        {
            forward_x /= forward_len;
            forward_y /= forward_len;
        }

        return true;
    }

    auto read_editor_vector_from_command(
        const std::string& command_text,
        std::initializer_list<const char*> x_keys,
        std::initializer_list<const char*> y_keys,
        std::initializer_list<const char*> z_keys,
        RC::Unreal::FVector& value) -> bool
    {
        double x{};
        double y{};
        double z{};
        const auto has_x = first_regex_number(command_text, x_keys, x);
        const auto has_y = first_regex_number(command_text, y_keys, y);
        const auto has_z = first_regex_number(command_text, z_keys, z);
        if (!has_x || !has_y || !has_z || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        {
            return false;
        }
        value = make_vector(x, y, z);
        return true;
    }

    auto outer_path_from_full_name(const std::string& full_name_value) -> std::string
    {
        const auto path = object_path_from_full_name(full_name_value);
        const auto dot = path.find_last_of('.');
        if (dot == std::string::npos || dot == 0) return {};
        return path.substr(0, dot);
    }

    auto normalized_path_match(const std::string& left, const std::string& right) -> bool
    {
        if (left.empty() || right.empty()) return false;
        const auto left_lower = lower_copy(left);
        const auto right_lower = lower_copy(right);
        return left_lower == right_lower ||
               left_lower.find(right_lower) != std::string::npos ||
               right_lower.find(left_lower) != std::string::npos;
    }

    auto validated_uobject_pointer(RC::Unreal::UObject* object, std::string& detail) -> bool
    {
        detail.clear();
        if (object == nullptr)
        {
            detail = "null";
            return false;
        }
        const auto address = reinterpret_cast<std::uintptr_t>(object);
        if (!committed_readable_address(address, sizeof(std::uintptr_t) + 0x10))
        {
            detail = "unreadable:" + hex_address(address);
            return false;
        }

        std::int32_t object_index = -1;
        if (!safe_read(address + 0x0C, object_index) || object_index < 0)
        {
            detail = "internal-index-unreadable:" + hex_address(address);
            return false;
        }

        FUObjectItemAbi item{};
        std::uintptr_t item_address{};
        std::string item_detail;
        if (!read_uobject_item_by_index(object_index, item, item_detail, &item_address, address) || item.object != address)
        {
            detail = "object-item-mismatch:" + item_detail;
            return false;
        }

        detail = "index=" + std::to_string(object_index) + ",item=" + hex_address(item_address);
        return true;
    }

    struct EditorAimContext
    {
        RC::Unreal::FVector player_location{};
        RC::Unreal::FVector trace_start{};
        RC::Unreal::FVector trace_end{};
        double forward_x{1.0};
        double forward_y{};
        double forward_z{};
        double yaw{};
        double pitch{};
        double eye_offset_z{120.0};
        double max_distance_cm{5000.0};
        double sphere_radius_cm{45.0};
        std::uint8_t first_trace_channel{};
        std::uint8_t last_trace_channel{4};
        RC::Unreal::AActor* pawn_actor{};
        RC::Unreal::AActor* controller_actor{};
        std::string pawn_full_name{};
        std::string controller_full_name{};
    };

    struct EditorTraceSelection
    {
        bool attempted{};
        bool invoked{};
        bool hit{};
        bool safe{};
        bool used_sphere{};
        std::uint8_t trace_channel{};
        std::string method{};
        RC::Unreal::AActor* actor{};
        std::string actor_full_name{};
        std::string actor_class_full_name{};
        std::string block_reason{};
        FHitResultTraceAbi hit_result{};
        RC::Unreal::FVector location{};
        RC::Unreal::FRotator rotation{};
        bool location_ok{};
        bool rotation_ok{};
        double along_distance_cm{};
        double ray_distance_cm{};
        double selection_score{};
    };

    auto weak_get_noexcept(const RC::Unreal::FWeakObjectPtr& weak) -> RC::Unreal::UObject*
    {
        __try
        {
            return weak.Get();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    auto actor_from_weak_noexcept(const RC::Unreal::FWeakObjectPtr& weak) -> RC::Unreal::AActor*
    {
        auto* object = weak_get_noexcept(weak);
        if (object == nullptr) return nullptr;
        RC::Unreal::FVector location{};
        if (!actor_location_noexcept(object, location)) return nullptr;
        return static_cast<RC::Unreal::AActor*>(object);
    }

    auto hit_result_actor_noexcept(const FHitResultTraceAbi& hit_result) -> RC::Unreal::AActor*
    {
        if (auto* actor = actor_from_weak_noexcept(hit_result.actor))
        {
            return actor;
        }
        auto* component = weak_get_noexcept(hit_result.component);
        auto* owner = object_property(component, L"Owner");
        RC::Unreal::FVector location{};
        if (owner != nullptr && actor_location_noexcept(owner, location))
        {
            return static_cast<RC::Unreal::AActor*>(owner);
        }
        return nullptr;
    }

    auto find_optional_editor_actor_by_full_name(
        const std::string& requested_full_name,
        std::vector<std::string>& attempts,
        const char* label) -> RC::Unreal::AActor*
    {
        if (requested_full_name.empty()) return nullptr;
        auto* object = find_object_by_full_name(
            {L"Actor", L"AActor", L"Pawn", L"Controller", L"Object"},
            requested_full_name,
            attempts,
            label);
        RC::Unreal::FVector location{};
        if (object != nullptr && actor_location_noexcept(object, location))
        {
            return static_cast<RC::Unreal::AActor*>(object);
        }
        return nullptr;
    }

    auto read_editor_aim_context_from_command(
        const std::string& command_text,
        EditorAimContext& context,
        std::vector<std::string>& attempts) -> bool
    {
        if (!read_editor_player_context_from_command(
                command_text,
                context.player_location,
                context.forward_x,
                context.forward_y,
                context.yaw,
                attempts))
        {
            return false;
        }

        first_regex_number(command_text, {"pitch", "Pitch", "playerPitch", "PlayerPitch"}, context.pitch);
        first_regex_number(command_text, {"forwardZ", "ForwardZ", "fz", "dirZ"}, context.forward_z);
        first_regex_number(command_text, {"eyeOffsetZ", "traceEyeOffsetZ", "startOffsetZ"}, context.eye_offset_z);
        first_regex_number(command_text, {"traceDistanceCm", "maxTraceDistanceCm", "maxDistanceCm"}, context.max_distance_cm);
        first_regex_number(command_text, {"traceRadiusCm", "sphereRadiusCm", "radiusCm"}, context.sphere_radius_cm);

        double trace_channel_value{};
        if (first_regex_number(command_text, {"traceChannel", "TraceChannel"}, trace_channel_value))
        {
            const auto channel = static_cast<int>(std::max(0.0, std::min(31.0, trace_channel_value)));
            context.first_trace_channel = static_cast<std::uint8_t>(channel);
            context.last_trace_channel = context.first_trace_channel;
        }
        double last_trace_channel_value{};
        if (first_regex_number(command_text, {"lastTraceChannel", "maxTraceChannel"}, last_trace_channel_value))
        {
            context.last_trace_channel = static_cast<std::uint8_t>(std::max<int>(
                context.first_trace_channel,
                std::min(31, static_cast<int>(last_trace_channel_value))));
        }

        context.max_distance_cm = std::max(100.0, std::min(context.max_distance_cm, 15000.0));
        context.sphere_radius_cm = std::max(0.0, std::min(context.sphere_radius_cm, 300.0));
        context.eye_offset_z = std::max(-80.0, std::min(context.eye_offset_z, 220.0));

        const auto has_pitch = std::isfinite(context.pitch) && std::abs(context.pitch) > 0.01;
        const auto has_forward_z = std::isfinite(context.forward_z) && std::abs(context.forward_z) > 0.001;
        if (has_pitch && !has_forward_z)
        {
            const auto yaw_rad = context.yaw * 3.14159265358979323846 / 180.0;
            const auto pitch_rad = context.pitch * 3.14159265358979323846 / 180.0;
            const auto cos_pitch = std::cos(pitch_rad);
            context.forward_x = cos_pitch * std::cos(yaw_rad);
            context.forward_y = cos_pitch * std::sin(yaw_rad);
            context.forward_z = std::sin(pitch_rad);
            attempts.push_back("editorAimContext forward vector rebuilt from yaw/pitch");
        }

        const auto forward_len = std::sqrt(
            context.forward_x * context.forward_x +
            context.forward_y * context.forward_y +
            context.forward_z * context.forward_z);
        if (forward_len <= 0.01)
        {
            const auto yaw_rad = context.yaw * 3.14159265358979323846 / 180.0;
            context.forward_x = std::cos(yaw_rad);
            context.forward_y = std::sin(yaw_rad);
            context.forward_z = 0.0;
            attempts.push_back("editorAimContext forward vector fallback from yaw");
        }
        else
        {
            context.forward_x /= forward_len;
            context.forward_y /= forward_len;
            context.forward_z /= forward_len;
        }

        context.trace_start = context.player_location;
        context.trace_start.z += static_cast<float>(context.eye_offset_z);
        context.trace_end = context.trace_start;
        context.trace_end.x += static_cast<float>(context.forward_x * context.max_distance_cm);
        context.trace_end.y += static_cast<float>(context.forward_y * context.max_distance_cm);
        context.trace_end.z += static_cast<float>(context.forward_z * context.max_distance_cm);

        context.pawn_full_name = first_regex_value(command_text, {"pawnFullName", "playerPawnFullName", "pawn"});
        context.controller_full_name = first_regex_value(command_text, {"controllerFullName", "playerControllerFullName", "controller"});
        context.pawn_actor = find_optional_editor_actor_by_full_name(context.pawn_full_name, attempts, "editorAimPawn");
        context.controller_actor = find_optional_editor_actor_by_full_name(context.controller_full_name, attempts, "editorAimController");
        return true;
    }

    auto append_editor_aim_context_json(std::ostringstream& ss, const EditorAimContext& context) -> void
    {
        ss << "{\"playerLocation\":";
        append_vector_json(ss, context.player_location);
        ss << ",\"traceStart\":";
        append_vector_json(ss, context.trace_start);
        ss << ",\"traceEnd\":";
        append_vector_json(ss, context.trace_end);
        ss << ",\"direction\":{\"x\":" << context.forward_x
           << ",\"y\":" << context.forward_y
           << ",\"z\":" << context.forward_z
           << ",\"yaw\":" << context.yaw
           << ",\"pitch\":" << context.pitch << "}"
           << ",\"eyeOffsetZ\":" << context.eye_offset_z
           << ",\"maxDistanceCm\":" << context.max_distance_cm
           << ",\"sphereRadiusCm\":" << context.sphere_radius_cm
           << ",\"traceChannels\":{\"first\":" << static_cast<int>(context.first_trace_channel)
           << ",\"last\":" << static_cast<int>(context.last_trace_channel) << "}"
           << ",\"pawn\":\"" << json_escape(full_name(static_cast<RC::Unreal::UObject*>(context.pawn_actor))) << "\""
           << ",\"controller\":\"" << json_escape(full_name(static_cast<RC::Unreal::UObject*>(context.controller_actor))) << "\"}";
    }

    auto append_editor_trace_selection_json(std::ostringstream& ss, const EditorTraceSelection& selection) -> void
    {
        const auto method = selection.method.empty()
            ? std::string(selection.used_sphere ? "sphere" : "line")
            : selection.method;
        ss << "{\"attempted\":" << (selection.attempted ? "true" : "false")
           << ",\"invoked\":" << (selection.invoked ? "true" : "false")
           << ",\"hit\":" << (selection.hit ? "true" : "false")
           << ",\"safe\":" << (selection.safe ? "true" : "false")
           << ",\"method\":\"" << json_escape(method) << "\""
           << ",\"traceChannel\":" << static_cast<int>(selection.trace_channel)
           << ",\"actorFullName\":\"" << json_escape(selection.actor_full_name) << "\""
           << ",\"actorClass\":\"" << json_escape(selection.actor_class_full_name) << "\""
           << ",\"blockReason\":\"" << json_escape(selection.block_reason) << "\""
           << ",\"locationOk\":" << (selection.location_ok ? "true" : "false")
           << ",\"rotationOk\":" << (selection.rotation_ok ? "true" : "false")
           << ",\"actorLocation\":";
        append_vector_json(ss, selection.location);
        ss << ",\"actorRotation\":";
        append_rotator_json(ss, selection.rotation);
        ss << ",\"hitDistanceCm\":" << selection.hit_result.distance
           << ",\"hitTime\":" << selection.hit_result.time
           << ",\"alongDistanceCm\":" << selection.along_distance_cm
           << ",\"rayDistanceCm\":" << selection.ray_distance_cm
           << ",\"selectionScore\":" << selection.selection_score
           << ",\"hitLocation\":";
        append_vector_json(ss, selection.hit_result.location);
        ss << ",\"impactPoint\":";
        append_vector_json(ss, selection.hit_result.impact_point);
        ss << "}";
    }

    auto invoke_editor_trace(
        RC::Unreal::UObject* kismet,
        const EditorAimContext& context,
        bool use_sphere,
        std::uint8_t trace_channel,
        std::vector<RC::Unreal::AActor*>& ignored,
        std::vector<std::string>& attempts,
        EditorTraceSelection& selection) -> bool
    {
        selection = {};
        selection.attempted = true;
        selection.used_sphere = use_sphere;
        selection.method = use_sphere ? "sphere" : "line";
        selection.trace_channel = trace_channel;

        TArrayAbi actors_to_ignore{};
        if (!ignored.empty())
        {
            actors_to_ignore.data = ignored.data();
            actors_to_ignore.count = static_cast<std::int32_t>(ignored.size());
            actors_to_ignore.max = actors_to_ignore.count;
        }

        std::vector<std::string> details;
        if (use_sphere)
        {
            SphereTraceSingleParams params{};
            params.world_context_object = context.pawn_actor != nullptr
                ? static_cast<RC::Unreal::UObject*>(context.pawn_actor)
                : current_streaming_world(&attempts);
            params.start = context.trace_start;
            params.end = context.trace_end;
            params.radius = static_cast<float>(context.sphere_radius_cm);
            params.trace_channel = trace_channel;
            params.trace_complex = false;
            params.actors_to_ignore = actors_to_ignore;
            params.draw_debug_type = 0;
            params.ignore_self = true;

            selection.invoked = call_reflected_function(
                kismet,
                "KismetSystemLibrary",
                "SphereTraceSingle",
                {
                    "/Script/Engine.KismetSystemLibrary:SphereTraceSingle",
                    "Function /Script/Engine.KismetSystemLibrary:SphereTraceSingle",
                    "/Script/Engine.KismetSystemLibrary.SphereTraceSingle",
                    "Function /Script/Engine.KismetSystemLibrary.SphereTraceSingle",
                },
                &params,
                "editor.Trace.SphereTraceSingle",
                details);
            selection.hit = selection.invoked && params.return_value;
            selection.hit_result = params.out_hit;
        }
        else
        {
            LineTraceSingleParams params{};
            params.world_context_object = context.pawn_actor != nullptr
                ? static_cast<RC::Unreal::UObject*>(context.pawn_actor)
                : current_streaming_world(&attempts);
            params.start = context.trace_start;
            params.end = context.trace_end;
            params.trace_channel = trace_channel;
            params.trace_complex = false;
            params.actors_to_ignore = actors_to_ignore;
            params.draw_debug_type = 0;
            params.ignore_self = true;

            selection.invoked = call_reflected_function(
                kismet,
                "KismetSystemLibrary",
                "LineTraceSingle",
                {
                    "/Script/Engine.KismetSystemLibrary:LineTraceSingle",
                    "Function /Script/Engine.KismetSystemLibrary:LineTraceSingle",
                    "/Script/Engine.KismetSystemLibrary.LineTraceSingle",
                    "Function /Script/Engine.KismetSystemLibrary.LineTraceSingle",
                },
                &params,
                "editor.Trace.LineTraceSingle",
                details);
            selection.hit = selection.invoked && params.return_value;
            selection.hit_result = params.out_hit;
        }

        for (const auto& detail : details)
        {
            attempts.push_back(detail);
        }

        if (!selection.hit)
        {
            attempts.push_back(std::string("editor.Trace no hit method=") + (use_sphere ? "sphere" : "line") + " channel=" + std::to_string(trace_channel));
            return false;
        }

        selection.actor = hit_result_actor_noexcept(selection.hit_result);
        selection.actor_full_name = full_name(static_cast<RC::Unreal::UObject*>(selection.actor));
        selection.actor_class_full_name = object_class_full_name_noexcept(static_cast<RC::Unreal::UObject*>(selection.actor));
        selection.location_ok = actor_location_noexcept(static_cast<RC::Unreal::UObject*>(selection.actor), selection.location);
        selection.rotation_ok = actor_rotation_noexcept(static_cast<RC::Unreal::UObject*>(selection.actor), selection.rotation);
        selection.along_distance_cm = selection.hit_result.distance;
        selection.ray_distance_cm = 0.0;
        selection.selection_score = selection.hit_result.distance;
        selection.block_reason = editor_actor_selection_block_reason(selection.actor_full_name, selection.actor_class_full_name);
        selection.safe = selection.actor != nullptr && selection.location_ok && selection.block_reason.empty();
        attempts.push_back(
            std::string("editor.Trace hit method=") + (use_sphere ? "sphere" : "line") +
            " channel=" + std::to_string(trace_channel) +
            " actor=" + selection.actor_full_name +
            " safe=" + (selection.safe ? "true" : "false") +
            (selection.block_reason.empty() ? "" : (" block=" + selection.block_reason)));
        return selection.safe;
    }

    auto select_editor_actor_by_server_aim_scan(
        const std::string& command_text,
        std::vector<std::string>& attempts,
        const EditorAimContext& context,
        EditorTraceSelection& selection,
        std::vector<EditorTraceSelection>* samples = nullptr) -> bool
    {
        attempts.push_back("editor.ServerAimScan disabled: exact client-captured actor handle required after live crashes");
        (void)command_text;
        (void)context;
        (void)selection;
        (void)samples;
        return false;

        std::vector<EditorTraceSelection> hits;
        std::unordered_set<RC::Unreal::UObject*> seen_actors;
        std::unordered_set<RC::Unreal::UObject*> seen_levels;
        std::vector<std::string> notes;

        double min_along_cm = 20.0;
        first_regex_number(command_text, {"minTraceDistanceCm", "minAimDistanceCm", "minAlongDistanceCm"}, min_along_cm);
        min_along_cm = std::max(0.0, std::min(min_along_cm, context.max_distance_cm));

        double aim_tolerance_cm = std::max(180.0, std::min(450.0, context.sphere_radius_cm * 4.0));
        first_regex_number(command_text, {"aimToleranceCm", "traceToleranceCm", "rayToleranceCm"}, aim_tolerance_cm);
        aim_tolerance_cm = std::max(40.0, std::min(aim_tolerance_cm, 900.0));

        double dynamic_tolerance_per_meter = 1.5;
        first_regex_number(command_text, {"aimTolerancePerMeter", "traceTolerancePerMeter"}, dynamic_tolerance_per_meter);
        dynamic_tolerance_per_meter = std::max(0.0, std::min(dynamic_tolerance_per_meter, 8.0));

        double max_levels_value = 256.0;
        first_regex_number(command_text, {"maxLevels"}, max_levels_value);
        auto max_levels = static_cast<int>(std::max(1.0, std::min(max_levels_value, 512.0)));

        double max_actor_slots_value = 30000.0;
        first_regex_number(command_text, {"maxActorSlots", "maxActors"}, max_actor_slots_value);
        auto max_actor_slots = static_cast<int>(std::max(1.0, std::min(max_actor_slots_value, 75000.0)));

        auto* world = current_streaming_world(&attempts);
        if (world == nullptr)
        {
            attempts.push_back("editor.ServerAimScan skipped: world unresolved");
            return false;
        }

        const auto ignored_pawn = static_cast<RC::Unreal::UObject*>(context.pawn_actor);
        const auto ignored_controller = static_cast<RC::Unreal::UObject*>(context.controller_actor);

        int scanned_levels = 0;
        int actor_slots = 0;
        int location_reads = 0;
        int location_failures = 0;
        int ray_hits = 0;
        int blocked_hits = 0;
        bool limit_hit = false;

        auto scan_actor_array = [&](const TArrayAbi* actors, const std::string& source_name) -> void
        {
            if (limit_hit) return;
            if (!is_reasonable_array(actors) || actors->data == nullptr)
            {
                if (notes.size() < 10) notes.push_back("Actors missing: " + source_name);
                return;
            }

            const auto actor_count = actors->count;
            for (std::int32_t actor_index = 0; actor_index < actor_count; ++actor_index)
            {
                if (actor_slots >= max_actor_slots)
                {
                    limit_hit = true;
                    return;
                }
                ++actor_slots;

                auto* actor_object = read_uobject_array_item_noexcept(actors, actor_index);
                if (actor_object == nullptr) continue;
                if (actor_object == ignored_pawn || actor_object == ignored_controller) continue;
                if (!seen_actors.insert(actor_object).second) continue;

                RC::Unreal::FVector actor_location{};
                if (!actor_location_noexcept(actor_object, actor_location))
                {
                    ++location_failures;
                    continue;
                }
                ++location_reads;

                const double vx = static_cast<double>(actor_location.x) - static_cast<double>(context.trace_start.x);
                const double vy = static_cast<double>(actor_location.y) - static_cast<double>(context.trace_start.y);
                const double vz = static_cast<double>(actor_location.z) - static_cast<double>(context.trace_start.z);
                const double along = vx * context.forward_x + vy * context.forward_y + vz * context.forward_z;
                if (!std::isfinite(along)) continue;
                if (along < min_along_cm || along > context.max_distance_cm) continue;

                const double dist_sq = vx * vx + vy * vy + vz * vz;
                if (!std::isfinite(dist_sq)) continue;
                const double ray_dist_sq = std::max(0.0, dist_sq - along * along);
                const double ray_distance = std::sqrt(ray_dist_sq);
                if (!std::isfinite(ray_distance)) continue;
                const double tolerance = std::min(900.0, aim_tolerance_cm + (along / 100.0) * dynamic_tolerance_per_meter);
                if (ray_distance > tolerance) continue;
                ++ray_hits;

                const auto actor_full_name = full_name(actor_object);
                const auto actor_class_full_name = object_class_full_name_noexcept(actor_object);
                const auto block_reason = editor_actor_selection_block_reason(actor_full_name, actor_class_full_name);
                if (!block_reason.empty())
                {
                    ++blocked_hits;
                    if (notes.size() < 10)
                    {
                        notes.push_back("blocked " + block_reason + ": " + actor_full_name);
                    }
                    continue;
                }

                RC::Unreal::FRotator actor_rotation{};
                const auto rotation_ok = actor_rotation_noexcept(actor_object, actor_rotation);
                const auto score = ray_distance * 12.0 + along * 0.035;

                EditorTraceSelection hit{};
                hit.attempted = true;
                hit.invoked = true;
                hit.hit = true;
                hit.safe = true;
                hit.used_sphere = false;
                hit.method = "server-ray-actor-scan";
                hit.actor = static_cast<RC::Unreal::AActor*>(actor_object);
                hit.actor_full_name = actor_full_name;
                hit.actor_class_full_name = actor_class_full_name;
                hit.location = actor_location;
                hit.rotation = actor_rotation;
                hit.location_ok = true;
                hit.rotation_ok = rotation_ok;
                hit.along_distance_cm = along;
                hit.ray_distance_cm = ray_distance;
                hit.selection_score = score;
                hit.hit_result.distance = static_cast<float>(along);
                hit.hit_result.time = context.max_distance_cm > 1.0
                    ? static_cast<float>(along / context.max_distance_cm)
                    : 0.0f;
                hit.hit_result.location = actor_location;
                hit.hit_result.impact_point = actor_location;
                hits.push_back(std::move(hit));
            }
        };

        auto scan_level = [&](RC::Unreal::UObject* level, const std::string& level_name) -> void
        {
            if (level == nullptr || limit_hit) return;
            if (!seen_levels.insert(level).second) return;
            if (scanned_levels >= max_levels)
            {
                limit_hit = true;
                return;
            }
            ++scanned_levels;

            TArrayAbi level_actors{};
            std::string actor_source;
            if (read_level_actor_array(level, level_name, level_actors, actor_source, notes))
            {
                scan_actor_array(&level_actors, actor_source);
            }
        };

        const auto* world_levels = array_property(world, L"Levels");
        if (is_reasonable_array(world_levels) && world_levels->data != nullptr)
        {
            auto** loaded_levels = static_cast<RC::Unreal::UObject**>(world_levels->data);
            const auto world_level_count = std::min<std::int32_t>(world_levels->count, static_cast<std::int32_t>(max_levels));
            for (std::int32_t index = 0; index < world_level_count && !limit_hit; ++index)
            {
                scan_level(loaded_levels[index], full_name(loaded_levels[index]));
            }
        }

        const auto* streaming_levels = array_property(world, L"StreamingLevels");
        if (is_reasonable_array(streaming_levels) && streaming_levels->data != nullptr)
        {
            auto** levels = static_cast<RC::Unreal::UObject**>(streaming_levels->data);
            const auto streaming_count = std::min<std::int32_t>(streaming_levels->count, 4096);
            for (std::int32_t index = 0; index < streaming_count && !limit_hit; ++index)
            {
                auto* level_streaming = levels[index];
                if (level_streaming == nullptr) continue;
                scan_level(object_property(level_streaming, L"LoadedLevel"), full_name(level_streaming) + ".LoadedLevel");
                scan_level(object_property(level_streaming, L"PendingUnloadLevel"), full_name(level_streaming) + ".PendingUnloadLevel");
            }
        }

        std::sort(hits.begin(), hits.end(), [](const EditorTraceSelection& left, const EditorTraceSelection& right)
        {
            if (std::abs(left.ray_distance_cm - right.ray_distance_cm) > 25.0)
            {
                return left.ray_distance_cm < right.ray_distance_cm;
            }
            return left.selection_score < right.selection_score;
        });

        if (samples != nullptr)
        {
            const auto sample_count = std::min<std::size_t>(hits.size(), 8);
            for (std::size_t index = 0; index < sample_count; ++index)
            {
                samples->push_back(hits[index]);
            }
        }

        std::ostringstream route;
        route << "editor.ServerAimScan world=" << full_name(world)
              << " levels=" << scanned_levels
              << " actorSlots=" << actor_slots
              << " locationReads=" << location_reads
              << " locationFailures=" << location_failures
              << " rayHits=" << ray_hits
              << " blockedHits=" << blocked_hits
              << " safeHits=" << hits.size()
              << " maxDistanceCm=" << context.max_distance_cm
              << " aimToleranceCm=" << aim_tolerance_cm
              << " tolerancePerMeter=" << dynamic_tolerance_per_meter
              << " minAlongCm=" << min_along_cm
              << " limitHit=" << (limit_hit ? "true" : "false");
        attempts.push_back(route.str());
        for (const auto& note : notes)
        {
            attempts.push_back("editor.ServerAimScan." + note);
        }

        if (hits.empty())
        {
            attempts.push_back("editor.ServerAimScan no safe actor under aim ray");
            return false;
        }

        selection = hits.front();
        attempts.push_back(
            "editor.ServerAimScan selected actor=" + selection.actor_full_name +
            " alongCm=" + std::to_string(selection.along_distance_cm) +
            " rayCm=" + std::to_string(selection.ray_distance_cm));
        return true;
    }

    auto select_editor_actor_by_aim_trace(
        const std::string& command_text,
        std::vector<std::string>& attempts,
        EditorAimContext& context,
        EditorTraceSelection& selection,
        std::vector<EditorTraceSelection>* samples = nullptr) -> bool
    {
        attempts.push_back("editor.Trace disabled: exact client-captured actor handle required after live crashes");
        (void)command_text;
        (void)context;
        (void)selection;
        (void)samples;
        return false;
    }

    auto find_single_path_object(
        std::initializer_list<const wchar_t*> class_names,
        std::initializer_list<const wchar_t*> object_paths,
        std::vector<std::string>& route_counts) -> RC::Unreal::UObject*
    {
        for (const auto* class_name : class_names)
        {
            for (const auto* object_path : object_paths)
            {
                auto* object = RC::Unreal::UObjectGlobals::FindObject(class_name, object_path, 0, object_flag_class_default_object);
                std::ostringstream route;
                route << "FindObject(" << narrow(class_name) << "," << narrow(object_path) << ")=" << (object != nullptr ? full_name(object) : "0");
                route_counts.push_back(route.str());
                if (object != nullptr) return object;
            }
        }
        return nullptr;
    }

    auto find_loaded_object_by_path(
        std::initializer_list<const wchar_t*> class_names,
        const std::wstring& object_path,
        std::vector<std::string>& route_counts) -> RC::Unreal::UObject*
    {
        if (object_path.empty())
        {
            route_counts.push_back("FindObject skipped: empty path");
            return nullptr;
        }
        for (const auto* class_name : class_names)
        {
            auto* object = RC::Unreal::UObjectGlobals::FindObject(class_name, object_path.c_str(), 0, object_flag_class_default_object);
            std::ostringstream route;
            route << "FindObject(" << narrow(class_name) << "," << narrow(object_path) << ")=" << (object != nullptr ? full_name(object) : "0");
            route_counts.push_back(route.str());
            if (object != nullptr) return object;
        }
        return nullptr;
    }

    auto object_name_from_path(const std::string& object_path) -> std::string
    {
        auto raw = object_path;
        const auto quote_pos = raw.find('\'');
        if (quote_pos != std::string::npos)
        {
            const auto end_quote_pos = raw.find_last_of('\'');
            if (end_quote_pos != std::string::npos && end_quote_pos > quote_pos)
            {
                raw = raw.substr(quote_pos + 1, end_quote_pos - quote_pos - 1);
            }
        }
        const auto dot_pos = raw.find_last_of('.');
        const auto slash_pos = raw.find_last_of("/\\");
        const auto pos = dot_pos != std::string::npos ? dot_pos : slash_pos;
        return pos == std::string::npos ? raw : raw.substr(pos + 1);
    }

    auto find_loaded_object_by_path_or_name(
        std::initializer_list<const wchar_t*> class_names,
        const std::string& object_path,
        std::vector<std::string>& route_counts) -> RC::Unreal::UObject*
    {
        auto* object = find_loaded_object_by_path(class_names, widen(object_path), route_counts);
        if (object != nullptr) return object;

        const auto object_name = object_name_from_path(object_path);
        if (object_name.empty()) return nullptr;
        const auto wide_name = widen(object_name);
        for (const auto* class_name : class_names)
        {
            std::vector<RC::Unreal::UObject*> found;
            RC::Unreal::UObjectGlobals::FindObjects(class_name, wide_name.c_str(), found, 0, object_flag_class_default_object, false);
            std::ostringstream route;
            route << "FindObjects(" << narrow(class_name) << "," << object_name << ")=" << found.size();
            if (!found.empty()) route << ":" << full_name(found.front());
            route_counts.push_back(route.str());
            if (!found.empty()) return found.front();
        }
        return nullptr;
    }

    auto looks_like_uclass(RC::Unreal::UObject* object) -> bool
    {
        const auto full = full_name(object);
        return full.rfind("BlueprintGeneratedClass ", 0) == 0 || full.rfind("Class ", 0) == 0;
    }

    auto find_trade_outpost_manager_class(std::vector<std::string>& route_counts) -> RC::Unreal::UClass*
    {
        auto* native_object = find_single_path_object(
            {L"Class", L"UClass", L"Object"},
            {
                L"/Script/SCUM.TradeOutpostManager",
                L"Class /Script/SCUM.TradeOutpostManager",
                L"Class'/Script/SCUM.TradeOutpostManager'",
            },
            route_counts);
        if (looks_like_uclass(native_object)) return static_cast<RC::Unreal::UClass*>(native_object);

        auto* object = find_single_path_object(
            {L"BlueprintGeneratedClass", L"Class", L"UClass"},
            {
                L"/Game/ConZ_Files/Economy/BP_TradeOutpostManager.BP_TradeOutpostManager_C",
                L"BlueprintGeneratedClass /Game/ConZ_Files/Economy/BP_TradeOutpostManager.BP_TradeOutpostManager_C",
                L"BlueprintGeneratedClass'/Game/ConZ_Files/Economy/BP_TradeOutpostManager.BP_TradeOutpostManager_C'",
            },
            route_counts);
        if (looks_like_uclass(object)) return static_cast<RC::Unreal::UClass*>(object);

        object = RC::Unreal::UObjectGlobals::FindFirstOf(L"BP_TradeOutpostManager_C");
        route_counts.push_back(std::string("FindFirstOf(BP_TradeOutpostManager_C)=") + (object != nullptr ? full_name(object) : "0"));
        if (looks_like_uclass(object)) return static_cast<RC::Unreal::UClass*>(object);
        return nullptr;
    }

    auto find_outpost_description(const std::string& outpost_key, std::vector<std::string>& route_counts) -> RC::Unreal::UObject*
    {
        if (outpost_key == "a_0")
        {
            return find_single_path_object(
                {L"TradingOutpostDescriptionDataAsset", L"Object"},
                {L"/Game/ConZ_Files/Economy/OutpostDescriptions/A_0_TradeOutpostDescription.A_0_TradeOutpostDescription"},
                route_counts);
        }
        if (outpost_key == "b_4")
        {
            return find_single_path_object(
                {L"TradingOutpostDescriptionDataAsset", L"Object"},
                {L"/Game/ConZ_Files/Economy/OutpostDescriptions/B_4_TradeOutpostDescription.B_4_TradeOutpostDescription"},
                route_counts);
        }
        if (outpost_key == "c_2")
        {
            return find_single_path_object(
                {L"TradingOutpostDescriptionDataAsset", L"Object"},
                {L"/Game/ConZ_Files/Economy/OutpostDescriptions/C_2_TradeOutpostDescription.C_2_TradeOutpostDescription"},
                route_counts);
        }
        if (outpost_key == "z_3")
        {
            return find_single_path_object(
                {L"TradingOutpostDescriptionDataAsset", L"Object"},
                {L"/Game/ConZ_Files/Economy/OutpostDescriptions/Z_3_TradeOutpostDescription.Z_3_TradeOutpostDescription"},
                route_counts);
        }
        route_counts.push_back("outpost description skipped: empty/unknown key");
        return nullptr;
    }

    auto find_trade_outpost_managers(std::vector<std::string>* route_counts = nullptr) -> std::vector<RC::Unreal::UObject*>
    {
        auto managers = find_all_unique(
            {L"TradeOutpostManager", L"ATradeOutpostManager", L"BP_TradeOutpostManager_C", L"TradeOutpostManager_C", L"BP_TradeOutpostManager"},
            route_counts);
        if (!managers.empty()) return managers;

        auto named = find_named_objects_unique(
            {L"TradeOutpostManager", L"BP_TradeOutpostManager_C"},
            {L"BP_TradeOutpostManager_2", L"BP_TradeOutpostManager_B_4"},
            route_counts);
        std::unordered_set<RC::Unreal::UObject*> seen(managers.begin(), managers.end());
        for (auto* object : named)
        {
            append_unique_object(managers, seen, object);
        }
        if (!managers.empty()) return managers;

        auto paths = find_path_objects_unique(
            {L"BP_TradeOutpostManager_C", L"TradeOutpostManager"},
            {
                L"/Game/ConZ_Files/Maps/The_Island/A_0_Outpost.A_0_Outpost:PersistentLevel.BP_TradeOutpostManager_2",
                L"/Game/ConZ_Files/Maps/The_Island/B_4_Outpost.B_4_Outpost:PersistentLevel.BP_TradeOutpostManager_B_4",
                L"/Game/ConZ_Files/Maps/The_Island/C_2_Outpost.C_2_Outpost:PersistentLevel.BP_TradeOutpostManager_2",
                L"/Game/ConZ_Files/Maps/The_Island/Z_3_Outpost.Z_3_Outpost:PersistentLevel.BP_TradeOutpostManager_2",
            },
            route_counts);
        for (auto* object : paths)
        {
            append_unique_object(managers, seen, object);
        }
        if (managers.empty())
        {
            std::vector<std::string> class_routes;
            auto* manager_class = find_trade_outpost_manager_class(class_routes);
            if (route_counts != nullptr)
            {
                route_counts->insert(route_counts->end(), class_routes.begin(), class_routes.end());
            }
            if (manager_class != nullptr)
            {
                for (auto* object : collect_actors_of_class_via_gameplay_statics(manager_class, "TradeOutpostManagers", route_counts))
                {
                    append_unique_object(managers, seen, object);
                }
            }
        }
        if (managers.empty())
        {
            int matched_extra_managers = 0;
            for (auto* object : collect_world_extra_referenced_objects(route_counts))
            {
                const auto name = lower_copy(full_name(object));
                if (name.find("tradeoutpostmanager") == std::string::npos &&
                    name.find("bp_tradeoutpostmanager") == std::string::npos)
                {
                    continue;
                }
                ++matched_extra_managers;
                append_unique_object(managers, seen, object);
            }
            if (route_counts != nullptr)
            {
                route_counts->push_back("ExtraRefs.TradeOutpostManagers=" + std::to_string(matched_extra_managers));
            }
        }
        if (!managers.empty()) return managers;
        if (managers.empty())
        {
            int matched_level_managers = 0;
            for (auto* actor : collect_loaded_outpost_level_actors(route_counts))
            {
                const auto name = lower_copy(full_name(actor));
                if (name.find("tradeoutpostmanager") == std::string::npos &&
                    name.find("bp_tradeoutpostmanager") == std::string::npos)
                {
                    continue;
                }
                ++matched_level_managers;
                append_unique_object(managers, seen, actor);
            }
            if (route_counts != nullptr)
            {
                route_counts->push_back("LevelActors.TradeOutpostManagers=" + std::to_string(matched_level_managers));
            }
        }
        return managers;
    }

    auto manager_matches_outpost_key(RC::Unreal::UObject* manager, const std::string& outpost_key) -> bool;

    auto manager_paths_for_outpost_key(const std::string& outpost_key) -> std::vector<const wchar_t*>
    {
        if (outpost_key == "a_0")
        {
            return {
                L"/Game/ConZ_Files/Maps/The_Island/A_0_Outpost.A_0_Outpost:PersistentLevel.BP_TradeOutpostManager_2"};
        }
        if (outpost_key == "b_4")
        {
            return {
                L"/Game/ConZ_Files/Maps/The_Island/B_4_Outpost.B_4_Outpost:PersistentLevel.BP_TradeOutpostManager_B_4"};
        }
        if (outpost_key == "c_2")
        {
            return {
                L"/Game/ConZ_Files/Maps/The_Island/C_2_Outpost.C_2_Outpost:PersistentLevel.BP_TradeOutpostManager_2"};
        }
        if (outpost_key == "z_3")
        {
            return {
                L"/Game/ConZ_Files/Maps/The_Island/Z_3_Outpost.Z_3_Outpost:PersistentLevel.BP_TradeOutpostManager_2"};
        }
        return {};
    }

    auto armory_tradepost_paths_for_outpost_key(const std::string& outpost_key) -> std::vector<const wchar_t*>
    {
        if (outpost_key == "a_0")
        {
            return {
                L"/Game/ConZ_Files/Maps/The_Island/A_0_Outpost.A_0_Outpost:PersistentLevel.BP_Outpost_Armory_NPCInteractionBox_2"};
        }
        if (outpost_key == "b_4")
        {
            return {
                L"/Game/ConZ_Files/Maps/The_Island/B_4_Outpost.B_4_Outpost:PersistentLevel.BP_Outpost_Armory_NPCInteractionBox_2"};
        }
        if (outpost_key == "c_2")
        {
            return {
                L"/Game/ConZ_Files/Maps/The_Island/C_2_Outpost.C_2_Outpost:PersistentLevel.BP_Outpost_Armory_NPCInteractionBox_2"};
        }
        if (outpost_key == "z_3")
        {
            return {
                L"/Game/ConZ_Files/Maps/The_Island/Z_3_Outpost.Z_3_Outpost:PersistentLevel.BP_Outpost_Armory_NPCInteractionBox_2"};
        }
        return {};
    }

    auto cached_trade_outpost_manager_count() -> int
    {
        int count = 0;
        for (const auto& entry : g_trade_outpost_manager_cache)
        {
            if (entry.second.object != nullptr) ++count;
        }
        return count;
    }

    auto cache_trade_outpost_managers_for_key(const std::string& outpost_key, const std::vector<RC::Unreal::UObject*>& managers) -> void
    {
        (void)outpost_key;
        (void)managers;
        // Live-server safety: do not retain UObject pointers across ticks/commands.
        // Manager lookup is intentionally command-scoped after the 0.1.86 stale-cache crash.
        return;
    }

    auto disabled_cache_trade_outpost_managers_for_key(const std::string& outpost_key, const std::vector<RC::Unreal::UObject*>& managers) -> void
    {
        for (auto* manager : managers)
        {
            if (manager == nullptr || !manager_matches_outpost_key(manager, outpost_key)) continue;
            auto& cached = g_trade_outpost_manager_cache[outpost_key];
            cached.object = manager;
            cached.full_name = full_name(manager);
            ++cached.refresh_count;
            return;
        }
    }

    auto cached_trade_outpost_manager_for_key(const std::string& outpost_key, std::vector<std::string>* route_counts) -> RC::Unreal::UObject*
    {
        if (route_counts != nullptr)
        {
            route_counts->push_back("ManagerCache(" + outpost_key + ")=disabled:command-scoped-lookup-only");
        }
        return nullptr;
    }

    auto disabled_cached_trade_outpost_manager_for_key(const std::string& outpost_key, std::vector<std::string>* route_counts) -> RC::Unreal::UObject*
    {
        const auto it = g_trade_outpost_manager_cache.find(outpost_key);
        if (it == g_trade_outpost_manager_cache.end() || it->second.object == nullptr)
        {
            if (route_counts != nullptr)
            {
                route_counts->push_back("ManagerCache(" + outpost_key + ")=miss");
            }
            return nullptr;
        }

        auto* manager = it->second.object;
        if (!manager_matches_outpost_key(manager, outpost_key))
        {
            if (route_counts != nullptr)
            {
                route_counts->push_back("ManagerCache(" + outpost_key + ")=stale:" + it->second.full_name);
            }
            g_trade_outpost_manager_cache.erase(it);
            return nullptr;
        }

        if (route_counts != nullptr)
        {
            route_counts->push_back("ManagerCache(" + outpost_key + ")=" + full_name(manager));
        }
        return manager;
    }

    auto find_trade_outpost_managers_for_key(
        const std::string& outpost_key,
        std::vector<std::string>* route_counts = nullptr,
        bool include_slow_fallbacks = true,
        bool use_cache = true) -> std::vector<RC::Unreal::UObject*>
    {
        std::vector<RC::Unreal::UObject*> managers;
        std::unordered_set<RC::Unreal::UObject*> seen;
        if (use_cache)
        {
            if (auto* cached = cached_trade_outpost_manager_for_key(outpost_key, route_counts))
            {
                append_unique_object(managers, seen, cached);
                return managers;
            }
        }

        const auto paths = manager_paths_for_outpost_key(outpost_key);
        for (const auto* class_name : {L"BP_TradeOutpostManager_C", L"TradeOutpostManager", L"ATradeOutpostManager", L"Object", L"UObject"})
        {
            for (const auto* object_path : paths)
            {
                auto* object = RC::Unreal::UObjectGlobals::FindObject(class_name, object_path, 0, object_flag_class_default_object);
                if (route_counts != nullptr)
                {
                    std::ostringstream route;
                    route << "FindObject(" << narrow(class_name) << "," << narrow(object_path) << ")=" << (object != nullptr ? full_name(object) : "0");
                    route_counts->push_back(route.str());
                }
                if (object != nullptr && manager_matches_outpost_key(object, outpost_key))
                {
                    append_unique_object(managers, seen, object);
                }
            }
        }

        const auto object_names = outpost_key == "b_4"
            ? std::vector<const wchar_t*>{L"BP_TradeOutpostManager_B_4", L"BP_TradeOutpostManager_2"}
            : std::vector<const wchar_t*>{L"BP_TradeOutpostManager_2"};
        for (const auto* class_name : {L"BP_TradeOutpostManager_C", L"TradeOutpostManager", L"ATradeOutpostManager", L"Object", L"UObject"})
        {
            for (const auto* object_name : object_names)
            {
                std::vector<RC::Unreal::UObject*> found;
                RC::Unreal::UObjectGlobals::FindObjects(class_name, object_name, found, 0, object_flag_class_default_object, false);
                if (route_counts != nullptr)
                {
                    std::ostringstream route;
                    route << "FindObjects(" << narrow(class_name) << "," << narrow(object_name) << ")=" << found.size();
                    if (!found.empty()) route << ":" << full_name(found.front());
                    route_counts->push_back(route.str());
                }
                for (auto* object : found)
                {
                    if (object != nullptr && manager_matches_outpost_key(object, outpost_key))
                    {
                        append_unique_object(managers, seen, object);
                    }
                }
            }
        }
        if (!managers.empty())
        {
            cache_trade_outpost_managers_for_key(outpost_key, managers);
            return managers;
        }
        if (!include_slow_fallbacks)
        {
            if (route_counts != nullptr)
            {
                route_counts->push_back("SlowManagerFallbacks=skipped:fast-manager-lookup-only");
            }
            return managers;
        }

        std::vector<std::string> class_routes;
        auto* manager_class = find_trade_outpost_manager_class(class_routes);
        if (route_counts != nullptr)
        {
            route_counts->insert(route_counts->end(), class_routes.begin(), class_routes.end());
        }
        if (manager_class != nullptr)
        {
            int matched_gameplay_managers = 0;
            for (auto* actor : collect_actors_of_class_via_gameplay_statics(manager_class, "TradeOutpostManagers(" + outpost_key + ")", route_counts))
            {
                if (manager_matches_outpost_key(actor, outpost_key))
                {
                    ++matched_gameplay_managers;
                    append_unique_object(managers, seen, actor);
                }
            }
            if (route_counts != nullptr)
            {
                route_counts->push_back("GameplayStatics.TradeOutpostManagers(" + outpost_key + ")=" + std::to_string(matched_gameplay_managers));
            }
        }
        if (!managers.empty())
        {
            cache_trade_outpost_managers_for_key(outpost_key, managers);
            return managers;
        }

        int matched_extra_managers = 0;
        for (auto* object : collect_world_extra_referenced_objects(route_counts))
        {
            const auto name = lower_copy(full_name(object));
            if (name.find("tradeoutpostmanager") == std::string::npos &&
                name.find("bp_tradeoutpostmanager") == std::string::npos)
            {
                continue;
            }
            if (manager_matches_outpost_key(object, outpost_key))
            {
                ++matched_extra_managers;
                append_unique_object(managers, seen, object);
            }
        }
        if (route_counts != nullptr)
        {
            route_counts->push_back("ExtraRefs.TradeOutpostManagers(" + outpost_key + ")=" + std::to_string(matched_extra_managers));
        }
        if (!managers.empty())
        {
            cache_trade_outpost_managers_for_key(outpost_key, managers);
            return managers;
        }

        int matched_level_managers = 0;
        for (auto* actor : collect_loaded_outpost_level_actors(route_counts))
        {
            const auto name = lower_copy(full_name(actor));
            if (name.find("tradeoutpostmanager") == std::string::npos &&
                name.find("bp_tradeoutpostmanager") == std::string::npos)
            {
                continue;
            }
            if (manager_matches_outpost_key(actor, outpost_key))
            {
                ++matched_level_managers;
                append_unique_object(managers, seen, actor);
            }
        }
        if (route_counts != nullptr)
        {
            route_counts->push_back("LevelActors.TradeOutpostManagers(" + outpost_key + ")=" + std::to_string(matched_level_managers));
        }
        cache_trade_outpost_managers_for_key(outpost_key, managers);
        return managers;
    }

    auto refresh_trade_outpost_manager_cache_fast(std::vector<std::string>* route_counts = nullptr) -> int
    {
        for (const auto* key : {"a_0", "b_4", "c_2", "z_3"})
        {
            std::vector<std::string> local_routes;
            auto managers = find_trade_outpost_managers_for_key(key, &local_routes, false, false);
            if (route_counts != nullptr)
            {
                route_counts->push_back(std::string("RefreshManagerCache(") + key + ")=" + std::to_string(managers.size()));
                const auto limit = std::min<std::size_t>(local_routes.size(), 6);
                for (std::size_t index = 0; index < limit; ++index)
                {
                    route_counts->push_back(std::string("  ") + local_routes[index]);
                }
            }
        }
        return cached_trade_outpost_manager_count();
    }

    auto array_contains_object(const TArrayAbi* array, RC::Unreal::UObject* object) -> bool
    {
        if (!is_reasonable_array(array) || array->data == nullptr || object == nullptr) return false;
        auto** items = static_cast<RC::Unreal::UObject**>(array->data);
        for (std::int32_t index = 0; index < array->count; ++index)
        {
            if (items[index] == object) return true;
        }
        return false;
    }

    auto find_assigned_manager(const std::vector<RC::Unreal::UObject*>& managers, RC::Unreal::UObject* trade_post) -> RC::Unreal::UObject*
    {
        for (auto* manager : managers)
        {
            const auto* assigned = array_property(manager, L"_assignedTradePosts");
            if (array_contains_object(assigned, trade_post))
            {
                return manager;
            }
        }
        return nullptr;
    }

    auto outpost_key_from_name(const std::string& full_name_lower) -> std::string
    {
        for (const auto* key : {"a_0", "b_4", "c_2", "z_3"})
        {
            const std::string token = std::string("/") + key + "_outpost";
            if (full_name_lower.find(token) != std::string::npos) return key;
            const std::string description_token = std::string("/") + key + "_tradeoutpostdescription";
            if (full_name_lower.find(description_token) != std::string::npos) return key;
        }
        return {};
    }

    auto canonical_outpost_key(std::string key) -> std::string
    {
        key = lower_copy(key);
        key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        }), key.end());
        std::replace(key.begin(), key.end(), '-', '_');
        if (key == "a0") return "a_0";
        if (key == "b4") return "b_4";
        if (key == "c2") return "c_2";
        if (key == "z3") return "z_3";
        if (key == "a_0" || key == "b_4" || key == "c_2" || key == "z_3") return key;
        return {};
    }

    auto stock_outpost_center(const std::string& key, double& x, double& y) -> bool
    {
        const auto canonical = canonical_outpost_key(key);
        if (canonical == "c_2")
        {
            x = -152584.15234375;
            y = 289693.0;
            return true;
        }
        if (canonical == "b_4")
        {
            x = 570831.0234375;
            y = -224794.244140625;
            return true;
        }
        if (canonical == "z_3")
        {
            x = 21713.100341796875;
            y = -677039.1875;
            return true;
        }
        if (canonical == "a_0")
        {
            x = -621392.3671875;
            y = -557528.75;
            return true;
        }
        return false;
    }

    auto manager_matches_outpost_key(RC::Unreal::UObject* manager, const std::string& outpost_key) -> bool
    {
        if (manager == nullptr || outpost_key.empty()) return false;
        const auto manager_name = lower_copy(full_name(manager));
        if (outpost_key_from_name(manager_name) == outpost_key) return true;
        auto* description = object_property(manager, L"_outpostDescription");
        return outpost_key_from_name(lower_copy(full_name(description))) == outpost_key;
    }

    auto find_manager_for_tradepost(
        const std::vector<RC::Unreal::UObject*>& managers,
        RC::Unreal::UObject* trade_post,
        std::string* route = nullptr) -> RC::Unreal::UObject*
    {
        if (auto* assigned = find_assigned_manager(managers, trade_post))
        {
            if (route != nullptr) *route = "_assignedTradePosts";
            return assigned;
        }

        const auto outpost_key = outpost_key_from_name(lower_copy(full_name(trade_post)));
        for (auto* manager : managers)
        {
            if (manager_matches_outpost_key(manager, outpost_key))
            {
                if (route != nullptr) *route = "outpost-name:" + outpost_key;
                return manager;
            }
        }

        if (route != nullptr) *route = outpost_key.empty() ? "not-found:no-outpost-key" : "not-found:" + outpost_key;
        return nullptr;
    }

    auto add_object_to_array(TArrayAbi* array, RC::Unreal::UObject* object, std::string& detail) -> bool
    {
        if (array == nullptr)
        {
            detail = "array pointer is null";
            return false;
        }
        if (object == nullptr)
        {
            detail = "object pointer is null";
            return false;
        }
        if (array->count < 0 || array->max < 0 || array->count > array->max || array->count > 10000)
        {
            detail = "array header is not sane";
            return false;
        }
        if (array->data == nullptr)
        {
            detail = "array data is null; refusing raw game TArray append";
            return false;
        }
        if (array_contains_object(array, object))
        {
            detail = "object already registered";
            return true;
        }

        if (array->count >= array->max)
        {
            detail = "array capacity is full; refusing FMemory::Realloc on game UPROPERTY TArray";
            return false;
        }
        else
        {
            detail = "array capacity reused";
        }

        auto** items = static_cast<RC::Unreal::UObject**>(array->data);
        items[array->count] = object;
        ++array->count;
        detail += "; appended index=" + std::to_string(array->count - 1);
        return true;
    }

    auto add_pending_trader_personality(
        RC::Unreal::UObject* economy_manager,
        RC::Unreal::UObject* personality,
        std::string& detail,
        int& before_count,
        int& after_count) -> bool
    {
        before_count = -1;
        after_count = -1;
        if (economy_manager == nullptr)
        {
            detail = "economy manager is null";
            return false;
        }
        if (personality == nullptr)
        {
            detail = "personality is null";
            return false;
        }

        auto* array = const_cast<TArrayAbi*>(array_property(economy_manager, L"_pendingTraderPersonalities"));
        if (array == nullptr)
        {
            detail = "_pendingTraderPersonalities pointer is null";
            return false;
        }
        if (array->count < 0 || array->max < 0 || array->count > array->max || array->count > 4096)
        {
            detail = "_pendingTraderPersonalities array header is not sane";
            return false;
        }

        before_count = array->count;
        if (array->data != nullptr)
        {
            auto* items = static_cast<FPendingTraderPersonalityDataHelperStructAbi*>(array->data);
            for (std::int32_t index = 0; index < array->count; ++index)
            {
                if (items[index].personality == personality)
                {
                    after_count = array->count;
                    detail = "personality already pending index=" + std::to_string(index);
                    return true;
                }
            }
        }

        if (array->count >= array->max)
        {
            const auto old_max = array->max;
            const auto new_max = std::max<std::int32_t>(array->count + 1, std::max<std::int32_t>(2, array->max * 2));
            void* new_data = RC::Unreal::FMemory::Realloc(
                array->data,
                static_cast<std::size_t>(new_max) * sizeof(FPendingTraderPersonalityDataHelperStructAbi),
                alignof(FPendingTraderPersonalityDataHelperStructAbi));
            if (new_data == nullptr)
            {
                detail = "FMemory::Realloc failed for _pendingTraderPersonalities";
                return false;
            }
            array->data = new_data;
            array->max = new_max;
            detail = "array reallocated " + std::to_string(old_max) + "->" + std::to_string(new_max);
        }
        else
        {
            detail = "array capacity reused";
        }

        auto* items = static_cast<FPendingTraderPersonalityDataHelperStructAbi*>(array->data);
        items[array->count].personality = personality;
        ++array->count;
        after_count = array->count;
        detail += "; appended pending personality index=" + std::to_string(array->count - 1);
        return true;
    }

    struct LinkedArmoryEconomyPrepareResult
    {
        bool requested{};
        bool allowed{};
        bool ok{};
        float sale_distance{};
        float relevancy_range{};
        float trade_outposts_update_time{};
        bool force_unlimited_stock{};
        bool force_unlimited_funds{};
        bool register_pending_personality{};
        bool sale_set{};
        bool relevancy_set{};
        bool update_set{};
        bool unlimited_stock_set{};
        bool unlimited_funds_set{};
        bool pending_personality_added{};
        int pending_before{-1};
        int pending_after{-1};
        RC::Unreal::UObject* economy_manager{};
        RC::Unreal::UObject* trader_component{};
        std::string sale_detail{"not-run"};
        std::string relevancy_detail{"not-run"};
        std::string update_detail{"not-run"};
        std::string unlimited_stock_detail{"disabled"};
        std::string unlimited_funds_detail{"disabled"};
        std::string pending_detail{"disabled"};
        std::string detail{"not-run"};
    };

    auto prepare_linked_armory_economy(
        const std::string& command_text,
        bool allow_prepare,
        RC::Unreal::UObject* selected_personality) -> LinkedArmoryEconomyPrepareResult
    {
        LinkedArmoryEconomyPrepareResult result{};
        result.requested = regex_bool(command_text, "prepareEconomyForLinkedTrader", true);
        result.allowed = allow_prepare;
        result.sale_distance = static_cast<float>(regex_number(command_text, "saleDistance", 2000.0));
        result.relevancy_range = static_cast<float>(regex_number(command_text, "relevancyRange", 3000.0));
        result.trade_outposts_update_time = static_cast<float>(regex_number(command_text, "tradeOutpostsUpdateTime", 60.0));
        result.force_unlimited_stock = regex_bool(command_text, "forceUnlimitedStock", false);
        result.force_unlimited_funds = regex_bool(command_text, "forceUnlimitedFunds", false);
        result.register_pending_personality = regex_bool(command_text, "registerPendingPersonality", false);

        result.sale_distance = std::clamp(result.sale_distance, 1.0f, 10000000.0f);
        result.relevancy_range = std::clamp(result.relevancy_range, 1.0f, 10000000.0f);
        result.trade_outposts_update_time = std::clamp(result.trade_outposts_update_time, 1.0f, 60.0f);

        if (!result.requested)
        {
            result.detail = "economy prepare disabled by payload";
            result.ok = true;
            return result;
        }

        if (!allow_prepare)
        {
            result.detail = "skipped:manager-linked-lifecycle-required";
            result.pending_detail = result.detail;
            return result;
        }

        result.economy_manager = RC::Unreal::UObjectGlobals::FindFirstOf(L"BP_EconomyManager_C");
        if (result.economy_manager == nullptr)
        {
            result.economy_manager = RC::Unreal::UObjectGlobals::FindFirstOf(L"ConZEconomyManager");
        }
        result.trader_component = object_property(result.economy_manager, L"_traderManagingComponent");

        result.sale_set = set_float_property(result.economy_manager, L"_maxSaleDistance", result.sale_distance, result.sale_detail, true);
        result.relevancy_set = set_float_property(result.trader_component, L"_pawnRelevancyRange", result.relevancy_range, result.relevancy_detail, true);
        result.update_set = set_float_property(result.trader_component, L"_tradeOutpostsUpdateTime", result.trade_outposts_update_time, result.update_detail, false);
        if (result.force_unlimited_stock)
        {
            result.unlimited_stock_set = set_bool_property(result.economy_manager, L"_tradersUnlimitedStock", true, result.unlimited_stock_detail);
        }
        if (result.force_unlimited_funds)
        {
            result.unlimited_funds_set = set_bool_property(result.economy_manager, L"_tradersUnlimitedFunds", true, result.unlimited_funds_detail);
        }

        if (result.register_pending_personality)
        {
            result.pending_personality_added = add_pending_trader_personality(
                result.economy_manager,
                selected_personality,
                result.pending_detail,
                result.pending_before,
                result.pending_after);
        }

        const auto pending_ok = !result.register_pending_personality || result.pending_personality_added;
        result.ok =
            result.economy_manager != nullptr &&
            result.trader_component != nullptr &&
            result.sale_set &&
            result.relevancy_set &&
            result.update_set &&
            pending_ok;
        result.detail = result.ok ? "economy prepared for linked Armory trader" : "economy prepare incomplete";
        return result;
    }

    void append_linked_armory_economy_prepare_json(
        std::ostringstream& ss,
        const LinkedArmoryEconomyPrepareResult& result)
    {
        ss << "{\"requested\":" << (result.requested ? "true" : "false")
           << ",\"allowed\":" << (result.allowed ? "true" : "false")
           << ",\"ok\":" << (result.ok ? "true" : "false")
           << ",\"detail\":\"" << json_escape(result.detail) << "\""
           << ",\"economyManager\":\"" << json_escape(full_name(result.economy_manager)) << "\""
           << ",\"traderManagingComponent\":\"" << json_escape(full_name(result.trader_component)) << "\""
           << ",\"requestedValues\":{\"saleDistance\":" << result.sale_distance
           << ",\"relevancyRange\":" << result.relevancy_range
           << ",\"tradeOutpostsUpdateTime\":" << result.trade_outposts_update_time
           << ",\"forceUnlimitedStock\":" << (result.force_unlimited_stock ? "true" : "false")
           << ",\"forceUnlimitedFunds\":" << (result.force_unlimited_funds ? "true" : "false")
           << ",\"registerPendingPersonality\":" << (result.register_pending_personality ? "true" : "false") << "}"
           << ",\"currentValues\":{\"maxSaleDistance\":" << float_property(result.economy_manager, L"_maxSaleDistance", -1.0f)
           << ",\"pawnRelevancyRange\":" << float_property(result.trader_component, L"_pawnRelevancyRange", -1.0f)
           << ",\"tradeOutpostsUpdateTime\":" << float_property(result.trader_component, L"_tradeOutpostsUpdateTime", -1.0f)
           << ",\"tradersUnlimitedStock\":" << (bool_property(result.economy_manager, L"_tradersUnlimitedStock", false) ? "true" : "false")
           << ",\"tradersUnlimitedFunds\":" << (bool_property(result.economy_manager, L"_tradersUnlimitedFunds", false) ? "true" : "false") << "}"
           << ",\"setResults\":{\"saleSet\":" << (result.sale_set ? "true" : "false")
           << ",\"saleDetail\":\"" << json_escape(result.sale_detail) << "\""
           << ",\"relevancySet\":" << (result.relevancy_set ? "true" : "false")
           << ",\"relevancyDetail\":\"" << json_escape(result.relevancy_detail) << "\""
           << ",\"updateSet\":" << (result.update_set ? "true" : "false")
           << ",\"updateDetail\":\"" << json_escape(result.update_detail) << "\""
           << ",\"unlimitedStockSet\":" << (result.unlimited_stock_set ? "true" : "false")
           << ",\"unlimitedStockDetail\":\"" << json_escape(result.unlimited_stock_detail) << "\""
           << ",\"unlimitedFundsSet\":" << (result.unlimited_funds_set ? "true" : "false")
           << ",\"unlimitedFundsDetail\":\"" << json_escape(result.unlimited_funds_detail) << "\""
           << ",\"pendingPersonalityAdded\":" << (result.pending_personality_added ? "true" : "false")
           << ",\"pendingBefore\":" << result.pending_before
           << ",\"pendingAfter\":" << result.pending_after
           << ",\"pendingDetail\":\"" << json_escape(result.pending_detail) << "\"}}";
    }

    auto copy_raw_array_property(
        RC::Unreal::UObject* destination,
        RC::Unreal::UObject* source,
        const wchar_t* property_name,
        std::size_t element_size,
        std::int32_t max_allowed_count,
        std::string& detail) -> bool
    {
        auto* destination_array = const_cast<TArrayAbi*>(array_property(destination, property_name));
        const auto* source_array = array_property(source, property_name);
        if (destination_array == nullptr || source_array == nullptr)
        {
            detail = "array missing: " + narrow(property_name);
            return false;
        }
        if (source_array->count < 0 || source_array->max < source_array->count || source_array->count > max_allowed_count)
        {
            detail = "source array not sane: " + narrow(property_name);
            return false;
        }
        if (source_array->count == 0)
        {
            destination_array->count = 0;
            detail = "copied empty array: " + narrow(property_name);
            return true;
        }
        if (source_array->data == nullptr)
        {
            detail = "source array data null: " + narrow(property_name);
            return false;
        }
        void* data = RC::Unreal::FMemory::Realloc(
            destination_array->data,
            static_cast<std::size_t>(source_array->count) * element_size,
            16);
        if (data == nullptr)
        {
            detail = "FMemory::Realloc failed: " + narrow(property_name);
            return false;
        }
        std::memcpy(data, source_array->data, static_cast<std::size_t>(source_array->count) * element_size);
        destination_array->data = data;
        destination_array->count = source_array->count;
        destination_array->max = source_array->count;
        detail = "copied " + narrow(property_name) + " count=" + std::to_string(source_array->count);
        return true;
    }

    auto make_transform(double x, double y, double z, double yaw_degrees) -> RC::Unreal::FTransform
    {
        constexpr auto pi = 3.14159265358979323846;
        const auto half_yaw = static_cast<float>((yaw_degrees * pi / 180.0) * 0.5);
        RC::Unreal::FTransform transform{};
        transform.rotation.x = 0.0f;
        transform.rotation.y = 0.0f;
        transform.rotation.z = std::sin(half_yaw);
        transform.rotation.w = std::cos(half_yaw);
        transform.translation.x = static_cast<float>(x);
        transform.translation.y = static_cast<float>(y);
        transform.translation.z = static_cast<float>(z);
        transform.scale3d.x = 1.0f;
        transform.scale3d.y = 1.0f;
        transform.scale3d.z = 1.0f;
        return transform;
    }

    auto trader_kind_tokens(const std::string& actor_class) -> std::vector<std::string>
    {
        const auto actor = lower_copy(actor_class);
        if (contains_any(actor, {"arm", "weapon", "gun", "armsdealer"})) return {"armory"};
        if (contains_any(actor, {"doctor", "physician", "medic", "hospital"})) return {"hospital"};
        if (contains_any(actor, {"mechanic", "car", "vehicle"})) return {"carshop", "vehicle"};
        if (contains_any(actor, {"fisher", "boat"})) return {"fisherman", "boat"};
        if (contains_any(actor, {"barber"})) return {"barber"};
        if (contains_any(actor, {"barmen", "barman", "saloon", "bar"})) return {"saloon"};
        if (contains_any(actor, {"banker", "bank"})) return {"bank"};
        return {"outpost_trader", "trader_npcinteractionbox"};
    }

    auto trader_personality_kind_for_actor(const std::string& actor_class) -> std::string
    {
        const auto actor = lower_copy(actor_class);
        if (contains_any(actor, {"banker", "bank"})) return {};
        if (contains_any(actor, {"arm", "weapon", "gun", "armsdealer"})) return "Armory";
        if (contains_any(actor, {"doctor", "physician", "medic", "hospital"})) return "Doctor";
        if (contains_any(actor, {"mechanic", "car", "vehicle"})) return "Mechanic";
        if (contains_any(actor, {"fisher", "harbour", "harbor", "boat"})) return "Boat_Shop";
        if (contains_any(actor, {"barber"})) return "Barber";
        if (contains_any(actor, {"barmen", "barman", "bartender", "saloon", "bar"})) return "Barmen";
        if (contains_any(actor, {"general", "goods", "trader"})) return "TraderPersonality";
        return "TraderPersonality";
    }

    auto nearest_stock_outpost_key(double x, double y) -> const char*
    {
        struct OutpostCenter
        {
            const char* key;
            double x;
            double y;
        };

        const OutpostCenter centers[] = {
            {"C_2", -152584.15234375, 289693.0},
            {"B_4", 570831.0234375, -224794.244140625},
            {"Z_3", 21713.100341796875, -677039.1875},
            {"A_0", -621392.3671875, -557528.75},
        };

        const OutpostCenter* best = &centers[0];
        auto best_distance = std::numeric_limits<double>::max();
        for (const auto& center : centers)
        {
            const auto dx = x - center.x;
            const auto dy = y - center.y;
            const auto distance = (dx * dx) + (dy * dy);
            if (distance < best_distance)
            {
                best = &center;
                best_distance = distance;
            }
        }
        return best->key;
    }

    auto stock_trader_personality_path(const std::string& outpost_key, const std::string& personality_kind) -> std::string
    {
        if (outpost_key.empty() || personality_kind.empty()) return {};
        const auto asset_name = personality_kind == "TraderPersonality"
            ? ("Outpost_" + outpost_key + "_TraderPersonality_01")
            : ("Outpost_" + outpost_key + "_" + personality_kind + "_Personality_01");
        return "/Game/ConZ_Files/Economy/TraderPersonalities/Outpost_" + outpost_key + "/" + asset_name + "." + asset_name;
    }

    auto resolve_trader_personality_asset(
        const std::string& actor_class,
        const std::string& requested_path,
        double x,
        double y,
        std::string& selected_outpost_key,
        std::string& selected_kind,
        std::string& selected_path,
        std::vector<std::string>& attempts) -> RC::Unreal::UObject*
    {
        selected_kind = trader_personality_kind_for_actor(actor_class);
        if (selected_kind.empty())
        {
            attempts.push_back("trader personality skipped: actor is not an ATrader personality route");
            return nullptr;
        }

        const auto nearest_key = std::string(nearest_stock_outpost_key(x, y));
        selected_outpost_key = nearest_key;
        selected_path = requested_path.empty() ? stock_trader_personality_path(nearest_key, selected_kind) : requested_path;
        if (selected_path.empty())
        {
            attempts.push_back("trader personality path unresolved");
            return nullptr;
        }

        auto* personality = find_loaded_object_by_path_or_name(
            {L"TraderPersonalityDataAsset", L"Object", L"UObject", L"DataAsset"},
            selected_path,
            attempts);
        if (personality != nullptr) return personality;

        for (const auto* fallback_key : {"C_2", "B_4", "Z_3", "A_0"})
        {
            const auto fallback_path = stock_trader_personality_path(fallback_key, selected_kind);
            if (fallback_path == selected_path) continue;
            personality = find_loaded_object_by_path_or_name(
                {L"TraderPersonalityDataAsset", L"Object", L"UObject", L"DataAsset"},
                fallback_path,
                attempts);
            if (personality != nullptr)
            {
                selected_outpost_key = fallback_key;
                selected_path = fallback_path;
                return personality;
            }
        }

        return nullptr;
    }

    auto tradepost_matches_tokens(const std::string& full_name_lower, const std::vector<std::string>& tokens) -> bool
    {
        if (tokens.empty()) return false;
        for (const auto& token : tokens)
        {
            if (full_name_lower.find(token) != std::string::npos) return true;
        }
        return false;
    }

    auto tradepost_reference_distance_2d(RC::Unreal::UObject* object, double x, double y) -> double;

    struct ExactTradePostCandidate
    {
        const char* kind{};
        const wchar_t* class_name{};
        const wchar_t* object_path{};
    };

    const ExactTradePostCandidate exact_trade_post_candidates[] = {
        {"armory", L"BP_Outpost_Armory_NPCInteractionBox_C", L"/Game/ConZ_Files/Maps/The_Island/C_2_Outpost.C_2_Outpost:PersistentLevel.BP_Outpost_Armory_NPCInteractionBox_2"},
        {"armory", L"BP_Outpost_Armory_NPCInteractionBox_C", L"/Game/ConZ_Files/Maps/The_Island/B_4_Outpost.B_4_Outpost:PersistentLevel.BP_Outpost_Armory_NPCInteractionBox_2"},
        {"armory", L"BP_Outpost_Armory_NPCInteractionBox_C", L"/Game/ConZ_Files/Maps/The_Island/A_0_Outpost.A_0_Outpost:PersistentLevel.BP_Outpost_Armory_NPCInteractionBox_2"},
        {"armory", L"BP_Outpost_Armory_NPCInteractionBox_C", L"/Game/ConZ_Files/Maps/The_Island/Z_3_Outpost.Z_3_Outpost:PersistentLevel.BP_Outpost_Armory_NPCInteractionBox_2"},
        {"hospital", L"BP_Outpost_Hospital_NPC_InteractionBoxes_C", L"/Game/ConZ_Files/Maps/The_Island/C_2_Outpost.C_2_Outpost:PersistentLevel.BP_Outpost_Hospital_NPC_InterractionBoxes3_2"},
        {"hospital", L"BP_Outpost_Hospital_NPC_InteractionBoxes_C", L"/Game/ConZ_Files/Maps/The_Island/B_4_Outpost.B_4_Outpost:PersistentLevel.BP_Outpost_Hospital_NPC_InterractionBoxes2"},
        {"hospital", L"BP_Outpost_Hospital_NPC_InteractionBoxes_C", L"/Game/ConZ_Files/Maps/The_Island/A_0_Outpost.A_0_Outpost:PersistentLevel.BP_Outpost_Hospital_NPC_InterractionBoxes_2"},
        {"hospital", L"BP_Outpost_Hospital_NPC_InteractionBoxes_C", L"/Game/ConZ_Files/Maps/The_Island/Z_3_Outpost.Z_3_Outpost:PersistentLevel.BP_Outpost_Hospital_NPC_InterractionBoxes_2"},
        {"bank", L"BP_Outpost_Bank_open_NPC_InteractionBoxes_C", L"/Game/ConZ_Files/Maps/The_Island/C_2_Outpost.C_2_Outpost:PersistentLevel.BP_Outpost_Bank_open_NPC_InterractionBoxes2_2"},
        {"bank", L"BP_Outpost_Bank_open_NPC_InteractionBoxes_C", L"/Game/ConZ_Files/Maps/The_Island/B_4_Outpost.B_4_Outpost:PersistentLevel.BP_Outpost_Bank_open_NPC_InterractionBoxes_2"},
        {"bank", L"BP_Outpost_Bank_open_NPC_InteractionBoxes_C", L"/Game/ConZ_Files/Maps/The_Island/A_0_Outpost.A_0_Outpost:PersistentLevel.BP_Outpost_Bank_open_NPC_InterractionBoxes_2"},
        {"bank", L"BP_Outpost_Bank_open_NPC_InteractionBoxes_C", L"/Game/ConZ_Files/Maps/The_Island/Z_3_Outpost.Z_3_Outpost:PersistentLevel.BP_Outpost_Bank_open_NPC_InterractionBoxes3_2"},
        {"carshop", L"BP_Outpost_CarShop_NPC_and_VehicleSpawner_C", L"/Game/ConZ_Files/Maps/The_Island/C_2_Outpost.C_2_Outpost:PersistentLevel.BP_Outpost_CarShop_NPC_and_VehicleSpawner_2"},
        {"carshop", L"BP_Outpost_CarShop_NPC_and_VehicleSpawner_C", L"/Game/ConZ_Files/Maps/The_Island/B_4_Outpost.B_4_Outpost:PersistentLevel.BP_Outpost_CarShop_NPC_and_VehicleSpawner2_2"},
        {"carshop", L"BP_Outpost_CarShop_NPC_and_VehicleSpawner_C", L"/Game/ConZ_Files/Maps/The_Island/A_0_Outpost.A_0_Outpost:PersistentLevel.BP_Outpost_CarShop_NPC_and_VehicleSpawner_5"},
        {"carshop", L"BP_Outpost_CarShop_NPC_and_VehicleSpawner_C", L"/Game/ConZ_Files/Maps/The_Island/Z_3_Outpost.Z_3_Outpost:PersistentLevel.BP_Outpost_CarShop_NPC_and_VehicleSpawner_2"},
        {"fisherman", L"BP_FishermanTrader_NPC_InteractionBox_C", L"/Game/ConZ_Files/Maps/The_Island/C_2_Outpost.C_2_Outpost:PersistentLevel.BP_FishermanTrader_NPC_InterractionBox_2"},
        {"fisherman", L"BP_FishermanTrader_NPC_InteractionBox_C", L"/Game/ConZ_Files/Maps/The_Island/B_4_Outpost.B_4_Outpost:PersistentLevel.BP_FishermanTrader_NPC_InterractionBox_2"},
        {"fisherman", L"BP_FishermanTrader_NPC_InteractionBox_C", L"/Game/ConZ_Files/Maps/The_Island/A_0_Outpost.A_0_Outpost:PersistentLevel.BP_FishermanTrader_NPC_InterractionBox_2"},
        {"fisherman", L"BP_FishermanTrader_NPC_InteractionBox_C", L"/Game/ConZ_Files/Maps/The_Island/Z_3_Outpost.Z_3_Outpost:PersistentLevel.BP_FishermanTrader_NPC_InterractionBox2_2"},
        {"barber", L"BP_BarberShop_NPC_InteractionBox_C", L"/Game/ConZ_Files/Maps/The_Island/C_2_Outpost.C_2_Outpost:PersistentLevel.BP_BarberShop_NPC_InterractionBox_2"},
        {"barber", L"BP_BarberShop_NPC_InteractionBox_C", L"/Game/ConZ_Files/Maps/The_Island/B_4_Outpost.B_4_Outpost:PersistentLevel.BP_BarberShop_NPC_InterractionBox_2"},
        {"barber", L"BP_BarberShop_NPC_InteractionBox_C", L"/Game/ConZ_Files/Maps/The_Island/A_0_Outpost.A_0_Outpost:PersistentLevel.BP_BarberShop_NPC_InterractionBox_2"},
        {"barber", L"BP_BarberShop_NPC_InteractionBox_C", L"/Game/ConZ_Files/Maps/The_Island/Z_3_Outpost.Z_3_Outpost:PersistentLevel.BP_BarberShop_NPC_InterractionBox_2"},
        {"saloon", L"BP_SaloonOutpost_NPC_InteractionBoxes_C", L"/Game/ConZ_Files/Maps/The_Island/C_2_Outpost.C_2_Outpost:PersistentLevel.BP_SaloonOutpost_NPC_InterractionBoxe_2"},
        {"saloon", L"BP_SaloonOutpost_NPC_InteractionBoxes_C", L"/Game/ConZ_Files/Maps/The_Island/B_4_Outpost.B_4_Outpost:PersistentLevel.BP_SaloonOutpost_NPC_InterractionBoxe_2"},
        {"saloon", L"BP_SaloonOutpost_NPC_InteractionBoxes_C", L"/Game/ConZ_Files/Maps/The_Island/A_0_Outpost.A_0_Outpost:PersistentLevel.BP_SaloonOutpost_NPC_InterractionBoxe_2"},
        {"saloon", L"BP_SaloonOutpost_NPC_InteractionBoxes_C", L"/Game/ConZ_Files/Maps/The_Island/Z_3_Outpost.Z_3_Outpost:PersistentLevel.BP_SaloonOutpost_NPC_InterractionBoxe_2"},
        {"outpost_trader", L"BP_Outpost_Trader_NPCInteractionBox_C", L"/Game/ConZ_Files/Maps/The_Island/C_2_Outpost.C_2_Outpost:PersistentLevel.BP_Outpost_Trader_NPCInteractionBox_2"},
        {"outpost_trader", L"BP_Outpost_Trader_NPCInteractionBox_C", L"/Game/ConZ_Files/Maps/The_Island/B_4_Outpost.B_4_Outpost:PersistentLevel.BP_Outpost_Trader_NPCInteractionBox_2"},
        {"outpost_trader", L"BP_Outpost_Trader_NPCInteractionBox_C", L"/Game/ConZ_Files/Maps/The_Island/A_0_Outpost.A_0_Outpost:PersistentLevel.BP_Outpost_Trader_NPCInteractionBox_5"},
        {"outpost_trader", L"BP_Outpost_Trader_NPCInteractionBox_C", L"/Game/ConZ_Files/Maps/The_Island/Z_3_Outpost.Z_3_Outpost:PersistentLevel.BP_Outpost_Trader_NPCInteractionBox_2"},
    };

    auto resolve_trader_actor_class(const std::string& actor_class, const std::string& catalog_path, std::vector<std::string>& attempts) -> RC::Unreal::UClass*
    {
        if (!catalog_path.empty())
        {
            auto* object = find_loaded_object_by_path_or_name(
                {L"BlueprintGeneratedClass", L"Class", L"UClass"},
                catalog_path,
                attempts);
            if (looks_like_uclass(object)) return static_cast<RC::Unreal::UClass*>(object);
        }

        const auto wide_actor = widen(actor_class);
        if (!wide_actor.empty())
        {
            auto* object = RC::Unreal::UObjectGlobals::FindFirstOf(wide_actor.c_str());
            attempts.push_back("FindFirstOf(" + actor_class + ")=" + (object != nullptr ? full_name(object) : "0"));
            if (looks_like_uclass(object)) return static_cast<RC::Unreal::UClass*>(object);
        }

        return nullptr;
    }

    auto tradepost_has_trader_markers(RC::Unreal::UObject* object, std::vector<std::string>& attempts) -> bool
    {
        if (object == nullptr) return false;
        const auto full = full_name(object);
        if (full.find("Default__") != std::string::npos)
        {
            attempts.push_back("candidate skipped CDO/default object: " + full);
            return false;
        }

        const auto* markers = array_property(object, L"_traderMarkers");
        if (!is_reasonable_array(markers) || markers->count <= 0 || markers->data == nullptr)
        {
            attempts.push_back("candidate has no sane _traderMarkers: " + full);
            return false;
        }
        return true;
    }

    auto find_donor_tradepost_for_actor(
        const std::vector<std::string>& tokens,
        double x,
        double y,
        std::vector<std::string>& attempts,
        double& donor_distance) -> RC::Unreal::UObject*
    {
        RC::Unreal::UObject* best = nullptr;
        donor_distance = std::numeric_limits<double>::max();
        std::unordered_set<RC::Unreal::UObject*> seen_candidates;
        auto consider_candidate = [&](RC::Unreal::UObject* object, const std::string& route) -> void
        {
            if (object == nullptr || !seen_candidates.insert(object).second) return;
            attempts.push_back(route + ":" + full_name(object));
            if (!tradepost_has_trader_markers(object, attempts)) return;
            const auto distance = tradepost_reference_distance_2d(object, x, y);
            if (best == nullptr || distance < donor_distance)
            {
                best = object;
                donor_distance = distance;
            }
        };

        for (const auto& candidate : exact_trade_post_candidates)
        {
            const auto key = lower_copy(std::string(candidate.kind) + " " + narrow(candidate.class_name) + " " + narrow(candidate.object_path));
            if (!tradepost_matches_tokens(key, tokens)) continue;
            auto* object = RC::Unreal::UObjectGlobals::FindObject(candidate.class_name, candidate.object_path, 0, object_flag_class_default_object);
            attempts.push_back(std::string("FindObject(") + narrow(candidate.class_name) + "," + narrow(candidate.object_path) + ")=" + (object != nullptr ? "1" : "0"));
            if (object == nullptr) continue;
            consider_candidate(object, "exact-path");
        }

        std::unordered_set<std::wstring> searched_names;
        for (const auto& candidate : exact_trade_post_candidates)
        {
            const auto key = lower_copy(std::string(candidate.kind) + " " + narrow(candidate.class_name) + " " + narrow(candidate.object_path));
            if (!tradepost_matches_tokens(key, tokens)) continue;
            const auto object_name = widen(object_name_from_path(narrow(candidate.object_path)));
            const auto search_key = std::wstring(candidate.class_name) + L"\n" + object_name;
            if (object_name.empty() || !searched_names.insert(search_key).second) continue;

            std::vector<RC::Unreal::UObject*> found;
            RC::Unreal::UObjectGlobals::FindObjects(candidate.class_name, object_name.c_str(), found, 0, object_flag_class_default_object, false);
            attempts.push_back(std::string("FindObjects(") + narrow(candidate.class_name) + "," + narrow(object_name) + ")=" + std::to_string(found.size()));
            for (auto* object : found)
            {
                consider_candidate(object, "name-search");
            }
        }
        if (best != nullptr) return best;

        int extra_ref_candidates = 0;
        for (auto* object : collect_world_extra_referenced_objects(&attempts))
        {
            const auto name = lower_copy(full_name(object));
            if (!tradepost_matches_tokens(name, tokens)) continue;
            if (name.find("npcinteractionbox") == std::string::npos &&
                name.find("npc_interaction") == std::string::npos &&
                name.find("tradepost") == std::string::npos &&
                name.find("armory") == std::string::npos &&
                name.find("hospital") == std::string::npos &&
                name.find("carshop") == std::string::npos &&
                name.find("fisherman") == std::string::npos &&
                name.find("saloon") == std::string::npos &&
                name.find("barbershop") == std::string::npos &&
                name.find("outpost_trader") == std::string::npos)
            {
                continue;
            }
            ++extra_ref_candidates;
            consider_candidate(object, "extra-refs");
        }
        attempts.push_back("ExtraRefs.TradePostCandidates=" + std::to_string(extra_ref_candidates));
        if (best != nullptr) return best;

        int level_actor_candidates = 0;
        for (auto* actor : collect_loaded_outpost_level_actors(&attempts))
        {
            const auto name = lower_copy(full_name(actor));
            if (!tradepost_matches_tokens(name, tokens)) continue;
            if (name.find("npcinteractionbox") == std::string::npos &&
                name.find("npc_interaction") == std::string::npos &&
                name.find("armory") == std::string::npos &&
                name.find("hospital") == std::string::npos &&
                name.find("carshop") == std::string::npos &&
                name.find("fisherman") == std::string::npos &&
                name.find("saloon") == std::string::npos &&
                name.find("barbershop") == std::string::npos &&
                name.find("outpost_trader") == std::string::npos)
            {
                continue;
            }
            ++level_actor_candidates;
            consider_candidate(actor, "level-actors");
        }
        attempts.push_back("LevelActors.TradePostCandidates=" + std::to_string(level_actor_candidates));
        if (best != nullptr) return best;

        attempts.push_back("class-wide donor TradePost search skipped; exact/name/loaded-level actor search only");

        return best;
    }

    auto actor_distance_2d(RC::Unreal::UObject* object, double x, double y) -> double
    {
        if (object == nullptr) return std::numeric_limits<double>::max();
        auto* actor = static_cast<RC::Unreal::AActor*>(object);
        const auto location = actor->K2_GetActorLocation();
        const auto dx = static_cast<double>(location.x) - x;
        const auto dy = static_cast<double>(location.y) - y;
        return std::sqrt(dx * dx + dy * dy);
    }

    auto tradepost_reference_distance_2d(RC::Unreal::UObject* object, double x, double y) -> double
    {
        if (object == nullptr) return std::numeric_limits<double>::max();
        const auto key = outpost_key_from_name(lower_copy(full_name(object)));
        double center_x{};
        double center_y{};
        if (stock_outpost_center(key, center_x, center_y))
        {
            const auto dx = center_x - x;
            const auto dy = center_y - y;
            return std::sqrt(dx * dx + dy * dy);
        }

        return std::numeric_limits<double>::max();
    }

    auto normalize_tradepost_markers(RC::Unreal::UObject* trade_post, std::string& detail) -> int
    {
        if (trade_post == nullptr)
        {
            detail = "tradepost null";
            return 0;
        }

        auto* actor = static_cast<RC::Unreal::AActor*>(trade_post);
        const auto actor_location = actor->K2_GetActorLocation();
        int adjusted = 0;

        auto* trader_markers = const_cast<TArrayAbi*>(array_property(trade_post, L"_traderMarkers"));
        if (is_reasonable_array(trader_markers) && trader_markers->data != nullptr)
        {
            auto* markers = static_cast<FTraderMarkerAbi*>(trader_markers->data);
            for (std::int32_t index = 0; index < trader_markers->count; ++index)
            {
                if (normalize_local_transform_if_needed(markers[index].spawn_transform, actor_location)) ++adjusted;
                if (normalize_local_transform_if_needed(markers[index].purchased_tradeables_spawn_transform, actor_location)) ++adjusted;
                if (normalize_local_transform_if_needed(markers[index].depot_spawn_transform, actor_location)) ++adjusted;
            }
        }

        auto* location_markers = const_cast<TArrayAbi*>(array_property(trade_post, L"_locationMarkers"));
        if (is_reasonable_array(location_markers) && location_markers->data != nullptr)
        {
            auto* markers = static_cast<FTraderLocationMarkerAbi*>(location_markers->data);
            for (std::int32_t index = 0; index < location_markers->count; ++index)
            {
                if (normalize_local_transform_if_needed(markers[index].transform, actor_location)) ++adjusted;
            }
        }

        auto* sedentary_markers = const_cast<TArrayAbi*>(array_property(trade_post, L"_sedentaryNPCMarkers"));
        if (is_reasonable_array(sedentary_markers) && sedentary_markers->data != nullptr)
        {
            auto* markers = static_cast<FSedentaryNPCMarkerAbi*>(sedentary_markers->data);
            for (std::int32_t index = 0; index < sedentary_markers->count; ++index)
            {
                if (normalize_local_transform_if_needed(markers[index].spawn_transform, actor_location)) ++adjusted;
            }
        }

        detail = adjusted == 0 ? "marker transforms already look actor-local" : "converted probable world marker transforms to actor-local offsets";
        return adjusted;
    }
}

namespace RC::SCUMTraderManager
{
    SCUMTraderManager::SCUMTraderManager()
    {
        ModName = STR("SCUMTraderManager");
        ModVersion = STR("0.1.129-hektor-native-placeable-finalize-abi");
        ModDescription = STR("SCUM native trader lifecycle bridge");
        ModAuthors = STR("NeDjin");
        append_log(std::string("constructor loaded version=") + mod_version);
    }

    SCUMTraderManager::~SCUMTraderManager()
    {
        append_log("destructor unloaded");
    }

    auto SCUMTraderManager::base_dir() const -> fs::path
    {
        wchar_t buffer[MAX_PATH]{};
        const DWORD size = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (size == 0 || size >= MAX_PATH) return fs::current_path();
        return fs::path(buffer).parent_path();
    }

    auto SCUMTraderManager::log_path() const -> fs::path
    {
        return base_dir() / L"ScumNeDjin" / L"logs" / L"SCUMTraderManager.cppmod.log";
    }

    auto SCUMTraderManager::command_path() const -> fs::path
    {
        const auto saved_path = base_dir().parent_path().parent_path() / L"Saved" / L"ScumNeDjin" / L"state" / L"native-trader-command.json";
        if (fs::exists(saved_path)) return saved_path;
        return base_dir() / L"ScumNeDjin" / L"state" / L"native-trader-command.json";
    }

    auto SCUMTraderManager::inline_command_path() const -> fs::path
    {
        const auto saved_path = base_dir().parent_path().parent_path() / L"Saved" / L"ScumNeDjin" / L"state" / L"native-trader-inline-command.json";
        if (fs::exists(saved_path)) return saved_path;
        return base_dir() / L"ScumNeDjin" / L"state" / L"native-trader-inline-command.json";
    }

    auto SCUMTraderManager::result_path() const -> fs::path
    {
        return base_dir().parent_path().parent_path() / L"Saved" / L"ScumNeDjin" / L"state" / L"native-trader-result.json";
    }

    auto SCUMTraderManager::inline_result_path() const -> fs::path
    {
        return base_dir().parent_path().parent_path() / L"Saved" / L"ScumNeDjin" / L"state" / L"native-trader-inline-result.json";
    }

    auto SCUMTraderManager::append_log(const std::string& line) const -> void
    {
        const auto path = log_path();
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream file(path, std::ios::app | std::ios::binary);
        if (!file) return;
        file << utc_now() << " " << line << "\n";
    }

    auto SCUMTraderManager::write_result(const std::string& id, bool ok, const std::string& message, const std::string& data_json) const -> void
    {
        const auto path = m_result_path_override.empty() ? result_path() : m_result_path_override;
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) return;
        file << "{\"id\":\"" << json_escape(id) << "\","
             << "\"ok\":" << (ok ? "true" : "false") << ","
             << "\"message\":\"" << json_escape(message) << "\","
             << "\"version\":\"" << mod_version << "\","
             << "\"unrealInitialized\":" << (m_unreal_initialized ? "true" : "false") << ","
             << "\"cppmodsLoaded\":" << (m_cppmods_loaded ? "true" : "false") << ","
             << "\"data\":" << (data_json.empty() ? "{}" : data_json) << "}";
    }

    auto SCUMTraderManager::on_program_start() -> void
    {
        append_log("on_program_start");
    }

    auto SCUMTraderManager::on_unreal_init() -> void
    {
        m_unreal_initialized = true;
        append_log("on_unreal_init");
    }

    auto SCUMTraderManager::on_ui_init() -> void
    {
        append_log("on_ui_init");
    }

    auto SCUMTraderManager::on_cpp_mods_loaded() -> void
    {
        m_cppmods_loaded = true;
        append_log("on_cpp_mods_loaded");
    }

    auto SCUMTraderManager::on_dll_load(StringViewType dll_name) -> void
    {
        append_log(std::string("on_dll_load ") + narrow(std::wstring(dll_name)));
    }

    auto SCUMTraderManager::tick_from_lua_host() -> void
    {
        if (!m_unreal_initialized)
        {
            m_unreal_initialized = true;
            append_log("lua_host_unreal_initialized_assumed");
        }
        if (!m_cppmods_loaded)
        {
            m_cppmods_loaded = true;
            append_log("lua_host_cppmods_loaded_assumed");
        }
        poll_command_file();
    }

    auto SCUMTraderManager::tick_inline_from_lua_host() -> void
    {
        if (!m_unreal_initialized)
        {
            m_unreal_initialized = true;
            append_log("lua_inline_host_unreal_initialized_assumed");
        }
        if (!m_cppmods_loaded)
        {
            m_cppmods_loaded = true;
            append_log("lua_inline_host_cppmods_loaded_assumed");
        }
        poll_inline_command_file();
    }

    auto SCUMTraderManager::pin_loaded_outpost_runtime(const std::string& reason) -> std::pair<bool, std::string>
    {
        const auto started = std::chrono::steady_clock::now();
        std::vector<std::string> manager_route_counts;
        auto managers = find_trade_outpost_managers(&manager_route_counts);

        auto* economy_manager = Unreal::UObjectGlobals::FindFirstOf(L"BP_EconomyManager_C");
        if (economy_manager == nullptr)
        {
            economy_manager = Unreal::UObjectGlobals::FindFirstOf(L"ConZEconomyManager");
        }

        Unreal::UWorld* world = economy_manager != nullptr ? economy_manager->GetWorld() : nullptr;
        if (world == nullptr)
        {
            for (auto* manager : managers)
            {
                if (manager == nullptr) continue;
                world = manager->GetWorld();
                if (world != nullptr) break;
            }
        }

        auto* extra_refs = const_cast<TArrayAbi*>(array_property(world, L"ExtraReferencedObjects"));
        const auto* streaming_levels = array_property(world, L"StreamingLevels");
        const auto extra_before = is_reasonable_array(extra_refs) ? extra_refs->count : -1;

        std::vector<Unreal::UObject*> refs;
        std::unordered_set<Unreal::UObject*> seen_refs;
        std::vector<std::string> ref_samples;
        std::vector<std::string> level_samples;
        std::vector<std::string> flag_details;

        auto remember_ref = [&](Unreal::UObject* object) -> bool
        {
            if (object == nullptr) return false;
            if (!seen_refs.insert(object).second) return false;
            refs.push_back(object);
            if (ref_samples.size() < 24)
            {
                ref_samples.push_back(full_name(object));
            }
            return true;
        };

        auto remember_object_array = [&](const TArrayAbi* array, std::int32_t max_items) -> int
        {
            if (!is_reasonable_array(array) || array->data == nullptr || array->count <= 0) return 0;
            auto** items = static_cast<Unreal::UObject**>(array->data);
            const auto count = std::min<std::int32_t>(array->count, max_items);
            int added = 0;
            for (std::int32_t index = 0; index < count; ++index)
            {
                if (remember_ref(items[index])) ++added;
            }
            return added;
        };

        auto remember_tradepost_graph = [&](Unreal::UObject* trade_post) -> void
        {
            if (trade_post == nullptr) return;
            remember_ref(trade_post);
            remember_object_array(array_property(trade_post, L"_spawnedTraders"), 64);
            remember_object_array(array_property(trade_post, L"_spawnedSedentaryNPCs"), 64);
            remember_object_array(array_property(trade_post, L"_spawnedDepots"), 64);

            const auto* markers = array_property(trade_post, L"_traderMarkers");
            if (is_reasonable_array(markers) && markers->data != nullptr)
            {
                auto* marker_items = static_cast<FTraderMarkerAbi*>(markers->data);
                const auto count = std::min<std::int32_t>(markers->count, 32);
                for (std::int32_t index = 0; index < count; ++index)
                {
                    remember_ref(marker_items[index].trader_personality);
                }
            }
        };

        auto set_streaming_bool_bit = [&](Unreal::UObject* object, const wchar_t* property_name, std::uint8_t mask, bool desired) -> bool
        {
            if (object == nullptr) return false;
            auto* value = static_cast<std::uint8_t*>(object->GetValuePtrByPropertyNameInChain(property_name));
            if (value == nullptr)
            {
                if (flag_details.size() < 24)
                {
                    flag_details.push_back(narrow(property_name) + "=missing on " + full_name(object));
                }
                return false;
            }
            const auto before = *value;
            if (desired)
            {
                *value = static_cast<std::uint8_t>(*value | mask);
            }
            else
            {
                *value = static_cast<std::uint8_t>(*value & ~mask);
            }
            const auto after = *value;
            if (flag_details.size() < 24)
            {
                std::ostringstream detail;
                detail << narrow(property_name)
                       << " mask=0x" << std::hex << static_cast<int>(mask)
                       << std::dec << " before=" << static_cast<int>(before)
                       << " after=" << static_cast<int>(after)
                       << " object=" << full_name(object);
                flag_details.push_back(detail.str());
            }
            return desired ? ((after & mask) != 0) : ((after & mask) == 0);
        };

        remember_ref(economy_manager);
        if (auto* component = object_property(economy_manager, L"_traderManagingComponent"))
        {
            remember_ref(component);
        }
        remember_ref(world);

        for (auto* manager : managers)
        {
            remember_ref(manager);
            remember_ref(object_property(manager, L"_outpostDescription"));

            const auto* assigned = array_property(manager, L"_assignedTradePosts");
            if (is_reasonable_array(assigned) && assigned->data != nullptr)
            {
                auto** items = static_cast<Unreal::UObject**>(assigned->data);
                const auto count = std::min<std::int32_t>(assigned->count, 64);
                for (std::int32_t index = 0; index < count; ++index)
                {
                    remember_tradepost_graph(items[index]);
                }
            }

            const auto* other = array_property(manager, L"_otherAssignedTradeOutpostBuildings");
            if (is_reasonable_array(other) && other->data != nullptr)
            {
                auto** items = static_cast<Unreal::UObject**>(other->data);
                const auto count = std::min<std::int32_t>(other->count, 64);
                for (std::int32_t index = 0; index < count; ++index)
                {
                    remember_tradepost_graph(items[index]);
                }
            }
        }

        int matched_streaming_levels = 0;
        if (is_reasonable_array(streaming_levels) && streaming_levels->data != nullptr)
        {
            auto** levels = static_cast<Unreal::UObject**>(streaming_levels->data);
            const auto count = std::min<std::int32_t>(streaming_levels->count, 4096);
            for (std::int32_t index = 0; index < count; ++index)
            {
                auto* level_streaming = levels[index];
                if (level_streaming == nullptr) continue;
                auto* loaded_level = object_property(level_streaming, L"LoadedLevel");
                auto* pending_unload_level = object_property(level_streaming, L"PendingUnloadLevel");
                const auto combined = lower_copy(full_name(level_streaming) + " " + full_name(loaded_level) + " " + full_name(pending_unload_level));
                if (!outpost_streaming_name_matches(combined)) continue;

                ++matched_streaming_levels;
                remember_ref(level_streaming);
                remember_ref(loaded_level);
                remember_ref(pending_unload_level);
                set_streaming_bool_bit(level_streaming, L"bDisableDistanceStreaming", 0x02, true);
                set_streaming_bool_bit(level_streaming, L"bShouldBeLoaded", 0x10, true);
                set_streaming_bool_bit(level_streaming, L"bShouldBeVisible", 0x08, true);
                if (level_samples.size() < 24)
                {
                    level_samples.push_back(full_name(level_streaming) + " loaded=" + full_name(loaded_level));
                }
            }
        }

        int appended = 0;
        int already = 0;
        int failed = 0;
        std::vector<std::string> add_details;
        if (extra_refs != nullptr)
        {
            for (auto* object : refs)
            {
                std::string detail;
                if (add_object_to_array(extra_refs, object, detail))
                {
                    if (detail.find("already registered") != std::string::npos)
                    {
                        ++already;
                    }
                    else
                    {
                        ++appended;
                    }
                }
                else
                {
                    ++failed;
                }

                if (add_details.size() < 24)
                {
                    add_details.push_back(full_name(object) + " => " + detail);
                }
            }
        }
        else
        {
            failed = static_cast<int>(refs.size());
        }

        const auto extra_after = is_reasonable_array(extra_refs) ? extra_refs->count : -1;
        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        const auto ok = managers.size() >= 4 && world != nullptr && extra_refs != nullptr && failed == 0 && (appended + already) > 0;

        m_outpost_runtime_pinned = ok;
        m_outpost_pin_object_count = static_cast<int>(refs.size());

        std::ostringstream ss;
        ss << "{\"ok\":" << (ok ? "true" : "false") << ","
           << "\"route\":\"pin-loaded-outpost-runtime\","
           << "\"reason\":\"" << json_escape(reason) << "\","
           << "\"managerCount\":" << managers.size() << ","
           << "\"world\":\"" << json_escape(full_name(world)) << "\","
           << "\"economyManager\":\"" << json_escape(full_name(economy_manager)) << "\","
           << "\"extraReferencedObjects\":{\"before\":" << extra_before
           << ",\"after\":" << extra_after
           << ",\"queued\":" << refs.size()
           << ",\"appended\":" << appended
           << ",\"already\":" << already
           << ",\"failed\":" << failed << "},"
           << "\"streamingLevels\":{\"total\":" << (is_reasonable_array(streaming_levels) ? streaming_levels->count : -1)
           << ",\"matchedOutposts\":" << matched_streaming_levels
           << ",\"samples\":";
        append_json_string_array(ss, level_samples);
        ss << "},\"managerSearch\":";
        append_json_string_array(ss, manager_route_counts);
        ss << ",\"referenceSamples\":";
        append_json_string_array(ss, ref_samples);
        ss << ",\"addDetails\":";
        append_json_string_array(ss, add_details);
        ss << ",\"flagDetails\":";
        append_json_string_array(ss, flag_details);
        ss << ",\"durationMs\":" << duration_ms << "}";

        m_last_outpost_pin_result = ss.str();
        return {ok, m_last_outpost_pin_result};
    }

    auto SCUMTraderManager::maybe_pin_loaded_outpost_runtime() -> void
    {
        if (!m_unreal_initialized || m_outpost_runtime_pinned) return;
        if (m_outpost_pin_attempts >= 240) return;

        const auto now = std::chrono::steady_clock::now();
        if (m_next_outpost_pin_attempt.time_since_epoch().count() != 0 && now < m_next_outpost_pin_attempt) return;

        ++m_outpost_pin_attempts;
        m_next_outpost_pin_attempt = now + std::chrono::seconds(1);
        const auto [ok, data] = pin_loaded_outpost_runtime("startup-auto");
        if (ok)
        {
            append_log("pin_loaded_outpost_runtime ok attempts=" + std::to_string(m_outpost_pin_attempts) + " refs=" + std::to_string(m_outpost_pin_object_count) + " data=" + data.substr(0, 1800));
        }
        else if (m_outpost_pin_attempts <= 5 || m_outpost_pin_attempts % 15 == 0)
        {
            append_log("pin_loaded_outpost_runtime waiting attempts=" + std::to_string(m_outpost_pin_attempts) + " data=" + data.substr(0, 1800));
        }
    }

    auto SCUMTraderManager::maybe_refresh_manager_cache_fast() -> void
    {
        // Live-server safety: manager discovery is command-scoped only. Background UObject
        // scans stayed active after startup and crashed the dedicated server in 0.1.86.
        if (!m_unreal_initialized || m_manager_cache_refresh_attempts != 0) return;
        ++m_manager_cache_refresh_attempts;
        append_log("manager_cache_fast_refresh disabled; using command-scoped manager lookup only");
    }

    auto SCUMTraderManager::on_update() -> void
    {
        const auto now = std::chrono::steady_clock::now();
        if (m_last_poll.time_since_epoch().count() == 0 || now - m_last_poll >= std::chrono::milliseconds(500))
        {
            m_last_poll = now;
            poll_command_file();
        }
    }

    auto SCUMTraderManager::check_spawned_tradepost_monitors() -> void
    {
        if (m_spawned_tradepost_monitors.empty()) return;

        const auto now = std::chrono::steady_clock::now();
        auto it = m_spawned_tradepost_monitors.begin();
        while (it != m_spawned_tradepost_monitors.end())
        {
            const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->created_at).count();
            if (it->trade_post == nullptr || age > 180)
            {
                it = m_spawned_tradepost_monitors.erase(it);
                continue;
            }

            const auto* spawned_traders = array_property(it->trade_post, L"_spawnedTraders");
            const auto* spawned_sedentary = array_property(it->trade_post, L"_spawnedSedentaryNPCs");
            const auto trader_count = is_reasonable_array(spawned_traders) ? spawned_traders->count : -1;
            const auto sedentary_count = is_reasonable_array(spawned_sedentary) ? spawned_sedentary->count : -1;
            if (trader_count != it->last_spawned_traders || sedentary_count != it->last_spawned_sedentary)
            {
                std::ostringstream ss;
                ss << "monitor actorClass=" << it->actor_class
                   << " ageSec=" << age
                   << " spawnedTraders=" << trader_count
                   << " spawnedSedentary=" << sedentary_count
                   << " tradePost=" << it->full_name;
                append_log(ss.str());
                it->last_spawned_traders = trader_count;
                it->last_spawned_sedentary = sedentary_count;
            }

            if (trader_count > 0 || sedentary_count > 0)
            {
                it = m_spawned_tradepost_monitors.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    auto SCUMTraderManager::poll_command_file() -> void
    {
        const auto path = command_path();
        std::ifstream file(path, std::ios::binary);
        if (!file) return;
        std::ostringstream ss;
        ss << file.rdbuf();
        const auto text = ss.str();
        file.close();
        if (text.empty()) return;
        const auto id = regex_value(text, "id");
        if (!id.empty() && id == m_last_command_id) return;
        m_last_command_id = id;
        std::error_code ec;
        fs::remove(path, ec);
        handle_command(text);
    }

    auto SCUMTraderManager::poll_inline_command_file() -> void
    {
        const auto path = inline_command_path();
        std::ifstream file(path, std::ios::binary);
        if (!file) return;
        std::ostringstream ss;
        ss << file.rdbuf();
        const auto text = ss.str();
        file.close();
        if (text.empty()) return;
        const auto id = regex_value(text, "id");
        if (!id.empty() && id == m_last_inline_command_id) return;
        m_last_inline_command_id = id;
        std::error_code ec;
        fs::remove(path, ec);

        const auto previous_path = m_result_path_override;
        const auto previous_inline = m_inline_game_thread_command;
        m_result_path_override = inline_result_path();
        m_inline_game_thread_command = true;
        handle_command(text);
        m_inline_game_thread_command = previous_inline;
        m_result_path_override = previous_path;
    }

    auto SCUMTraderManager::handle_command(const std::string& text) -> void
    {
        const auto id = regex_value(text, "id");
        const auto command = regex_value(text, "command");
        append_log(std::string("command id=") + id + " command=" + command);

        if (command == "ping" || command == "native_trader_ping")
        {
            write_result(id, true, "SCUMTraderManager loaded", "{\"capability\":\"abi-shim-command-bridge\"}");
            return;
        }

        if (command == "cfcore_native_status_probe" || command == "debug_cfcore_native_status_probe")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto [probe_ok, data] = cfcore_native_status_probe();
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("cfcore_native_status_probe complete id=") + id + " ok=" + (probe_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            write_result(id, probe_ok, probe_ok ? "CFCore native status probe ok" : "CFCore native status probe incomplete", data);
            return;
        }

        if (command == "cfcore_editor_settings_probe" || command == "debug_cfcore_editor_settings_probe")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto [probe_ok, data] = cfcore_editor_settings_probe();
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("cfcore_editor_settings_probe complete id=") + id + " ok=" + (probe_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            write_result(id, probe_ok, probe_ok ? "CFCore editor settings probe ok" : "CFCore editor settings probe incomplete", data);
            return;
        }

        if (command == "cfcore_configure_server_runtime" || command == "debug_cfcore_configure_server_runtime")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto [probe_ok, data] = cfcore_configure_server_runtime(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("cfcore_configure_server_runtime complete id=") + id + " ok=" + (probe_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            write_result(id, probe_ok, probe_ok ? "CFCore server runtime configure ok" : "CFCore server runtime configure failed", data);
            return;
        }

        if (command == "cfcore_api_mods_summary_probe" || command == "debug_cfcore_api_mods_summary_probe")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto [probe_ok, data] = cfcore_api_mods_summary_probe(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("cfcore_api_mods_summary_probe complete id=") + id + " ok=" + (probe_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            write_result(id, probe_ok, probe_ok ? "CFCore API mods summary probe ok" : "CFCore API mods summary probe failed", data);
            return;
        }

        if (command == "cfcore_api_files_summary_probe" || command == "debug_cfcore_api_files_summary_probe")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto [probe_ok, data] = cfcore_api_files_summary_probe(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("cfcore_api_files_summary_probe complete id=") + id + " ok=" + (probe_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            write_result(id, probe_ok, probe_ok ? "CFCore API files summary probe ok" : "CFCore API files summary probe failed", data);
            return;
        }

        if (command == "cfcore_server_mod_sync_probe" || command == "debug_cfcore_server_mod_sync_probe")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto [probe_ok, data] = cfcore_server_mod_sync_probe(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("cfcore_server_mod_sync_probe complete id=") + id + " ok=" + (probe_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            write_result(id, probe_ok, probe_ok ? "CFCore server mod sync probe ok" : "CFCore server mod sync probe failed", data);
            return;
        }

        if (command == "cfcore_client_file_sync_probe" || command == "debug_cfcore_client_file_sync_probe")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto [probe_ok, data] = cfcore_client_file_sync_probe(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("cfcore_client_file_sync_probe complete id=") + id + " ok=" + (probe_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            write_result(id, probe_ok, probe_ok ? "CFCore client file sync probe ok" : "CFCore client file sync probe failed", data);
            return;
        }

        if (command == "raw_byte_stream_probe" || command == "debug_raw_byte_stream_probe")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto [probe_ok, data] = raw_byte_stream_probe(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("raw_byte_stream_probe complete id=") + id + " ok=" + (probe_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            write_result(id, probe_ok, probe_ok ? "Raw byte stream probe ok" : "Raw byte stream probe incomplete", data);
            return;
        }

        if (command == "scum_mod_manager_probe" || command == "debug_scum_mod_manager_probe")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto [probe_ok, data] = scum_mod_manager_probe();
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("scum_mod_manager_probe complete id=") + id + " ok=" + (probe_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            write_result(id, probe_ok, probe_ok ? "SCUM mod manager probe ok" : "SCUM mod manager probe incomplete", data);
            return;
        }

        if (command == "world_edit_server_only_probe" || command == "server_only_world_edit_probe" || command == "world_edit_runtime_probe")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto data = world_edit_server_only_probe();
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("world_edit_server_only_probe complete id=") + id + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            write_result(id, true, "World edit server-only read-only probe", data);
            return;
        }

        if (command == "native_actor_editor_probe" || command == "editor_actor_probe")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto [probe_ok, data] = editor_actor_probe(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("native_actor_editor_probe complete id=") + id + " ok=" + (probe_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            write_result(id, probe_ok, probe_ok ? "Actor editor probe ok" : "Actor editor probe failed", data);
            return;
        }

        if (command == "native_actor_editor_target_probe" || command == "editor_actor_target_probe" || command == "hektor_target_probe")
        {
            const auto data = std::string("{\"ok\":false,\"blocked\":true,\"stage\":\"hektor-target-probe-disabled\",\"route\":\"native_actor_editor_target_probe\",\"message\":\"Blocked after live SCUMServer crash in 0.1.99. Do not call reflected prisoner target getters on the dedicated server; use a proven client/network hook or offline SDK route instead.\",\"version\":\"") + mod_version + "\"}";
            append_log(std::string("native_actor_editor_target_probe blocked id=") + id + " version=" + mod_version);
            write_result(id, false, "Actor editor target probe blocked after live crash", data);
            return;
        }

        if (command == "native_actor_editor_select" || command == "editor_actor_select")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto [select_ok, data] = editor_actor_select(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("native_actor_editor_select complete id=") + id + " ok=" + (select_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            if (!select_ok)
            {
                append_log(std::string("native_actor_editor_select failureData id=") + id + " data=" + data.substr(0, 1800));
            }
            write_result(id, select_ok, select_ok ? "Actor editor select ok" : "Actor editor select failed", data);
            return;
        }

        if (command == "native_actor_editor_trace_select" || command == "editor_actor_trace_select")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto [select_ok, data] = editor_actor_trace_select(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("native_actor_editor_trace_select complete id=") + id + " ok=" + (select_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            if (!select_ok)
            {
                append_log(std::string("native_actor_editor_trace_select failureData id=") + id + " data=" + data.substr(0, 1800));
            }
            write_result(id, select_ok, select_ok ? "Actor editor trace select ok" : "Actor editor trace select failed", data);
            return;
        }

        if (command == "native_actor_editor_move" || command == "editor_actor_move")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto [move_ok, data] = editor_actor_move(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("native_actor_editor_move complete id=") + id + " ok=" + (move_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            if (!move_ok)
            {
                append_log(std::string("native_actor_editor_move failureData id=") + id + " data=" + data.substr(0, 1800));
            }
            write_result(id, move_ok, move_ok ? "Actor editor move applied" : "Actor editor move failed", data);
            return;
        }

        if (command == "native_actor_editor_nudge" || command == "editor_actor_nudge")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto [nudge_ok, data] = editor_actor_nudge(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("native_actor_editor_nudge complete id=") + id + " ok=" + (nudge_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            if (!nudge_ok)
            {
                append_log(std::string("native_actor_editor_nudge failureData id=") + id + " data=" + data.substr(0, 1800));
            }
            write_result(id, nudge_ok, nudge_ok ? "Actor editor nudge applied" : "Actor editor nudge failed", data);
            return;
        }

        if (command == "native_actor_editor_copy" || command == "editor_actor_copy")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto [copy_ok, data] = editor_actor_copy(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("native_actor_editor_copy complete id=") + id + " ok=" + (copy_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            if (!copy_ok)
            {
                append_log(std::string("native_actor_editor_copy failureData id=") + id + " data=" + data.substr(0, 1800));
            }
            write_result(id, copy_ok, copy_ok ? "Actor editor copy applied" : "Actor editor copy failed", data);
            return;
        }

        if (command == "native_actor_editor_hide" || command == "editor_actor_hide" || command == "native_actor_editor_remove" || command == "editor_actor_remove")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto [hide_ok, data] = editor_actor_hide(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("native_actor_editor_hide complete id=") + id + " ok=" + (hide_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            if (!hide_ok)
            {
                append_log(std::string("native_actor_editor_hide failureData id=") + id + " data=" + data.substr(0, 1800));
            }
            write_result(id, hide_ok, hide_ok ? "Actor editor hide applied" : "Actor editor hide failed", data);
            return;
        }

        if (command == "native_actor_spawn_persistent" || command == "server_actor_spawn_persistent" || command == "persistent_actor_spawn")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto [spawn_ok, data] = server_actor_spawn_persistent(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("native_actor_spawn_persistent complete id=") + id + " ok=" + (spawn_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            if (!spawn_ok)
            {
                append_log(std::string("native_actor_spawn_persistent failureData id=") + id + " data=" + data.substr(0, 1800));
            }
            write_result(id, spawn_ok, spawn_ok ? "Persistent server actor spawned" : "Persistent server actor spawn failed", data);
            return;
        }

        if (command == "native_placeable_server_place" || command == "placeable_server_place")
        {
            if (!m_inline_game_thread_command)
            {
                std::ostringstream data;
                data << "{\"ok\":false,"
                     << "\"stage\":\"blocked-unsafe-placeable-server-place-thread\","
                     << "\"route\":\"native-placeable-server-place\","
                     << "\"message\":\"native_placeable_server_place is blocked outside the explicit inline game-thread route.\","
                     << "\"version\":\"" << mod_version << "\"}";
                append_log(std::string("native_placeable_server_place blocked unsafe async host id=") + id);
                write_result(id, false, "Native placeable Server_Place blocked outside inline game-thread route", data.str());
                return;
            }
            const auto started = std::chrono::steady_clock::now();
            const auto [place_ok, data] = placeable_server_place_native(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("native_placeable_server_place complete id=") + id + " ok=" + (place_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            if (!place_ok)
            {
                append_log(std::string("native_placeable_server_place failureData id=") + id + " data=" + data.substr(0, 1800));
            }
            write_result(id, place_ok, place_ok ? "Native Placeable Server_Place dispatched" : "Native Placeable Server_Place failed", data);
            return;
        }

        if (command == "native_placeable_godmode_fill" || command == "placeable_godmode_fill")
        {
            if (!m_inline_game_thread_command)
            {
                std::ostringstream data;
                data << "{\"ok\":false,"
                     << "\"stage\":\"blocked-unsafe-placeable-godmode-fill-thread\","
                     << "\"route\":\"native-placeable-godmode-fill\","
                     << "\"message\":\"native_placeable_godmode_fill is blocked outside the explicit inline game-thread route.\","
                     << "\"version\":\"" << mod_version << "\"}";
                append_log(std::string("native_placeable_godmode_fill blocked unsafe async host id=") + id);
                write_result(id, false, "Native placeable GodModeFill blocked outside inline game-thread route", data.str());
                return;
            }
            const auto started = std::chrono::steady_clock::now();
            const auto [fill_ok, data] = placeable_godmode_fill_native(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("native_placeable_godmode_fill complete id=") + id + " ok=" + (fill_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            if (!fill_ok)
            {
                append_log(std::string("native_placeable_godmode_fill failureData id=") + id + " data=" + data.substr(0, 1800));
            }
            write_result(id, fill_ok, fill_ok ? "Native Placeable GodModeFill dispatched" : "Native Placeable GodModeFill failed", data);
            return;
        }

        if (command == "base_loot_store_item" || command == "base_loot_native_store_item")
        {
            if (!m_inline_game_thread_command)
            {
                std::ostringstream data;
                data << "{\"ok\":false,"
                     << "\"stage\":\"blocked-unsafe-base-loot-store-thread\","
                     << "\"route\":\"native-base-loot-store-item\","
                     << "\"message\":\"base_loot_store_item is blocked outside the explicit inline game-thread route.\","
                     << "\"version\":\"" << mod_version << "\"}";
                append_log(std::string("base_loot_store_item blocked unsafe async host id=") + id);
                write_result(id, false, "Native base loot Store blocked outside inline game-thread route", data.str());
                return;
            }
            const auto started = std::chrono::steady_clock::now();
            const auto [store_ok, data] = base_loot_store_item_native(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("base_loot_store_item complete id=") + id + " ok=" + (store_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            if (!store_ok)
            {
                append_log(std::string("base_loot_store_item failureData id=") + id + " data=" + data.substr(0, 1800));
            }
            write_result(id, store_ok, store_ok ? "Native base loot Store dispatched" : "Native base loot Store failed", data);
            return;
        }

        if (command == "base_loot_first_reflected_property_probe" || command == "base_loot_nameable_reflected_probe")
        {
            if (!m_inline_game_thread_command)
            {
                std::ostringstream data;
                data << "{\"ok\":false,"
                     << "\"stage\":\"blocked-unsafe-base-loot-reflected-probe-thread\","
                     << "\"route\":\"base-loot-first-reflected-property-probe\","
                     << "\"message\":\"base_loot_first_reflected_property_probe is blocked outside the explicit inline game-thread route.\","
                     << "\"version\":\"" << mod_version << "\"}";
                append_log(std::string("base_loot_first_reflected_property_probe blocked unsafe async host id=") + id);
                write_result(id, false, "Base loot reflected property probe blocked outside inline game-thread route", data.str());
                return;
            }
            const auto started = std::chrono::steady_clock::now();
            const auto [probe_ok, data] = base_loot_first_reflected_property_probe(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("base_loot_first_reflected_property_probe complete id=") + id + " ok=" + (probe_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            if (!probe_ok)
            {
                append_log(std::string("base_loot_first_reflected_property_probe failureData id=") + id + " data=" + data.substr(0, 1800));
            }
            write_result(id, probe_ok, probe_ok ? "Base loot reflected property probe ok" : "Base loot reflected property probe failed", data);
            return;
        }

        if (command == "native_trader_status")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto pin_ok = m_outpost_runtime_pinned;
            const auto pin_data = m_last_outpost_pin_result.empty()
                ? std::string("{\"ok\":false,\"stage\":\"not-run\",\"message\":\"Auto pin disabled in safe static research build; use explicit pin_outpost_runtime only during a maintenance window.\"}")
                : m_last_outpost_pin_result;
            std::ostringstream data_stream;
            data_stream << "{\"probe\":\"safe-status\","
                        << "\"engineAccess\":\"cached native mod state only; no UObject lookup, no manager discovery, no actor scans\","
                        << "\"deepStatusBlocked\":true,"
                        << "\"deepStatusReason\":\"native_trader_status manager discovery caused 19-20s GameThread hangs on 2026-06-05; use explicit maintenance probes only when the server is empty.\","
                        << "\"outpostPin\":{\"ok\":" << (pin_ok ? "true" : "false")
                        << ",\"attempts\":" << m_outpost_pin_attempts
                        << ",\"pinned\":" << (m_outpost_runtime_pinned ? "true" : "false")
                        << ",\"objectCount\":" << m_outpost_pin_object_count
                        << ",\"last\":" << (pin_data.empty() ? "{}" : pin_data) << "},"
                        << "\"economyManager\":\"not-probed-in-safe-status\","
                        << "\"outposts\":[]}";
            const auto data = data_stream.str();
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("native_trader_status complete id=") + id + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            write_result(id, true, "SCUMTraderManager runtime status", data);
            return;
        }

        if (command == "pin_outpost_runtime" || command == "armory_pin_outpost_runtime")
        {
            const auto started = std::chrono::steady_clock::now();
            ++m_outpost_pin_attempts;
            const auto [pin_ok, data] = pin_loaded_outpost_runtime("explicit-command");
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("pin_outpost_runtime command complete id=") + id + " ok=" + (pin_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " data=" + data.substr(0, 1800));
            write_result(id, pin_ok, pin_ok ? "Loaded outpost runtime pinned" : "Loaded outpost runtime pin failed", data);
            return;
        }

        if (command == "armory_runtime_probe" || command == "native_armory_runtime_probe")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto data = armory_runtime_probe();
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("armory_runtime_probe complete id=") + id + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            write_result(id, true, "Armory runtime probe", data);
            return;
        }

        if (command == "armory_trade_session_probe" || command == "native_armory_trade_session_probe" || command == "debug_armory_trade_session_probe")
        {
            const auto data = std::string("{\"ok\":false,\"blocked\":true,\"probe\":\"armory-trade-session-readonly\",")
                + "\"version\":\"" + mod_version + "\","
                + "\"reason\":\"armory_trade_session_probe 0.1.74 terminated SCUMServer during live response/depot class scans at 2026-06-02T10:54Z; disabling this route until a no-scan exact object handle path exists.\","
                + "\"mutates\":false,"
                + "\"safeOnline\":true,"
                + "\"engineAccess\":\"blocked before native object scans; no ProcessEvent; no serializer deref; no array or map writes\"}";
            append_log(std::string("armory_trade_session_probe blocked by crashguard id=") + id);
            write_result(id, false, "Armory trade session probe blocked by crash guard", data);
            return;
        }

        if (command == "armory_identity_probe" || command == "native_armory_identity_probe")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto data = armory_identity_probe(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("armory_identity_probe complete id=") + id + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            write_result(id, true, "Armory identity probe", data);
            return;
        }

        if (command == "verify_armory_linked_trader" || command == "armory_verify_linked_trader")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto [verify_ok, data] = verify_armory_linked_trader(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("verify_armory_linked_trader complete id=") + id + " ok=" + (verify_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            if (!verify_ok)
            {
                append_log(std::string("verify_armory_linked_trader failureData id=") + id + " data=" + data.substr(0, 1800));
            }
            write_result(
                id,
                verify_ok,
                verify_ok ? "Armory linked trader graph verified" : "Armory linked trader graph verification failed",
                data);
            return;
        }

        if (command == "link_existing_summoned_trader" || command == "armory_link_existing_summoned_trader")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto [link_ok, data] = link_existing_summoned_trader(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("link_existing_summoned_trader complete id=") + id + " ok=" + (link_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            if (!link_ok)
            {
                append_log(std::string("link_existing_summoned_trader failureData id=") + id + " data=" + data.substr(0, 1800));
            }
            write_result(
                id,
                link_ok,
                link_ok ? "Existing summoned Armory trader linked to stock TradePost/personality" : "Existing summoned Armory trader link failed",
                data);
            return;
        }

        if (command == "armory_deep_probe" || command == "trader_rpc_introspection")
        {
            const auto data = std::string("{\"ok\":false,\"blocked\":true,\"stage\":\"armory-deep-probe-disabled\",\"route\":\"safe-live-armory\",\"message\":\"Broad GUObjectArray armory_deep_probe is disabled on the live SCUM server because it can stall the native command handler. Use narrow status/runtime probes and static dumps instead.\",\"version\":\"") + mod_version + "\"}";
            append_log(std::string("armory_deep_probe blocked id=") + id);
            write_result(id, false, "Armory deep runtime probe is disabled on live server", data);
            return;
        }

        if (command == "armory_anywhere_prepare")
        {
            const auto started = std::chrono::steady_clock::now();
            const auto [prepare_ok, data] = prepare_armory_anywhere(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("armory_anywhere_prepare complete id=") + id + " ok=" + (prepare_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            if (!prepare_ok)
            {
                append_log(std::string("armory_anywhere_prepare failureData id=") + id + " data=" + data);
            }
            write_result(
                id,
                prepare_ok,
                prepare_ok
                    ? "Armory anywhere runtime prepared through stock economy/trader relevancy route"
                    : "Armory anywhere runtime prepare failed",
                data);
            return;
        }

        if (command == "armory_open_tradebuy" || command == "armory_native_tradebuy")
        {
            if (!m_inline_game_thread_command)
            {
                std::ostringstream data;
                data << "{\"ok\":false,"
                     << "\"stage\":\"blocked-unsafe-tradebuy-thread\","
                     << "\"route\":\"native-prisoner-client-interact-tradebuy\","
                     << "\"message\":\"armory_open_tradebuy is blocked from the async native file host because ProcessEvent/Client_Interact can hang or crash SCUM when it is not executed by the explicit inline game-thread route.\","
                     << "\"version\":\"" << mod_version << "\"}";
                append_log(std::string("armory_open_tradebuy blocked unsafe async host id=") + id);
                write_result(id, false, "Native TradeBuy blocked outside inline game-thread route", data.str());
                return;
            }
            const auto started = std::chrono::steady_clock::now();
            const auto [open_ok, data] = open_armory_tradebuy_native(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("armory_open_tradebuy complete id=") + id + " ok=" + (open_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            if (!open_ok)
            {
                append_log(std::string("armory_open_tradebuy failureData id=") + id + " data=" + data.substr(0, 1800));
            }
            write_result(
                id,
                open_ok,
                open_ok ? "Native Prisoner.Client_Interact TradeBuy dispatched" : "Native TradeBuy dispatch failed",
                data);
            return;
        }

        if (command == "armory_target_interact" || command == "armory_native_target_interact")
        {
            if (!m_inline_game_thread_command)
            {
                std::ostringstream data;
                data << "{\"ok\":false,"
                     << "\"stage\":\"blocked-unsafe-target-interact-thread\","
                     << "\"route\":\"native-target-interactable-interface-interact\","
                     << "\"message\":\"armory_target_interact is blocked outside the explicit inline game-thread route.\","
                     << "\"version\":\"" << mod_version << "\"}";
                append_log(std::string("armory_target_interact blocked unsafe async host id=") + id);
                write_result(id, false, "Native target Interact blocked outside inline game-thread route", data.str());
                return;
            }
            const auto started = std::chrono::steady_clock::now();
            const auto [interact_ok, data] = target_armory_interact_native(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("armory_target_interact complete id=") + id + " ok=" + (interact_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            if (!interact_ok)
            {
                append_log(std::string("armory_target_interact failureData id=") + id + " data=" + data.substr(0, 1800));
            }
            write_result(
                id,
                interact_ok,
                interact_ok ? "Native target Interact dispatched" : "Native target Interact failed",
                data);
            return;
        }

        if (command == "armory_server_tradebuy" || command == "armory_native_server_tradebuy")
        {
            if (!m_inline_game_thread_command)
            {
                std::ostringstream data;
                data << "{\"ok\":false,"
                     << "\"stage\":\"blocked-unsafe-server-tradebuy-thread\","
                     << "\"route\":\"native-rpc-server-interact-tradebuy\","
                     << "\"message\":\"armory_server_tradebuy is blocked outside the explicit inline game-thread route.\","
                     << "\"version\":\"" << mod_version << "\"}";
                append_log(std::string("armory_server_tradebuy blocked unsafe async host id=") + id);
                write_result(id, false, "Native server TradeBuy blocked outside inline game-thread route", data.str());
                return;
            }
            const auto started = std::chrono::steady_clock::now();
            const auto [server_ok, data] = server_armory_tradebuy_native(text);
            const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            append_log(std::string("armory_server_tradebuy complete id=") + id + " ok=" + (server_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
            if (!server_ok)
            {
                append_log(std::string("armory_server_tradebuy failureData id=") + id + " data=" + data.substr(0, 1800));
            }
            write_result(
                id,
                server_ok,
                server_ok ? "Native PlayerRpcChannel.InteractWithObjectOnServer TradeBuy dispatched" : "Native server TradeBuy dispatch failed",
                data);
            return;
        }

        if (command == "native_trader_probe" || command == "probe_trade_runtime")
        {
            std::ostringstream data;
            data << "{\"ok\":false,"
                 << "\"stage\":\"runtime-probe-blocked\","
                 << "\"route\":\"blocked-after-live-requestexit\","
                 << "\"message\":\"native_trader_probe is blocked in this build because the previous broad runtime probe made SCUMServer request exit on 2026-05-26. Use native_trader_status or a future narrow probe tied to a single known object/property.\"}";
            append_log(std::string("native_trader_probe blocked id=") + id);
            write_result(id, false, "Trader runtime probe blocked after live RequestExit", data.str());
            return;
        }

        if (command == "spawn_functional_trader")
        {
            const auto mode = lower_copy(regex_value(text, "nativeMode") + regex_value(text, "mode"));
            if (mode.find("blocked") == std::string::npos && mode.find("dry-run") == std::string::npos)
            {
                const auto use_linked_actor_route =
                    mode.find("linked") != std::string::npos ||
                    mode.find("actor") != std::string::npos ||
                    mode.find("personality") != std::string::npos;
                const auto started = std::chrono::steady_clock::now();
                const auto [spawn_ok, data] = use_linked_actor_route
                    ? spawn_linked_trader_actor(text)
                    : spawn_functional_tradepost_clone(text);
                const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
                append_log(std::string("spawn_functional_trader complete id=") + id + " route=" + (use_linked_actor_route ? "linked-actor" : "registered-tradepost") + " ok=" + (spawn_ok ? "true" : "false") + " durationMs=" + std::to_string(duration_ms) + " dataBytes=" + std::to_string(data.size()));
                if (!spawn_ok)
                {
                    append_log(std::string("spawn_functional_trader failureData id=") + id + " data=" + data);
                }
                write_result(
                    id,
                    spawn_ok,
                    spawn_ok
                        ? (use_linked_actor_route ? "Native linked trader actor spawned; still requires real client stock/sell proof" : "Native registered TradePost spawned; waiting for SCUM trader lifecycle/client trade UI proof")
                        : (use_linked_actor_route ? "Native linked trader actor route failed" : "Native registered TradePost route failed"),
                    data);
                return;
            }

            std::ostringstream data;
            data << "{\"ok\":false,"
                 << "\"stage\":\"native-lifecycle-not-implemented\","
                 << "\"route\":\"blocked-until-real-new-trader-spawn-lifecycle\","
                 << "\"actorClass\":\"" << json_escape(regex_value(text, "actorClass")) << "\","
                 << "\"message\":\"Teleporting an existing TradePost and spawning/cloning an unregistered NPC are not valid solutions. A new functional trader requires the real SCUM spawn + economy lifecycle path.\"}";
            write_result(
                id,
                false,
                "Functional trader spawn blocked until the real SCUM trader/economy lifecycle route is implemented",
                data.str());
            return;
        }

        write_result(id, false, std::string("unknown native trader command: ") + command);
    }

    auto SCUMTraderManager::editor_actor_probe(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        std::vector<std::string> attempts;
        std::string requested_handle;
        auto* actor = find_editor_actor_from_command(command_text, attempts, requested_handle);
        if (actor == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"server-actor-editor\","
               << "\"stage\":\"resolve-actor\","
               << "\"requestedHandle\":\"" << json_escape(requested_handle) << "\","
               << "\"message\":\"Exact loaded actor not found. Pass actorFullName or objectPath from a loaded streamed level/client selection.\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        const auto actor_full_name = editor_actor_submitted_full_name(command_text, requested_handle, static_cast<Unreal::UObject*>(actor));
        const auto actor_class_full_name = editor_actor_submitted_class_name(command_text, static_cast<Unreal::UObject*>(actor));
        const auto block_reason = editor_actor_block_reason(actor_full_name, actor_class_full_name);
        Unreal::FVector location{};
        Unreal::FRotator rotation{};
        const auto location_ok = actor_location_noexcept(static_cast<Unreal::UObject*>(actor), location);
        const auto rotation_ok = actor_rotation_noexcept(static_cast<Unreal::UObject*>(actor), rotation);
        auto* root_component = object_property(static_cast<Unreal::UObject*>(actor), L"RootComponent");

        std::ostringstream ss;
        ss << "{\"ok\":" << (location_ok ? "true" : "false") << ","
           << "\"route\":\"server-actor-editor\","
           << "\"operation\":\"probe\","
           << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
           << "\"actorClass\":\"" << json_escape(actor_class_full_name) << "\","
           << "\"rootComponent\":\"" << json_escape(full_name(root_component)) << "\","
           << "\"movementBlocked\":" << (!block_reason.empty() ? "true" : "false") << ","
           << "\"blockReason\":\"" << json_escape(block_reason) << "\","
           << "\"locationOk\":" << (location_ok ? "true" : "false") << ","
           << "\"rotationOk\":" << (rotation_ok ? "true" : "false") << ","
           << "\"location\":";
        append_vector_json(ss, location);
        ss << ",\"rotation\":";
        append_rotator_json(ss, rotation);
        ss << ",\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << "}";
        return {location_ok, ss.str()};
    }

    auto append_editor_target_object_json(
        std::ostringstream& ss,
        const char* source,
        RC::Unreal::UObject* object) -> bool
    {
        if (object == nullptr)
        {
            ss << "{\"source\":\"" << json_escape(source == nullptr ? "" : source) << "\","
               << "\"object\":\"\","
               << "\"class\":\"\","
               << "\"locationOk\":false,"
               << "\"rotationOk\":false,"
               << "\"selectable\":false,"
               << "\"blockReason\":\"null\","
               << "\"location\":{\"x\":0,\"y\":0,\"z\":0},"
               << "\"rotation\":{\"pitch\":0,\"yaw\":0,\"roll\":0}}";
            return false;
        }
        const auto object_full_name = safe_full_name(object);
        const auto object_class = object_class_full_name_noexcept(object);
        RC::Unreal::FVector location{};
        RC::Unreal::FRotator rotation{};
        const auto location_ok = actor_location_noexcept(object, location);
        const auto rotation_ok = actor_rotation_noexcept(object, rotation);
        const auto block_reason = editor_actor_selection_block_reason(object_full_name, object_class);
        const auto selectable = object != nullptr && location_ok && block_reason.empty();

        ss << "{\"source\":\"" << json_escape(source == nullptr ? "" : source) << "\","
           << "\"object\":\"" << json_escape(object_full_name) << "\","
           << "\"class\":\"" << json_escape(object_class) << "\","
           << "\"locationOk\":" << (location_ok ? "true" : "false") << ","
           << "\"rotationOk\":" << (rotation_ok ? "true" : "false") << ","
           << "\"selectable\":" << (selectable ? "true" : "false") << ","
           << "\"blockReason\":\"" << json_escape(block_reason) << "\","
           << "\"location\":";
        append_vector_json(ss, location);
        ss << ",\"rotation\":";
        append_rotator_json(ss, rotation);
        ss << "}";
        return selectable;
    }

    auto resolve_editor_target_prisoner_from_command(
        const std::string& command_text,
        std::vector<std::string>& attempts,
        std::string& requested_handle) -> RC::Unreal::UObject*
    {
        requested_handle = first_regex_value(
            command_text,
            {"pawnFullName", "prisonerFullName", "playerPawnFullName", "playerFullName"});
        if (!requested_handle.empty())
        {
            auto* object = find_object_by_full_name(
                {L"BP_Prisoner_C", L"Prisoner", L"APrisoner", L"Actor", L"Object"},
                requested_handle,
                attempts,
                "targetProbePawn");
            if (object != nullptr) return object;
            attempts.push_back("targetProbePawn exact handle unresolved");
        }

        std::vector<std::string> routes;
        auto candidates = find_all_unique({L"BP_Prisoner_C", L"Prisoner", L"APrisoner"}, &routes);
        for (const auto& route : routes)
        {
            attempts.push_back("targetProbePawn." + route);
        }

        for (auto* object : candidates)
        {
            const auto object_name = lower_copy(safe_full_name(object));
            if (object_name.empty()) continue;
            if (object_name.find("default__") != std::string::npos) continue;
            if (object_name.rfind("class ", 0) == 0) continue;
            RC::Unreal::FVector location{};
            if (actor_location_noexcept(object, location))
            {
                requested_handle = safe_full_name(object);
                attempts.push_back("targetProbePawn fallback selected " + requested_handle);
                return object;
            }
        }

        return nullptr;
    }

    auto SCUMTraderManager::editor_actor_target_probe(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        std::vector<std::string> attempts;
        std::string requested_handle;
        auto* prisoner = resolve_editor_target_prisoner_from_command(command_text, attempts, requested_handle);
        if (prisoner == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"hektor-target-probe\","
               << "\"stage\":\"resolve-prisoner\","
               << "\"requestedHandle\":\"" << json_escape(requested_handle) << "\","
               << "\"message\":\"No live prisoner pawn was resolved for target probe.\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << ",\"version\":\"" << mod_version << "\"}";
            return {false, ss.str()};
        }

        const auto prisoner_full_name = safe_full_name(prisoner);
        const auto prisoner_class = object_class_full_name_noexcept(prisoner);
        std::vector<std::string> details;
        auto* controller_from_command = find_object_by_full_name(
            {L"BP_ConZPlayerController_C", L"ConZPlayerController", L"PlayerController", L"Controller", L"Object"},
            first_regex_value(command_text, {"controllerFullName", "playerControllerFullName"}),
            attempts,
            "targetProbeController");
        auto* controller_property = object_property_noexcept(prisoner, L"Controller", details);
        auto* interaction_component = object_property_noexcept(prisoner, L"InteractionComponent", details);
        auto* base_interaction_component = object_property_noexcept(prisoner, L"BaseInteractionComponent", details);
        auto* eyes_target = object_property_noexcept(prisoner, L"EyesLookAtTargetOverride", details);
        auto* root_component = object_property_noexcept(prisoner, L"RootComponent", details);

        auto* rotation_target = call_object_return_function(
            prisoner,
            "Prisoner",
            "GetRotationTarget",
            {
                "/Script/SCUM.Prisoner:GetRotationTarget",
                "Function /Script/SCUM.Prisoner:GetRotationTarget",
                "/Script/SCUM.Prisoner.GetRotationTarget",
                "Function /Script/SCUM.Prisoner.GetRotationTarget",
            },
            "Prisoner.GetRotationTarget",
            details);
        auto* melee_target = call_object_return_function(
            prisoner,
            "Prisoner",
            "GetMeleeTarget",
            {
                "/Script/SCUM.Prisoner:GetMeleeTarget",
                "Function /Script/SCUM.Prisoner:GetMeleeTarget",
                "/Script/SCUM.Prisoner.GetMeleeTarget",
                "Function /Script/SCUM.Prisoner.GetMeleeTarget",
            },
            "Prisoner.GetMeleeTarget",
            details);

        std::ostringstream candidates;
        bool first_candidate = true;
        bool any_selectable = false;
        auto append_candidate = [&](const char* source, RC::Unreal::UObject* object)
        {
            if (!first_candidate) candidates << ",";
            first_candidate = false;
            if (append_editor_target_object_json(candidates, source, object))
            {
                any_selectable = true;
            }
        };
        append_candidate("controllerFromCommand", controller_from_command);
        append_candidate("controllerProperty", controller_property);
        append_candidate("interactionComponent", interaction_component);
        append_candidate("baseInteractionComponent", base_interaction_component);
        append_candidate("eyesLookAtTargetOverride", eyes_target);
        append_candidate("rootComponent", root_component);
        append_candidate("getRotationTarget", rotation_target);
        append_candidate("getMeleeTarget", melee_target);

        RC::Unreal::FVector prisoner_location{};
        RC::Unreal::FRotator prisoner_rotation{};
        const auto prisoner_location_ok = actor_location_noexcept(prisoner, prisoner_location);
        const auto prisoner_rotation_ok = actor_rotation_noexcept(prisoner, prisoner_rotation);

        std::ostringstream ss;
        ss << "{\"ok\":true,"
           << "\"route\":\"hektor-target-probe\","
           << "\"operation\":\"target-probe\","
           << "\"message\":\"Read-only player target state captured; no trace, no interaction dispatch, no transform mutation.\","
           << "\"requestedHandle\":\"" << json_escape(requested_handle) << "\","
           << "\"prisoner\":\"" << json_escape(prisoner_full_name) << "\","
           << "\"prisonerClass\":\"" << json_escape(prisoner_class) << "\","
           << "\"prisonerLocationOk\":" << (prisoner_location_ok ? "true" : "false") << ","
           << "\"prisonerRotationOk\":" << (prisoner_rotation_ok ? "true" : "false") << ","
           << "\"prisonerLocation\":";
        append_vector_json(ss, prisoner_location);
        ss << ",\"prisonerRotation\":";
        append_rotator_json(ss, prisoner_rotation);
        ss << ",\"selectableTargetFound\":" << (any_selectable ? "true" : "false")
           << ",\"candidates\":[" << candidates.str() << "],"
           << "\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << ",\"details\":";
        append_json_string_array(ss, details);
        ss << ",\"version\":\"" << mod_version << "\"}";
        return {true, ss.str()};
    }

    auto SCUMTraderManager::editor_actor_trace_select(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        std::vector<std::string> attempts;
        std::string requested_handle;
        (void)find_editor_actor_from_command(command_text, attempts, requested_handle);

        std::ostringstream ss;
        ss << "{\"ok\":false,"
           << "\"route\":\"hektor-server-actor-editor\","
           << "\"operation\":\"trace-select\","
           << "\"blocked\":true,"
           << "\"stage\":\"server-target-selection-disabled\","
           << "\"requestedHandle\":\"" << json_escape(requested_handle) << "\","
           << "\"message\":\"Server-side aim trace/level actor selection is disabled after live SCUM crashes. Capture the exact actor on the client through a proven game event and pass actorFullName/objectPath to move/nudge/copy/hide.\","
           << "\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << ",\"version\":\"" << mod_version << "\"}";
        return {false, ss.str()};
    }

    auto SCUMTraderManager::editor_actor_select(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        std::vector<std::string> attempts;
        std::string requested_handle;
        (void)find_editor_actor_from_command(command_text, attempts, requested_handle);
        std::ostringstream ss;
        ss << "{\"ok\":false,"
           << "\"route\":\"hektor-server-actor-editor\","
           << "\"operation\":\"select\","
           << "\"blocked\":true,"
           << "\"stage\":\"server-target-selection-disabled\","
           << "\"requestedHandle\":\"" << json_escape(requested_handle) << "\","
           << "\"message\":\"Server-side selection is disabled after Kismet trace, raw ULevel scan, and reflected target-read crashes. Use client event target capture and pass actorFullName/objectPath to exact-handle operations.\","
           << "\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << ",\"version\":\"" << mod_version << "\"}";
        return {false, ss.str()};
    }

    auto editor_actor_mutation_enabled(const std::string& command_text) -> bool
    {
        return regex_bool(command_text, "allowUnsafeEditorActorMutation", false) ||
            regex_bool(command_text, "allowUnsafeEditorActorServerMutation", false);
    }

    auto editor_actor_mutation_disabled_json(const std::string& operation) -> std::string
    {
        std::ostringstream ss;
        ss << "{\"ok\":false,"
           << "\"blocked\":true,"
           << "\"route\":\"hektor-server-actor-editor\","
           << "\"operation\":\"" << json_escape(operation) << "\","
           << "\"stage\":\"actor-editor-server-mutation-disabled\","
           << "\"message\":\"Actor editor server mutation is disabled after the 2026-06-05 14:31Z SCUMServer crash. Do not move/copy/hide streamed map actors until a safe SCUM lifecycle route is proven offline.\","
           << "\"enableFlag\":\"allowUnsafeEditorActorMutation\","
           << "\"version\":\"" << mod_version << "\"}";
        return ss.str();
    }

    auto SCUMTraderManager::editor_actor_move(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        constexpr double max_move_distance_cm = 10000.0;
        if (!editor_actor_mutation_enabled(command_text))
        {
            return {false, editor_actor_mutation_disabled_json("move")};
        }

        std::vector<std::string> attempts;
        std::string requested_handle;
        auto* actor = find_editor_actor_from_command(command_text, attempts, requested_handle);
        if (actor == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"server-actor-editor\","
               << "\"stage\":\"resolve-actor\","
               << "\"requestedHandle\":\"" << json_escape(requested_handle) << "\","
               << "\"message\":\"Exact loaded actor not found. Move requires actorFullName/objectPath of a loaded server actor.\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        const auto actor_full_name = editor_actor_submitted_full_name(command_text, requested_handle, static_cast<Unreal::UObject*>(actor));
        const auto actor_class_full_name = editor_actor_submitted_class_name(command_text, static_cast<Unreal::UObject*>(actor));
        const auto block_reason = editor_actor_block_reason(actor_full_name, actor_class_full_name);
        if (!block_reason.empty())
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"server-actor-editor\","
               << "\"stage\":\"blocked-actor-category\","
               << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
               << "\"actorClass\":\"" << json_escape(actor_class_full_name) << "\","
               << "\"blockReason\":\"" << json_escape(block_reason) << "\","
               << "\"message\":\"Actor editor refuses traders, NPCs, players, items, controllers and economy/tradepost objects.\"}";
            return {false, ss.str()};
        }

        Unreal::FVector before_location{};
        Unreal::FRotator before_rotation{};
        auto before_location_ok = actor_location_noexcept(static_cast<Unreal::UObject*>(actor), before_location);
        auto before_rotation_ok = actor_rotation_noexcept(static_cast<Unreal::UObject*>(actor), before_rotation);
        if (!before_location_ok)
        {
            before_location_ok = cached_editor_actor_transform_from_command(
                command_text,
                requested_handle,
                actor,
                before_location,
                before_rotation,
                before_rotation_ok,
                attempts);
        }
        if (!before_location_ok)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"server-actor-editor\","
               << "\"stage\":\"read-before-transform\","
               << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
               << "\"actorClass\":\"" << json_escape(actor_class_full_name) << "\","
               << "\"message\":\"K2_GetActorLocation failed before move.\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        double value{};
        auto desired_location = before_location;
        if (first_regex_number(command_text, {"x", "X", "worldX", "WorldX"}, value)) desired_location.x = static_cast<float>(value);
        if (first_regex_number(command_text, {"y", "Y", "worldY", "WorldY"}, value)) desired_location.y = static_cast<float>(value);
        if (first_regex_number(command_text, {"z", "Z", "worldZ", "WorldZ"}, value)) desired_location.z = static_cast<float>(value);
        if (first_regex_number(command_text, {"dx", "deltaX", "offsetX"}, value)) desired_location.x += static_cast<float>(value);
        if (first_regex_number(command_text, {"dy", "deltaY", "offsetY"}, value)) desired_location.y += static_cast<float>(value);
        if (first_regex_number(command_text, {"dz", "deltaZ", "offsetZ"}, value)) desired_location.z += static_cast<float>(value);

        auto desired_rotation = before_rotation_ok ? before_rotation : Unreal::FRotator{};
        if (first_regex_number(command_text, {"pitch", "Pitch"}, value)) desired_rotation.pitch = static_cast<float>(value);
        if (first_regex_number(command_text, {"yaw", "Yaw", "rotationYaw", "RotationYaw"}, value)) desired_rotation.yaw = static_cast<float>(normalize_yaw(value));
        if (first_regex_number(command_text, {"roll", "Roll"}, value)) desired_rotation.roll = static_cast<float>(value);
        if (first_regex_number(command_text, {"yawDelta", "deltaYaw", "dyaw"}, value)) desired_rotation.yaw = static_cast<float>(normalize_yaw(static_cast<double>(desired_rotation.yaw) + value));

        const auto dry_run = regex_bool(command_text, "dryRun", false) || regex_bool(command_text, "dry", false);
        const auto allow_large_move = regex_bool(command_text, "allowLargeMove", false);
        const auto confirm = first_regex_value(command_text, {"confirm", "Confirm"});
        const auto requested_distance = vector_distance_3d(before_location, desired_location);
        if (!allow_large_move && requested_distance > max_move_distance_cm)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"server-actor-editor\","
               << "\"stage\":\"blocked-large-move\","
               << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
               << "\"distanceCm\":" << requested_distance << ","
               << "\"maxDistanceCm\":" << max_move_distance_cm << ","
               << "\"message\":\"Move distance is above per-command safety limit. Split it into smaller steps or pass allowLargeMove:true during maintenance.\"}";
            return {false, ss.str()};
        }

        if (!dry_run && confirm != "MOVE_WORLD_ACTOR")
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"server-actor-editor\","
               << "\"stage\":\"missing-confirmation\","
               << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
               << "\"message\":\"Set confirm to MOVE_WORLD_ACTOR or dryRun:true before moving a world actor.\"}";
            return {false, ss.str()};
        }

        std::vector<std::string> details;
        auto actual_location = before_location;
        auto actual_rotation = before_rotation;
        if (!dry_run)
        {
            std::ostringstream stage;
            stage << "native_actor_editor_nudge stage=before-teleport actor=" << actor_full_name
                  << " before=(" << before_location.x << "," << before_location.y << "," << before_location.z << ")"
                  << " desired=(" << desired_location.x << "," << desired_location.y << "," << desired_location.z << ")"
                  << " distanceCm=" << requested_distance
                  << " version=" << mod_version;
            append_log(stage.str());
        }
        const auto applied = dry_run
            ? true
            : editor_set_actor_location_and_rotation(actor, desired_location, desired_rotation, actual_location, actual_rotation, details);
        if (!dry_run)
        {
            std::ostringstream stage;
            stage << "native_actor_editor_nudge stage=after-teleport actor=" << actor_full_name
                  << " applied=" << (applied ? "true" : "false")
                  << " actual=(" << actual_location.x << "," << actual_location.y << "," << actual_location.z << ")"
                  << " details=" << (details.empty() ? "" : details.back());
            append_log(stage.str());
        }
        if (applied && !dry_run)
        {
            wake_actor_for_editor_transform(actor, static_cast<Unreal::UObject*>(actor), details);
            update_editor_actor_identity_cache_transform(command_text, requested_handle, actor, actual_location, actual_rotation, true);
        }

        std::ostringstream ss;
        ss << "{\"ok\":" << (applied ? "true" : "false") << ","
           << "\"route\":\"server-actor-editor\","
           << "\"operation\":\"move\","
           << "\"dryRun\":" << (dry_run ? "true" : "false") << ","
           << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
           << "\"actorClass\":\"" << json_escape(actor_class_full_name) << "\","
           << "\"confirm\":\"" << json_escape(confirm) << "\","
           << "\"distanceCm\":" << requested_distance << ","
           << "\"beforeLocation\":";
        append_vector_json(ss, before_location);
        ss << ",\"requestedLocation\":";
        append_vector_json(ss, desired_location);
        ss << ",\"afterLocation\":";
        append_vector_json(ss, actual_location);
        ss << ",\"beforeRotation\":";
        append_rotator_json(ss, before_rotation);
        ss << ",\"requestedRotation\":";
        append_rotator_json(ss, desired_rotation);
        ss << ",\"afterRotation\":";
        append_rotator_json(ss, actual_rotation);
        ss << ",\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << ",\"details\":";
        append_json_string_array(ss, details);
        ss << "}";
        return {applied, ss.str()};
    }

    auto SCUMTraderManager::editor_actor_nudge(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        constexpr double max_nudge_distance_cm = 2000.0;
        if (!editor_actor_mutation_enabled(command_text))
        {
            return {false, editor_actor_mutation_disabled_json("nudge")};
        }

        std::vector<std::string> attempts;
        Unreal::FVector player_location{};
        double forward_x{};
        double forward_y{};
        double player_yaw{};
        const auto has_player_context = read_editor_player_context_from_command(command_text, player_location, forward_x, forward_y, player_yaw, attempts);

        std::string requested_handle;
        auto* actor = find_editor_actor_from_command(command_text, attempts, requested_handle);
        if (actor == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"hektor-server-actor-editor\","
               << "\"operation\":\"nudge\","
               << "\"stage\":\"resolve-actor\","
               << "\"requestedHandle\":\"" << json_escape(requested_handle) << "\","
               << "\"message\":\"Nudge requires a server pointer, exact loaded actor path, or strict client identity match. Server-side aim trace, raw level scan, and target getter fallbacks remain disabled after live crashes.\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << ",\"version\":\"" << mod_version << "\"";
            ss << "}";
            return {false, ss.str()};
        }

        const auto actor_full_name = editor_actor_submitted_full_name(command_text, requested_handle, static_cast<Unreal::UObject*>(actor));
        const auto actor_class_full_name = editor_actor_submitted_class_name(command_text, static_cast<Unreal::UObject*>(actor));
        const auto block_reason = editor_actor_block_reason(actor_full_name, actor_class_full_name);
        if (!block_reason.empty())
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"hektor-server-actor-editor\","
               << "\"operation\":\"nudge\","
               << "\"stage\":\"blocked-actor-category\","
               << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
               << "\"actorClass\":\"" << json_escape(actor_class_full_name) << "\","
               << "\"blockReason\":\"" << json_escape(block_reason) << "\","
               << "\"message\":\"Actor editor refuses traders, NPCs, players, items, controllers and economy/tradepost objects.\"}";
            return {false, ss.str()};
        }

        Unreal::FVector before_location{};
        Unreal::FRotator before_rotation{};
        auto before_location_ok = actor_location_noexcept(static_cast<Unreal::UObject*>(actor), before_location);
        auto before_rotation_ok = actor_rotation_noexcept(static_cast<Unreal::UObject*>(actor), before_rotation);
        if (!before_location_ok)
        {
            before_location_ok = cached_editor_actor_transform_from_command(
                command_text,
                requested_handle,
                actor,
                before_location,
                before_rotation,
                before_rotation_ok,
                attempts);
        }
        if (!before_location_ok)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"hektor-server-actor-editor\","
               << "\"operation\":\"nudge\","
               << "\"stage\":\"read-before-transform\","
               << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
               << "\"actorClass\":\"" << json_escape(actor_class_full_name) << "\","
               << "\"message\":\"K2_GetActorLocation failed before nudge.\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        double value{};
        double dx = 0.0;
        double dy = 0.0;
        double dz = 0.0;
        if (first_regex_number(command_text, {"dx", "deltaX", "offsetX"}, value)) dx += value;
        if (first_regex_number(command_text, {"dy", "deltaY", "offsetY"}, value)) dy += value;
        if (first_regex_number(command_text, {"dz", "deltaZ", "offsetZ"}, value)) dz += value;
        if (first_regex_number(command_text, {"upCm", "up", "verticalCm"}, value)) dz += value;

        double forward_cm = 0.0;
        first_regex_number(command_text, {"forwardCm", "forward", "localForwardCm"}, forward_cm);
        double right_cm = 0.0;
        first_regex_number(command_text, {"rightCm", "right", "localRightCm"}, right_cm);
        if (!has_player_context)
        {
            const auto radians = static_cast<double>(before_rotation.yaw) * 3.14159265358979323846 / 180.0;
            forward_x = std::cos(radians);
            forward_y = std::sin(radians);
            player_yaw = before_rotation.yaw;
            attempts.push_back("nudge local axes fallback from actor yaw");
        }

        dx += forward_x * forward_cm;
        dy += forward_y * forward_cm;
        dx += (-forward_y) * right_cm;
        dy += forward_x * right_cm;

        auto desired_location = before_location;
        desired_location.x += static_cast<float>(dx);
        desired_location.y += static_cast<float>(dy);
        desired_location.z += static_cast<float>(dz);

        auto desired_rotation = before_rotation_ok ? before_rotation : Unreal::FRotator{};
        double yaw_delta = 0.0;
        if (first_regex_number(command_text, {"yawDelta", "deltaYaw", "rotateYaw", "dyaw"}, yaw_delta))
        {
            desired_rotation.yaw = static_cast<float>(normalize_yaw(static_cast<double>(desired_rotation.yaw) + yaw_delta));
        }
        if (first_regex_number(command_text, {"pitchDelta", "deltaPitch", "dpitch"}, value))
        {
            desired_rotation.pitch = static_cast<float>(static_cast<double>(desired_rotation.pitch) + value);
        }
        if (first_regex_number(command_text, {"rollDelta", "deltaRoll", "droll"}, value))
        {
            desired_rotation.roll = static_cast<float>(static_cast<double>(desired_rotation.roll) + value);
        }

        const auto dry_run = regex_bool(command_text, "dryRun", false) || regex_bool(command_text, "dry", false);
        const auto allow_large_move = regex_bool(command_text, "allowLargeMove", false);
        const auto confirm = first_regex_value(command_text, {"confirm", "Confirm"});
        const auto requested_distance = vector_distance_3d(before_location, desired_location);
        if (!allow_large_move && requested_distance > max_nudge_distance_cm)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"hektor-server-actor-editor\","
               << "\"operation\":\"nudge\","
               << "\"stage\":\"blocked-large-nudge\","
               << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
               << "\"distanceCm\":" << requested_distance << ","
               << "\"maxDistanceCm\":" << max_nudge_distance_cm << ","
               << "\"message\":\"Nudge distance is above per-command safety limit.\"}";
            return {false, ss.str()};
        }

        if (!dry_run && confirm != "MOVE_WORLD_ACTOR")
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"hektor-server-actor-editor\","
               << "\"operation\":\"nudge\","
               << "\"stage\":\"missing-confirmation\","
               << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
               << "\"message\":\"Set confirm to MOVE_WORLD_ACTOR or dryRun:true before nudging a world actor.\"}";
            return {false, ss.str()};
        }

        std::vector<std::string> details;
        auto actual_location = before_location;
        auto actual_rotation = before_rotation;
        const auto applied = dry_run
            ? true
            : editor_set_actor_location_and_rotation(actor, desired_location, desired_rotation, actual_location, actual_rotation, details);
        if (applied && !dry_run)
        {
            wake_actor_for_editor_transform(actor, static_cast<Unreal::UObject*>(actor), details);
            update_editor_actor_identity_cache_transform(command_text, requested_handle, actor, actual_location, actual_rotation, true);
        }

        std::ostringstream ss;
        ss << "{\"ok\":" << (applied ? "true" : "false") << ","
           << "\"route\":\"hektor-server-actor-editor\","
           << "\"operation\":\"nudge\","
           << "\"dryRun\":" << (dry_run ? "true" : "false") << ","
           << "\"selectionMethod\":\"server-pointer-exact-or-strict-client-identity\","
           << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
           << "\"actorClass\":\"" << json_escape(actor_class_full_name) << "\","
           << "\"confirm\":\"" << json_escape(confirm) << "\","
           << "\"localAxes\":{\"forwardX\":" << forward_x << ",\"forwardY\":" << forward_y << ",\"yaw\":" << player_yaw << "},"
           << "\"delta\":{\"x\":" << dx << ",\"y\":" << dy << ",\"z\":" << dz << ",\"forwardCm\":" << forward_cm << ",\"rightCm\":" << right_cm << ",\"yawDelta\":" << yaw_delta << "},"
           << "\"distanceCm\":" << requested_distance << ","
           << "\"beforeLocation\":";
        append_vector_json(ss, before_location);
        ss << ",\"requestedLocation\":";
        append_vector_json(ss, desired_location);
        ss << ",\"afterLocation\":";
        append_vector_json(ss, actual_location);
        ss << ",\"beforeRotation\":";
        append_rotator_json(ss, before_rotation);
        ss << ",\"requestedRotation\":";
        append_rotator_json(ss, desired_rotation);
        ss << ",\"afterRotation\":";
        append_rotator_json(ss, actual_rotation);
        ss << ",\"selectedCandidate\":null"
           << ",\"selectedTrace\":null"
           << ",\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << ",\"details\":";
        append_json_string_array(ss, details);
        ss << "}";
        return {applied, ss.str()};
    }

    auto SCUMTraderManager::editor_actor_copy(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        constexpr double max_copy_offset_cm = 2000.0;
        if (!editor_actor_mutation_enabled(command_text))
        {
            return {false, editor_actor_mutation_disabled_json("copy")};
        }

        std::vector<std::string> attempts;
        Unreal::FVector player_location{};
        double forward_x{};
        double forward_y{};
        double player_yaw{};
        const auto has_player_context = read_editor_player_context_from_command(command_text, player_location, forward_x, forward_y, player_yaw, attempts);

        std::string requested_handle;
        auto* actor = find_editor_actor_from_command(command_text, attempts, requested_handle);
        if (actor == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"hektor-server-actor-editor\","
               << "\"operation\":\"copy\","
               << "\"stage\":\"resolve-actor\","
               << "\"requestedHandle\":\"" << json_escape(requested_handle) << "\","
               << "\"message\":\"Copy requires a server pointer, exact loaded actor path, or strict client identity match. Server-side aim trace and scan fallbacks remain disabled after live crashes.\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << ",\"version\":\"" << mod_version << "\"}";
            return {false, ss.str()};
        }

        const auto actor_full_name = editor_actor_submitted_full_name(command_text, requested_handle, static_cast<Unreal::UObject*>(actor));
        const auto actor_class_full_name = editor_actor_submitted_class_name(command_text, static_cast<Unreal::UObject*>(actor));
        const auto block_reason = editor_actor_block_reason(actor_full_name, actor_class_full_name);
        if (!block_reason.empty())
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"hektor-server-actor-editor\","
               << "\"operation\":\"copy\","
               << "\"stage\":\"blocked-actor-category\","
               << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
               << "\"actorClass\":\"" << json_escape(actor_class_full_name) << "\","
               << "\"blockReason\":\"" << json_escape(block_reason) << "\","
               << "\"message\":\"Actor editor refuses traders, NPCs, players, items, controllers and economy/tradepost objects.\"}";
            return {false, ss.str()};
        }

        Unreal::FVector before_location{};
        Unreal::FRotator before_rotation{};
        auto before_location_ok = actor_location_noexcept(static_cast<Unreal::UObject*>(actor), before_location);
        auto before_rotation_ok = actor_rotation_noexcept(static_cast<Unreal::UObject*>(actor), before_rotation);
        if (!before_location_ok)
        {
            before_location_ok = cached_editor_actor_transform_from_command(
                command_text,
                requested_handle,
                actor,
                before_location,
                before_rotation,
                before_rotation_ok,
                attempts);
        }
        if (!before_location_ok)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"hektor-server-actor-editor\","
               << "\"operation\":\"copy\","
               << "\"stage\":\"read-source-transform\","
               << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
               << "\"actorClass\":\"" << json_escape(actor_class_full_name) << "\","
               << "\"message\":\"K2_GetActorLocation failed before copy.\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        if (!has_player_context)
        {
            const auto radians = static_cast<double>(before_rotation.yaw) * 3.14159265358979323846 / 180.0;
            forward_x = std::cos(radians);
            forward_y = std::sin(radians);
            player_yaw = before_rotation.yaw;
            attempts.push_back("copy local axes fallback from actor yaw");
        }

        double value{};
        double dx = 0.0;
        double dy = 0.0;
        double dz = 0.0;
        bool explicit_offset = false;
        if (first_regex_number(command_text, {"dx", "deltaX", "offsetX"}, value)) { dx += value; explicit_offset = true; }
        if (first_regex_number(command_text, {"dy", "deltaY", "offsetY"}, value)) { dy += value; explicit_offset = true; }
        if (first_regex_number(command_text, {"dz", "deltaZ", "offsetZ"}, value)) { dz += value; explicit_offset = true; }
        if (first_regex_number(command_text, {"upCm", "up", "verticalCm"}, value)) { dz += value; explicit_offset = true; }

        double forward_cm = 0.0;
        if (first_regex_number(command_text, {"forwardCm", "forward", "localForwardCm"}, forward_cm)) explicit_offset = true;
        double right_cm = 0.0;
        if (first_regex_number(command_text, {"rightCm", "right", "localRightCm"}, right_cm)) explicit_offset = true;
        if (!explicit_offset)
        {
            right_cm = 120.0;
            attempts.push_back("copy default rightCm=120");
        }

        dx += forward_x * forward_cm;
        dy += forward_y * forward_cm;
        dx += (-forward_y) * right_cm;
        dy += forward_x * right_cm;

        auto desired_location = before_location;
        desired_location.x += static_cast<float>(dx);
        desired_location.y += static_cast<float>(dy);
        desired_location.z += static_cast<float>(dz);

        if (first_regex_number(command_text, {"x", "X", "worldX", "WorldX"}, value)) desired_location.x = static_cast<float>(value);
        if (first_regex_number(command_text, {"y", "Y", "worldY", "WorldY"}, value)) desired_location.y = static_cast<float>(value);
        if (first_regex_number(command_text, {"z", "Z", "worldZ", "WorldZ"}, value)) desired_location.z = static_cast<float>(value);

        auto desired_rotation = before_rotation_ok ? before_rotation : Unreal::FRotator{};
        double yaw_delta = 0.0;
        if (first_regex_number(command_text, {"yawDelta", "deltaYaw", "rotateYaw", "dyaw"}, yaw_delta))
        {
            desired_rotation.yaw = static_cast<float>(normalize_yaw(static_cast<double>(desired_rotation.yaw) + yaw_delta));
        }

        const auto dry_run = regex_bool(command_text, "dryRun", false) || regex_bool(command_text, "dry", false);
        const auto allow_large_move = regex_bool(command_text, "allowLargeMove", false);
        const auto confirm = first_regex_value(command_text, {"confirm", "Confirm"});
        const auto requested_distance = vector_distance_3d(before_location, desired_location);
        if (!allow_large_move && requested_distance > max_copy_offset_cm)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"hektor-server-actor-editor\","
               << "\"operation\":\"copy\","
               << "\"stage\":\"blocked-large-copy-offset\","
               << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
               << "\"distanceCm\":" << requested_distance << ","
               << "\"maxDistanceCm\":" << max_copy_offset_cm << ","
               << "\"message\":\"Copy offset is above per-command safety limit.\"}";
            return {false, ss.str()};
        }

        if (!dry_run && confirm != "COPY_WORLD_ACTOR")
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"hektor-server-actor-editor\","
               << "\"operation\":\"copy\","
               << "\"stage\":\"missing-confirmation\","
               << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
               << "\"message\":\"Set confirm to COPY_WORLD_ACTOR or dryRun:true before copying a world actor.\"}";
            return {false, ss.str()};
        }

        auto* source_object = static_cast<Unreal::UObject*>(actor);
        auto* actor_class = safe_class_private(source_object);
        if (actor_class == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"hektor-server-actor-editor\","
               << "\"operation\":\"copy\","
               << "\"stage\":\"resolve-source-class\","
               << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
               << "\"message\":\"Source actor class could not be resolved.\"}";
            return {false, ss.str()};
        }

        auto* world = actor_world_noexcept(actor);
        if (world == nullptr)
        {
            world = current_streaming_world(&attempts);
        }
        if (world == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"hektor-server-actor-editor\","
               << "\"operation\":\"copy\","
               << "\"stage\":\"resolve-world\","
               << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
               << "\"sourceClass\":\"" << json_escape(safe_full_name(actor_class)) << "\","
               << "\"message\":\"World could not be resolved for copy.\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        if (dry_run)
        {
            std::ostringstream ss;
            ss << "{\"ok\":true,"
               << "\"route\":\"hektor-server-actor-editor\","
               << "\"operation\":\"copy\","
               << "\"dryRun\":true,"
               << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
               << "\"actorClass\":\"" << json_escape(actor_class_full_name) << "\","
               << "\"sourceClass\":\"" << json_escape(safe_full_name(actor_class)) << "\","
               << "\"world\":\"" << json_escape(safe_full_name(world)) << "\","
               << "\"sourceLocation\":";
            append_vector_json(ss, before_location);
            ss << ",\"requestedLocation\":";
            append_vector_json(ss, desired_location);
            ss << ",\"requestedRotation\":";
            append_rotator_json(ss, desired_rotation);
            ss << ",\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << ",\"version\":\"" << mod_version << "\"}";
            return {true, ss.str()};
        }

        const auto allow_unsafe_copy_spawn =
            regex_bool(command_text, "allowUnsafeEditorActorCopySpawn", false) ||
            regex_bool(command_text, "allowUnsafeCopySpawn", false);
        if (!allow_unsafe_copy_spawn)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"hektor-server-actor-editor\","
               << "\"operation\":\"copy\","
               << "\"stage\":\"blocked-unsafe-copy-spawn\","
               << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
               << "\"actorClass\":\"" << json_escape(actor_class_full_name) << "\","
               << "\"sourceClass\":\"" << json_escape(safe_full_name(actor_class)) << "\","
               << "\"world\":\"" << json_escape(safe_full_name(world)) << "\","
               << "\"requestedLocation\":";
            append_vector_json(ss, desired_location);
            ss << ",\"requestedRotation\":";
            append_rotator_json(ss, desired_rotation);
            ss << ",\"message\":\"Copy transport is wired, but native SpawnActor copy is fail-closed by default until a class-limited SCUM Blueprint/map-actor copy route is proven stable and persistent.\","
               << "\"enableFlag\":\"allowUnsafeEditorActorCopySpawn\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << ",\"version\":\"" << mod_version << "\"}";
            return {false, ss.str()};
        }

        auto transform = make_transform(desired_location.x, desired_location.y, desired_location.z, desired_rotation.yaw);
        auto* spawned_actor = world->SpawnActor(actor_class, &transform);
        if (spawned_actor == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"hektor-server-actor-editor\","
               << "\"operation\":\"copy\","
               << "\"stage\":\"spawn-copy\","
               << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
               << "\"sourceClass\":\"" << json_escape(safe_full_name(actor_class)) << "\","
               << "\"world\":\"" << json_escape(safe_full_name(world)) << "\","
               << "\"requestedLocation\":";
            append_vector_json(ss, desired_location);
            ss << ",\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        auto* spawned_object = static_cast<Unreal::UObject*>(spawned_actor);
        std::vector<std::string> details;
        copy_static_mesh_actor_visuals(source_object, spawned_object, details);

        auto actual_location = desired_location;
        auto actual_rotation = desired_rotation;
        const auto placement_applied = editor_set_actor_location_and_rotation(spawned_actor, desired_location, desired_rotation, actual_location, actual_rotation, details);
        wake_actor_for_editor_spawned_copy(spawned_actor, spawned_object, details);

        const auto actual_location_ok = actor_location_noexcept(spawned_object, actual_location);
        const auto actual_rotation_ok = actor_rotation_noexcept(spawned_object, actual_rotation);
        const auto placement_distance_cm = actual_location_ok
            ? vector_distance_3d(actual_location, desired_location)
            : std::numeric_limits<double>::infinity();
        const auto copy_ok = actual_location_ok && placement_distance_cm <= 5.0;
        attempts.push_back("SpawnActor(copy " + safe_full_name(actor_class) + ")=" + safe_full_name(spawned_object));

        std::ostringstream ss;
        ss << "{\"ok\":" << (copy_ok ? "true" : "false") << ","
           << "\"route\":\"hektor-server-actor-editor\","
           << "\"operation\":\"copy\","
           << "\"dryRun\":false,"
           << "\"selectionMethod\":\"server-pointer-exact-or-strict-client-identity\","
           << "\"sourceActorFullName\":\"" << json_escape(actor_full_name) << "\","
           << "\"sourceActorClass\":\"" << json_escape(actor_class_full_name) << "\","
           << "\"sourceClass\":\"" << json_escape(safe_full_name(actor_class)) << "\","
           << "\"spawnedActorFullName\":\"" << json_escape(safe_full_name(spawned_object)) << "\","
           << "\"spawnedActorClass\":\"" << json_escape(object_class_full_name_noexcept(spawned_object)) << "\","
           << "\"spawnedClassLeaf\":\"" << json_escape(object_leaf_from_full_name(safe_full_name(actor_class))) << "\","
           << "\"world\":\"" << json_escape(safe_full_name(world)) << "\","
           << "\"placementApplied\":" << (placement_applied ? "true" : "false") << ","
           << "\"locationOk\":" << (actual_location_ok ? "true" : "false") << ","
           << "\"rotationOk\":" << (actual_rotation_ok ? "true" : "false") << ","
           << "\"placementDistanceCm\":" << placement_distance_cm << ","
           << "\"delta\":{\"x\":" << dx << ",\"y\":" << dy << ",\"z\":" << dz << ",\"forwardCm\":" << forward_cm << ",\"rightCm\":" << right_cm << ",\"yawDelta\":" << yaw_delta << "},"
           << "\"sourceLocation\":";
        append_vector_json(ss, before_location);
        ss << ",\"requestedLocation\":";
        append_vector_json(ss, desired_location);
        ss << ",\"actualLocation\":";
        append_vector_json(ss, actual_location);
        ss << ",\"sourceRotation\":";
        append_rotator_json(ss, before_rotation);
        ss << ",\"requestedRotation\":";
        append_rotator_json(ss, desired_rotation);
        ss << ",\"actualRotation\":";
        append_rotator_json(ss, actual_rotation);
        ss << ",\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << ",\"selectedTrace\":null"
           << ",\"details\":";
        append_json_string_array(ss, details);
        ss << ",\"version\":\"" << mod_version << "\"}";
        return {copy_ok, ss.str()};
    }

    auto SCUMTraderManager::editor_actor_hide(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        if (!editor_actor_mutation_enabled(command_text))
        {
            return {false, editor_actor_mutation_disabled_json("hide")};
        }

        std::vector<std::string> attempts;

        std::string requested_handle;
        auto* actor = find_editor_actor_from_command(command_text, attempts, requested_handle);
        if (actor == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"hektor-server-actor-editor\","
               << "\"operation\":\"hide\","
               << "\"stage\":\"resolve-actor\","
               << "\"requestedHandle\":\"" << json_escape(requested_handle) << "\","
               << "\"message\":\"Hide/remove requires a server pointer, exact loaded actor path, or strict client identity match. Server-side aim trace and scan fallbacks remain disabled after live crashes.\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << ",\"version\":\"" << mod_version << "\"}";
            return {false, ss.str()};
        }

        const auto actor_full_name = editor_actor_submitted_full_name(command_text, requested_handle, static_cast<Unreal::UObject*>(actor));
        const auto actor_class_full_name = editor_actor_submitted_class_name(command_text, static_cast<Unreal::UObject*>(actor));
        const auto block_reason = editor_actor_block_reason(actor_full_name, actor_class_full_name);
        if (!block_reason.empty())
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"hektor-server-actor-editor\","
               << "\"operation\":\"hide\","
               << "\"stage\":\"blocked-actor-category\","
               << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
               << "\"actorClass\":\"" << json_escape(actor_class_full_name) << "\","
               << "\"blockReason\":\"" << json_escape(block_reason) << "\","
               << "\"message\":\"Actor editor refuses traders, NPCs, players, items, controllers and economy/tradepost objects.\"}";
            return {false, ss.str()};
        }

        Unreal::FVector before_location{};
        Unreal::FRotator before_rotation{};
        auto before_location_ok = actor_location_noexcept(static_cast<Unreal::UObject*>(actor), before_location);
        auto before_rotation_ok = actor_rotation_noexcept(static_cast<Unreal::UObject*>(actor), before_rotation);
        if (!before_location_ok)
        {
            before_location_ok = cached_editor_actor_transform_from_command(
                command_text,
                requested_handle,
                actor,
                before_location,
                before_rotation,
                before_rotation_ok,
                attempts);
        }
        if (!before_location_ok)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"hektor-server-actor-editor\","
               << "\"operation\":\"hide\","
               << "\"stage\":\"read-before-hide\","
               << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
               << "\"actorClass\":\"" << json_escape(actor_class_full_name) << "\","
               << "\"message\":\"K2_GetActorLocation failed before hide.\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        const auto dry_run = regex_bool(command_text, "dryRun", false) || regex_bool(command_text, "dry", false);
        const auto confirm = first_regex_value(command_text, {"confirm", "Confirm"});
        const auto confirmed = confirm == "HIDE_WORLD_ACTOR" || confirm == "REMOVE_WORLD_ACTOR";
        if (!dry_run && !confirmed)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"hektor-server-actor-editor\","
               << "\"operation\":\"hide\","
               << "\"stage\":\"missing-confirmation\","
               << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
               << "\"message\":\"Set confirm to HIDE_WORLD_ACTOR/REMOVE_WORLD_ACTOR or dryRun:true before hiding a world actor.\"}";
            return {false, ss.str()};
        }

        std::vector<std::string> details;
        if (!dry_run)
        {
            wake_actor_for_editor_hide(actor, static_cast<Unreal::UObject*>(actor), details);
        }

        std::ostringstream ss;
        ss << "{\"ok\":true,"
           << "\"route\":\"hektor-server-actor-editor\","
           << "\"operation\":\"hide\","
           << "\"dryRun\":" << (dry_run ? "true" : "false") << ","
           << "\"selectionMethod\":\"server-pointer-exact-or-strict-client-identity\","
           << "\"actorFullName\":\"" << json_escape(actor_full_name) << "\","
           << "\"actorClass\":\"" << json_escape(actor_class_full_name) << "\","
           << "\"confirm\":\"" << json_escape(confirm) << "\","
           << "\"hidden\":true,"
           << "\"collisionEnabled\":false,"
           << "\"beforeLocation\":";
        append_vector_json(ss, before_location);
        ss << ",\"beforeRotation\":";
        append_rotator_json(ss, before_rotation_ok ? before_rotation : Unreal::FRotator{});
        ss << ",\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << ",\"selectedTrace\":null"
           << ",\"details\":";
        append_json_string_array(ss, details);
        ss << ",\"version\":\"" << mod_version << "\"}";
        return {true, ss.str()};
    }

    auto SCUMTraderManager::server_actor_spawn_persistent(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        std::vector<std::string> attempts;
        std::vector<std::string> details;
        const auto site_id = first_regex_value(command_text, {"siteId", "idempotencyKey", "persistentId", "spawnId"});
        const auto actor_class = first_regex_value(command_text, {"actorClass", "className", "actor", "name"});
        const auto catalog_path = first_regex_value(command_text, {"catalogPath", "objectPath", "assetPath", "classPath"});
        const auto dry_run = regex_bool(command_text, "dryRun", false) || regex_bool(command_text, "dry", false);
        const auto replay = regex_bool(command_text, "replay", false) || regex_bool(command_text, "restore", false);
        const auto visual_proxy =
            regex_bool(command_text, "visualProxy", false) ||
            regex_bool(command_text, "staticMeshVisualProxy", false) ||
            regex_bool(command_text, "useStaticMeshProxy", false);
        const auto original_actor_class = first_regex_value(command_text, {"originalActorClass", "sourceActorClass", "targetActorClass"});
        const auto source_actor_full_name = first_regex_value(command_text, {"sourceActorFullName", "targetActorFullName", "actorFullName"});
        const auto source_object_path = first_regex_value(command_text, {"sourceObjectPath", "targetObjectPath"});
        const auto source_component_full_name = first_regex_value(command_text, {"sourceComponentFullName", "targetComponentFullName", "componentFullName"});
        const auto source_mesh_full_name = first_regex_value(command_text, {"sourceMeshFullName", "meshFullName", "staticMeshFullName"});
        const auto spawn_actor_class = visual_proxy ? std::string("StaticMeshActor") : actor_class;
        const auto spawn_catalog_path = visual_proxy ? std::string("/Script/Engine.StaticMeshActor") : catalog_path;

        double x{};
        double y{};
        double z{};
        double yaw{};
        const auto has_x = first_regex_number(command_text, {"x", "X", "worldX", "WorldX"}, x);
        const auto has_y = first_regex_number(command_text, {"y", "Y", "worldY", "WorldY"}, y);
        const auto has_z = first_regex_number(command_text, {"z", "Z", "worldZ", "WorldZ"}, z);
        if (!first_regex_number(command_text, {"yaw", "Yaw", "rotationYaw", "RotationYaw"}, yaw))
        {
            yaw = 0.0;
        }
        yaw = normalize_yaw(yaw);

        if (spawn_actor_class.empty() && spawn_catalog_path.empty())
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"native-persistent-server-spawn\","
               << "\"stage\":\"missing-actor-class\","
               << "\"message\":\"actorClass or catalogPath is required.\"}";
            return {false, ss.str()};
        }

        if (!has_x || !has_y || !has_z || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"native-persistent-server-spawn\","
               << "\"stage\":\"missing-location\","
               << "\"actorClass\":\"" << json_escape(actor_class) << "\","
               << "\"siteId\":\"" << json_escape(site_id) << "\","
               << "\"message\":\"x, y and z world coordinates are required for server actor spawn.\"}";
            return {false, ss.str()};
        }

        auto* world = current_streaming_world(&attempts);
        if (world == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"native-persistent-server-spawn\","
               << "\"stage\":\"resolve-world\","
               << "\"actorClass\":\"" << json_escape(actor_class) << "\","
               << "\"siteId\":\"" << json_escape(site_id) << "\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        RC::Unreal::UObject* visual_source_actor = nullptr;
        RC::Unreal::UObject* visual_source_component = nullptr;
        RC::Unreal::UObject* visual_source_mesh = nullptr;
        std::string visual_source_mesh_full_name{};

        if (visual_proxy)
        {
            if (!source_actor_full_name.empty())
            {
                visual_source_actor = find_object_by_full_name(
                    {L"Actor", L"Object"},
                    source_actor_full_name,
                    attempts,
                    "visualProxy.sourceActor",
                    false);
            }
            if (visual_source_actor == nullptr && !source_object_path.empty())
            {
                visual_source_actor = find_object_by_full_name(
                    {L"Actor", L"Object"},
                    source_object_path,
                    attempts,
                    "visualProxy.sourceObjectPath",
                    false);
            }
            if (!source_component_full_name.empty())
            {
                visual_source_component = find_object_by_full_name(
                    {
                        L"StaticMeshComponent",
                        L"InstancedStaticMeshComponent",
                        L"HierarchicalInstancedStaticMeshComponent",
                        L"MeshComponent",
                        L"PrimitiveComponent",
                        L"SceneComponent",
                        L"ActorComponent",
                        L"Object",
                    },
                    source_component_full_name,
                    attempts,
                    "visualProxy.sourceComponent",
                    false);
            }
            if (!source_mesh_full_name.empty())
            {
                visual_source_mesh = find_static_mesh_asset_for_visual_proxy(source_mesh_full_name, attempts);
            }
            if (visual_source_mesh == nullptr && visual_source_component != nullptr)
            {
                visual_source_mesh = object_property(visual_source_component, L"StaticMesh");
                attempts.push_back("visualProxy.sourceComponent.StaticMesh=" + safe_full_name(visual_source_mesh));
            }
            if (visual_source_component == nullptr && visual_source_actor != nullptr)
            {
                visual_source_component = preferred_static_mesh_component(visual_source_actor, "visualProxy.sourceActorComponent", details);
                if (visual_source_mesh == nullptr && visual_source_component != nullptr)
                {
                    visual_source_mesh = object_property(visual_source_component, L"StaticMesh");
                    attempts.push_back("visualProxy.sourceActorComponent.StaticMesh=" + safe_full_name(visual_source_mesh));
                }
            }
            if (visual_source_mesh == nullptr)
            {
                std::ostringstream ss;
                ss << "{\"ok\":false,"
                   << "\"route\":\"native-persistent-server-spawn\","
                   << "\"stage\":\"visual-proxy-source-mesh\","
                   << "\"visualProxy\":true,"
                   << "\"siteId\":\"" << json_escape(site_id) << "\","
                   << "\"actorClass\":\"" << json_escape(actor_class) << "\","
                   << "\"originalActorClass\":\"" << json_escape(original_actor_class) << "\","
                   << "\"sourceActorFullName\":\"" << json_escape(source_actor_full_name) << "\","
                   << "\"sourceComponentFullName\":\"" << json_escape(source_component_full_name) << "\","
                   << "\"sourceMeshFullName\":\"" << json_escape(source_mesh_full_name) << "\","
                   << "\"message\":\"Static mesh visual proxy needs an exact loaded source component or a loaded source mesh asset.\","
                   << "\"attempts\":";
                append_json_string_array(ss, attempts);
                ss << ",\"details\":";
                append_json_string_array(ss, details);
                ss << "}";
                return {false, ss.str()};
            }
            visual_source_mesh_full_name = safe_full_name(visual_source_mesh);
        }

        auto* actor_class_ptr = resolve_trader_actor_class(spawn_actor_class, spawn_catalog_path, attempts);
        if (actor_class_ptr == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"native-persistent-server-spawn\","
               << "\"stage\":\"resolve-class\","
               << "\"actorClass\":\"" << json_escape(spawn_actor_class) << "\","
               << "\"catalogPath\":\"" << json_escape(spawn_catalog_path) << "\","
               << "\"siteId\":\"" << json_escape(site_id) << "\","
               << "\"world\":\"" << json_escape(full_name(world)) << "\","
               << "\"message\":\"Loaded UClass was not found. Lua must LoadAsset/StaticFindObject before native SpawnActor, or pass a loaded class path.\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        Unreal::FVector requested_location{};
        requested_location.x = static_cast<float>(x);
        requested_location.y = static_cast<float>(y);
        requested_location.z = static_cast<float>(z);
        Unreal::FRotator requested_rotation{};
        requested_rotation.pitch = 0.0f;
        requested_rotation.yaw = static_cast<float>(yaw);
        requested_rotation.roll = 0.0f;

        if (dry_run)
        {
            std::ostringstream ss;
            ss << "{\"ok\":true,"
               << "\"route\":\"native-persistent-server-spawn\","
               << "\"operation\":\"dry-run\","
               << "\"dryRun\":true,"
               << "\"visualProxy\":" << (visual_proxy ? "true" : "false") << ","
               << "\"siteId\":\"" << json_escape(site_id) << "\","
               << "\"actorClass\":\"" << json_escape(spawn_actor_class) << "\","
               << "\"catalogPath\":\"" << json_escape(spawn_catalog_path) << "\","
               << "\"originalActorClass\":\"" << json_escape(original_actor_class) << "\","
               << "\"sourceActorFullName\":\"" << json_escape(source_actor_full_name) << "\","
               << "\"sourceComponentFullName\":\"" << json_escape(source_component_full_name) << "\","
               << "\"sourceMeshFullName\":\"" << json_escape(visual_source_mesh_full_name) << "\","
               << "\"resolvedClass\":\"" << json_escape(full_name(actor_class_ptr)) << "\","
               << "\"world\":\"" << json_escape(full_name(world)) << "\","
               << "\"requestedLocation\":";
            append_vector_json(ss, requested_location);
            ss << ",\"requestedRotation\":";
            append_rotator_json(ss, requested_rotation);
            ss << ",\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << ",\"details\":";
            append_json_string_array(ss, details);
            ss << "}";
            return {true, ss.str()};
        }

        auto transform = make_transform(x, y, z, yaw);
        ScopedActorReplicationDefaults spawn_replication_defaults(actor_class_ptr, details, visual_proxy);
        auto* spawned_actor = world->SpawnActor(actor_class_ptr, &transform);
        if (spawned_actor == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"native-persistent-server-spawn\","
               << "\"stage\":\"spawn-actor\","
               << "\"siteId\":\"" << json_escape(site_id) << "\","
                << "\"actorClass\":\"" << json_escape(spawn_actor_class) << "\","
                << "\"catalogPath\":\"" << json_escape(spawn_catalog_path) << "\","
                << "\"resolvedClass\":\"" << json_escape(full_name(actor_class_ptr)) << "\","
               << "\"world\":\"" << json_escape(full_name(world)) << "\","
               << "\"requestedLocation\":";
            append_vector_json(ss, requested_location);
            ss << ",\"requestedRotation\":";
            append_rotator_json(ss, requested_rotation);
            ss << ",\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        auto* spawned_object = static_cast<Unreal::UObject*>(spawned_actor);
        bool visual_ok = true;
        if (visual_proxy)
        {
            auto* destination_component = preferred_static_mesh_component(spawned_object, "visualProxy.destination", details);
            visual_ok = copy_static_mesh_component_visuals(
                visual_source_component,
                destination_component,
                visual_source_mesh,
                "visualProxy.copyMesh",
                details);
        }
        if (visual_proxy)
        {
            wake_actor_for_replicated_spawn_direct_only(spawned_actor, spawned_object, details);
        }
        else
        {
            wake_actor_for_persistent_server_spawn(spawned_actor, spawned_object, details);
        }
        if (visual_proxy && !visual_ok)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"route\":\"native-persistent-server-spawn\","
               << "\"stage\":\"visual-proxy-apply\","
               << "\"visualProxy\":true,"
               << "\"siteId\":\"" << json_escape(site_id) << "\","
               << "\"actorClass\":\"" << json_escape(spawn_actor_class) << "\","
               << "\"catalogPath\":\"" << json_escape(spawn_catalog_path) << "\","
               << "\"resolvedClass\":\"" << json_escape(full_name(actor_class_ptr)) << "\","
               << "\"actorFullName\":\"" << json_escape(full_name(spawned_object)) << "\","
               << "\"sourceActorFullName\":\"" << json_escape(source_actor_full_name) << "\","
               << "\"sourceComponentFullName\":\"" << json_escape(source_component_full_name) << "\","
               << "\"sourceMeshFullName\":\"" << json_escape(visual_source_mesh_full_name) << "\","
               << "\"message\":\"StaticMeshActor spawned, but source mesh could not be applied. It will not be persisted.\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << ",\"details\":";
            append_json_string_array(ss, details);
            ss << ",\"version\":\"" << mod_version << "\"}";
            return {false, ss.str()};
        }

        Unreal::FVector actual_location = requested_location;
        Unreal::FRotator actual_rotation = requested_rotation;
        const auto actual_location_ok = actor_location_noexcept(spawned_object, actual_location);
        const auto actual_rotation_ok = actor_rotation_noexcept(spawned_object, actual_rotation);
        attempts.push_back("SpawnActor(" + full_name(actor_class_ptr) + ")=" + full_name(spawned_object));

        const auto lower_actor = lower_copy(actor_class + " " + catalog_path + " " + original_actor_class + " " + full_name(actor_class_ptr));
        const auto trader_like =
            !visual_proxy &&
            (lower_actor.find("trader") != std::string::npos ||
             lower_actor.find("banker") != std::string::npos ||
             lower_actor.find("armsdealer") != std::string::npos ||
             lower_actor.find("npcinteractionbox") != std::string::npos ||
             lower_actor.find("tradepost") != std::string::npos);

        std::ostringstream ss;
        ss << "{\"ok\":true,"
           << "\"route\":\"native-persistent-server-spawn\","
           << "\"operation\":\"spawn\","
           << "\"dryRun\":false,"
           << "\"replay\":" << (replay ? "true" : "false") << ","
           << "\"persistent\":true,"
           << "\"visualProxy\":" << (visual_proxy ? "true" : "false") << ","
           << "\"visualOk\":" << (visual_ok ? "true" : "false") << ","
           << "\"siteId\":\"" << json_escape(site_id) << "\","
           << "\"actorClass\":\"" << json_escape(spawn_actor_class) << "\","
           << "\"catalogPath\":\"" << json_escape(spawn_catalog_path) << "\","
           << "\"originalActorClass\":\"" << json_escape(original_actor_class) << "\","
           << "\"sourceActorFullName\":\"" << json_escape(source_actor_full_name) << "\","
           << "\"sourceComponentFullName\":\"" << json_escape(source_component_full_name) << "\","
           << "\"sourceMeshFullName\":\"" << json_escape(visual_source_mesh_full_name) << "\","
           << "\"resolvedClass\":\"" << json_escape(full_name(actor_class_ptr)) << "\","
           << "\"world\":\"" << json_escape(full_name(world)) << "\","
           << "\"actorFullName\":\"" << json_escape(full_name(spawned_object)) << "\","
           << "\"actorClassFullName\":\"" << json_escape(object_class_full_name_noexcept(spawned_object)) << "\","
           << "\"locationOk\":" << (actual_location_ok ? "true" : "false") << ","
           << "\"rotationOk\":" << (actual_rotation_ok ? "true" : "false") << ","
           << "\"requestedLocation\":";
        append_vector_json(ss, requested_location);
        ss << ",\"actualLocation\":";
        append_vector_json(ss, actual_location);
        ss << ",\"requestedRotation\":";
        append_rotator_json(ss, requested_rotation);
        ss << ",\"actualRotation\":";
        append_rotator_json(ss, actual_rotation);
        ss << ",\"genericSpawnOnly\":" << (trader_like ? "true" : "false") << ","
           << "\"traderLifecycle\":\"" << (trader_like ? "not-registered-with-economy-manager" : "not-applicable") << "\","
           << "\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << ",\"details\":";
        append_json_string_array(ss, details);
        ss << ",\"version\":\"" << mod_version << "\"}";
        return {true, ss.str()};
    }

    auto SCUMTraderManager::read_only_trader_probe() const -> std::string
    {
        std::vector<std::string> manager_route_counts;
        std::vector<std::string> trade_post_route_counts;
        std::vector<std::string> trader_route_counts;
        auto managers = find_trade_outpost_managers(&manager_route_counts);
        auto trade_posts = find_all_unique(
            {L"TradePost",
             L"ATradePost",
             L"BP_Outpost_Armory_NPCInteractionBox_C",
             L"BP_Outpost_Bank_open_NPC_InteractionBoxes_C",
             L"BP_Outpost_CarShop_NPC_and_VehicleSpawner_C",
             L"BP_FishermanTrader_NPC_InteractionBox_C",
             L"BP_Outpost_Hospital_NPC_InteractionBoxes_C",
             L"BP_SaloonOutpost_NPC_InteractionBoxes_C",
             L"BP_BarberShop_NPC_InteractionBox_C",
             L"BP_Outpost_Trader_NPCInteractionBox_C",
             L"BP_Outpost_Hunter_01_NPC_C"},
            &trade_post_route_counts);
        auto traders = find_all_unique(
            {L"Trader",
             L"ATrader",
             L"Doctor",
             L"Mechanic",
             L"Banker",
             L"BP_Master_Trader_C",
             L"BP_ArmsDealer_01_C",
             L"BP_Barber_01_C",
             L"BP_Bartender_01_C",
             L"BP_General_Goods_01_C",
             L"BP_Harbourmaster_01_C",
             L"BP_Doctor_01_C",
             L"BP_Mechanic_C",
             L"BP_Banker01_C"},
            &trader_route_counts);

        auto* economy_manager = Unreal::UObjectGlobals::FindFirstOf(L"BP_EconomyManager_C");
        if (economy_manager == nullptr)
        {
            economy_manager = Unreal::UObjectGlobals::FindFirstOf(L"ConZEconomyManager");
        }

        std::ostringstream ss;
        ss << "{\"probe\":\"read-only\","
           << "\"engineAccess\":\"UE4SS UObjectGlobals/Property ABI\","
           << "\"classSearch\":{\"managers\":";
        append_json_string_array(ss, manager_route_counts);
        ss << ",\"tradePosts\":";
        append_json_string_array(ss, trade_post_route_counts);
        ss << ",\"traders\":";
        append_json_string_array(ss, trader_route_counts);
        ss << "},"
           << "\"economyManager\":";
        if (economy_manager == nullptr)
        {
            ss << "null";
        }
        else
        {
            auto* trader_component = object_property(economy_manager, L"_traderManagingComponent");
            const auto* pending_personalities = array_property(economy_manager, L"_pendingTraderPersonalities");
            ss << "{\"fullName\":\"" << json_escape(full_name(economy_manager)) << "\","
               << "\"traderManagingComponent\":\"" << json_escape(full_name(trader_component)) << "\","
               << "\"traderManagingComponentSettings\":{\"pawnRelevancyRange\":" << float_property(trader_component, L"_pawnRelevancyRange", -1.0f)
               << ",\"tradeOutpostsUpdateTime\":" << float_property(trader_component, L"_tradeOutpostsUpdateTime", -1.0f) << "},"
               << "\"pendingTraderPersonalitiesCount\":" << (is_reasonable_array(pending_personalities) ? pending_personalities->count : -1) << ","
               << "\"pendingTraderPersonalitiesMax\":" << (is_reasonable_array(pending_personalities) ? pending_personalities->max : -1) << ","
               << "\"pendingTraderPersonalitySamples\":";
            append_object_array_samples_json(ss, pending_personalities, 16);
            ss << "}";
        }

        ss << ",\"tradeOutpostManagerCount\":" << managers.size()
           << ",\"tradePostCount\":" << trade_posts.size()
           << ",\"traderCount\":" << traders.size()
           << ",\"managers\":[";
        for (size_t index = 0; index < managers.size(); ++index)
        {
            auto* manager = managers[index];
            auto* outpost_description = object_property(manager, L"_outpostDescription");
            const auto* assigned_trade_posts = array_property(manager, L"_assignedTradePosts");
            const auto* other_buildings = array_property(manager, L"_otherAssignedTradeOutpostBuildings");
            const auto outpost_id = guid_to_string(guid_property(outpost_description, L"TradeOutpostPersistentId"));
            if (index > 0) ss << ",";
            ss << "{\"fullName\":\"" << json_escape(full_name(manager)) << "\","
               << "\"outpostDescription\":\"" << json_escape(full_name(outpost_description)) << "\","
               << "\"outpostPersistentId\":\"" << json_escape(outpost_id) << "\","
               << "\"assignedTradePostsCount\":" << (is_reasonable_array(assigned_trade_posts) ? assigned_trade_posts->count : -1) << ","
               << "\"assignedTradePostsMax\":" << (is_reasonable_array(assigned_trade_posts) ? assigned_trade_posts->max : -1) << ","
               << "\"otherAssignedBuildingsCount\":" << (is_reasonable_array(other_buildings) ? other_buildings->count : -1) << ","
               << "\"otherAssignedBuildingsMax\":" << (is_reasonable_array(other_buildings) ? other_buildings->max : -1) << ","
               << "\"assignedTradePostSamples\":";
            append_object_array_samples_json(ss, assigned_trade_posts, 12);
            ss << ",\"otherAssignedBuildingSamples\":";
            append_object_array_samples_json(ss, other_buildings, 12);
            ss << "}";
        }

        ss << "],\"tradePosts\":[";
        for (size_t index = 0; index < trade_posts.size(); ++index)
        {
            auto* trade_post = trade_posts[index];
            std::string manager_route;
            auto* assigned_manager = find_manager_for_tradepost(managers, trade_post, &manager_route);
            const auto* trader_markers = array_property(trade_post, L"_traderMarkers");
            const auto* location_markers = array_property(trade_post, L"_locationMarkers");
            const auto* spawned_traders = array_property(trade_post, L"_spawnedTraders");
            const auto* spawned_sedentary_npcs = array_property(trade_post, L"_spawnedSedentaryNPCs");
            const auto* spawned_depots = array_property(trade_post, L"_spawnedDepots");
            const auto* sedentary_markers = array_property(trade_post, L"_sedentaryNPCMarkers");
            auto* trade_post_actor = static_cast<Unreal::AActor*>(trade_post);
            const auto trade_post_location = trade_post_actor->K2_GetActorLocation();
            const auto trade_post_rotation = trade_post_actor->K2_GetActorRotation();
            if (index > 0) ss << ",";
            ss << "{\"fullName\":\"" << json_escape(full_name(trade_post)) << "\","
               << "\"assignedManager\":\"" << json_escape(full_name(assigned_manager)) << "\","
               << "\"assignedManagerRoute\":\"" << json_escape(manager_route) << "\","
               << "\"assignedToManager\":" << (assigned_manager != nullptr ? "true" : "false") << ","
               << "\"actorLocation\":";
            append_vector_json(ss, trade_post_location);
            ss << ",\"actorRotation\":";
            append_rotator_json(ss, trade_post_rotation);
            ss << ","
               << "\"traderMarkersCount\":" << (is_reasonable_array(trader_markers) ? trader_markers->count : -1) << ","
               << "\"traderMarkersMax\":" << (is_reasonable_array(trader_markers) ? trader_markers->max : -1) << ","
               << "\"sedentaryNPCMarkersCount\":" << (is_reasonable_array(sedentary_markers) ? sedentary_markers->count : -1) << ","
               << "\"sedentaryNPCMarkersMax\":" << (is_reasonable_array(sedentary_markers) ? sedentary_markers->max : -1) << ","
               << "\"locationMarkersCount\":" << (is_reasonable_array(location_markers) ? location_markers->count : -1) << ","
               << "\"locationMarkersMax\":" << (is_reasonable_array(location_markers) ? location_markers->max : -1) << ","
               << "\"spawnedTradersCount\":" << (is_reasonable_array(spawned_traders) ? spawned_traders->count : -1) << ","
               << "\"spawnedTradersMax\":" << (is_reasonable_array(spawned_traders) ? spawned_traders->max : -1) << ","
               << "\"spawnedSedentaryNPCsCount\":" << (is_reasonable_array(spawned_sedentary_npcs) ? spawned_sedentary_npcs->count : -1) << ","
               << "\"spawnedSedentaryNPCsMax\":" << (is_reasonable_array(spawned_sedentary_npcs) ? spawned_sedentary_npcs->max : -1) << ","
               << "\"spawnedDepotsCount\":" << (is_reasonable_array(spawned_depots) ? spawned_depots->count : -1) << ","
               << "\"spawnedDepotsMax\":" << (is_reasonable_array(spawned_depots) ? spawned_depots->max : -1) << ","
               << "\"spawnedTraderSamples\":";
            append_object_array_samples_json(ss, spawned_traders, 8);
            ss << ",\"spawnedSedentaryNPCSamples\":";
            append_object_array_samples_json(ss, spawned_sedentary_npcs, 8);
            ss << ",\"traderMarkerSamples\":";
            append_trader_marker_samples(ss, trader_markers, 3);
            ss << ",\"locationMarkerSamples\":";
            append_location_marker_samples(ss, location_markers, 3);
            ss << "}";
        }

        ss << "],\"traderSamples\":";
        std::vector<std::string> trader_samples;
        for (size_t index = 0; index < std::min<size_t>(traders.size(), 16); ++index)
        {
            trader_samples.push_back(full_name(traders[index]));
        }
        append_json_string_array(ss, trader_samples);
        ss << "}";
        return ss.str();
    }

    namespace
    {
        struct CfcoreNativeRefs
        {
            RC::Unreal::UObject* subsystem_class{};
            RC::Unreal::UObject* subsystem_cdo{};
            RC::Unreal::UObject* bp_library_class{};
            RC::Unreal::UObject* editor_settings_class{};
            RC::Unreal::UObject* subsystem_instance{};
            RC::Unreal::UObject* assure_server_mods_updated{};
            RC::Unreal::UObject* assure_client_mods_updated{};
            RC::Unreal::UObject* get_installed_mods{};
            RC::Unreal::UObject* get_mods_dir_info{};
            std::string process_event_detail{};
            bool subsystem_instance_looks_cdo{};
            std::vector<std::string> attempts{};
        };

        auto looks_like_default_object(RC::Unreal::UObject* object) -> bool
        {
            const auto full = lower_copy(safe_full_name(object));
            return full.find("default__") != std::string::npos ||
                full.find("classdefaultobject") != std::string::npos;
        }

        auto cfcore_find_object_exact(
            const wchar_t* class_name,
            std::initializer_list<const wchar_t*> object_paths,
            const char* label,
            std::vector<std::string>& attempts) -> RC::Unreal::UObject*
        {
            for (const auto* object_path : object_paths)
            {
                if (class_name == nullptr || object_path == nullptr || object_path[0] == L'\0') continue;
                auto* object = find_object_exact_noexcept(class_name, object_path, 0, object_flag_class_default_object);
                attempts.push_back(std::string(label) + ".FindObject(" + narrow(class_name) + "," + narrow(object_path) + ")=" + (object != nullptr ? safe_full_name(object) : "0"));
                if (object != nullptr) return object;
            }
            return nullptr;
        }

        auto cfcore_find_function_exact(
            std::initializer_list<const wchar_t*> object_paths,
            const char* function_name,
            const char* label,
            std::vector<std::string>& attempts) -> RC::Unreal::UObject*
        {
            for (const auto* object_path : object_paths)
            {
                if (object_path == nullptr || object_path[0] == L'\0') continue;
                for (const auto* class_name : {L"Function", L"UFunction", L"Object"})
                {
                    auto* object = find_object_exact_noexcept(class_name, object_path, 0, object_flag_class_default_object);
                    attempts.push_back(std::string(label) + ".FindObject(" + narrow(class_name) + "," + narrow(object_path) + ")=" + (object != nullptr ? safe_full_name(object) : "0"));
                    if (object != nullptr) return object;
                }
            }

            const auto function_name_text = function_name == nullptr ? std::string{} : std::string(function_name);
            if (!function_name_text.empty())
            {
                const auto function_name_w = widen(function_name_text);
                const auto needle = lower_copy(function_name_text);
                for (const auto* class_name : {L"Function", L"UFunction", L"Object"})
                {
                    std::vector<RC::Unreal::UObject*> found;
                    RC::Unreal::UObjectGlobals::FindObjects(class_name, function_name_w.c_str(), found, 0, object_flag_class_default_object, false);
                    attempts.push_back(std::string(label) + ".FindObjects(" + narrow(class_name) + "," + function_name_text + ")=" + std::to_string(found.size()));
                    for (auto* object : found)
                    {
                        if (object == nullptr) continue;
                        const auto full = safe_full_name(object);
                        const auto lower = lower_copy(full);
                        if (lower.find("cfcore") != std::string::npos &&
                            lower.find("cfcoresubsystem") != std::string::npos &&
                            lower.find(needle) != std::string::npos)
                        {
                            attempts.push_back(std::string(label) + ".matched=" + full);
                            return object;
                        }
                    }
                }
            }
            return nullptr;
        }

        auto cfcore_find_first_of_noexcept(const wchar_t* class_name) -> RC::Unreal::UObject*
        {
            __try
            {
                return RC::Unreal::UObjectGlobals::FindFirstOf(class_name);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return nullptr;
            }
        }

        auto cfcore_find_subsystem_instance(std::vector<std::string>& attempts) -> RC::Unreal::UObject*
        {
            RC::Unreal::UObject* fallback{};
            for (const auto* class_name : {L"CFCoreSubsystem", L"UCFCoreSubsystem"})
            {
                auto* object = cfcore_find_first_of_noexcept(class_name);
                attempts.push_back(std::string("subsystem.FindFirstOf(") + narrow(class_name) + ")=" + (object != nullptr ? safe_full_name(object) : "0"));
                if (object == nullptr) continue;
                if (fallback == nullptr) fallback = object;
                if (!looks_like_default_object(object)) return object;
            }
            return fallback;
        }

        auto cfcore_resolve_native_refs() -> CfcoreNativeRefs
        {
            CfcoreNativeRefs refs;
            refs.subsystem_class = cfcore_find_object_exact(
                L"Class",
                {L"/Script/cfcore.CFCoreSubsystem", L"CFCoreSubsystem"},
                "cfcoreSubsystemClass",
                refs.attempts);
            refs.subsystem_cdo = cfcore_find_object_exact(
                L"CFCoreSubsystem",
                {L"/Script/cfcore.Default__CFCoreSubsystem", L"Default__CFCoreSubsystem"},
                "cfcoreSubsystemCdo",
                refs.attempts);
            refs.bp_library_class = cfcore_find_object_exact(
                L"Class",
                {L"/Script/cfcore.CFCoreBPLibrary", L"CFCoreBPLibrary"},
                "cfcoreBpLibraryClass",
                refs.attempts);
            refs.editor_settings_class = cfcore_find_object_exact(
                L"Class",
                {L"/Script/cfcore.CFCoreEditorSettings", L"CFCoreEditorSettings"},
                "cfcoreEditorSettingsClass",
                refs.attempts);

            refs.assure_server_mods_updated = cfcore_find_function_exact(
                {
                    L"/Script/cfcore.CFCoreSubsystem:AssureServerModsUpdated",
                    L"Function /Script/cfcore.CFCoreSubsystem:AssureServerModsUpdated",
                    L"/Script/cfcore.CFCoreSubsystem.AssureServerModsUpdated",
                    L"Function /Script/cfcore.CFCoreSubsystem.AssureServerModsUpdated",
                },
                "AssureServerModsUpdated",
                "AssureServerModsUpdated",
                refs.attempts);
            refs.assure_client_mods_updated = cfcore_find_function_exact(
                {
                    L"/Script/cfcore.CFCoreSubsystem:AssureClientModsUpdated",
                    L"Function /Script/cfcore.CFCoreSubsystem:AssureClientModsUpdated",
                    L"/Script/cfcore.CFCoreSubsystem.AssureClientModsUpdated",
                    L"Function /Script/cfcore.CFCoreSubsystem.AssureClientModsUpdated",
                },
                "AssureClientModsUpdated",
                "AssureClientModsUpdated",
                refs.attempts);
            refs.get_installed_mods = cfcore_find_function_exact(
                {
                    L"/Script/cfcore.CFCoreSubsystem:GetInstalledMods",
                    L"Function /Script/cfcore.CFCoreSubsystem:GetInstalledMods",
                    L"/Script/cfcore.CFCoreSubsystem.GetInstalledMods",
                    L"Function /Script/cfcore.CFCoreSubsystem.GetInstalledMods",
                },
                "GetInstalledMods",
                "GetInstalledMods",
                refs.attempts);
            refs.get_mods_dir_info = cfcore_find_function_exact(
                {
                    L"/Script/cfcore.CFCoreSubsystem:GetModsDirInfo",
                    L"Function /Script/cfcore.CFCoreSubsystem:GetModsDirInfo",
                    L"/Script/cfcore.CFCoreSubsystem.GetModsDirInfo",
                    L"Function /Script/cfcore.CFCoreSubsystem.GetModsDirInfo",
                },
                "GetModsDirInfo",
                "GetModsDirInfo",
                refs.attempts);

            refs.subsystem_instance = cfcore_find_subsystem_instance(refs.attempts);
            refs.subsystem_instance_looks_cdo = looks_like_default_object(refs.subsystem_instance);
            if (refs.subsystem_instance != nullptr)
            {
                (void)process_event_from_vtable(refs.subsystem_instance, refs.process_event_detail);
            }
            else
            {
                refs.process_event_detail = "subsystem-instance-null";
            }
            return refs;
        }

        auto append_cfcore_refs_json(std::ostringstream& ss, const CfcoreNativeRefs& refs) -> void
        {
            ss << "\"refs\":{"
               << "\"subsystemClass\":\"" << json_escape(safe_full_name(refs.subsystem_class)) << "\","
               << "\"subsystemCdo\":\"" << json_escape(safe_full_name(refs.subsystem_cdo)) << "\","
               << "\"bpLibraryClass\":\"" << json_escape(safe_full_name(refs.bp_library_class)) << "\","
               << "\"editorSettingsClass\":\"" << json_escape(safe_full_name(refs.editor_settings_class)) << "\","
               << "\"subsystemInstance\":\"" << json_escape(safe_full_name(refs.subsystem_instance)) << "\","
               << "\"subsystemInstanceClass\":\"" << json_escape(object_class_full_name_noexcept(refs.subsystem_instance)) << "\","
               << "\"subsystemInstanceLooksCdo\":" << (refs.subsystem_instance_looks_cdo ? "true" : "false") << ","
               << "\"processEvent\":\"" << json_escape(refs.process_event_detail) << "\","
               << "\"assureServerModsUpdated\":\"" << json_escape(safe_full_name(refs.assure_server_mods_updated)) << "\","
               << "\"assureClientModsUpdated\":\"" << json_escape(safe_full_name(refs.assure_client_mods_updated)) << "\","
               << "\"getInstalledMods\":\"" << json_escape(safe_full_name(refs.get_installed_mods)) << "\","
               << "\"getModsDirInfo\":\"" << json_escape(safe_full_name(refs.get_mods_dir_info)) << "\"}";
        }

        struct WinHttpJsonResponse
        {
            bool ok{};
            DWORD status{};
            std::string body{};
            std::string error{};
            DWORD last_error{};
        };

        struct ScopedWinHttpHandle
        {
            HINTERNET value{};

            explicit ScopedWinHttpHandle(HINTERNET handle = nullptr) : value(handle) {}
            ~ScopedWinHttpHandle()
            {
                if (value != nullptr) WinHttpCloseHandle(value);
            }

            ScopedWinHttpHandle(const ScopedWinHttpHandle&) = delete;
            auto operator=(const ScopedWinHttpHandle&) -> ScopedWinHttpHandle& = delete;
            operator HINTERNET() const { return value; }
        };

        auto cfcore_find_settings_object_for_api(std::vector<std::string>& attempts) -> RC::Unreal::UObject*
        {
            RC::Unreal::UObject* fallback{};
            for (const auto* class_name : {L"CFCoreEditorSettings", L"UCFCoreEditorSettings", L"Object"})
            {
                for (const auto* object_path : {
                         L"/Script/cfcore.Default__CFCoreEditorSettings",
                         L"Default__CFCoreEditorSettings",
                         L"/Script/cfcore.CFCoreEditorSettings",
                         L"CFCoreEditorSettings",
                     })
                {
                    auto* object = find_object_exact_noexcept(class_name, object_path, 0, 0);
                    attempts.push_back(std::string("settings.FindObject(") + narrow(class_name) + "," + narrow(object_path) + ")=" + (object != nullptr ? safe_full_name(object) : "0"));
                    if (object == nullptr) continue;
                    if (fallback == nullptr) fallback = object;
                    if (looks_like_default_object(object)) return object;
                }
            }

            for (const auto* class_name : {L"CFCoreEditorSettings", L"UCFCoreEditorSettings"})
            {
                auto* object = cfcore_find_first_of_noexcept(class_name);
                attempts.push_back(std::string("settings.FindFirstOf(") + narrow(class_name) + ")=" + (object != nullptr ? safe_full_name(object) : "0"));
                if (object == nullptr) continue;
                if (fallback == nullptr) fallback = object;
                if (looks_like_default_object(object)) return object;
            }

            return fallback;
        }

        auto winhttp_read_response_body(HINTERNET request, std::string& body, std::string& error, DWORD& last_error) -> bool
        {
            for (;;)
            {
                DWORD available{};
                if (!WinHttpQueryDataAvailable(request, &available))
                {
                    last_error = GetLastError();
                    error = "WinHttpQueryDataAvailable failed";
                    return false;
                }
                if (available == 0) return true;

                std::vector<char> buffer(static_cast<std::size_t>(available));
                DWORD read{};
                if (!WinHttpReadData(request, buffer.data(), available, &read))
                {
                    last_error = GetLastError();
                    error = "WinHttpReadData failed";
                    return false;
                }
                body.append(buffer.data(), buffer.data() + read);
            }
        }

        auto winhttp_json_request(const wchar_t* method, const std::wstring& path, const std::string& api_key, const std::string& body, bool mod_server_request = true) -> WinHttpJsonResponse
        {
            WinHttpJsonResponse result;
            ScopedWinHttpHandle session(WinHttpOpen(
                L"cfcore.ue 1.35.0",
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                0));
            if (session.value == nullptr)
            {
                result.last_error = GetLastError();
                result.error = "WinHttpOpen failed";
                return result;
            }
            WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);

            ScopedWinHttpHandle connect(WinHttpConnect(session, L"93746.api.curseforge.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
            if (connect.value == nullptr)
            {
                result.last_error = GetLastError();
                result.error = "WinHttpConnect failed";
                return result;
            }

            ScopedWinHttpHandle request(WinHttpOpenRequest(
                connect,
                method,
                path.c_str(),
                nullptr,
                WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES,
                WINHTTP_FLAG_SECURE));
            if (request.value == nullptr)
            {
                result.last_error = GetLastError();
                result.error = "WinHttpOpenRequest failed";
                return result;
            }

            const auto headers =
                std::wstring(L"content-type: application/json\r\n") +
                L"user-agent: cfcore.ue 1.35.0\r\n" +
                L"x-platform: windows\r\n" +
                L"x-provider: Steam\r\n" +
                (mod_server_request ? std::wstring(L"x-mod-server-request: true\r\n") : std::wstring{}) +
                L"x-api-key: " + widen(api_key) + L"\r\n";

            auto* optional_body = body.empty() ? WINHTTP_NO_REQUEST_DATA : static_cast<LPVOID>(const_cast<char*>(body.data()));
            const auto body_size = static_cast<DWORD>(body.size());
            if (!WinHttpSendRequest(
                    request,
                    headers.c_str(),
                    static_cast<DWORD>(headers.size()),
                    optional_body,
                    body_size,
                    body_size,
                    0))
            {
                result.last_error = GetLastError();
                result.error = "WinHttpSendRequest failed";
                return result;
            }

            if (!WinHttpReceiveResponse(request, nullptr))
            {
                result.last_error = GetLastError();
                result.error = "WinHttpReceiveResponse failed";
                return result;
            }

            DWORD status{};
            DWORD status_size = sizeof(status);
            if (WinHttpQueryHeaders(
                    request,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX,
                    &status,
                    &status_size,
                    WINHTTP_NO_HEADER_INDEX))
            {
                result.status = status;
            }

            (void)winhttp_read_response_body(request, result.body, result.error, result.last_error);
            result.ok = result.status >= 200 && result.status < 300;
            return result;
        }

        auto find_matching_json_scope(const std::string& text, std::size_t start, char open_char, char close_char) -> std::size_t
        {
            if (start >= text.size() || text[start] != open_char) return std::string::npos;

            bool in_string = false;
            bool escaped = false;
            int depth = 0;
            for (std::size_t index = start; index < text.size(); ++index)
            {
                const auto ch = text[index];
                if (in_string)
                {
                    if (escaped)
                    {
                        escaped = false;
                    }
                    else if (ch == '\\')
                    {
                        escaped = true;
                    }
                    else if (ch == '"')
                    {
                        in_string = false;
                    }
                    continue;
                }

                if (ch == '"')
                {
                    in_string = true;
                    continue;
                }
                if (ch == open_char)
                {
                    ++depth;
                    continue;
                }
                if (ch == close_char)
                {
                    --depth;
                    if (depth == 0) return index;
                }
            }
            return std::string::npos;
        }

        auto json_string_field(const std::string& object, const char* key) -> std::string
        {
            const std::regex pattern(std::string("\"") + key + "\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"");
            std::smatch match;
            if (!std::regex_search(object, match, pattern) || match.size() < 2) return {};
            return match[1].str();
        }

        auto json_int64_field(const std::string& object, const char* key, std::int64_t& value) -> bool
        {
            const std::regex pattern(std::string("\"") + key + "\"\\s*:\\s*(-?\\d+)");
            std::smatch match;
            if (!std::regex_search(object, match, pattern) || match.size() < 2) return false;
            try
            {
                value = std::stoll(match[1].str());
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        auto json_array_for_key(const std::string& object, const char* key) -> std::string
        {
            const auto key_pos = object.find(std::string("\"") + key + "\"");
            if (key_pos == std::string::npos) return {};
            const auto open_pos = object.find('[', key_pos);
            if (open_pos == std::string::npos) return {};
            const auto close_pos = find_matching_json_scope(object, open_pos, '[', ']');
            if (close_pos == std::string::npos || close_pos <= open_pos) return {};
            return object.substr(open_pos, close_pos - open_pos + 1);
        }

        auto json_int64_values_for_key(const std::string& object, const char* key) -> std::vector<std::int64_t>
        {
            std::vector<std::int64_t> values;
            const std::regex pattern(std::string("\"") + key + "\"\\s*:\\s*(-?\\d+)");
            for (auto it = std::sregex_iterator(object.begin(), object.end(), pattern); it != std::sregex_iterator(); ++it)
            {
                try
                {
                    values.push_back(std::stoll((*it)[1].str()));
                }
                catch (...)
                {
                }
            }
            return values;
        }

        auto json_string_values_for_key(const std::string& object, const char* key, std::size_t max_items) -> std::vector<std::string>
        {
            std::vector<std::string> values;
            const std::regex pattern(std::string("\"") + key + "\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"");
            for (auto it = std::sregex_iterator(object.begin(), object.end(), pattern); it != std::sregex_iterator(); ++it)
            {
                values.push_back((*it)[1].str());
                if (values.size() >= max_items) break;
            }
            return values;
        }

        auto json_data_array_objects(const std::string& body, std::size_t max_items) -> std::vector<std::string>
        {
            std::vector<std::string> objects;
            const auto data_pos = body.find("\"data\"");
            if (data_pos == std::string::npos) return objects;
            const auto open_pos = body.find('[', data_pos);
            if (open_pos == std::string::npos) return objects;
            const auto close_pos = find_matching_json_scope(body, open_pos, '[', ']');
            if (close_pos == std::string::npos) return objects;

            auto pos = open_pos + 1;
            while (pos < close_pos && objects.size() < max_items)
            {
                const auto object_start = body.find('{', pos);
                if (object_start == std::string::npos || object_start >= close_pos) break;
                const auto object_end = find_matching_json_scope(body, object_start, '{', '}');
                if (object_end == std::string::npos || object_end > close_pos) break;
                objects.push_back(body.substr(object_start, object_end - object_start + 1));
                pos = object_end + 1;
            }
            return objects;
        }

        auto json_mod_object_by_id(const std::string& body, std::int64_t mod_id) -> std::string
        {
            const std::regex pattern(std::string("\"id\"\\s*:\\s*") + std::to_string(mod_id) + "(\\D|$)");
            std::smatch match;
            if (!std::regex_search(body, match, pattern)) return {};
            const auto id_pos = static_cast<std::size_t>(match.position());
            const auto object_start = body.rfind('{', id_pos);
            if (object_start == std::string::npos) return {};
            const auto object_end = find_matching_json_scope(body, object_start, '{', '}');
            if (object_end == std::string::npos) return {};
            return body.substr(object_start, object_end - object_start + 1);
        }

        auto append_cfcore_mod_summary_json(std::ostringstream& ss, const std::string& object, std::int64_t requested_id, bool requested) -> void
        {
            std::int64_t id{};
            const auto has_id = json_int64_field(object, "id", id);
            std::int64_t main_file_id{};
            const auto has_main_file_id = json_int64_field(object, "mainFileId", main_file_id);
            const auto latest_files = json_array_for_key(object, "latestFiles");
            auto latest_file_ids = json_int64_values_for_key(latest_files, "id");
            auto latest_file_names = json_string_values_for_key(latest_files, "fileName", 6);
            const auto main_in_latest = has_main_file_id && std::find(latest_file_ids.begin(), latest_file_ids.end(), main_file_id) != latest_file_ids.end();

            ss << "{\"requested\":" << (requested ? "true" : "false")
               << ",\"requestedId\":" << requested_id
               << ",\"found\":" << (object.empty() ? "false" : "true");
            if (!object.empty())
            {
                ss << ",\"idReadable\":" << (has_id ? "true" : "false");
                if (has_id) ss << ",\"id\":" << id;
                ss << ",\"name\":\"" << json_escape(json_string_field(object, "name")) << "\""
                   << ",\"slug\":\"" << json_escape(json_string_field(object, "slug")) << "\""
                   << ",\"mainFileIdReadable\":" << (has_main_file_id ? "true" : "false");
                if (has_main_file_id) ss << ",\"mainFileId\":" << main_file_id;
                ss << ",\"latestFilesCount\":" << latest_file_ids.size()
                   << ",\"latestFileIds\":";
                append_json_int64_array(ss, latest_file_ids);
                ss << ",\"latestFileNames\":";
                append_json_string_array(ss, latest_file_names);
                ss << ",\"mainFileInLatestFiles\":" << (main_in_latest ? "true" : "false");
            }
            ss << "}";
        }

        auto json_bool_field(const std::string& object, const char* key, bool& value) -> bool
        {
            const std::regex pattern(std::string("\"") + key + "\"\\s*:\\s*(true|false)");
            std::smatch match;
            if (!std::regex_search(object, match, pattern) || match.size() < 2) return false;
            value = match[1].str() == "true";
            return true;
        }

        auto url_host_only(const std::string& url) -> std::string
        {
            const auto scheme_pos = url.find("://");
            auto start = scheme_pos == std::string::npos ? 0 : scheme_pos + 3;
            const auto slash_pos = url.find('/', start);
            if (slash_pos == std::string::npos) return url.substr(start);
            return url.substr(start, slash_pos - start);
        }

        auto append_cfcore_file_summary_json(std::ostringstream& ss, const std::string& object, std::int64_t requested_id, bool requested, bool include_download_url) -> void
        {
            std::int64_t id{};
            const auto has_id = json_int64_field(object, "id", id);
            std::int64_t game_id{};
            const auto has_game_id = json_int64_field(object, "gameId", game_id);
            std::int64_t mod_id{};
            const auto has_mod_id = json_int64_field(object, "modId", mod_id);
            std::int64_t file_length{};
            const auto has_file_length = json_int64_field(object, "fileLength", file_length);
            std::int64_t file_size_on_disk{};
            const auto has_file_size_on_disk = json_int64_field(object, "fileSizeOnDisk", file_size_on_disk);
            std::int64_t file_fingerprint{};
            const auto has_file_fingerprint = json_int64_field(object, "fileFingerprint", file_fingerprint);
            std::int64_t release_type{};
            const auto has_release_type = json_int64_field(object, "releaseType", release_type);
            std::int64_t file_status{};
            const auto has_file_status = json_int64_field(object, "fileStatus", file_status);
            bool is_available{};
            const auto has_is_available = json_bool_field(object, "isAvailable", is_available);
            bool is_server_pack{};
            const auto has_is_server_pack = json_bool_field(object, "isServerPack", is_server_pack);
            const auto download_url = json_string_field(object, "downloadUrl");
            const auto modules = json_array_for_key(object, "modules");
            const auto module_names = json_string_values_for_key(modules, "name", 8);
            const auto module_fingerprints = json_int64_values_for_key(modules, "fingerprint");
            const auto hashes = json_array_for_key(object, "hashes");
            const auto hash_values = json_string_values_for_key(hashes, "value", 6);
            const auto hash_algos = json_int64_values_for_key(hashes, "algo");
            const auto game_versions = json_array_for_key(object, "gameVersions");
            const auto game_version_samples = json_string_values_for_key(game_versions, "", 6);

            ss << "{\"requested\":" << (requested ? "true" : "false")
               << ",\"requestedId\":" << requested_id
               << ",\"found\":" << (object.empty() ? "false" : "true");
            if (!object.empty())
            {
                ss << ",\"idReadable\":" << (has_id ? "true" : "false");
                if (has_id) ss << ",\"id\":" << id;
                ss << ",\"gameIdReadable\":" << (has_game_id ? "true" : "false");
                if (has_game_id) ss << ",\"gameId\":" << game_id;
                ss << ",\"modIdReadable\":" << (has_mod_id ? "true" : "false");
                if (has_mod_id) ss << ",\"modId\":" << mod_id;
                ss << ",\"displayName\":\"" << json_escape(json_string_field(object, "displayName")) << "\""
                   << ",\"fileName\":\"" << json_escape(json_string_field(object, "fileName")) << "\""
                   << ",\"isAvailableReadable\":" << (has_is_available ? "true" : "false");
                if (has_is_available) ss << ",\"isAvailable\":" << (is_available ? "true" : "false");
                ss << ",\"isServerPackReadable\":" << (has_is_server_pack ? "true" : "false");
                if (has_is_server_pack) ss << ",\"isServerPack\":" << (is_server_pack ? "true" : "false");
                ss << ",\"releaseTypeReadable\":" << (has_release_type ? "true" : "false");
                if (has_release_type) ss << ",\"releaseType\":" << release_type;
                ss << ",\"fileStatusReadable\":" << (has_file_status ? "true" : "false");
                if (has_file_status) ss << ",\"fileStatus\":" << file_status;
                ss << ",\"fileLengthReadable\":" << (has_file_length ? "true" : "false");
                if (has_file_length) ss << ",\"fileLength\":" << file_length;
                ss << ",\"fileSizeOnDiskReadable\":" << (has_file_size_on_disk ? "true" : "false");
                if (has_file_size_on_disk) ss << ",\"fileSizeOnDisk\":" << file_size_on_disk;
                ss << ",\"fileFingerprintReadable\":" << (has_file_fingerprint ? "true" : "false");
                if (has_file_fingerprint) ss << ",\"fileFingerprint\":" << file_fingerprint;
                ss << ",\"downloadUrlPresent\":" << (!download_url.empty() ? "true" : "false")
                   << ",\"downloadUrlLength\":" << download_url.size()
                   << ",\"downloadHost\":\"" << json_escape(url_host_only(download_url)) << "\""
                   << ",\"downloadUrlExposed\":" << (include_download_url ? "true" : "false");
                if (include_download_url)
                {
                    ss << ",\"downloadUrl\":\"" << json_escape(download_url) << "\"";
                }
                ss
                   << ",\"modulesCount\":" << module_names.size()
                   << ",\"moduleNames\":";
                append_json_string_array(ss, module_names);
                ss << ",\"moduleFingerprints\":";
                append_json_int64_array(ss, module_fingerprints);
                ss << ",\"hashesCount\":" << hash_values.size()
                   << ",\"hashValues\":";
                append_json_string_array(ss, hash_values);
                ss << ",\"hashAlgos\":";
                append_json_int64_array(ss, hash_algos);
                ss << ",\"gameVersionsRawBytes\":" << game_versions.size()
                   << ",\"gameVersionSamples\":";
                append_json_string_array(ss, game_version_samples);
            }
            ss << "}";
        }

        auto cfcore_search_path_from_command(const std::string& command_text) -> std::wstring
        {
            auto index = static_cast<int>(regex_number(command_text, "index", 0.0));
            auto page_size = static_cast<int>(regex_number(command_text, "pageSize", 20.0));
            auto sort_field = static_cast<int>(regex_number(command_text, "sortField", 3.0));
            if (index < 0) index = 0;
            if (page_size < 1) page_size = 1;
            if (page_size > 100) page_size = 100;
            if (sort_field < 0 || sort_field > 12) sort_field = 3;

            std::ostringstream query;
            query << "/v1/mods/search?index=" << index
                  << "&pageSize=" << page_size
                  << "&sortField=" << sort_field
                  << "&sortOrder=desc"
                  << "&gameId=93746";

            const auto search_filter = regex_value(command_text, "searchFilter");
            if (!search_filter.empty())
            {
                query << "&searchFilter=";
                for (const auto ch : search_filter)
                {
                    if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-' || ch == '.')
                    {
                        query << ch;
                    }
                    else if (ch == ' ')
                    {
                        query << "%20";
                    }
                    else
                    {
                        query << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(ch)) << std::nouppercase << std::dec;
                    }
                }
            }

            return widen(query.str());
        }
    }

    auto SCUMTraderManager::cfcore_native_status_probe() const -> std::pair<bool, std::string>
    {
        const auto started = std::chrono::steady_clock::now();
        const auto refs = cfcore_resolve_native_refs();
        const auto has_required_route =
            refs.subsystem_class != nullptr &&
            refs.assure_server_mods_updated != nullptr &&
            refs.assure_client_mods_updated != nullptr;
        const auto has_live_instance =
            refs.subsystem_instance != nullptr &&
            !refs.subsystem_instance_looks_cdo &&
            !refs.process_event_detail.empty() &&
            refs.process_event_detail.find("processEvent=") != std::string::npos;
        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

        std::ostringstream ss;
        ss << "{\"ok\":" << (has_required_route ? "true" : "false") << ","
           << "\"route\":\"cfcore-native-status-probe\","
           << "\"version\":\"" << mod_version << "\","
           << "\"durationMs\":" << duration_ms << ","
           << "\"readOnly\":true,"
           << "\"mutationCalls\":false,"
           << "\"broadScan\":false,"
           << "\"narrowFunctionNameScan\":true,"
           << "\"engineAccess\":\"exact FindObject for CFCore classes/functions plus single-class FindFirstOf(CFCoreSubsystem)\","
           << "\"hasRequiredRoute\":" << (has_required_route ? "true" : "false") << ","
           << "\"hasLiveSubsystemInstance\":" << (has_live_instance ? "true" : "false") << ","
           << "\"abi\":{\"TArrayAbi\":" << sizeof(TArrayAbi)
           << ",\"FScriptDelegateAbi\":" << sizeof(FScriptDelegateAbi)
           << ",\"FAssureServerModsUpdatedParamsAbi\":" << sizeof(FAssureServerModsUpdatedParamsAbi)
           << ",\"AssureServerModsUpdatedProcessEventParamsAbi\":" << sizeof(AssureServerModsUpdatedProcessEventParamsAbi) << "},";
        append_cfcore_refs_json(ss, refs);
        ss << ",\"attempts\":";
        append_json_string_array(ss, refs.attempts);
        ss << "}";
        return {has_required_route, ss.str()};
    }

    auto SCUMTraderManager::cfcore_editor_settings_probe() const -> std::pair<bool, std::string>
    {
        const auto started = std::chrono::steady_clock::now();
        std::vector<std::string> attempts;

        auto find_settings_object = [&attempts]() -> RC::Unreal::UObject*
        {
            RC::Unreal::UObject* fallback{};
            for (const auto* class_name : {L"CFCoreEditorSettings", L"UCFCoreEditorSettings", L"Object"})
            {
                for (const auto* object_path : {
                         L"/Script/cfcore.Default__CFCoreEditorSettings",
                         L"Default__CFCoreEditorSettings",
                         L"/Script/cfcore.CFCoreEditorSettings",
                         L"CFCoreEditorSettings",
                     })
                {
                    auto* object = find_object_exact_noexcept(class_name, object_path, 0, 0);
                    attempts.push_back(std::string("settings.FindObject(") + narrow(class_name) + "," + narrow(object_path) + ")=" + (object != nullptr ? safe_full_name(object) : "0"));
                    if (object == nullptr) continue;
                    if (fallback == nullptr) fallback = object;
                    if (looks_like_default_object(object)) return object;
                }
            }

            for (const auto* class_name : {L"CFCoreEditorSettings", L"UCFCoreEditorSettings"})
            {
                auto* object = cfcore_find_first_of_noexcept(class_name);
                attempts.push_back(std::string("settings.FindFirstOf(") + narrow(class_name) + ")=" + (object != nullptr ? safe_full_name(object) : "0"));
                if (object == nullptr) continue;
                if (fallback == nullptr) fallback = object;
                if (looks_like_default_object(object)) return object;
            }

            return fallback;
        };

        auto* settings = find_settings_object();
        auto* settings_class = cfcore_find_object_exact(
            L"Class",
            {L"/Script/cfcore.CFCoreEditorSettings", L"CFCoreEditorSettings"},
            "settingsClass",
            attempts);

        auto property_address_info = [](RC::Unreal::UObject* object, const wchar_t* property_name, bool& present, bool& readable, std::uintptr_t& offset) -> std::uintptr_t
        {
            present = false;
            readable = false;
            offset = 0;
            bool missing = false;
            std::uintptr_t address{};
            if (!property_address_noexcept(object, property_name, address, missing))
            {
                present = !missing;
                return 0;
            }
            present = true;
            readable = committed_readable_address(address);
            const auto base = reinterpret_cast<std::uintptr_t>(object);
            if (base != 0 && address >= base && address - base < 0x20000)
            {
                offset = address - base;
            }
            return address;
        };

        auto append_string_setting = [&property_address_info](std::ostringstream& ss, const char* json_name, RC::Unreal::UObject* object, const wchar_t* property_name, bool& first, bool sensitive) -> bool
        {
            if (!first) ss << ",";
            first = false;

            bool present = false;
            bool address_readable = false;
            std::uintptr_t offset = 0;
            property_address_info(object, property_name, present, address_readable, offset);

            bool missing = false;
            std::string value;
            const auto readable = string_property_noexcept(object, property_name, value, missing);
            ss << "\"" << json_name << "\":{\"present\":" << (present && !missing ? "true" : "false")
               << ",\"readable\":" << (readable ? "true" : "false")
               << ",\"addressReadable\":" << (address_readable ? "true" : "false")
               << ",\"offset\":" << offset;
            if (readable)
            {
                ss << ",\"length\":" << value.size()
                   << ",\"empty\":" << (value.empty() ? "true" : "false");
                if (!sensitive)
                {
                    ss << ",\"value\":\"" << json_escape(value) << "\"";
                }
                else
                {
                    ss << ",\"masked\":\"" << (value.empty() ? std::string{} : std::string("***")) << "\"";
                }
            }
            ss << "}";
            return readable;
        };

        auto append_bool_setting = [&property_address_info](std::ostringstream& ss, const char* json_name, RC::Unreal::UObject* object, const wchar_t* property_name, bool& first, bool& value_out) -> bool
        {
            if (!first) ss << ",";
            first = false;

            bool present = false;
            bool address_readable = false;
            std::uintptr_t offset = 0;
            const auto address = property_address_info(object, property_name, present, address_readable, offset);

            std::uint8_t raw{};
            const auto readable = address != 0 && committed_readable_address(address, sizeof(raw)) && safe_read(address, raw);
            if (readable) value_out = raw != 0;
            ss << "\"" << json_name << "\":{\"present\":" << (present ? "true" : "false")
               << ",\"readable\":" << (readable ? "true" : "false")
               << ",\"addressReadable\":" << (address_readable ? "true" : "false")
               << ",\"offset\":" << offset;
            if (readable)
            {
                ss << ",\"value\":" << (raw != 0 ? "true" : "false")
                   << ",\"raw\":" << static_cast<int>(raw);
            }
            ss << "}";
            return readable;
        };

        auto append_uint8_setting = [&property_address_info](std::ostringstream& ss, const char* json_name, RC::Unreal::UObject* object, const wchar_t* property_name, bool& first, int& value_out) -> bool
        {
            if (!first) ss << ",";
            first = false;

            bool present = false;
            bool address_readable = false;
            std::uintptr_t offset = 0;
            const auto address = property_address_info(object, property_name, present, address_readable, offset);

            std::uint8_t raw{};
            const auto readable = address != 0 && committed_readable_address(address, sizeof(raw)) && safe_read(address, raw);
            if (readable) value_out = static_cast<int>(raw);
            ss << "\"" << json_name << "\":{\"present\":" << (present ? "true" : "false")
               << ",\"readable\":" << (readable ? "true" : "false")
               << ",\"addressReadable\":" << (address_readable ? "true" : "false")
               << ",\"offset\":" << offset;
            if (readable)
            {
                ss << ",\"value\":" << static_cast<int>(raw);
            }
            ss << "}";
            return readable;
        };

        auto append_int32_setting = [&property_address_info](std::ostringstream& ss, const char* json_name, RC::Unreal::UObject* object, const wchar_t* property_name, bool& first, std::int32_t& value_out) -> bool
        {
            if (!first) ss << ",";
            first = false;

            bool present = false;
            bool address_readable = false;
            std::uintptr_t offset = 0;
            const auto address = property_address_info(object, property_name, present, address_readable, offset);

            std::int32_t raw{};
            const auto readable = address != 0 && committed_readable_address(address, sizeof(raw)) && safe_read(address, raw);
            if (readable) value_out = raw;
            ss << "\"" << json_name << "\":{\"present\":" << (present ? "true" : "false")
               << ",\"readable\":" << (readable ? "true" : "false")
               << ",\"addressReadable\":" << (address_readable ? "true" : "false")
               << ",\"offset\":" << offset;
            if (readable)
            {
                ss << ",\"value\":" << raw;
            }
            ss << "}";
            return readable;
        };

        auto append_int64_setting = [&property_address_info](std::ostringstream& ss, const char* json_name, RC::Unreal::UObject* object, const wchar_t* property_name, bool& first, std::int64_t& value_out) -> bool
        {
            if (!first) ss << ",";
            first = false;

            bool present = false;
            bool address_readable = false;
            std::uintptr_t offset = 0;
            const auto address = property_address_info(object, property_name, present, address_readable, offset);

            std::int64_t raw{};
            const auto readable = address != 0 && committed_readable_address(address, sizeof(raw)) && safe_read(address, raw);
            if (readable) value_out = raw;
            ss << "\"" << json_name << "\":{\"present\":" << (present ? "true" : "false")
               << ",\"readable\":" << (readable ? "true" : "false")
               << ",\"addressReadable\":" << (address_readable ? "true" : "false")
               << ",\"offset\":" << offset;
            if (readable)
            {
                ss << ",\"value\":" << raw;
            }
            ss << "}";
            return readable;
        };

        bool is_server = false;
        bool is_server_pc_only = false;
        int mods_directory_mode = -1;
        int provider = -1;
        std::int32_t max_concurrent_installations = 0;
        std::int64_t game_id = 0;

        bool first_property = true;
        std::ostringstream properties;
        properties << "{";
        const auto default_language_readable = append_string_setting(properties, "defaultLanguage", settings, L"defaultLanguage", first_property, false);
        const auto game_id_readable = append_int64_setting(properties, "gameId", settings, L"gameId", first_property, game_id);
        const auto api_key_readable = append_string_setting(properties, "apiKey", settings, L"apiKey", first_property, true);
        const auto provider_readable = append_uint8_setting(properties, "provider", settings, L"provider", first_property, provider);
        const auto max_concurrent_readable = append_int32_setting(properties, "maxConcurrentInstallations", settings, L"maxConcurrentInstallations", first_property, max_concurrent_installations);
        const auto mods_directory_readable = append_string_setting(properties, "modsDirectory", settings, L"modsDirectory", first_property, false);
        const auto mods_directory_mode_readable = append_uint8_setting(properties, "modsDirectoryMode", settings, L"modsDirectoryMode", first_property, mods_directory_mode);
        const auto user_data_directory_readable = append_string_setting(properties, "userDataDirectory", settings, L"userDataDirectory", first_property, false);
        const auto is_server_readable = append_bool_setting(properties, "isServer", settings, L"isServer", first_property, is_server);
        const auto is_server_pc_only_readable = append_bool_setting(properties, "isServerPcOnly", settings, L"isServerPcOnly", first_property, is_server_pc_only);
        properties << "}";

        const auto has_settings = settings != nullptr;
        const auto route_ready =
            has_settings &&
            game_id_readable &&
            mods_directory_readable &&
            mods_directory_mode_readable &&
            user_data_directory_readable &&
            is_server_readable;
        const auto looks_server_ready =
            route_ready &&
            game_id > 0 &&
            mods_directory_mode != 0 &&
            is_server;
        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

        std::ostringstream ss;
        ss << "{\"ok\":" << (route_ready ? "true" : "false") << ","
           << "\"route\":\"cfcore-editor-settings-probe\","
           << "\"version\":\"" << mod_version << "\","
           << "\"durationMs\":" << duration_ms << ","
           << "\"readOnly\":true,"
           << "\"mutationCalls\":false,"
           << "\"broadScan\":false,"
           << "\"engineAccess\":\"exact FindObject for CFCoreEditorSettings CDO plus single-class FindFirstOf(CFCoreEditorSettings)\","
           << "\"hasSettingsObject\":" << (has_settings ? "true" : "false") << ","
           << "\"looksDefaultObject\":" << (looks_like_default_object(settings) ? "true" : "false") << ","
           << "\"looksServerReady\":" << (looks_server_ready ? "true" : "false") << ","
           << "\"settingsObject\":\"" << json_escape(safe_full_name(settings)) << "\","
           << "\"settingsClass\":\"" << json_escape(object_class_full_name_noexcept(settings)) << "\","
           << "\"settingsClassObject\":\"" << json_escape(safe_full_name(settings_class)) << "\","
           << "\"summary\":{"
           << "\"gameIdReadable\":" << (game_id_readable ? "true" : "false") << ","
           << "\"gameId\":" << (game_id_readable ? std::to_string(game_id) : std::string("0")) << ","
           << "\"apiKeyReadable\":" << (api_key_readable ? "true" : "false") << ","
           << "\"providerReadable\":" << (provider_readable ? "true" : "false") << ","
           << "\"provider\":" << provider << ","
           << "\"modsDirectoryReadable\":" << (mods_directory_readable ? "true" : "false") << ","
           << "\"modsDirectoryModeReadable\":" << (mods_directory_mode_readable ? "true" : "false") << ","
           << "\"modsDirectoryMode\":" << mods_directory_mode << ","
           << "\"userDataDirectoryReadable\":" << (user_data_directory_readable ? "true" : "false") << ","
           << "\"isServerReadable\":" << (is_server_readable ? "true" : "false") << ","
           << "\"isServer\":" << (is_server ? "true" : "false") << ","
           << "\"isServerPcOnlyReadable\":" << (is_server_pc_only_readable ? "true" : "false") << ","
           << "\"isServerPcOnly\":" << (is_server_pc_only ? "true" : "false") << ","
           << "\"maxConcurrentInstallationsReadable\":" << (max_concurrent_readable ? "true" : "false") << ","
           << "\"maxConcurrentInstallations\":" << max_concurrent_installations << ","
           << "\"defaultLanguageReadable\":" << (default_language_readable ? "true" : "false")
           << "},"
           << "\"properties\":" << properties.str() << ","
           << "\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << "}";

        return {route_ready, ss.str()};
    }

    auto SCUMTraderManager::cfcore_api_mods_summary_probe(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        const auto started = std::chrono::steady_clock::now();
        std::vector<std::string> attempts;

        auto* settings = cfcore_find_settings_object_for_api(attempts);

        bool missing_api_key = false;
        std::string api_key;
        const auto api_key_readable = string_property_noexcept(settings, L"apiKey", api_key, missing_api_key);
        const auto mode = regex_value(command_text, "mode");
        const auto use_search = lower_copy(mode) == "search";
        auto max_items = static_cast<int>(regex_number(command_text, "maxItems", use_search ? 20.0 : 10.0));
        if (max_items < 1) max_items = 1;
        if (max_items > 100) max_items = 100;

        auto mod_ids = first_regex_int64_list(command_text, {"modIds", "mods"}, {"modId"});
        if (!use_search && mod_ids.empty())
        {
            mod_ids = {1380356, 1513314};
        }

        WinHttpJsonResponse response;
        std::string request_body;
        std::wstring request_path;
        if (api_key_readable && !api_key.empty())
        {
            if (use_search)
            {
                request_path = cfcore_search_path_from_command(command_text);
                response = winhttp_json_request(L"GET", request_path, api_key, {});
            }
            else
            {
                std::ostringstream body;
                body << "{\"modIds\":";
                append_json_int64_array(body, mod_ids);
                body << ",\"devModIds\":[],\"filterPcOnly\":" << (regex_bool(command_text, "filterPcOnly", false) ? "true" : "false") << "}";
                request_body = body.str();
                request_path = L"/v1/mods";
                response = winhttp_json_request(L"POST", request_path, api_key, request_body);
            }
        }

        const auto request_ok = api_key_readable && !api_key.empty();
        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

        std::ostringstream summaries;
        summaries << "[";
        bool first = true;
        if (use_search)
        {
            const auto objects = response.ok ? json_data_array_objects(response.body, static_cast<std::size_t>(max_items)) : std::vector<std::string>{};
            for (const auto& object : objects)
            {
                if (!first) summaries << ",";
                first = false;
                append_cfcore_mod_summary_json(summaries, object, 0, false);
            }
        }
        else
        {
            for (const auto mod_id : mod_ids)
            {
                if (!first) summaries << ",";
                first = false;
                append_cfcore_mod_summary_json(summaries, response.ok ? json_mod_object_by_id(response.body, mod_id) : std::string{}, mod_id, true);
            }
        }
        summaries << "]";

        const auto ok = request_ok && response.ok;
        const auto body_snippet = response.ok ? std::string{} : response.body.substr(0, 600);
        std::ostringstream ss;
        ss << "{\"ok\":" << (ok ? "true" : "false") << ","
           << "\"route\":\"cfcore-api-mods-summary-probe\","
           << "\"version\":\"" << mod_version << "\","
           << "\"durationMs\":" << duration_ms << ","
           << "\"readOnly\":true,"
           << "\"mutationCalls\":false,"
           << "\"apiKeyReadable\":" << (api_key_readable ? "true" : "false") << ","
           << "\"apiKeyPresent\":" << (!api_key.empty() ? "true" : "false") << ","
           << "\"apiKeyLength\":" << api_key.size() << ","
           << "\"apiKeyExposed\":false,"
           << "\"settingsObject\":\"" << json_escape(safe_full_name(settings)) << "\","
           << "\"mode\":\"" << json_escape(use_search ? "search" : "ids") << "\","
           << "\"requestPath\":\"" << json_escape(narrow(request_path)) << "\","
           << "\"requestedModIds\":";
        append_json_int64_array(ss, mod_ids);
        ss << ",\"http\":{\"attempted\":" << (request_ok ? "true" : "false")
           << ",\"ok\":" << (response.ok ? "true" : "false")
           << ",\"status\":" << response.status
           << ",\"bodyBytes\":" << response.body.size()
           << ",\"lastError\":" << response.last_error
           << ",\"error\":\"" << json_escape(response.error) << "\"";
        if (!body_snippet.empty())
        {
            ss << ",\"bodySnippet\":\"" << json_escape(body_snippet) << "\"";
        }
        ss << "},\"summaries\":" << summaries.str()
           << ",\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << "}";

        return {ok, ss.str()};
    }

    auto SCUMTraderManager::cfcore_api_files_summary_probe(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        const auto started = std::chrono::steady_clock::now();
        std::vector<std::string> attempts;

        auto* settings = cfcore_find_settings_object_for_api(attempts);

        bool missing_api_key = false;
        std::string api_key;
        const auto api_key_readable = string_property_noexcept(settings, L"apiKey", api_key, missing_api_key);
        const auto mode = lower_copy(regex_value(command_text, "mode"));
        const auto use_get_files = mode == "files" || mode == "get" || mode == "ids";
        const auto endpoint_kind = use_get_files ? "files" : "match-platform";
        const auto simulate_client_headers = regex_bool(command_text, "simulateClientHeaders", false) || regex_bool(command_text, "clientHeaders", false);
        const auto include_download_url = regex_bool(command_text, "includeDownloadUrl", false) || regex_bool(command_text, "exposeDownloadUrl", false);
        const auto send_mod_server_request_header = !simulate_client_headers;
        auto max_items = static_cast<int>(regex_number(command_text, "maxItems", 20.0));
        if (max_items < 1) max_items = 1;
        if (max_items > 100) max_items = 100;

        auto file_ids = first_regex_int64_list(
            command_text,
            {"fileIds", "file_ids", "serverFileIds", "server_file_ids", "cfcoreFileIds"},
            {"fileId", "serverFileId", "cfcoreFileId"});
        const auto use_public_test_default = regex_bool(command_text, "usePublicTestFileDefault", true);
        if (file_ids.empty() && use_public_test_default)
        {
            file_ids = {8142191};
        }

        WinHttpJsonResponse response;
        std::string request_body;
        const auto request_path = std::wstring(use_get_files ? L"/v1/mods/files" : L"/v1/mods/files/match-platform");
        if (api_key_readable && !api_key.empty() && !file_ids.empty())
        {
            std::ostringstream body;
            body << "{\"fileIds\":";
            append_json_int64_array(body, file_ids);
            body << "}";
            request_body = body.str();
            response = winhttp_json_request(L"POST", request_path, api_key, request_body, send_mod_server_request_header);
        }

        const auto request_ok = api_key_readable && !api_key.empty() && !file_ids.empty();
        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

        const auto objects = response.ok ? json_data_array_objects(response.body, static_cast<std::size_t>(max_items)) : std::vector<std::string>{};
        std::ostringstream summaries;
        summaries << "[";
        bool first = true;
        if (!objects.empty())
        {
            for (const auto& object : objects)
            {
                if (!first) summaries << ",";
                first = false;
                std::int64_t object_id{};
                const auto has_object_id = json_int64_field(object, "id", object_id);
                const auto requested = has_object_id && std::find(file_ids.begin(), file_ids.end(), object_id) != file_ids.end();
                append_cfcore_file_summary_json(summaries, object, has_object_id ? object_id : 0, requested, include_download_url);
            }
        }
        else
        {
            for (const auto file_id : file_ids)
            {
                if (!first) summaries << ",";
                first = false;
                append_cfcore_file_summary_json(summaries, {}, file_id, true, include_download_url);
            }
        }
        summaries << "]";

        const auto ok = request_ok && response.ok;
        const auto body_snippet = response.ok ? std::string{} : response.body.substr(0, 600);
        std::ostringstream ss;
        ss << "{\"ok\":" << (ok ? "true" : "false") << ","
           << "\"route\":\"cfcore-api-files-summary-probe\","
           << "\"version\":\"" << mod_version << "\","
           << "\"durationMs\":" << duration_ms << ","
           << "\"readOnly\":true,"
           << "\"mutationCalls\":false,"
           << "\"apiKeyReadable\":" << (api_key_readable ? "true" : "false") << ","
           << "\"apiKeyPresent\":" << (!api_key.empty() ? "true" : "false") << ","
           << "\"apiKeyLength\":" << api_key.size() << ","
           << "\"apiKeyExposed\":false,"
           << "\"settingsObject\":\"" << json_escape(safe_full_name(settings)) << "\","
           << "\"mode\":\"" << json_escape(endpoint_kind) << "\","
           << "\"simulateClientHeaders\":" << (simulate_client_headers ? "true" : "false") << ","
           << "\"downloadUrlExposed\":" << (include_download_url ? "true" : "false") << ","
           << "\"modServerRequestHeader\":" << (send_mod_server_request_header ? "true" : "false") << ","
           << "\"requestPath\":\"" << json_escape(narrow(request_path)) << "\","
           << "\"requestBodyExposed\":false,"
           << "\"defaultedPublicTestFile\":" << (file_ids.size() == 1 && file_ids.front() == 8142191 && use_public_test_default ? "true" : "false") << ","
           << "\"requestedFileIds\":";
        append_json_int64_array(ss, file_ids);
        ss << ",\"http\":{\"attempted\":" << (request_ok ? "true" : "false")
           << ",\"ok\":" << (response.ok ? "true" : "false")
           << ",\"status\":" << response.status
           << ",\"bodyBytes\":" << response.body.size()
           << ",\"lastError\":" << response.last_error
           << ",\"error\":\"" << json_escape(response.error) << "\"";
        if (!body_snippet.empty())
        {
            ss << ",\"bodySnippet\":\"" << json_escape(body_snippet) << "\"";
        }
        ss << "},\"filesCount\":" << objects.size()
           << ",\"files\":" << summaries.str()
           << ",\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << "}";

        return {ok, ss.str()};
    }

    auto SCUMTraderManager::cfcore_configure_server_runtime(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        constexpr std::size_t cfcore_settings_abi_bytes = 2048;
        constexpr std::uintptr_t fcfcore_settings_default_language_offset = 16;

        struct CFCoreInitializeProcessEventParamsAbi
        {
            std::array<std::uint8_t, cfcore_settings_abi_bytes> settings{};
            FScriptDelegateAbi on_initialized{};
            FScriptDelegateAbi on_error{};
        };

        const auto started = std::chrono::steady_clock::now();
        std::vector<std::string> attempts;

        auto find_settings_object = [&attempts]() -> RC::Unreal::UObject*
        {
            RC::Unreal::UObject* fallback{};
            for (const auto* class_name : {L"CFCoreEditorSettings", L"UCFCoreEditorSettings", L"Object"})
            {
                for (const auto* object_path : {
                         L"/Script/cfcore.Default__CFCoreEditorSettings",
                         L"Default__CFCoreEditorSettings",
                         L"/Script/cfcore.CFCoreEditorSettings",
                         L"CFCoreEditorSettings",
                     })
                {
                    auto* object = find_object_exact_noexcept(class_name, object_path, 0, 0);
                    attempts.push_back(std::string("settings.FindObject(") + narrow(class_name) + "," + narrow(object_path) + ")=" + (object != nullptr ? safe_full_name(object) : "0"));
                    if (object == nullptr) continue;
                    if (fallback == nullptr) fallback = object;
                    if (looks_like_default_object(object)) return object;
                }
            }
            for (const auto* class_name : {L"CFCoreEditorSettings", L"UCFCoreEditorSettings"})
            {
                auto* object = cfcore_find_first_of_noexcept(class_name);
                attempts.push_back(std::string("settings.FindFirstOf(") + narrow(class_name) + ")=" + (object != nullptr ? safe_full_name(object) : "0"));
                if (object == nullptr) continue;
                if (fallback == nullptr) fallback = object;
                if (looks_like_default_object(object)) return object;
            }
            return fallback;
        };

        auto write_string_property = [&attempts](RC::Unreal::UObject* object, const wchar_t* property_name, const char* storage_key, const std::string& value) -> bool
        {
            bool missing = false;
            std::uintptr_t address{};
            if (!property_address_noexcept(object, property_name, address, missing))
            {
                attempts.push_back(std::string("writeString.") + storage_key + "=" + (missing ? "missing" : "address-failed"));
                return false;
            }
            if (!committed_readable_address(address, sizeof(TArrayAbi)))
            {
                attempts.push_back(std::string("writeString.") + storage_key + "=unreadable-address");
                return false;
            }
            auto wide_value = widen(value);
            wide_value.push_back(L'\0');
            auto& storage = g_cfcore_runtime_string_storage[storage_key];
            storage = std::move(wide_value);

            TArrayAbi array{};
            array.data = static_cast<void*>(storage.data());
            array.count = static_cast<std::int32_t>(storage.size());
            array.max = static_cast<std::int32_t>(storage.size());
            const auto ok = safe_write(address, array);
            attempts.push_back(std::string("writeString.") + storage_key + "=" + (ok ? "ok" : "write-failed"));
            return ok;
        };

        auto write_u8_property = [&attempts](RC::Unreal::UObject* object, const wchar_t* property_name, const char* label, std::uint8_t value) -> bool
        {
            bool missing = false;
            std::uintptr_t address{};
            if (!property_address_noexcept(object, property_name, address, missing))
            {
                attempts.push_back(std::string("writeU8.") + label + "=" + (missing ? "missing" : "address-failed"));
                return false;
            }
            if (!committed_readable_address(address, sizeof(value)))
            {
                attempts.push_back(std::string("writeU8.") + label + "=unreadable-address");
                return false;
            }
            const auto ok = safe_write(address, value);
            attempts.push_back(std::string("writeU8.") + label + "=" + (ok ? "ok" : "write-failed"));
            return ok;
        };

        auto write_i32_property = [&attempts](RC::Unreal::UObject* object, const wchar_t* property_name, const char* label, std::int32_t value) -> bool
        {
            bool missing = false;
            std::uintptr_t address{};
            if (!property_address_noexcept(object, property_name, address, missing))
            {
                attempts.push_back(std::string("writeI32.") + label + "=" + (missing ? "missing" : "address-failed"));
                return false;
            }
            if (!committed_readable_address(address, sizeof(value)))
            {
                attempts.push_back(std::string("writeI32.") + label + "=unreadable-address");
                return false;
            }
            const auto ok = safe_write(address, value);
            attempts.push_back(std::string("writeI32.") + label + "=" + (ok ? "ok" : "write-failed"));
            return ok;
        };

        auto copy_settings_from_cdo = [&attempts](RC::Unreal::UObject* settings_object, CFCoreInitializeProcessEventParamsAbi& params) -> bool
        {
            bool missing = false;
            std::uintptr_t default_language_address{};
            if (!property_address_noexcept(settings_object, L"defaultLanguage", default_language_address, missing))
            {
                attempts.push_back(std::string("copySettings.defaultLanguageAddress=") + (missing ? "missing" : "failed"));
                return false;
            }
            if (default_language_address < fcfcore_settings_default_language_offset)
            {
                attempts.push_back("copySettings.source=underflow");
                return false;
            }
            const auto source = default_language_address - fcfcore_settings_default_language_offset;
            if (!committed_readable_address(source, cfcore_settings_abi_bytes))
            {
                attempts.push_back("copySettings.source=unreadable");
                return false;
            }
            if (safe_copy_from_address(params.settings.data(), source, cfcore_settings_abi_bytes))
            {
                attempts.push_back("copySettings=ok");
                return true;
            }
            attempts.push_back("copySettings=exception");
            return false;
        };

        auto* settings = find_settings_object();
        const auto refs = cfcore_resolve_native_refs();
        auto* initialize_function = cfcore_find_function_exact(
            {
                L"/Script/cfcore.CFCoreSubsystem:Initialize",
                L"Function /Script/cfcore.CFCoreSubsystem:Initialize",
                L"/Script/cfcore.CFCoreSubsystem.Initialize",
                L"Function /Script/cfcore.CFCoreSubsystem.Initialize",
            },
            "Initialize",
            "Initialize",
            attempts);

        const auto mods_directory = [&command_text]()
        {
            auto value = regex_value(command_text, "modsDirectory");
            if (!value.empty()) return value;
            return std::string("D:/scum_dedicated/SCUM/Saved/CFCore/Mods");
        }();
        const auto user_data_directory = [&command_text]()
        {
            auto value = regex_value(command_text, "userDataDirectory");
            if (!value.empty()) return value;
            return std::string("D:/scum_dedicated/SCUM/Saved/CFCore/UserData");
        }();
        const auto is_server_pc_only_requested = regex_bool(command_text, "isServerPcOnly", true);

        const auto dry_run = regex_bool(command_text, "dryRun", true);
        const auto confirm = regex_value(command_text, "confirm");
        const auto invoke_requested = !dry_run && confirm == "CONFIGURE_CFCORE_SERVER_RUNTIME";
        bool wrote_mods_dir = false;
        bool wrote_user_data_dir = false;
        bool wrote_mode = false;
        bool wrote_max_concurrent = false;
        bool wrote_is_server = false;
        bool wrote_is_server_pc_only = false;
        bool copied_settings = false;
        bool initialized = false;
        unsigned long exception_code = 0;
        std::string process_event_detail;
        std::string stage = invoke_requested ? "configure-requested" : "dry-run";

        const auto has_settings = settings != nullptr;
        const auto has_subsystem =
            refs.subsystem_instance != nullptr &&
            !refs.subsystem_instance_looks_cdo;
        const auto has_initialize = initialize_function != nullptr;

        if (invoke_requested && has_settings && has_subsystem && has_initialize)
        {
            wrote_mods_dir = write_string_property(settings, L"modsDirectory", "cfcore.modsDirectory", mods_directory);
            wrote_user_data_dir = write_string_property(settings, L"userDataDirectory", "cfcore.userDataDirectory", user_data_directory);
            wrote_mode = write_u8_property(settings, L"modsDirectoryMode", "modsDirectoryMode", 1);
            wrote_max_concurrent = write_i32_property(settings, L"maxConcurrentInstallations", "maxConcurrentInstallations", 3);
            wrote_is_server = write_u8_property(settings, L"isServer", "isServer", 1);
            wrote_is_server_pc_only = write_u8_property(settings, L"isServerPcOnly", "isServerPcOnly", is_server_pc_only_requested ? 1 : 0);

            CFCoreInitializeProcessEventParamsAbi params{};
            copied_settings = copy_settings_from_cdo(settings, params);

            auto* process_event = process_event_from_vtable(refs.subsystem_instance, process_event_detail);
            if (process_event != nullptr &&
                wrote_mods_dir &&
                wrote_user_data_dir &&
                wrote_mode &&
                wrote_is_server &&
                copied_settings)
            {
                initialized = invoke_process_event_noexcept(process_event, refs.subsystem_instance, initialize_function, &params, exception_code);
                stage = initialized ? "initialize-dispatched" : "initialize-process-event-failed";
            }
            else if (process_event == nullptr)
            {
                stage = "blocked-process-event-unavailable";
            }
            else
            {
                stage = "blocked-settings-write-or-copy-failed";
            }
        }
        else if (invoke_requested && !has_settings)
        {
            stage = "blocked-settings-object-not-found";
        }
        else if (invoke_requested && !has_subsystem)
        {
            stage = "blocked-live-subsystem-not-found";
        }
        else if (invoke_requested && !has_initialize)
        {
            stage = "blocked-initialize-function-not-found";
        }

        std::string mods_dir_after;
        std::string user_data_after;
        bool string_missing = false;
        (void)string_property_noexcept(settings, L"modsDirectory", mods_dir_after, string_missing);
        string_missing = false;
        (void)string_property_noexcept(settings, L"userDataDirectory", user_data_after, string_missing);
        std::uint8_t is_server_after = 0;
        bool bool_missing = false;
        const auto is_server_after_readable = read_pod_property_noexcept(settings, L"isServer", is_server_after, bool_missing);
        std::uint8_t is_server_pc_only_after = 0;
        bool_missing = false;
        const auto is_server_pc_only_after_readable = read_pod_property_noexcept(settings, L"isServerPcOnly", is_server_pc_only_after, bool_missing);
        std::uint8_t mode_after = 0;
        bool_missing = false;
        const auto mode_after_readable = read_pod_property_noexcept(settings, L"modsDirectoryMode", mode_after, bool_missing);

        const auto route_ready = has_settings && has_subsystem && has_initialize;
        const auto ok_result = invoke_requested ? initialized : route_ready;
        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

        std::ostringstream ss;
        ss << "{\"ok\":" << (ok_result ? "true" : "false") << ","
           << "\"route\":\"cfcore-configure-server-runtime\","
           << "\"version\":\"" << mod_version << "\","
           << "\"durationMs\":" << duration_ms << ","
           << "\"stage\":\"" << json_escape(stage) << "\","
           << "\"dryRun\":" << (dry_run ? "true" : "false") << ","
           << "\"invokeRequested\":" << (invoke_requested ? "true" : "false") << ","
           << "\"initialized\":" << (initialized ? "true" : "false") << ","
           << "\"mutationCalls\":" << (invoke_requested ? "true" : "false") << ","
           << "\"requiresConfirm\":\"CONFIGURE_CFCORE_SERVER_RUNTIME\","
           << "\"confirmMatched\":" << (confirm == "CONFIGURE_CFCORE_SERVER_RUNTIME" ? "true" : "false") << ","
           << "\"settingsObject\":\"" << json_escape(safe_full_name(settings)) << "\","
           << "\"subsystemInstance\":\"" << json_escape(safe_full_name(refs.subsystem_instance)) << "\","
           << "\"initializeFunction\":\"" << json_escape(safe_full_name(initialize_function)) << "\","
           << "\"processEvent\":\"" << json_escape(process_event_detail.empty() ? refs.process_event_detail : process_event_detail) << "\","
           << "\"requested\":{\"modsDirectory\":\"" << json_escape(mods_directory)
           << "\",\"userDataDirectory\":\"" << json_escape(user_data_directory)
           << "\",\"isServerPcOnly\":" << (is_server_pc_only_requested ? "true" : "false") << "},"
           << "\"writes\":{\"modsDirectory\":" << (wrote_mods_dir ? "true" : "false")
           << ",\"userDataDirectory\":" << (wrote_user_data_dir ? "true" : "false")
           << ",\"modsDirectoryMode\":" << (wrote_mode ? "true" : "false")
           << ",\"maxConcurrentInstallations\":" << (wrote_max_concurrent ? "true" : "false")
           << ",\"isServer\":" << (wrote_is_server ? "true" : "false")
           << ",\"isServerPcOnly\":" << (wrote_is_server_pc_only ? "true" : "false")
           << ",\"settingsCopied\":" << (copied_settings ? "true" : "false") << "},"
           << "\"after\":{\"modsDirectory\":\"" << json_escape(mods_dir_after)
           << "\",\"userDataDirectory\":\"" << json_escape(user_data_after)
           << "\",\"modsDirectoryModeReadable\":" << (mode_after_readable ? "true" : "false")
           << ",\"modsDirectoryMode\":" << static_cast<int>(mode_after)
           << ",\"isServerReadable\":" << (is_server_after_readable ? "true" : "false")
           << ",\"isServer\":" << (is_server_after != 0 ? "true" : "false")
           << ",\"isServerPcOnlyReadable\":" << (is_server_pc_only_after_readable ? "true" : "false")
           << ",\"isServerPcOnly\":" << (is_server_pc_only_after != 0 ? "true" : "false") << "},"
           << "\"exception\":\"" << (exception_code != 0 ? json_escape(hex_address(static_cast<std::uintptr_t>(exception_code))) : std::string{}) << "\","
           << "\"abi\":{\"FScriptDelegateAbi\":" << sizeof(FScriptDelegateAbi)
           << ",\"CFCoreInitializeProcessEventParamsAbi\":" << sizeof(CFCoreInitializeProcessEventParamsAbi)
           << ",\"settingsBytesCopied\":" << cfcore_settings_abi_bytes << "},";
        append_cfcore_refs_json(ss, refs);
        ss << ",\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << "}";
        return {ok_result, ss.str()};
    }

    auto SCUMTraderManager::cfcore_server_mod_sync_probe(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        const auto started = std::chrono::steady_clock::now();
        const auto refs = cfcore_resolve_native_refs();
        const auto use_public_test_default = regex_bool(command_text, "usePublicTestModDefault", true);
        auto mod_ids = first_regex_int64_list(
            command_text,
            {"modIds", "mod_ids", "cfcoreModIds"},
            {"modId", "cfcoreModId"},
            use_public_test_default ? 1380356 : 0);
        auto dev_mod_ids = first_regex_int64_list(
            command_text,
            {"devModIds", "dev_mod_ids", "cfcoreDevModIds"},
            {"devModId", "cfcoreDevModId"},
            0);

        const auto confirm = regex_value(command_text, "confirm");
        const auto dry_run = regex_bool(command_text, "dryRun", true);
        const auto allow_cdo_invocation = regex_bool(command_text, "allowCdoInvocation", false);
        const auto invoke_requested = !dry_run && confirm == "ASSURE_SERVER_MODS_UPDATED";
        const auto has_function = refs.assure_server_mods_updated != nullptr;
        const auto has_subsystem = refs.subsystem_instance != nullptr;
        const auto has_process_event = refs.process_event_detail.find("processEvent=") != std::string::npos;
        const auto blocked_by_cdo = refs.subsystem_instance_looks_cdo && !allow_cdo_invocation;
        bool invoked = false;
        unsigned long exception_code = 0;
        std::string stage = invoke_requested ? "invoke-requested" : "dry-run";

        if (invoke_requested && has_function && has_subsystem && has_process_event && !blocked_by_cdo && !mod_ids.empty())
        {
            AssureServerModsUpdatedProcessEventParamsAbi params{};
            params.params.mod_ids.data = mod_ids.empty() ? nullptr : static_cast<void*>(mod_ids.data());
            params.params.mod_ids.count = static_cast<std::int32_t>(mod_ids.size());
            params.params.mod_ids.max = static_cast<std::int32_t>(mod_ids.size());
            params.params.dev_mod_ids.data = dev_mod_ids.empty() ? nullptr : static_cast<void*>(dev_mod_ids.data());
            params.params.dev_mod_ids.count = static_cast<std::int32_t>(dev_mod_ids.size());
            params.params.dev_mod_ids.max = static_cast<std::int32_t>(dev_mod_ids.size());

            ProcessEventFn process_event{};
            std::string process_event_detail;
            process_event = process_event_from_vtable(refs.subsystem_instance, process_event_detail);
            if (process_event != nullptr)
            {
                invoked = invoke_process_event_noexcept(process_event, refs.subsystem_instance, refs.assure_server_mods_updated, &params, exception_code);
                stage = invoked ? "assure-server-mods-updated-dispatched" : "process-event-failed";
            }
            else
            {
                stage = "process-event-unavailable";
            }
        }
        else if (invoke_requested && mod_ids.empty())
        {
            stage = "blocked-empty-mod-ids";
        }
        else if (invoke_requested && !has_function)
        {
            stage = "blocked-function-not-found";
        }
        else if (invoke_requested && !has_subsystem)
        {
            stage = "blocked-subsystem-not-found";
        }
        else if (invoke_requested && !has_process_event)
        {
            stage = "blocked-process-event-unavailable";
        }
        else if (invoke_requested && blocked_by_cdo)
        {
            stage = "blocked-cdo-subsystem";
        }

        const auto route_ready =
            has_function &&
            has_subsystem &&
            has_process_event &&
            !blocked_by_cdo &&
            !mod_ids.empty();
        const auto ok_result = invoke_requested ? invoked : route_ready;
        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

        std::ostringstream ss;
        ss << "{\"ok\":" << (ok_result ? "true" : "false") << ","
           << "\"route\":\"cfcore-server-mod-sync-probe\","
           << "\"version\":\"" << mod_version << "\","
           << "\"durationMs\":" << duration_ms << ","
           << "\"stage\":\"" << json_escape(stage) << "\","
           << "\"dryRun\":" << (dry_run ? "true" : "false") << ","
           << "\"invokeRequested\":" << (invoke_requested ? "true" : "false") << ","
           << "\"invoked\":" << (invoked ? "true" : "false") << ","
           << "\"callbacksBound\":false,"
           << "\"broadScan\":false,"
           << "\"narrowFunctionNameScan\":true,"
           << "\"requiresConfirm\":\"ASSURE_SERVER_MODS_UPDATED\","
           << "\"confirmMatched\":" << (confirm == "ASSURE_SERVER_MODS_UPDATED" ? "true" : "false") << ","
           << "\"defaultedPublicTestMod\":" << ((use_public_test_default && mod_ids.size() == 1 && mod_ids.front() == 1380356) ? "true" : "false") << ","
           << "\"modIds\":";
        append_json_int64_array(ss, mod_ids);
        ss << ",\"devModIds\":";
        append_json_int64_array(ss, dev_mod_ids);
        ss << ",\"routeReady\":" << (route_ready ? "true" : "false")
           << ",\"exception\":\"" << (exception_code != 0 ? json_escape(hex_address(static_cast<std::uintptr_t>(exception_code))) : std::string{}) << "\","
           << "\"abi\":{\"TArrayAbi\":" << sizeof(TArrayAbi)
           << ",\"FScriptDelegateAbi\":" << sizeof(FScriptDelegateAbi)
           << ",\"FAssureServerModsUpdatedParamsAbi\":" << sizeof(FAssureServerModsUpdatedParamsAbi)
           << ",\"AssureServerModsUpdatedProcessEventParamsAbi\":" << sizeof(AssureServerModsUpdatedProcessEventParamsAbi) << "},";
        append_cfcore_refs_json(ss, refs);
        ss << ",\"attempts\":";
        append_json_string_array(ss, refs.attempts);
        ss << "}";
        return {ok_result, ss.str()};
    }

    auto SCUMTraderManager::cfcore_client_file_sync_probe(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        const auto started = std::chrono::steady_clock::now();
        const auto refs = cfcore_resolve_native_refs();
        const auto use_public_test_default = regex_bool(command_text, "usePublicTestFileDefault", true);
        auto file_ids = first_regex_int64_list(
            command_text,
            {"fileIds", "file_ids", "serverFileIds", "server_file_ids", "cfcoreFileIds"},
            {"fileId", "serverFileId", "cfcoreFileId"},
            use_public_test_default ? 8142191 : 0);

        const auto confirm = regex_value(command_text, "confirm");
        const auto dry_run = regex_bool(command_text, "dryRun", true);
        const auto allow_cdo_invocation = regex_bool(command_text, "allowCdoInvocation", false);
        const auto invoke_requested = !dry_run && confirm == "ASSURE_CLIENT_MODS_UPDATED";
        const auto has_function = refs.assure_client_mods_updated != nullptr;
        const auto has_subsystem = refs.subsystem_instance != nullptr;
        const auto has_process_event = refs.process_event_detail.find("processEvent=") != std::string::npos;
        const auto blocked_by_cdo = refs.subsystem_instance_looks_cdo && !allow_cdo_invocation;
        bool invoked = false;
        unsigned long exception_code = 0;
        std::string stage = invoke_requested ? "invoke-requested" : "dry-run";

        if (invoke_requested && has_function && has_subsystem && has_process_event && !blocked_by_cdo && !file_ids.empty())
        {
            AssureClientModsUpdatedProcessEventParamsAbi params{};
            params.server_file_ids.data = file_ids.empty() ? nullptr : static_cast<void*>(file_ids.data());
            params.server_file_ids.count = static_cast<std::int32_t>(file_ids.size());
            params.server_file_ids.max = static_cast<std::int32_t>(file_ids.size());

            ProcessEventFn process_event{};
            std::string process_event_detail;
            process_event = process_event_from_vtable(refs.subsystem_instance, process_event_detail);
            if (process_event != nullptr)
            {
                invoked = invoke_process_event_noexcept(process_event, refs.subsystem_instance, refs.assure_client_mods_updated, &params, exception_code);
                stage = invoked ? "assure-client-mods-updated-dispatched" : "process-event-failed";
            }
            else
            {
                stage = "process-event-unavailable";
            }
        }
        else if (invoke_requested && file_ids.empty())
        {
            stage = "blocked-empty-file-ids";
        }
        else if (invoke_requested && !has_function)
        {
            stage = "blocked-function-not-found";
        }
        else if (invoke_requested && !has_subsystem)
        {
            stage = "blocked-subsystem-not-found";
        }
        else if (invoke_requested && !has_process_event)
        {
            stage = "blocked-process-event-unavailable";
        }
        else if (invoke_requested && blocked_by_cdo)
        {
            stage = "blocked-cdo-subsystem";
        }

        const auto route_ready =
            has_function &&
            has_subsystem &&
            has_process_event &&
            !blocked_by_cdo &&
            !file_ids.empty();
        const auto ok_result = invoke_requested ? invoked : route_ready;
        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

        std::ostringstream ss;
        ss << "{\"ok\":" << (ok_result ? "true" : "false") << ","
           << "\"route\":\"cfcore-client-file-sync-probe\","
           << "\"version\":\"" << mod_version << "\","
           << "\"durationMs\":" << duration_ms << ","
           << "\"stage\":\"" << json_escape(stage) << "\","
           << "\"dryRun\":" << (dry_run ? "true" : "false") << ","
           << "\"invokeRequested\":" << (invoke_requested ? "true" : "false") << ","
           << "\"invoked\":" << (invoked ? "true" : "false") << ","
           << "\"callbacksBound\":false,"
           << "\"broadScan\":false,"
           << "\"narrowFunctionNameScan\":true,"
           << "\"requiresConfirm\":\"ASSURE_CLIENT_MODS_UPDATED\","
           << "\"confirmMatched\":" << (confirm == "ASSURE_CLIENT_MODS_UPDATED" ? "true" : "false") << ","
           << "\"defaultedPublicTestFile\":" << ((use_public_test_default && file_ids.size() == 1 && file_ids.front() == 8142191) ? "true" : "false") << ","
           << "\"fileIds\":";
        append_json_int64_array(ss, file_ids);
        ss << ",\"routeReady\":" << (route_ready ? "true" : "false")
           << ",\"exception\":\"" << (exception_code != 0 ? json_escape(hex_address(static_cast<std::uintptr_t>(exception_code))) : std::string{}) << "\","
           << "\"abi\":{\"TArrayAbi\":" << sizeof(TArrayAbi)
           << ",\"FScriptDelegateAbi\":" << sizeof(FScriptDelegateAbi)
           << ",\"AssureClientModsUpdatedProcessEventParamsAbi\":" << sizeof(AssureClientModsUpdatedProcessEventParamsAbi) << "},";
        append_cfcore_refs_json(ss, refs);
        ss << ",\"attempts\":";
        append_json_string_array(ss, refs.attempts);
        ss << "}";
        return {ok_result, ss.str()};
    }

    auto SCUMTraderManager::raw_byte_stream_probe(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        const auto started = std::chrono::steady_clock::now();
        const auto confirm = regex_value(command_text, "confirm");
        const auto dry_run = regex_bool(command_text, "dryRun", true);
        const auto invoke_requested = !dry_run && confirm == "SEND_GENERIC_BYTE_STREAM";
        const auto max_targets = std::clamp(static_cast<int>(regex_number(command_text, "maxTargets", 1.0)), 1, 4);
        const auto payload_bytes = std::clamp(static_cast<int>(regex_number(command_text, "payloadBytes", 96.0)), 1, 4096);
        const auto stream_id = static_cast<std::int32_t>(regex_number(command_text, "streamId", 0x4E444A01));
        const auto stream_type = static_cast<std::uint8_t>(std::clamp(static_cast<int>(regex_number(command_text, "streamType", 0.0)), 0, 3));
        const auto controller_full_name = first_regex_value(command_text, {"controllerFullName", "playerControllerFullName"});

        std::vector<std::string> attempts;
        std::vector<std::string> controller_routes;
        auto* begin_function = resolve_function_by_name(
            "ConZPlayerController",
            "Client_BeginByteStream",
            {
                "/Script/SCUM.ConZPlayerController:Client_BeginByteStream",
                "Function /Script/SCUM.ConZPlayerController:Client_BeginByteStream",
                "/Script/SCUM.ConZPlayerController.Client_BeginByteStream",
                "Function /Script/SCUM.ConZPlayerController.Client_BeginByteStream",
            },
            attempts);
        auto* receive_function = resolve_function_by_name(
            "ConZPlayerController",
            "Client_ReceiveBytesFromStream",
            {
                "/Script/SCUM.ConZPlayerController:Client_ReceiveBytesFromStream",
                "Function /Script/SCUM.ConZPlayerController:Client_ReceiveBytesFromStream",
                "/Script/SCUM.ConZPlayerController.Client_ReceiveBytesFromStream",
                "Function /Script/SCUM.ConZPlayerController.Client_ReceiveBytesFromStream",
            },
            attempts);
        auto* end_function = resolve_function_by_name(
            "ConZPlayerController",
            "Client_EndByteStream",
            {
                "/Script/SCUM.ConZPlayerController:Client_EndByteStream",
                "Function /Script/SCUM.ConZPlayerController:Client_EndByteStream",
                "/Script/SCUM.ConZPlayerController.Client_EndByteStream",
                "Function /Script/SCUM.ConZPlayerController.Client_EndByteStream",
            },
            attempts);

        std::vector<RC::Unreal::UObject*> controllers;
        std::unordered_set<RC::Unreal::UObject*> seen;
        if (!controller_full_name.empty())
        {
            auto* controller = find_object_by_full_name(
                {L"BP_ConZPlayerController_C", L"ConZPlayerController", L"PlayerController", L"Controller", L"Object", L"UObject"},
                controller_full_name,
                attempts,
                "controller",
                false);
            append_unique_object(controllers, seen, controller);
        }
        else
        {
            auto found = find_all_unique({L"BP_ConZPlayerController_C", L"ConZPlayerController"}, &controller_routes);
            for (auto* controller : found)
            {
                if (controller == nullptr || looks_like_default_object(controller)) continue;
                append_unique_object(controllers, seen, controller);
                if (static_cast<int>(controllers.size()) >= max_targets) break;
            }
        }

        std::vector<std::uint8_t> payload(static_cast<std::size_t>(payload_bytes), 0);
        const std::string marker = "ScumNeDjin raw byte stream probe 2026-06-24; no file path; Generic stream.";
        for (std::size_t index = 0; index < payload.size(); ++index)
        {
            payload[index] = static_cast<std::uint8_t>(marker[index % marker.size()]);
        }

        bool has_process_event = false;
        int begin_invoked = 0;
        int receive_invoked = 0;
        int end_invoked = 0;
        int target_count = 0;
        unsigned long exception_code = 0;
        std::string stage = invoke_requested ? "invoke-requested" : "dry-run";
        std::vector<std::string> target_samples;
        std::vector<std::string> invoke_details;

        const auto functions_resolved = begin_function != nullptr && receive_function != nullptr && end_function != nullptr;
        for (auto* controller : controllers)
        {
            if (controller == nullptr) continue;
            ++target_count;
            if (target_samples.size() < 8) target_samples.push_back(safe_full_name(controller));

            std::string process_event_detail;
            auto* process_event = process_event_from_vtable(controller, process_event_detail);
            if (process_event != nullptr) has_process_event = true;
            if (!invoke_requested || !functions_resolved || process_event == nullptr) {
                if (invoke_details.size() < 8)
                {
                    invoke_details.push_back(safe_full_name(controller) + " ProcessEvent=" + process_event_detail);
                }
                continue;
            }

            ClientBeginByteStreamProcessEventParamsAbi begin_params{};
            begin_params.stream_id = stream_id;
            begin_params.stream_type = stream_type;

            ClientReceiveBytesFromStreamProcessEventParamsAbi receive_params{};
            receive_params.stream_id = stream_id;
            receive_params.bytes.data = payload.empty() ? nullptr : static_cast<void*>(payload.data());
            receive_params.bytes.count = static_cast<std::int32_t>(payload.size());
            receive_params.bytes.max = static_cast<std::int32_t>(payload.size());

            ClientEndByteStreamProcessEventParamsAbi end_params{};
            end_params.stream_id = stream_id;

            const auto begin_ok = invoke_process_event_noexcept(process_event, controller, begin_function, &begin_params, exception_code);
            if (begin_ok) ++begin_invoked;
            const auto receive_ok = begin_ok && invoke_process_event_noexcept(process_event, controller, receive_function, &receive_params, exception_code);
            if (receive_ok) ++receive_invoked;
            const auto end_ok = begin_ok && invoke_process_event_noexcept(process_event, controller, end_function, &end_params, exception_code);
            if (end_ok) ++end_invoked;

            if (invoke_details.size() < 8)
            {
                std::ostringstream detail;
                detail << safe_full_name(controller)
                       << " begin=" << (begin_ok ? "1" : "0")
                       << " receive=" << (receive_ok ? "1" : "0")
                       << " end=" << (end_ok ? "1" : "0")
                       << " ProcessEvent=" << process_event_detail;
                invoke_details.push_back(detail.str());
            }
        }

        if (!functions_resolved)
        {
            stage = "blocked-functions-not-found";
        }
        else if (controllers.empty())
        {
            stage = "blocked-no-live-controller";
        }
        else if (!has_process_event)
        {
            stage = "blocked-process-event-unavailable";
        }
        else if (invoke_requested)
        {
            stage = (begin_invoked > 0 && receive_invoked > 0 && end_invoked > 0)
                ? "generic-byte-stream-dispatched"
                : "generic-byte-stream-dispatch-incomplete";
        }

        const auto route_ready = functions_resolved && !controllers.empty() && has_process_event;
        const auto ok_result = invoke_requested ? (begin_invoked > 0 && receive_invoked > 0 && end_invoked > 0) : route_ready;
        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

        std::ostringstream ss;
        ss << "{\"ok\":" << (ok_result ? "true" : "false") << ","
           << "\"route\":\"raw-byte-stream-probe\","
           << "\"version\":\"" << mod_version << "\","
           << "\"durationMs\":" << duration_ms << ","
           << "\"stage\":\"" << json_escape(stage) << "\","
           << "\"dryRun\":" << (dry_run ? "true" : "false") << ","
           << "\"invokeRequested\":" << (invoke_requested ? "true" : "false") << ","
           << "\"requiresConfirm\":\"SEND_GENERIC_BYTE_STREAM\","
           << "\"confirmMatched\":" << (confirm == "SEND_GENERIC_BYTE_STREAM" ? "true" : "false") << ","
           << "\"routeReady\":" << (route_ready ? "true" : "false") << ","
           << "\"streamId\":" << stream_id << ","
           << "\"streamType\":" << static_cast<int>(stream_type) << ","
           << "\"payloadBytes\":" << payload.size() << ","
           << "\"targetCount\":" << target_count << ","
           << "\"beginInvoked\":" << begin_invoked << ","
           << "\"receiveInvoked\":" << receive_invoked << ","
           << "\"endInvoked\":" << end_invoked << ","
           << "\"exception\":\"" << (exception_code != 0 ? json_escape(hex_address(static_cast<std::uintptr_t>(exception_code))) : std::string{}) << "\","
           << "\"functions\":{\"begin\":\"" << json_escape(safe_full_name(begin_function)) << "\","
           << "\"receive\":\"" << json_escape(safe_full_name(receive_function)) << "\","
           << "\"end\":\"" << json_escape(safe_full_name(end_function)) << "\"},"
           << "\"abi\":{\"ClientBeginByteStreamProcessEventParamsAbi\":" << sizeof(ClientBeginByteStreamProcessEventParamsAbi)
           << ",\"ClientReceiveBytesFromStreamProcessEventParamsAbi\":" << sizeof(ClientReceiveBytesFromStreamProcessEventParamsAbi)
           << ",\"ClientEndByteStreamProcessEventParamsAbi\":" << sizeof(ClientEndByteStreamProcessEventParamsAbi)
           << ",\"TArrayAbi\":" << sizeof(TArrayAbi) << "},"
           << "\"controllerSearch\":";
        append_json_string_array(ss, controller_routes);
        ss << ",\"targetSamples\":";
        append_json_string_array(ss, target_samples);
        ss << ",\"invokeDetails\":";
        append_json_string_array(ss, invoke_details);
        ss << ",\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << "}";
        return {ok_result, ss.str()};
    }

    auto SCUMTraderManager::scum_mod_manager_probe() const -> std::pair<bool, std::string>
    {
        const auto started = std::chrono::steady_clock::now();
        std::vector<std::string> attempts;
        std::vector<std::string> engine_route_counts;
        std::vector<std::string> mod_manager_route_counts;
        std::vector<std::string> manifest_route_counts;

        auto pick_first_non_cdo = [](const std::vector<RC::Unreal::UObject*>& objects) -> RC::Unreal::UObject*
        {
            RC::Unreal::UObject* fallback{};
            for (auto* object : objects)
            {
                if (object == nullptr) continue;
                if (fallback == nullptr) fallback = object;
                if (!looks_like_default_object(object)) return object;
            }
            return fallback;
        };

        auto append_mod_info_samples_json = [](std::ostringstream& out, const TArrayAbi* array, std::size_t max_items) -> void
        {
            out << "[";
            if (is_reasonable_array(array) && array->data != nullptr && array->count > 0)
            {
                const auto count = std::min<std::int32_t>(array->count, static_cast<std::int32_t>(max_items));
                const auto bytes = static_cast<std::size_t>(count) * sizeof(FModInfoAbi);
                if (committed_readable_address(reinterpret_cast<std::uintptr_t>(array->data), bytes))
                {
                    for (std::int32_t index = 0; index < count; ++index)
                    {
                        FModInfoAbi item{};
                        const auto address = reinterpret_cast<std::uintptr_t>(array->data) + (static_cast<std::uintptr_t>(index) * sizeof(FModInfoAbi));
                        if (index > 0) out << ",";
                        const auto readable = safe_read(address, item);
                        out << "{\"index\":" << index
                            << ",\"readable\":" << (readable ? "true" : "false")
                            << ",\"manifestClass\":\"" << json_escape(readable ? safe_full_name(item.mod_manifest_class) : std::string{}) << "\""
                            << ",\"manifestClassClass\":\"" << json_escape(readable ? object_class_full_name_noexcept(item.mod_manifest_class) : std::string{}) << "\"}";
                    }
                }
            }
            out << "]";
        };

        auto append_object_samples_json = [](std::ostringstream& out, const std::vector<RC::Unreal::UObject*>& objects, std::size_t max_items) -> void
        {
            std::vector<std::string> samples;
            for (size_t index = 0; index < objects.size() && samples.size() < max_items; ++index)
            {
                samples.push_back(safe_full_name(objects[index]));
            }
            append_json_string_array(out, samples);
        };

        auto* engine_first = cfcore_find_first_of_noexcept(L"ConZGameEngine");
        engine_route_counts.push_back(std::string("FindFirstOf(ConZGameEngine)=") + safe_full_name(engine_first));
        auto engine_objects = find_all_unique({L"ConZGameEngine", L"UConZGameEngine"}, &engine_route_counts);
        auto* engine = (engine_first != nullptr && !looks_like_default_object(engine_first)) ? engine_first : pick_first_non_cdo(engine_objects);
        if (engine == nullptr) engine = engine_first;

        auto* mod_manager = object_property_noexcept(engine, L"_modManager", attempts);
        auto mod_manager_objects = find_all_unique({L"ModManager", L"UModManager"}, &mod_manager_route_counts);
        auto* mod_manager_fallback = pick_first_non_cdo(mod_manager_objects);
        if (mod_manager == nullptr) mod_manager = mod_manager_fallback;

        const auto* installed_mods = array_property(mod_manager, L"_installedMods");
        const auto installed_mods_valid = is_reasonable_array(installed_mods);
        const auto installed_mods_count = installed_mods_valid ? installed_mods->count : -1;
        const auto installed_mods_max = installed_mods_valid ? installed_mods->max : -1;
        const auto installed_mods_sample_count = installed_mods_valid ? std::min<std::int32_t>(installed_mods->count, 16) : 0;
        const auto installed_mods_data_readable =
            installed_mods_valid &&
            (installed_mods_sample_count == 0 ||
             (installed_mods->data != nullptr &&
              committed_readable_address(
                  reinterpret_cast<std::uintptr_t>(installed_mods->data),
                  static_cast<std::size_t>(installed_mods_sample_count) * sizeof(FModInfoAbi))));

        auto manifest_objects = find_all_unique({L"ModManifest", L"UModManifest"}, &manifest_route_counts);

        auto* mod_manager_class = find_object_exact_noexcept(L"Class", L"/Script/SCUM.ModManager", 0, object_flag_class_default_object);
        auto* mod_manifest_class = find_object_exact_noexcept(L"Class", L"/Script/SCUM.ModManifest", 0, object_flag_class_default_object);
        auto* mod_selector_class = find_object_exact_noexcept(L"Class", L"/Script/SCUM.ModSelector", 0, object_flag_class_default_object);
        auto* mod_selector_controller_class = find_object_exact_noexcept(L"Class", L"/Script/SCUM.ModSelectorUIController", 0, object_flag_class_default_object);
        auto* character_selection_widget_class = find_object_exact_noexcept(L"Class", L"/Script/SCUM.CharacterSelectionWidget", 0, object_flag_class_default_object);
        auto* mod_manifest_on_loaded = find_object_exact_noexcept(L"Function", L"/Script/SCUM.ModManifest:OnLoaded", 0, object_flag_class_default_object);
        auto* mod_manifest_pre_unloaded = find_object_exact_noexcept(L"Function", L"/Script/SCUM.ModManifest:PreUnloaded", 0, object_flag_class_default_object);
        auto* cfcore_subsystem = cfcore_find_first_of_noexcept(L"CFCoreSubsystem");

        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        const auto ok_result = engine != nullptr && mod_manager != nullptr;

        std::ostringstream ss;
        ss << "{\"ok\":" << (ok_result ? "true" : "false") << ","
           << "\"route\":\"scum-mod-manager-probe\","
           << "\"version\":\"" << mod_version << "\","
           << "\"durationMs\":" << duration_ms << ","
           << "\"readOnly\":true,"
           << "\"mutationCalls\":false,"
           << "\"broadScan\":false,"
           << "\"classScopedFindAll\":true,"
           << "\"engineAccess\":\"FindFirstOf/FindAllOf for ConZGameEngine, ModManager, ModManifest plus reflected _modManager/_installedMods\","
           << "\"engine\":\"" << json_escape(safe_full_name(engine)) << "\","
           << "\"engineClass\":\"" << json_escape(object_class_full_name_noexcept(engine)) << "\","
           << "\"modManager\":\"" << json_escape(safe_full_name(mod_manager)) << "\","
           << "\"modManagerClass\":\"" << json_escape(object_class_full_name_noexcept(mod_manager)) << "\","
           << "\"modManagerFallback\":\"" << json_escape(safe_full_name(mod_manager_fallback)) << "\","
           << "\"installedMods\":{\"valid\":" << (installed_mods_valid ? "true" : "false")
           << ",\"dataReadable\":" << (installed_mods_data_readable ? "true" : "false")
           << ",\"count\":" << installed_mods_count
           << ",\"max\":" << installed_mods_max
           << ",\"structSize\":" << sizeof(FModInfoAbi)
           << ",\"samples\":";
        append_mod_info_samples_json(ss, installed_mods, 16);
        ss << "},\"loadedModManifestCount\":" << manifest_objects.size()
           << ",\"loadedModManifestSamples\":";
        append_object_samples_json(ss, manifest_objects, 16);
        ss << ",\"scumRefs\":{"
           << "\"modManagerClass\":\"" << json_escape(safe_full_name(mod_manager_class)) << "\","
           << "\"modManifestClass\":\"" << json_escape(safe_full_name(mod_manifest_class)) << "\","
           << "\"modManifestOnLoaded\":\"" << json_escape(safe_full_name(mod_manifest_on_loaded)) << "\","
           << "\"modManifestPreUnloaded\":\"" << json_escape(safe_full_name(mod_manifest_pre_unloaded)) << "\","
           << "\"modSelectorClass\":\"" << json_escape(safe_full_name(mod_selector_class)) << "\","
           << "\"modSelectorUIControllerClass\":\"" << json_escape(safe_full_name(mod_selector_controller_class)) << "\","
           << "\"characterSelectionWidgetClass\":\"" << json_escape(safe_full_name(character_selection_widget_class)) << "\"},"
           << "\"cfcoreSubsystem\":\"" << json_escape(safe_full_name(cfcore_subsystem)) << "\","
           << "\"cfcoreSubsystemClass\":\"" << json_escape(object_class_full_name_noexcept(cfcore_subsystem)) << "\","
           << "\"engineRoutes\":";
        append_json_string_array(ss, engine_route_counts);
        ss << ",\"modManagerRoutes\":";
        append_json_string_array(ss, mod_manager_route_counts);
        ss << ",\"manifestRoutes\":";
        append_json_string_array(ss, manifest_route_counts);
        ss << ",\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << "}";
        return {ok_result, ss.str()};
    }

    auto SCUMTraderManager::world_edit_server_only_probe() const -> std::string
    {
        const auto started = std::chrono::steady_clock::now();

        struct AdminCommandProbeTarget
        {
            const char* key;
            const wchar_t* native_class_name;
            const wchar_t* native_class_path;
            const wchar_t* legacy_bp_class_name;
            const wchar_t* legacy_bp_cdo_path;
        };

        static constexpr std::array<AdminCommandProbeTarget, 21> admin_targets{{
            {"CreateEntity", L"AdminCommand_CreateEntity", L"/Script/SCUM.AdminCommand_CreateEntity", L"CreateEntity_C", L"/Game/ConZ_Files/AdminCommands/Developer/CreateEntity.Default__CreateEntity_C"},
            {"CreateEntityOnClient", L"AdminCommand_CreateEntity", L"/Script/SCUM.AdminCommand_CreateEntity", L"CreateEntityOnClient_C", L"/Game/ConZ_Files/AdminCommands/Developer/CreateEntityOnClient.Default__CreateEntityOnClient_C"},
            {"PrintServerEntities", L"AdminCommand_PrintEntities", L"/Script/SCUM.AdminCommand_PrintEntities", L"PrintServerEntities_C", L"/Game/ConZ_Files/AdminCommands/Developer/PrintServerEntities.Default__PrintServerEntities_C"},
            {"PrintClientEntities", L"AdminCommand_PrintEntities", L"/Script/SCUM.AdminCommand_PrintEntities", L"PrintClientEntities_C", L"/Game/ConZ_Files/AdminCommands/Developer/PrintClientEntities.Default__PrintClientEntities_C"},
            {"GetMeshInfo", L"AdminCommand_GetMeshInfo", L"/Script/SCUM.AdminCommand_GetMeshInfo", L"GetMeshInfo_C", L"/Game/ConZ_Files/AdminCommands/GetMeshInfo.Default__GetMeshInfo_C"},
            {"DoorDebug", L"AdminCommand_DoorDebug", L"/Script/SCUM.AdminCommand_DoorDebug", nullptr, nullptr},
            {"PlacementDebug", L"AdminCommand_PlacementDebug", L"/Script/SCUM.AdminCommand_PlacementDebug", nullptr, nullptr},
            {"SpawnPrimaryActorAsset", L"AdminCommand_SpawnPrimaryActorAsset", L"/Script/SCUM.AdminCommand_SpawnPrimaryActorAsset", nullptr, nullptr},
            {"SpawnRandomPrimaryActorAsset", L"AdminCommand_SpawnRandomPrimaryActorAsset", L"/Script/SCUM.AdminCommand_SpawnRandomPrimaryActorAsset", nullptr, nullptr},
            {"ListAnimals", L"AdminCommand_ListPrimaryAssets", L"/Script/SCUM.AdminCommand_ListPrimaryAssets", L"ListAnimals_C", L"/Game/ConZ_Files/AdminCommands/ListAnimals.Default__ListAnimals_C"},
            {"ListItems", L"AdminCommand_ListPrimaryAssets", L"/Script/SCUM.AdminCommand_ListPrimaryAssets", L"ListItems_C", L"/Game/ConZ_Files/AdminCommands/ListItems.Default__ListItems_C"},
            {"ListVehicles", L"AdminCommand_ListPrimaryAssets", L"/Script/SCUM.AdminCommand_ListPrimaryAssets", L"ListVehicles_C", L"/Game/ConZ_Files/AdminCommands/ListVehicles.Default__ListVehicles_C"},
            {"ListZombies", L"AdminCommand_ListPrimaryAssets", L"/Script/SCUM.AdminCommand_ListPrimaryAssets", L"ListZombies_C", L"/Game/ConZ_Files/AdminCommands/ListZombies.Default__ListZombies_C"},
            {"SpawnItem", L"AdminCommand_SpawnItem", L"/Script/SCUM.AdminCommand_SpawnItem", L"SpawnItem_C", L"/Game/ConZ_Files/AdminCommands/SpawnItem.Default__SpawnItem_C"},
            {"SpawnVehicle", L"AdminCommand_SpawnVehicle", L"/Script/SCUM.AdminCommand_SpawnVehicle", L"SpawnVehicle_C", L"/Game/ConZ_Files/AdminCommands/SpawnVehicle.Default__SpawnVehicle_C"},
            {"SpawnAnimal", L"AdminCommand_SpawnAnimal", L"/Script/SCUM.AdminCommand_SpawnAnimal", L"SpawnAnimal_C", L"/Game/ConZ_Files/AdminCommands/SpawnAnimal.Default__SpawnAnimal_C"},
            {"SpawnZombie", L"AdminCommand_SpawnZombie", L"/Script/SCUM.AdminCommand_SpawnZombie", L"SpawnZombie_C", L"/Game/ConZ_Files/AdminCommands/SpawnZombie.Default__SpawnZombie_C"},
            {"SpawnArmedNPC", L"AdminCommand_SpawnArmedNPC", L"/Script/SCUM.AdminCommand_SpawnArmedNPC", L"SpawnArmedNPC_C", L"/Game/ConZ_Files/AdminCommands/SpawnArmedNPC.Default__SpawnArmedNPC_C"},
            {"SpawnRandomAnimal", L"AdminCommand_SpawnRandomAnimal", L"/Script/SCUM.AdminCommand_SpawnRandomAnimal", L"SpawnRandomAnimal_C", L"/Game/ConZ_Files/AdminCommands/SpawnRandomAnimal.Default__SpawnRandomAnimal_C"},
            {"SpawnRandomZombie", L"AdminCommand_SpawnRandomZombie", L"/Script/SCUM.AdminCommand_SpawnRandomZombie", L"SpawnRandomZombie_C", L"/Game/ConZ_Files/AdminCommands/SpawnRandomZombie.Default__SpawnRandomZombie_C"},
            {"SpawnRandomZombie2", L"AdminCommand_SpawnRandomZombie", L"/Script/SCUM.AdminCommand_SpawnRandomZombie", L"SpawnRandomZombie2_C", L"/Game/ConZ_Files/AdminCommands/SpawnRandomZombie2.Default__SpawnRandomZombie2_C"},
        }};

        auto class_default_object = [](RC::Unreal::UObject* klass, std::vector<std::string>& attempts) -> RC::Unreal::UObject*
        {
            bool missing = false;
            bool exception = false;
            auto* cdo = object_property_noexcept_raw(klass, L"ClassDefaultObject", missing, exception);
            if (exception)
            {
                attempts.push_back("ClassDefaultObject=exception");
                return nullptr;
            }
            if (missing)
            {
                attempts.push_back("ClassDefaultObject=missing");
                return nullptr;
            }
            attempts.push_back("ClassDefaultObject=" + safe_full_name(cdo));
            return cdo;
        };

        auto direct_default_object_by_class_name = [](const wchar_t* native_class_name, std::vector<std::string>& attempts) -> RC::Unreal::UObject*
        {
            if (native_class_name == nullptr || native_class_name[0] == L'\0') return nullptr;
            const std::array<std::wstring, 2> object_names{{
                std::wstring(L"Default__") + native_class_name,
                std::wstring(L"/Script/SCUM.Default__") + native_class_name,
            }};

            for (const auto& object_name : object_names)
            {
                auto* object = find_object_exact_noexcept(native_class_name, object_name.c_str(), object_flag_class_default_object, 0);
                attempts.push_back("directDefault.FindObject(requiredCDO," + narrow(native_class_name) + "," + narrow(object_name) + ")=" + (object != nullptr ? safe_full_name(object) : "0"));
                if (object != nullptr) return object;
            }

            for (const auto& object_name : object_names)
            {
                auto* object = find_object_exact_noexcept(native_class_name, object_name.c_str(), 0, 0);
                attempts.push_back("directDefault.FindObject(noFlags," + narrow(native_class_name) + "," + narrow(object_name) + ")=" + (object != nullptr ? safe_full_name(object) : "0"));
                if (object != nullptr) return object;
            }

            return nullptr;
        };

        struct EntitySetupProbeTarget
        {
            const char* key;
            const wchar_t* class_name;
            const wchar_t* class_path;
        };

        static constexpr std::array<EntitySetupProbeTarget, 5> entity_targets{{
            {"EntitySetup", L"EntitySetup", L"/Script/SCUM.EntitySetup"},
            {"BuildingEntitySetup", L"BuildingEntitySetup", L"/Script/SCUM.BuildingEntitySetup"},
            {"BaseBuildingBaseEntitySetup", L"BaseBuildingBaseEntitySetup", L"/Script/SCUM.BaseBuildingBaseEntitySetup"},
            {"DoorEntitySetup", L"DoorEntitySetup", L"/Script/SCUM.DoorEntitySetup"},
            {"ItemEntitySetup", L"ItemEntitySetup", L"/Script/SCUM.ItemEntitySetup"},
        }};

        auto exact_class_candidates = [](const wchar_t* class_path, const wchar_t* class_name) -> std::vector<std::wstring>
        {
            std::vector<std::wstring> candidates;
            if (class_path != nullptr && class_path[0] != L'\0') candidates.emplace_back(class_path);
            if (class_name != nullptr && class_name[0] != L'\0')
            {
                const std::wstring short_name{class_name};
                if (std::find(candidates.begin(), candidates.end(), short_name) == candidates.end())
                {
                    candidates.emplace_back(short_name);
                }
            }
            return candidates;
        };

        auto find_class_exact = [&](const wchar_t* class_path, const wchar_t* class_name, const char* label, std::vector<std::string>& attempts) -> RC::Unreal::UObject*
        {
            const auto candidates = exact_class_candidates(class_path, class_name);
            for (const auto& object_name : candidates)
            {
                auto* object = find_object_exact_noexcept(L"Class", object_name.c_str(), 0, object_flag_class_default_object);
                attempts.push_back(std::string(label) + ".FindObject(Class,noCDO," + narrow(object_name) + ")=" + (object != nullptr ? safe_full_name(object) : "0"));
                if (object != nullptr) return object;
            }

            return nullptr;
        };

        auto find_legacy_blueprint_cdo = [](const AdminCommandProbeTarget& target, std::vector<std::string>& attempts) -> RC::Unreal::UObject*
        {
            if (target.legacy_bp_cdo_path == nullptr || target.legacy_bp_cdo_path[0] == L'\0') return nullptr;
            if (target.legacy_bp_class_name == nullptr || target.legacy_bp_class_name[0] == L'\0') return nullptr;

            std::vector<std::wstring> object_names;
            object_names.emplace_back(target.legacy_bp_cdo_path);
            object_names.emplace_back(std::wstring(L"Default__") + target.legacy_bp_class_name);

            const std::wstring cdo_path{target.legacy_bp_cdo_path};
            const std::wstring marker{L".Default__"};
            const auto marker_pos = cdo_path.find(marker);
            if (marker_pos != std::wstring::npos)
            {
                const auto asset_path = cdo_path.substr(0, marker_pos);
                object_names.emplace_back(asset_path + L"." + target.legacy_bp_class_name + L":Default__" + target.legacy_bp_class_name);
            }

            for (const auto& object_name : object_names)
            {
                auto* object = find_object_exact_noexcept(target.legacy_bp_class_name, object_name.c_str(), object_flag_class_default_object, 0);
                attempts.push_back("legacyBp.FindObject(requiredCDO," + narrow(target.legacy_bp_class_name) + "," + narrow(object_name) + ")=" + (object != nullptr ? safe_full_name(object) : "0"));
                if (object != nullptr) return object;
            }

            for (const auto& object_name : object_names)
            {
                auto* object = find_object_exact_noexcept(target.legacy_bp_class_name, object_name.c_str(), 0, 0);
                attempts.push_back("legacyBp.FindObject(noFlags," + narrow(target.legacy_bp_class_name) + "," + narrow(object_name) + ")=" + (object != nullptr ? safe_full_name(object) : "0"));
                if (object != nullptr) return object;
            }

            return nullptr;
        };

        auto find_admin_cdo = [&](const AdminCommandProbeTarget& target, std::vector<std::string>& attempts) -> RC::Unreal::UObject*
        {
            auto* klass = find_class_exact(target.native_class_path, target.native_class_name, "nativeAdminClass", attempts);
            auto* cdo = class_default_object(klass, attempts);
            auto* blueprint_cdo = find_legacy_blueprint_cdo(target, attempts);
            if (blueprint_cdo != nullptr) return blueprint_cdo;
            if (cdo != nullptr) return cdo;
            cdo = direct_default_object_by_class_name(target.native_class_name, attempts);
            if (cdo != nullptr) return cdo;

            return nullptr;
        };

        int admin_found = 0;
        int entity_class_found = 0;
        int entity_cdo_found = 0;

        std::ostringstream ss;
        ss << "{\"route\":\"world-edit-server-only-probe\","
           << "\"version\":\"" << mod_version << "\","
           << "\"readOnly\":true,"
           << "\"mutatesWorld\":false,"
           << "\"broadScan\":false,"
           << "\"exactLookupsOnly\":true,"
           << "\"blockedRoutes\":[\"ForEachUObject\",\"FindAll\",\"broad FindObjects\",\"SpawnActor\",\"StaticMeshActor proxy\",\"admin_command_probe\"],"
           << "\"adminCommands\":[";

        for (size_t index = 0; index < admin_targets.size(); ++index)
        {
            const auto& target = admin_targets[index];
            std::vector<std::string> attempts;
            auto* object = find_admin_cdo(target, attempts);
            if (object != nullptr) ++admin_found;

            if (index > 0) ss << ",";
            ss << "{\"key\":\"" << json_escape(target.key) << "\","
               << "\"nativeClassPath\":\"" << json_escape(target.native_class_path != nullptr ? narrow(target.native_class_path) : std::string{}) << "\","
               << "\"nativeClassName\":\"" << json_escape(target.native_class_name != nullptr ? narrow(target.native_class_name) : std::string{}) << "\","
               << "\"legacyBpCdoPath\":\"" << json_escape(target.legacy_bp_cdo_path != nullptr ? narrow(target.legacy_bp_cdo_path) : std::string{}) << "\","
               << "\"found\":" << (object != nullptr ? "true" : "false") << ","
               << "\"fullName\":\"" << json_escape(safe_full_name(object)) << "\","
               << "\"classFullName\":\"" << json_escape(object_class_full_name_noexcept(object)) << "\","
               << "\"lookupAttempts\":";
            append_json_string_array(ss, attempts);
            ss << ",\"properties\":{";
            bool first_property = true;
            append_probe_string_property(ss, "verb", object, L"_verb", first_property);
            append_probe_array_count_property(ss, "argumentDescriptions", object, L"_argumentDescriptions", first_property);
            append_probe_int32_property(ss, "numberOfRequiredArguments", object, L"_numberOfRequiredArguments", first_property);
            append_probe_int32_property(ss, "numberOfRepeatingArguments", object, L"_numberOfRepeatingArguments", first_property);
            append_probe_bool_property(ss, "isEnabled", object, L"_isEnabled", first_property);
            append_probe_bool_property(ss, "isEnabledInShippingBuild", object, L"_isEnabledInShippingBuild", first_property);
            append_probe_bool_property(ss, "shouldExecuteOnServer", object, L"_shouldExecuteOnServer", first_property);
            append_probe_bool_property(ss, "shouldExecuteOnClient", object, L"_shouldExecuteOnClient", first_property);
            append_probe_bool_property(ss, "hasCooldown", object, L"_hasCooldown", first_property);
            append_probe_uint8_property(ss, "requiredExecutorLevel", object, L"_requiredExecutorLevel", first_property);
            append_probe_presence_property(ss, "primaryAssetType", object, L"_primaryAssetType", first_property);
            append_probe_array_count_property(ss, "primaryAssetsToExclude", object, L"_primaryAssetsToExclude", first_property);
            append_probe_presence_property(ss, "spawnOffset", object, L"_spawnOffset", first_property);
            append_probe_bool_property(ss, "isSpawnedActorCapableOfFalling", object, L"_isSpawnedActorCapableOfFalling", first_property);
            ss << "},\"argumentSamples\":";
            append_admin_argument_samples(ss, object, L"_argumentDescriptions", 4);
            ss << "}";
        }

        auto* game_instance = RC::Unreal::UObjectGlobals::FindFirstOf(L"ConZGameInstance");
        auto* entity_system = object_property(game_instance, L"_entitySystem");
        auto* entity_system_class = object_property(game_instance, L"_entitySystemClass");
        auto* admin_registry = object_property(game_instance, L"_adminCommandRegistry");
        auto* db_id_generator = object_property(game_instance, L"_dbIdGenerator");
        auto* asset_manager = RC::Unreal::UObjectGlobals::FindFirstOf(L"ConZAssetManager");
        if (asset_manager == nullptr) asset_manager = RC::Unreal::UObjectGlobals::FindFirstOf(L"AssetManager");

        ss << "],\"gameSystems\":{\"gameInstanceFound\":" << (game_instance != nullptr ? "true" : "false")
           << ",\"gameInstanceFullName\":\"" << json_escape(safe_full_name(game_instance)) << "\""
           << ",\"entitySystem\":\"" << json_escape(safe_full_name(entity_system)) << "\""
           << ",\"entitySystemClass\":\"" << json_escape(safe_full_name(entity_system_class)) << "\""
           << ",\"adminCommandRegistry\":\"" << json_escape(safe_full_name(admin_registry)) << "\""
           << ",\"dbIdGenerator\":\"" << json_escape(safe_full_name(db_id_generator)) << "\""
           << ",\"assetManager\":\"" << json_escape(safe_full_name(asset_manager)) << "\""
           << ",\"properties\":{";
        bool first_system_property = true;
        append_probe_presence_property(ss, "databaseConnections", game_instance, L"_databaseConnections", first_system_property);
        append_probe_presence_property(ss, "entitySystemSettings", entity_system, L"_settings", first_system_property);
        append_probe_presence_property(ss, "entitySystemUObjectReferences", entity_system, L"_uobjectReferences", first_system_property);
        append_probe_array_count_property(ss, "adminRegistryCommands", admin_registry, L"_commands", first_system_property);
        append_probe_presence_property(ss, "assetManagerLwobjectSetupByPrimaryAssetName", asset_manager, L"_lwobjectSetupByPrimaryAssetName", first_system_property);
        append_probe_array_count_property(ss, "assetManagerItemCraftingRecipes", asset_manager, L"_itemCraftingRecipes", first_system_property);
        append_probe_array_count_property(ss, "assetManagerPlaceableCraftingRecipes", asset_manager, L"_placeableCraftingRecipes", first_system_property);
        ss << "}},\"entitySetups\":[";

        for (size_t index = 0; index < entity_targets.size(); ++index)
        {
            const auto& target = entity_targets[index];
            std::vector<std::string> attempts;
            auto* klass = find_class_exact(target.class_path, target.class_name, "entitySetupClass", attempts);
            if (klass != nullptr) ++entity_class_found;
            auto* cdo = class_default_object(klass, attempts);
            if (cdo == nullptr) cdo = direct_default_object_by_class_name(target.class_name, attempts);
            if (cdo != nullptr) ++entity_cdo_found;

            if (index > 0) ss << ",";
            ss << "{\"key\":\"" << json_escape(target.key) << "\","
               << "\"classPath\":\"" << json_escape(narrow(target.class_path)) << "\","
               << "\"className\":\"" << json_escape(narrow(target.class_name)) << "\","
               << "\"classFound\":" << (klass != nullptr ? "true" : "false") << ","
               << "\"classFullName\":\"" << json_escape(safe_full_name(klass)) << "\","
               << "\"classClassFullName\":\"" << json_escape(object_class_full_name_noexcept(klass)) << "\","
               << "\"cdoFound\":" << (cdo != nullptr ? "true" : "false") << ","
               << "\"cdoFullName\":\"" << json_escape(safe_full_name(cdo)) << "\","
               << "\"cdoClassFullName\":\"" << json_escape(object_class_full_name_noexcept(cdo)) << "\","
               << "\"lookupAttempts\":";
            append_json_string_array(ss, attempts);
            ss << ",\"properties\":{";
            bool first_property = true;
            append_probe_bool_property(ss, "Transient", cdo, L"Transient", first_property);
            append_probe_bool_property(ss, "Hidden", cdo, L"Hidden", first_property);
            append_probe_float_property(ss, "BoundsRadius", cdo, L"BoundsRadius", first_property);
            append_probe_presence_property(ss, "Actor", cdo, L"Actor", first_property);
            append_probe_array_count_property(ss, "Components", cdo, L"Components", first_property);
            append_probe_bool_property(ss, "TickEnabled", cdo, L"TickEnabled", first_property);
            append_probe_bool_property(ss, "CanEverTickOnServer", cdo, L"CanEverTickOnServer", first_property);
            append_probe_bool_property(ss, "CanEverTickOnClient", cdo, L"CanEverTickOnClient", first_property);
            append_probe_float_property(ss, "TickInterval", cdo, L"TickInterval", first_property);
            ss << "}}";
        }

        std::vector<std::string> base_manager_attempts;
        auto find_base_manager_exact = [&]() -> RC::Unreal::UObject*
        {
            static constexpr std::array<const wchar_t*, 2> manager_paths{{
                L"/Game/ConZ_Files/Maps/The_Island/The_Island.The_Island:PersistentLevel.BP_ConZBaseManager_C_2147480159",
                L"BP_ConZBaseManager_C_2147480159",
            }};

            for (const auto* object_path : manager_paths)
            {
                auto* object = find_object_exact_noexcept(L"BP_ConZBaseManager_C", object_path, 0, object_flag_class_default_object);
                base_manager_attempts.push_back("baseManager.FindObject(BP_ConZBaseManager_C," + narrow(object_path) + ")=" + (object != nullptr ? safe_full_name(object) : "0"));
                if (object != nullptr) return object;
            }

            for (const auto* object_path : manager_paths)
            {
                auto* object = find_object_exact_noexcept(L"ConZBaseManager", object_path, 0, object_flag_class_default_object);
                base_manager_attempts.push_back("baseManager.FindObject(ConZBaseManager," + narrow(object_path) + ")=" + (object != nullptr ? safe_full_name(object) : "0"));
                if (object != nullptr) return object;
            }

            auto* narrow_first = RC::Unreal::UObjectGlobals::FindFirstOf(L"BP_ConZBaseManager_C");
            base_manager_attempts.push_back(std::string("baseManager.FindFirstOf(BP_ConZBaseManager_C)=") + (narrow_first != nullptr ? safe_full_name(narrow_first) : "0"));
            return narrow_first;
        };

        auto* base_manager = find_base_manager_exact();
        auto* base_element_class = object_property(base_manager, L"_baseElementClass");
        auto* base_class = object_property(base_manager, L"_baseClass");
        auto* flag_visualizer_class = object_property(base_manager, L"_flagAreaVisualizerClass");
        RC::Unreal::FVector base_manager_location{};
        const auto base_manager_location_ok = actor_location_noexcept(base_manager, base_manager_location);

        ss << "],\"baseManager\":{\"found\":" << (base_manager != nullptr ? "true" : "false")
           << ",\"fullName\":\"" << json_escape(safe_full_name(base_manager)) << "\""
           << ",\"classFullName\":\"" << json_escape(object_class_full_name_noexcept(base_manager)) << "\""
           << ",\"locationReadable\":" << (base_manager_location_ok ? "true" : "false")
           << ",\"location\":";
        append_vector_json(ss, base_manager_location);
        ss << ",\"baseClass\":\"" << json_escape(safe_full_name(base_class)) << "\""
           << ",\"baseElementClass\":\"" << json_escape(safe_full_name(base_element_class)) << "\""
           << ",\"flagAreaVisualizerClass\":\"" << json_escape(safe_full_name(flag_visualizer_class)) << "\""
           << ",\"lookupAttempts\":";
        append_json_string_array(ss, base_manager_attempts);
        ss << ",\"properties\":{";
        bool first_base_property = true;
        append_probe_float_property(ss, "damageEventProcessingPeriod", base_manager, L"_damageEventProcessingPeriod", first_base_property);
        append_probe_float_property(ss, "decayProcessingPeriod", base_manager, L"_decayProcessingPeriod", first_base_property);
        append_probe_float_property(ss, "flagOvertakePeriod", base_manager, L"_flagOvertakePeriod", first_base_property);
        append_probe_float_property(ss, "baseInteractionsPeriod", base_manager, L"_baseInteractionsPeriod", first_base_property);
        append_probe_float_property(ss, "flagInfluenceRadius", base_manager, L"_flagInfluenceRadius", first_base_property);
        append_probe_int32_property(ss, "flagOvertakeDuration", base_manager, L"_flagOvertakeDuration", first_base_property);
        append_probe_int32_property(ss, "maxElementsPerFlag", base_manager, L"_maxElementsPerFlag", first_base_property);
        append_probe_int32_property(ss, "extraElementsPerFlagForAdditionalSquadMember", base_manager, L"_extraElementsPerFlagForAdditionalSquadMember", first_base_property);
        append_probe_int32_property(ss, "maxNumberOfExpandedElementsPerFlag", base_manager, L"_maxNumberOfExpandedElementsPerFlag", first_base_property);
        append_probe_bool_property(ss, "allowMultipleFlagsPerPlayer", base_manager, L"_allowMultipleFlagsPerPlayer", first_base_property);
        append_probe_bool_property(ss, "allowFlagPlacementOnBBElements", base_manager, L"_allowFlagPlacementOnBBElements", first_base_property);
        append_probe_float_property(ss, "maxBaseBuildingHeight", base_manager, L"_maxBaseBuildingHeight", first_base_property);
        append_probe_presence_property(ss, "basesMap", base_manager, L"_bases", first_base_property);
        ss << "}}";

        struct BaseElementClassCandidate
        {
            const char* key;
            const wchar_t* class_path;
            const wchar_t* asset_path;
            const char* category;
            const char* risk;
        };

        static constexpr std::array<BaseElementClassCandidate, 5> base_element_candidates{{
            {"improvisedWoodenChest", L"/Game/ConZ_Files/BaseBuilding/BaseElements/BP_Base_Improvised_Wooden_Chest.BP_Base_Improvised_Wooden_Chest_C", L"/Game/ConZ_Files/BaseBuilding/BaseElements/BP_Base_Improvised_Wooden_Chest.BP_Base_Improvised_Wooden_Chest", "utility", "lowest-current-db-proof"},
            {"modularFoundationTwig", L"/Game/ConZ_Files/BaseBuilding/BaseElements/Modular/BP_Base_Modular_Foundation_Twig.BP_Base_Modular_Foundation_Twig_C", L"/Game/ConZ_Files/BaseBuilding/BaseElements/Modular/BP_Base_Modular_Foundation_Twig.BP_Base_Modular_Foundation_Twig", "modular", "first-structure-candidate"},
            {"modularWallTwig", L"/Game/ConZ_Files/BaseBuilding/BaseElements/Modular/BP_Base_Modular_Wall_Twig.BP_Base_Modular_Wall_Twig_C", L"/Game/ConZ_Files/BaseBuilding/BaseElements/Modular/BP_Base_Modular_Wall_Twig.BP_Base_Modular_Wall_Twig", "modular", "structure-candidate"},
            {"cabin", L"/Game/ConZ_Files/BaseBuilding/BaseElements/BP_Base_Cabin.BP_Base_Cabin_C", L"/Game/ConZ_Files/BaseBuilding/BaseElements/BP_Base_Cabin.BP_Base_Cabin", "structure", "large-object-candidate"},
            {"baseFlagSupporter", L"/Game/ConZ_Files/BaseBuilding/BaseElements/BP_Base_Flag_Supporter.BP_Base_Flag_Supporter_C", L"/Game/ConZ_Files/BaseBuilding/BaseElements/BP_Base_Flag_Supporter.BP_Base_Flag_Supporter", "utility", "current-db-proof-but-special-flag"},
        }};

        ss << ",\"baseElementClassCandidates\":[";
        int base_element_classes_found = 0;
        for (size_t index = 0; index < base_element_candidates.size(); ++index)
        {
            const auto& candidate = base_element_candidates[index];
            std::vector<std::string> attempts;
            auto* klass = find_object_exact_noexcept(L"BlueprintGeneratedClass", candidate.class_path, 0, object_flag_class_default_object);
            attempts.push_back("baseElementClass.FindObject(BlueprintGeneratedClass," + narrow(candidate.class_path) + ")=" + (klass != nullptr ? safe_full_name(klass) : "0"));
            if (klass == nullptr)
            {
                klass = find_object_exact_noexcept(L"Class", candidate.class_path, 0, object_flag_class_default_object);
                attempts.push_back("baseElementClass.FindObject(Class," + narrow(candidate.class_path) + ")=" + (klass != nullptr ? safe_full_name(klass) : "0"));
            }
            auto* cdo = class_default_object(klass, attempts);
            if (klass != nullptr) ++base_element_classes_found;

            if (index > 0) ss << ",";
            ss << "{\"key\":\"" << json_escape(candidate.key) << "\""
               << ",\"category\":\"" << json_escape(candidate.category) << "\""
               << ",\"risk\":\"" << json_escape(candidate.risk) << "\""
               << ",\"generatedClassPath\":\"" << json_escape(narrow(candidate.class_path)) << "\""
               << ",\"assetPath\":\"" << json_escape(narrow(candidate.asset_path)) << "\""
               << ",\"classFound\":" << (klass != nullptr ? "true" : "false")
               << ",\"classFullName\":\"" << json_escape(safe_full_name(klass)) << "\""
               << ",\"cdoFound\":" << (cdo != nullptr ? "true" : "false")
               << ",\"cdoFullName\":\"" << json_escape(safe_full_name(cdo)) << "\""
               << ",\"lookupAttempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
        }
        ss << "]";

        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        ss << ",\"summary\":{\"adminCommandTargets\":" << admin_targets.size()
           << ",\"adminCommandsFound\":" << admin_found
           << ",\"entitySetupTargets\":" << entity_targets.size()
           << ",\"entitySetupClassesFound\":" << entity_class_found
           << ",\"entitySetupCdosFound\":" << entity_cdo_found
           << ",\"baseManagerFound\":" << (base_manager != nullptr ? 1 : 0)
           << ",\"baseElementClassCandidates\":" << base_element_candidates.size()
           << ",\"baseElementClassesFound\":" << base_element_classes_found
           << ",\"durationMs\":" << duration_ms << "},"
           << "\"proofNext\":["
           << "\"If CreateEntity CDO is found, inspect verb/args/server flags before any whitelisted command execution.\","
           << "\"If typed EntitySetup CDOs expose Actor/Components, map only a concrete SCUM lifecycle family before mutation.\","
           << "\"BP_Base_* / AConZBaseManager is only a standard base-building reference family, not the arbitrary actor/outpost/trader solution.\","
           << "\"Do not test generic StaticMeshActor or SpawnActor persistence; previous SCUM tests already marked those routes invalid.\","
           << "\"For outpost/trader examples, research trader_economy registration through TradeOutpostManager and AConZEconomyManager, not base elements.\","
           << "\"Any later mutation test requires empty/no-player local server, second-client visibility, reconnect, restart, and DB/manager readback.\""
           << "]}";

        return ss.str();
    }

    auto SCUMTraderManager::armory_runtime_probe() const -> std::string
    {
        std::vector<std::string> tradepost_route_counts;
        std::vector<std::string> manager_route_counts;
        std::vector<std::string> trader_route_counts;

        auto* economy_manager = Unreal::UObjectGlobals::FindFirstOf(L"BP_EconomyManager_C");
        if (economy_manager == nullptr)
        {
            economy_manager = Unreal::UObjectGlobals::FindFirstOf(L"ConZEconomyManager");
        }
        auto* trader_component = object_property(economy_manager, L"_traderManagingComponent");

        auto trade_posts = find_named_objects_unique(
            {L"BP_Outpost_Armory_NPCInteractionBox_C"},
            {L"BP_Outpost_Armory_NPCInteractionBox_2"},
            &tradepost_route_counts);
        std::vector<RC::Unreal::UObject*> managers;
        std::unordered_set<RC::Unreal::UObject*> seen_managers;
        for (const auto* outpost_key : {"a_0", "b_4", "c_2", "z_3"})
        {
            for (const auto* class_name : {L"BP_TradeOutpostManager_C", L"TradeOutpostManager", L"ATradeOutpostManager", L"Object", L"UObject"})
            {
                for (const auto* object_path : manager_paths_for_outpost_key(outpost_key))
                {
                    auto* object = RC::Unreal::UObjectGlobals::FindObject(class_name, object_path, 0, object_flag_class_default_object);
                    std::ostringstream route;
                    route << "FindObject(" << narrow(class_name) << "," << narrow(object_path) << ")="
                          << (object != nullptr ? full_name(object) : "0");
                    manager_route_counts.push_back(route.str());
                    if (object != nullptr && manager_matches_outpost_key(object, outpost_key))
                    {
                        append_unique_object(managers, seen_managers, object);
                    }
                }
            }
        }
        auto traders = find_all_unique({L"BP_ArmsDealer_01_C"}, &trader_route_counts);

        auto append_optional_vector = [](std::ostringstream& out, const RC::Unreal::FVector* vector)
        {
            if (vector == nullptr)
            {
                out << "null";
                return;
            }
            append_vector_json(out, *vector);
        };

        std::ostringstream ss;
        ss << "{\"probe\":\"armory-runtime-narrow\","
           << "\"version\":\"" << mod_version << "\","
           << "\"outpostPin\":{\"pinned\":" << (m_outpost_runtime_pinned ? "true" : "false")
           << ",\"attempts\":" << m_outpost_pin_attempts
           << ",\"objectCount\":" << m_outpost_pin_object_count
           << ",\"last\":" << (m_last_outpost_pin_result.empty() ? "{}" : m_last_outpost_pin_result) << "},"
           << "\"engineAccess\":\"FindObjects by exact class/name plus one BP_ArmsDealer_01_C class scan\","
           << "\"economyManager\":";
        if (economy_manager == nullptr)
        {
            ss << "null";
        }
        else
        {
            const auto* pending_personalities = array_property(economy_manager, L"_pendingTraderPersonalities");
            ss << "{\"fullName\":\"" << json_escape(full_name(economy_manager)) << "\","
               << "\"traderManagingComponent\":\"" << json_escape(full_name(trader_component)) << "\","
               << "\"maxSaleDistance\":" << float_property(economy_manager, L"_maxSaleDistance", -1.0f) << ","
               << "\"pawnRelevancyRange\":" << float_property(trader_component, L"_pawnRelevancyRange", -1.0f) << ","
               << "\"tradeOutpostsUpdateTime\":" << float_property(trader_component, L"_tradeOutpostsUpdateTime", -1.0f) << ","
               << "\"pendingTraderPersonalitiesCount\":" << (is_reasonable_array(pending_personalities) ? pending_personalities->count : -1) << ","
               << "\"pendingTraderPersonalitySamples\":";
            append_object_array_samples_json(ss, pending_personalities, 8);
            ss << "}";
        }

        ss << ",\"search\":{\"tradePosts\":";
        append_json_string_array(ss, tradepost_route_counts);
        ss << ",\"managers\":";
        append_json_string_array(ss, manager_route_counts);
        ss << ",\"traders\":";
        append_json_string_array(ss, trader_route_counts);
        ss << "},\"counts\":{\"armoryTradePosts\":" << trade_posts.size()
           << ",\"tradeOutpostManagers\":" << managers.size()
           << ",\"armsDealers\":" << traders.size() << "},";

        ss << "\"managers\":[";
        for (size_t index = 0; index < managers.size(); ++index)
        {
            auto* manager = managers[index];
            auto* outpost_description = object_property(manager, L"_outpostDescription");
            const auto* assigned_trade_posts = array_property(manager, L"_assignedTradePosts");
            if (index > 0) ss << ",";
            ss << "{\"fullName\":\"" << json_escape(full_name(manager)) << "\","
               << "\"outpostKey\":\"" << json_escape(outpost_key_from_name(lower_copy(full_name(manager) + " " + full_name(outpost_description)))) << "\","
               << "\"outpostDescription\":\"" << json_escape(full_name(outpost_description)) << "\","
               << "\"outpostPersistentId\":\"" << json_escape(guid_to_string(guid_property(outpost_description, L"TradeOutpostPersistentId"))) << "\","
               << "\"assignedTradePostsCount\":" << (is_reasonable_array(assigned_trade_posts) ? assigned_trade_posts->count : -1) << ","
               << "\"assignedTradePostSamples\":";
            append_object_array_samples_json(ss, assigned_trade_posts, 8);
            ss << "}";
        }
        ss << "],";

        ss << "\"tradePosts\":[";
        for (size_t index = 0; index < trade_posts.size(); ++index)
        {
            auto* trade_post = trade_posts[index];
            auto* actor = static_cast<Unreal::AActor*>(trade_post);
            const auto location = actor != nullptr ? actor->K2_GetActorLocation() : RC::Unreal::FVector{};
            const auto rotation = actor != nullptr ? actor->K2_GetActorRotation() : RC::Unreal::FRotator{};
            std::string manager_route;
            auto* assigned_manager = find_manager_for_tradepost(managers, trade_post, &manager_route);
            const auto* trader_markers = array_property(trade_post, L"_traderMarkers");
            const auto* location_markers = array_property(trade_post, L"_locationMarkers");
            const auto* spawned_traders = array_property(trade_post, L"_spawnedTraders");
            const auto* spawned_sedentary_npcs = array_property(trade_post, L"_spawnedSedentaryNPCs");
            const auto* sedentary_markers = array_property(trade_post, L"_sedentaryNPCMarkers");
            if (index > 0) ss << ",";
            ss << "{\"fullName\":\"" << json_escape(full_name(trade_post)) << "\","
               << "\"outpostKey\":\"" << json_escape(outpost_key_from_name(lower_copy(full_name(trade_post)))) << "\","
               << "\"actorLocation\":";
            append_vector_json(ss, location);
            ss << ",\"actorRotation\":";
            append_rotator_json(ss, rotation);
            ss << ",\"assignedManager\":\"" << json_escape(full_name(assigned_manager)) << "\","
               << "\"assignedManagerRoute\":\"" << json_escape(manager_route) << "\","
               << "\"assignedToManager\":" << (assigned_manager != nullptr ? "true" : "false") << ","
               << "\"traderMarkersCount\":" << (is_reasonable_array(trader_markers) ? trader_markers->count : -1) << ","
               << "\"locationMarkersCount\":" << (is_reasonable_array(location_markers) ? location_markers->count : -1) << ","
               << "\"sedentaryNPCMarkersCount\":" << (is_reasonable_array(sedentary_markers) ? sedentary_markers->count : -1) << ","
               << "\"spawnedTradersCount\":" << (is_reasonable_array(spawned_traders) ? spawned_traders->count : -1) << ","
               << "\"spawnedSedentaryNPCsCount\":" << (is_reasonable_array(spawned_sedentary_npcs) ? spawned_sedentary_npcs->count : -1) << ","
               << "\"spawnedTraderSamples\":";
            append_object_array_samples_json(ss, spawned_traders, 8);
            ss << ",\"spawnedSedentaryNPCSamples\":";
            append_object_array_samples_json(ss, spawned_sedentary_npcs, 8);
            ss << ",\"traderMarkerSamples\":";
            append_trader_marker_samples(ss, trader_markers, 2);
            ss << ",\"locationMarkerSamples\":";
            append_location_marker_samples(ss, location_markers, 2);
            ss << "}";
        }
        ss << "],";

        ss << "\"armsDealers\":[";
        for (size_t index = 0; index < std::min<size_t>(traders.size(), 24); ++index)
        {
            auto* trader = traders[index];
            auto* actor = static_cast<Unreal::AActor*>(trader);
            const auto location = actor != nullptr ? actor->K2_GetActorLocation() : RC::Unreal::FVector{};
            auto* trade_post = object_property(trader, L"_tradeOutpostBuilding");
            auto* personality = weak_object_property(trader, L"_traderPersonalityDataAsset");
            const auto* original_location = static_cast<RC::Unreal::FVector*>(trader->GetValuePtrByPropertyNameInChain(L"_originalLocation"));
            std::string manager_route;
            auto* assigned_manager = find_manager_for_tradepost(managers, trade_post, &manager_route);
            if (index > 0) ss << ",";
            ss << "{\"fullName\":\"" << json_escape(full_name(trader)) << "\","
               << "\"actorLocation\":";
            append_vector_json(ss, location);
            ss << ",\"originalLocation\":";
            append_optional_vector(ss, original_location);
            ss << ",\"tradeOutpostBuilding\":\"" << json_escape(full_name(trade_post)) << "\","
               << "\"tradeOutpostManager\":\"" << json_escape(full_name(assigned_manager)) << "\","
               << "\"tradeOutpostManagerRoute\":\"" << json_escape(manager_route) << "\","
               << "\"personality\":\"" << json_escape(full_name(personality)) << "\","
               << "\"personalityGuid\":\"" << json_escape(guid_to_string(guid_property(personality, L"TraderPersistentId"))) << "\""
               << "}";
        }
        ss << "]}";
        return ss.str();
    }

    auto SCUMTraderManager::armory_trade_session_probe(const std::string& command_text) const -> std::string
    {
        std::vector<std::string> target_routes;
        std::vector<std::string> tradepost_routes;
        std::vector<std::string> trader_routes;
        std::vector<std::string> manager_routes;
        std::vector<std::string> economy_response_routes;
        std::vector<std::string> economy_response_data_routes;
        std::vector<std::string> trader_response_routes;
        std::vector<std::string> depot_routes;
        std::vector<std::string> depot_user_data_routes;
        std::vector<std::string> player_rpc_routes;

        auto requested_trader_full_name = regex_value(command_text, "traderFullName");
        if (requested_trader_full_name.empty()) requested_trader_full_name = regex_value(command_text, "targetTraderFullName");
        if (requested_trader_full_name.empty()) requested_trader_full_name = regex_value(command_text, "targetTrader");

        auto requested_tradepost_full_name = regex_value(command_text, "tradePostFullName");
        if (requested_tradepost_full_name.empty()) requested_tradepost_full_name = regex_value(command_text, "donorTradePost");
        if (requested_tradepost_full_name.empty()) requested_tradepost_full_name = regex_value(command_text, "targetTradePostFullName");

        auto outpost_key = canonical_outpost_key(regex_value(command_text, "outpostKey"));
        if (outpost_key.empty()) outpost_key = canonical_outpost_key(regex_value(command_text, "stockOutpostKey"));
        if (outpost_key.empty()) outpost_key = stocked_armory_outpost_key;

        auto* target_trader = find_object_by_full_name(
            {L"BP_ArmsDealer_01_C", L"Trader", L"ATrader", L"SedentaryNPC", L"Actor", L"Object", L"UObject"},
            requested_trader_full_name,
            target_routes,
            "targetTrader");

        auto* target_trade_post = find_object_by_full_name(
            {L"BP_Outpost_Armory_NPCInteractionBox_C", L"TradePost", L"ATradePost", L"Actor", L"Object", L"UObject"},
            requested_tradepost_full_name,
            target_routes,
            "targetTradePost");

        std::vector<RC::Unreal::UObject*> known_trade_posts;
        std::unordered_set<RC::Unreal::UObject*> known_trade_post_seen;
        if (target_trade_post != nullptr)
        {
            append_unique_object(known_trade_posts, known_trade_post_seen, target_trade_post);
        }

        const auto tradepost_paths = armory_tradepost_paths_for_outpost_key(outpost_key);
        for (const auto* class_name : {L"BP_Outpost_Armory_NPCInteractionBox_C", L"TradePost", L"ATradePost", L"Object", L"UObject"})
        {
            for (const auto* object_path : tradepost_paths)
            {
                auto* object = RC::Unreal::UObjectGlobals::FindObject(class_name, object_path, 0, object_flag_class_default_object);
                std::ostringstream route;
                route << "FindObject(" << narrow(class_name) << "," << narrow(object_path) << ")=" << (object != nullptr ? full_name(object) : "0");
                tradepost_routes.push_back(route.str());
                append_unique_object(known_trade_posts, known_trade_post_seen, object);
            }
        }

        for (const auto* class_name : {L"BP_Outpost_Armory_NPCInteractionBox_C", L"TradePost", L"ATradePost", L"Object", L"UObject"})
        {
            std::vector<RC::Unreal::UObject*> found;
            RC::Unreal::UObjectGlobals::FindObjects(class_name, L"BP_Outpost_Armory_NPCInteractionBox_2", found, 0, object_flag_class_default_object, false);
            std::ostringstream route;
            route << "FindObjects(" << narrow(class_name) << ",BP_Outpost_Armory_NPCInteractionBox_2)=" << found.size();
            if (!found.empty()) route << ":" << full_name(found.front());
            tradepost_routes.push_back(route.str());
            for (auto* object : found)
            {
                if (outpost_key_from_name(lower_copy(full_name(object))) == outpost_key)
                {
                    append_unique_object(known_trade_posts, known_trade_post_seen, object);
                }
            }
        }

        if (target_trade_post == nullptr && target_trader != nullptr)
        {
            target_trade_post = object_property(target_trader, L"_tradeOutpostBuilding");
            append_unique_object(known_trade_posts, known_trade_post_seen, target_trade_post);
        }
        if (target_trade_post == nullptr && !known_trade_posts.empty())
        {
            target_trade_post = known_trade_posts.front();
        }
        if (!full_name(target_trade_post).empty())
        {
            outpost_key = outpost_key_from_name(lower_copy(full_name(target_trade_post)));
            if (outpost_key.empty()) outpost_key = stocked_armory_outpost_key;
        }

        auto traders = find_all_unique({L"BP_ArmsDealer_01_C"}, &trader_routes);
        if (target_trader == nullptr)
        {
            for (auto* trader : traders)
            {
                auto* trader_trade_post = object_property(trader, L"_tradeOutpostBuilding");
                if (target_trade_post != nullptr && trader_trade_post == target_trade_post)
                {
                    target_trader = trader;
                    break;
                }
            }
        }
        if (target_trader == nullptr && !traders.empty())
        {
            target_trader = traders.front();
        }
        if (target_trade_post == nullptr && target_trader != nullptr)
        {
            target_trade_post = object_property(target_trader, L"_tradeOutpostBuilding");
            append_unique_object(known_trade_posts, known_trade_post_seen, target_trade_post);
        }

        auto managers = find_named_objects_unique(
            {L"BP_TradeOutpostManager_C", L"TradeOutpostManager", L"ATradeOutpostManager"},
            {L"BP_TradeOutpostManager_2", L"BP_TradeOutpostManager_B_4"},
            &manager_routes);
        std::string target_manager_route;
        auto* target_manager = find_manager_for_tradepost(managers, target_trade_post, &target_manager_route);

        auto economy_responses = find_all_unique({L"EconomyManagerResponse", L"UEconomyManagerResponse"}, &economy_response_routes);
        auto economy_response_data = find_all_unique({L"EconomyManagerResponseData", L"UEconomyManagerResponseData"}, &economy_response_data_routes);
        auto trader_responses = find_all_unique({L"TraderForPlayerAndDepotItemResponse", L"UTraderForPlayerAndDepotItemResponse"}, &trader_response_routes);
        auto depot_items = find_all_unique({L"BP_DepotItem_C", L"DepotItem", L"ADepotItem"}, &depot_routes);
        auto depot_user_data = find_all_unique({L"DepotItemUserData", L"UDepotItemUserData"}, &depot_user_data_routes);
        auto player_rpc_channels = find_all_unique({L"PlayerRpcChannel", L"UPlayerRpcChannel"}, &player_rpc_routes);

        auto trade_post_matches_target = [&](RC::Unreal::UObject* trade_post) -> bool
        {
            if (trade_post == nullptr) return false;
            if (target_trade_post != nullptr && trade_post == target_trade_post) return true;
            const auto name = full_name(trade_post);
            if (!requested_tradepost_full_name.empty() && name == requested_tradepost_full_name) return true;
            return !outpost_key.empty() && outpost_key_from_name(lower_copy(name)) == outpost_key;
        };

        auto trader_matches_target = [&](RC::Unreal::UObject* trader) -> bool
        {
            if (trader == nullptr) return false;
            if (target_trader != nullptr && trader == target_trader) return true;
            const auto name = full_name(trader);
            return !requested_trader_full_name.empty() && name == requested_trader_full_name;
        };

        auto append_object_name = [](std::ostringstream& ss, RC::Unreal::UObject* object)
        {
            ss << "\"" << json_escape(full_name(object)) << "\"";
        };

        auto append_actor_location = [](std::ostringstream& ss, RC::Unreal::UObject* object)
        {
            RC::Unreal::FVector location{};
            if (actor_location_noexcept(object, location))
            {
                append_vector_json(ss, location);
                return;
            }
            ss << "null";
        };

        const auto* target_spawned_traders = array_property(target_trade_post, L"_spawnedTraders");
        const auto* target_spawned_depots = array_property(target_trade_post, L"_spawnedDepots");
        const auto* target_trader_markers = array_property(target_trade_post, L"_traderMarkers");
        auto* target_personality = weak_object_property(target_trader, L"_traderPersonalityDataAsset");
        auto* target_trader_tradepost = object_property(target_trader, L"_tradeOutpostBuilding");

        int economy_response_payload_count = 0;
        int economy_response_payload_with_tradeables = 0;
        int economy_response_payload_matching_target = 0;
        int response_data_with_tradeables = 0;
        int response_data_matching_target_tradepost = 0;
        int response_data_matching_target_with_tradeables = 0;
        int depot_matching_target_tradepost = 0;
        int trader_response_matching_target = 0;
        bool any_buy_stock_evidence = false;
        bool target_buy_stock_evidence = false;

        std::ostringstream ss;
        ss << "{\"probe\":\"armory-trade-session-readonly\","
           << "\"version\":\"" << mod_version << "\","
           << "\"mutates\":false,"
           << "\"safeOnline\":true,"
           << "\"engineAccess\":\"exact target lookup plus narrow FindAllOf on EconomyManagerResponse/EconomyManagerResponseData/TraderForPlayerAndDepotItemResponse/DepotItem/PlayerRpcChannel classes; no ProcessEvent; no serializer deref; no array or map writes\","
           << "\"requested\":{\"traderFullName\":\"" << json_escape(requested_trader_full_name)
           << "\",\"tradePostFullName\":\"" << json_escape(requested_tradepost_full_name)
           << "\",\"outpostKey\":\"" << json_escape(outpost_key) << "\"},";

        ss << "\"target\":{\"trader\":";
        append_object_name(ss, target_trader);
        ss << ",\"traderLocation\":";
        append_actor_location(ss, target_trader);
        ss << ",\"traderTradePost\":";
        append_object_name(ss, target_trader_tradepost);
        ss << ",\"tradePost\":";
        append_object_name(ss, target_trade_post);
        ss << ",\"tradePostLocation\":";
        append_actor_location(ss, target_trade_post);
        ss << ",\"manager\":";
        append_object_name(ss, target_manager);
        ss << ",\"managerRoute\":\"" << json_escape(target_manager_route) << "\","
           << "\"personality\":";
        append_object_name(ss, target_personality);
        ss << ",\"personalityGuid\":\"" << json_escape(guid_to_string(guid_property(target_personality, L"TraderPersistentId"))) << "\","
           << "\"traderRegisteredInTradePost\":" << (array_contains_object(target_spawned_traders, target_trader) ? "true" : "false") << ","
           << "\"tradePostSpawnedTraders\":";
        append_array_count_json(ss, target_spawned_traders);
        ss << ",\"tradePostSpawnedDepots\":";
        append_array_count_json(ss, target_spawned_depots);
        ss << ",\"tradePostTraderMarkers\":";
        append_array_count_json(ss, target_trader_markers);
        ss << "},";

        ss << "\"search\":{\"targetRoutes\":";
        append_json_string_array(ss, target_routes);
        ss << ",\"tradePostRoutes\":";
        append_json_string_array(ss, tradepost_routes);
        ss << ",\"traderRoutes\":";
        append_json_string_array(ss, trader_routes);
        ss << ",\"managerRoutes\":";
        append_json_string_array(ss, manager_routes);
        ss << ",\"economyResponseRoutes\":";
        append_json_string_array(ss, economy_response_routes);
        ss << ",\"economyResponseDataRoutes\":";
        append_json_string_array(ss, economy_response_data_routes);
        ss << ",\"traderForDepotResponseRoutes\":";
        append_json_string_array(ss, trader_response_routes);
        ss << ",\"depotRoutes\":";
        append_json_string_array(ss, depot_routes);
        ss << ",\"depotUserDataRoutes\":";
        append_json_string_array(ss, depot_user_data_routes);
        ss << ",\"playerRpcRoutes\":";
        append_json_string_array(ss, player_rpc_routes);
        ss << "},";

        ss << "\"knownTradePosts\":[";
        for (size_t index = 0; index < known_trade_posts.size(); ++index)
        {
            auto* trade_post = known_trade_posts[index];
            const auto* spawned_traders = array_property(trade_post, L"_spawnedTraders");
            const auto* spawned_depots = array_property(trade_post, L"_spawnedDepots");
            if (index > 0) ss << ",";
            ss << "{\"fullName\":\"" << json_escape(full_name(trade_post)) << "\","
               << "\"outpostKey\":\"" << json_escape(outpost_key_from_name(lower_copy(full_name(trade_post)))) << "\","
               << "\"location\":";
            append_actor_location(ss, trade_post);
            ss << ",\"spawnedTraders\":";
            append_array_count_json(ss, spawned_traders);
            ss << ",\"spawnedTraderSamples\":";
            append_object_array_samples_json(ss, spawned_traders, 8);
            ss << ",\"spawnedDepots\":";
            append_array_count_json(ss, spawned_depots);
            ss << "}";
        }
        ss << "],";

        ss << "\"economyResponses\":[";
        for (size_t index = 0; index < std::min<size_t>(economy_responses.size(), 24); ++index)
        {
            auto* response = economy_responses[index];
            auto* payload = object_property(response, L"_payload");
            auto* assigned_depot = object_property(payload, L"_assignedDepotItem");
            auto* assigned_trade_post = object_property(assigned_depot, L"_assignedTradePost");
            const auto* tradeables = array_property(payload, L"_tradeables");
            const auto* special_deals = array_property(payload, L"_specialDeals");
            const auto tradeable_count = array_count_or_negative(tradeables);
            const auto matches_target = trade_post_matches_target(assigned_trade_post);
            if (payload != nullptr) ++economy_response_payload_count;
            if (tradeable_count > 0) ++economy_response_payload_with_tradeables;
            if (matches_target) ++economy_response_payload_matching_target;
            if (tradeable_count > 0 && assigned_trade_post != nullptr) any_buy_stock_evidence = true;
            if (tradeable_count > 0 && matches_target) target_buy_stock_evidence = true;
            if (index > 0) ss << ",";
            ss << "{\"fullName\":\"" << json_escape(full_name(response)) << "\","
               << "\"payload\":\"" << json_escape(full_name(payload)) << "\","
               << "\"assignedDepotItem\":\"" << json_escape(full_name(assigned_depot)) << "\","
               << "\"assignedDepotTradePost\":\"" << json_escape(full_name(assigned_trade_post)) << "\","
               << "\"matchesTargetTradePost\":" << (matches_target ? "true" : "false") << ","
               << "\"tradeables\":";
            append_array_count_json(ss, tradeables);
            ss << ",\"specialDeals\":";
            append_array_count_json(ss, special_deals);
            ss << "}";
        }
        ss << "],";

        ss << "\"economyResponseData\":[";
        for (size_t index = 0; index < std::min<size_t>(economy_response_data.size(), 48); ++index)
        {
            auto* data = economy_response_data[index];
            auto* assigned_depot = object_property(data, L"_assignedDepotItem");
            auto* assigned_trade_post = object_property(assigned_depot, L"_assignedTradePost");
            const auto* tradeables = array_property(data, L"_tradeables");
            const auto* special_deals = array_property(data, L"_specialDeals");
            const auto tradeable_count = array_count_or_negative(tradeables);
            const auto matches_target = trade_post_matches_target(assigned_trade_post);
            const auto looks_stock_ready = tradeable_count > 0 && assigned_depot != nullptr && assigned_trade_post != nullptr;
            if (tradeable_count > 0) ++response_data_with_tradeables;
            if (matches_target) ++response_data_matching_target_tradepost;
            if (matches_target && tradeable_count > 0) ++response_data_matching_target_with_tradeables;
            if (looks_stock_ready) any_buy_stock_evidence = true;
            if (matches_target && looks_stock_ready) target_buy_stock_evidence = true;
            if (index > 0) ss << ",";
            ss << "{\"fullName\":\"" << json_escape(full_name(data)) << "\","
               << "\"assignedDepotItem\":\"" << json_escape(full_name(assigned_depot)) << "\","
               << "\"assignedDepotTradePost\":\"" << json_escape(full_name(assigned_trade_post)) << "\","
               << "\"matchesTargetTradePost\":" << (matches_target ? "true" : "false") << ","
               << "\"looksStockReady\":" << (looks_stock_ready ? "true" : "false") << ","
               << "\"tradeables\":";
            append_array_count_json(ss, tradeables);
            ss << ",\"specialDeals\":";
            append_array_count_json(ss, special_deals);
            ss << "}";
        }
        ss << "],";

        ss << "\"traderForDepotResponses\":[";
        for (size_t index = 0; index < std::min<size_t>(trader_responses.size(), 32); ++index)
        {
            auto* response = trader_responses[index];
            auto* payload_trader = object_property(response, L"_payload");
            const auto matches_target = trader_matches_target(payload_trader);
            if (matches_target) ++trader_response_matching_target;
            if (index > 0) ss << ",";
            ss << "{\"fullName\":\"" << json_escape(full_name(response)) << "\","
               << "\"payloadTrader\":\"" << json_escape(full_name(payload_trader)) << "\","
               << "\"matchesTargetTrader\":" << (matches_target ? "true" : "false")
               << "}";
        }
        ss << "],";

        ss << "\"depotItems\":[";
        for (size_t index = 0; index < std::min<size_t>(depot_items.size(), 48); ++index)
        {
            auto* depot = depot_items[index];
            auto* assigned_trade_post = object_property(depot, L"_assignedTradePost");
            const auto matches_target = trade_post_matches_target(assigned_trade_post);
            if (matches_target) ++depot_matching_target_tradepost;
            if (index > 0) ss << ",";
            ss << "{\"fullName\":\"" << json_escape(full_name(depot)) << "\","
               << "\"assignedTradePost\":\"" << json_escape(full_name(assigned_trade_post)) << "\","
               << "\"matchesTargetTradePost\":" << (matches_target ? "true" : "false")
               << "}";
        }
        ss << "],";

        ss << "\"depotUserData\":[";
        for (size_t index = 0; index < std::min<size_t>(depot_user_data.size(), 24); ++index)
        {
            auto* user_data = depot_user_data[index];
            if (index > 0) ss << ",";
            ss << "{\"fullName\":\"" << json_escape(full_name(user_data)) << "\","
               << "\"traderPersistentId\":\"" << json_escape(string_property(user_data, L"TraderPersistentId")) << "\""
               << "}";
        }
        ss << "],";

        auto missing_reason = std::string{};
        if (target_trader == nullptr)
        {
            missing_reason = "no-target-trader-resolved";
        }
        else if (target_trade_post == nullptr)
        {
            missing_reason = "no-target-tradepost-resolved";
        }
        else if (!array_contains_object(target_spawned_traders, target_trader))
        {
            missing_reason = "target-trader-not-registered-in-tradepost-spawnedTraders";
        }
        else if (response_data_matching_target_with_tradeables <= 0 && economy_response_payload_matching_target <= 0)
        {
            missing_reason = response_data_with_tradeables > 0 || economy_response_payload_with_tradeables > 0
                ? "stock-response-exists-but-not-for-target-tradepost"
                : "no-economy-response-data-with-tradeables";
        }
        else if (!target_buy_stock_evidence)
        {
            missing_reason = "target-economy-response-has-no-stock-ready-depot";
        }
        else
        {
            missing_reason = "target-stock-session-observed";
        }

        ss << "\"counts\":{\"armsDealers\":" << traders.size()
           << ",\"knownTradePosts\":" << known_trade_posts.size()
           << ",\"tradeOutpostManagers\":" << managers.size()
           << ",\"playerRpcChannels\":" << player_rpc_channels.size()
           << ",\"economyResponses\":" << economy_responses.size()
           << ",\"economyResponsePayloads\":" << economy_response_payload_count
           << ",\"economyResponsePayloadsWithTradeables\":" << economy_response_payload_with_tradeables
           << ",\"economyResponsePayloadsMatchingTarget\":" << economy_response_payload_matching_target
           << ",\"economyResponseData\":" << economy_response_data.size()
           << ",\"economyResponseDataWithTradeables\":" << response_data_with_tradeables
           << ",\"economyResponseDataMatchingTargetTradePost\":" << response_data_matching_target_tradepost
           << ",\"economyResponseDataMatchingTargetWithTradeables\":" << response_data_matching_target_with_tradeables
           << ",\"traderForDepotResponses\":" << trader_responses.size()
           << ",\"traderForDepotResponsesMatchingTargetTrader\":" << trader_response_matching_target
           << ",\"depotItems\":" << depot_items.size()
           << ",\"depotItemsMatchingTargetTradePost\":" << depot_matching_target_tradepost
           << ",\"depotUserData\":" << depot_user_data.size()
           << "},"
           << "\"evidence\":{\"anyBuyStockEvidence\":" << (any_buy_stock_evidence ? "true" : "false")
           << ",\"targetBuyStockEvidence\":" << (target_buy_stock_evidence ? "true" : "false")
           << ",\"missingReason\":\"" << json_escape(missing_reason) << "\"}"
           << "}";
        return ss.str();
    }

    auto SCUMTraderManager::armory_identity_probe(const std::string& command_text) const -> std::string
    {
        auto outpost_key = canonical_outpost_key(regex_value(command_text, "outpostKey"));
        if (outpost_key.empty())
        {
            outpost_key = "a_0";
        }

        std::vector<std::string> tradepost_routes;
        std::vector<std::string> manager_routes;
        std::vector<RC::Unreal::UObject*> trade_posts;
        std::vector<RC::Unreal::UObject*> managers;
        std::unordered_set<RC::Unreal::UObject*> seen_trade_posts;
        std::unordered_set<RC::Unreal::UObject*> seen_managers;

        const auto tradepost_paths = armory_tradepost_paths_for_outpost_key(outpost_key);
        for (const auto* class_name : {L"BP_Outpost_Armory_NPCInteractionBox_C", L"TradePost", L"ATradePost", L"Object", L"UObject"})
        {
            for (const auto* object_path : tradepost_paths)
            {
                auto* object = RC::Unreal::UObjectGlobals::FindObject(class_name, object_path, 0, object_flag_class_default_object);
                std::ostringstream route;
                route << "FindObject(" << narrow(class_name) << "," << narrow(object_path) << ")=" << (object != nullptr ? full_name(object) : "0");
                tradepost_routes.push_back(route.str());
                append_unique_object(trade_posts, seen_trade_posts, object);
            }
        }

        for (const auto* class_name : {L"BP_Outpost_Armory_NPCInteractionBox_C", L"TradePost", L"ATradePost", L"Object", L"UObject"})
        {
            std::vector<RC::Unreal::UObject*> found;
            RC::Unreal::UObjectGlobals::FindObjects(class_name, L"BP_Outpost_Armory_NPCInteractionBox_2", found, 0, object_flag_class_default_object, false);
            std::ostringstream route;
            route << "FindObjects(" << narrow(class_name) << ",BP_Outpost_Armory_NPCInteractionBox_2)=" << found.size();
            if (!found.empty()) route << ":" << full_name(found.front());
            tradepost_routes.push_back(route.str());
            for (auto* object : found)
            {
                if (outpost_key_from_name(lower_copy(full_name(object))) == outpost_key)
                {
                    append_unique_object(trade_posts, seen_trade_posts, object);
                }
            }
        }

        const auto manager_paths = manager_paths_for_outpost_key(outpost_key);
        for (const auto* class_name : {L"BP_TradeOutpostManager_C", L"TradeOutpostManager", L"ATradeOutpostManager", L"Object", L"UObject"})
        {
            for (const auto* object_path : manager_paths)
            {
                auto* object = RC::Unreal::UObjectGlobals::FindObject(class_name, object_path, 0, object_flag_class_default_object);
                std::ostringstream route;
                route << "FindObject(" << narrow(class_name) << "," << narrow(object_path) << ")=" << (object != nullptr ? full_name(object) : "0");
                manager_routes.push_back(route.str());
                append_unique_object(managers, seen_managers, object);
            }
        }

        std::vector<const wchar_t*> manager_names{L"BP_TradeOutpostManager_2"};
        if (outpost_key == "b_4")
        {
            manager_names.insert(manager_names.begin(), L"BP_TradeOutpostManager_B_4");
        }
        for (const auto* class_name : {L"BP_TradeOutpostManager_C", L"TradeOutpostManager", L"ATradeOutpostManager", L"Object", L"UObject"})
        {
            for (const auto* object_name : manager_names)
            {
                std::vector<RC::Unreal::UObject*> found;
                RC::Unreal::UObjectGlobals::FindObjects(class_name, object_name, found, 0, object_flag_class_default_object, false);
                std::ostringstream route;
                route << "FindObjects(" << narrow(class_name) << "," << narrow(object_name) << ")=" << found.size();
                if (!found.empty()) route << ":" << full_name(found.front());
                manager_routes.push_back(route.str());
                for (auto* object : found)
                {
                    if (manager_matches_outpost_key(object, outpost_key))
                    {
                        append_unique_object(managers, seen_managers, object);
                    }
                }
            }
        }

        RC::Unreal::UObject* selected_trade_post = nullptr;
        for (auto* trade_post : trade_posts)
        {
            if (outpost_key_from_name(lower_copy(full_name(trade_post))) == outpost_key)
            {
                selected_trade_post = trade_post;
                break;
            }
        }
        if (selected_trade_post == nullptr && !trade_posts.empty())
        {
            selected_trade_post = trade_posts.front();
        }

        auto* assigned_manager = find_assigned_manager(managers, selected_trade_post);
        std::string diagnostic_manager_route;
        auto* diagnostic_manager = find_manager_for_tradepost(managers, selected_trade_post, &diagnostic_manager_route);
        auto* economy_manager = Unreal::UObjectGlobals::FindFirstOf(L"BP_EconomyManager_C");
        if (economy_manager == nullptr)
        {
            economy_manager = Unreal::UObjectGlobals::FindFirstOf(L"ConZEconomyManager");
        }
        auto* trader_component = object_property(economy_manager, L"_traderManagingComponent");
        const auto* pending_personalities = array_property(economy_manager, L"_pendingTraderPersonalities");

        auto append_tradepost_summary = [&](std::ostringstream& out, RC::Unreal::UObject* trade_post) -> void
        {
            const auto* trader_markers = array_property(trade_post, L"_traderMarkers");
            const auto* location_markers = array_property(trade_post, L"_locationMarkers");
            const auto* spawned_traders = array_property(trade_post, L"_spawnedTraders");
            const auto* spawned_sedentary = array_property(trade_post, L"_spawnedSedentaryNPCs");
            RC::Unreal::FVector location{};
            const auto location_ok = actor_location_noexcept(trade_post, location);
            out << "{\"fullName\":\"" << json_escape(full_name(trade_post)) << "\","
                << "\"outpostKey\":\"" << json_escape(outpost_key_from_name(lower_copy(full_name(trade_post)))) << "\","
                << "\"actorLocationOk\":" << (location_ok ? "true" : "false") << ","
                << "\"actorLocation\":";
            append_vector_json(out, location);
            out << ",\"traderMarkersCount\":" << (is_reasonable_array(trader_markers) ? trader_markers->count : -1)
                << ",\"locationMarkersCount\":" << (is_reasonable_array(location_markers) ? location_markers->count : -1)
                << ",\"spawnedTradersCount\":" << (is_reasonable_array(spawned_traders) ? spawned_traders->count : -1)
                << ",\"spawnedSedentaryNPCsCount\":" << (is_reasonable_array(spawned_sedentary) ? spawned_sedentary->count : -1)
                << ",\"spawnedTraderSamples\":";
            append_object_array_samples_json(out, spawned_traders, 8);
            out << ",\"spawnedSedentaryNPCSamples\":";
            append_object_array_samples_json(out, spawned_sedentary, 8);
            out << ",\"traderMarkerSamples\":";
            append_trader_marker_samples(out, trader_markers, 3);
            out << "}";
        };

        auto append_manager_summary = [&](std::ostringstream& out, RC::Unreal::UObject* manager) -> void
        {
            auto* outpost_description = object_property(manager, L"_outpostDescription");
            const auto* assigned_trade_posts = array_property(manager, L"_assignedTradePosts");
            const auto* other_buildings = array_property(manager, L"_otherAssignedTradeOutpostBuildings");
            const auto contains_selected = array_contains_object(assigned_trade_posts, selected_trade_post);
            out << "{\"fullName\":\"" << json_escape(full_name(manager)) << "\","
                << "\"outpostKey\":\"" << json_escape(outpost_key_from_name(lower_copy(full_name(manager) + " " + full_name(outpost_description)))) << "\","
                << "\"outpostDescription\":\"" << json_escape(full_name(outpost_description)) << "\","
                << "\"outpostPersistentId\":\"" << json_escape(guid_to_string(guid_property(outpost_description, L"TradeOutpostPersistentId"))) << "\","
                << "\"assignedContainsSelectedArmory\":" << (contains_selected ? "true" : "false") << ","
                << "\"assignedTradePostsCount\":" << (is_reasonable_array(assigned_trade_posts) ? assigned_trade_posts->count : -1) << ","
                << "\"otherAssignedBuildingsCount\":" << (is_reasonable_array(other_buildings) ? other_buildings->count : -1) << ","
                << "\"assignedTradePostSamples\":";
            append_object_array_samples_json(out, assigned_trade_posts, 12);
            out << ",\"otherAssignedBuildingSamples\":";
            append_object_array_samples_json(out, other_buildings, 8);
            out << "}";
        };

        const auto strict_lifecycle_ready = selected_trade_post != nullptr && assigned_manager != nullptr;
        std::ostringstream ss;
        ss << "{\"probe\":\"armory-identity-exact-read-only\","
           << "\"version\":\"" << mod_version << "\","
           << "\"mutates\":false,"
           << "\"safeOnline\":true,"
           << "\"outpostKey\":\"" << json_escape(outpost_key) << "\","
           << "\"strictLifecycleReady\":" << (strict_lifecycle_ready ? "true" : "false") << ","
           << "\"reason\":\"Functional buy stock requires selected Armory TradePost to be present in a live TradeOutpostManager._assignedTradePosts array.\","
           << "\"economyManager\":";
        if (economy_manager == nullptr)
        {
            ss << "null";
        }
        else
        {
            ss << "{\"fullName\":\"" << json_escape(full_name(economy_manager)) << "\","
               << "\"traderManagingComponent\":\"" << json_escape(full_name(trader_component)) << "\","
               << "\"maxSaleDistance\":" << float_property(economy_manager, L"_maxSaleDistance", -1.0f) << ","
               << "\"pawnRelevancyRange\":" << float_property(trader_component, L"_pawnRelevancyRange", -1.0f) << ","
               << "\"tradeOutpostsUpdateTime\":" << float_property(trader_component, L"_tradeOutpostsUpdateTime", -1.0f) << ","
               << "\"pendingTraderPersonalitiesCount\":" << (is_reasonable_array(pending_personalities) ? pending_personalities->count : -1) << ","
               << "\"pendingTraderPersonalitySamples\":";
            append_object_array_samples_json(ss, pending_personalities, 8);
            ss << "}";
        }

        ss << ",\"selectedTradePost\":";
        if (selected_trade_post == nullptr)
        {
            ss << "null";
        }
        else
        {
            append_tradepost_summary(ss, selected_trade_post);
        }

        ss << ",\"assignedManager\":";
        if (assigned_manager == nullptr)
        {
            ss << "null";
        }
        else
        {
            append_manager_summary(ss, assigned_manager);
        }

        ss << ",\"diagnosticManager\":\"" << json_escape(full_name(diagnostic_manager)) << "\","
           << "\"diagnosticManagerRoute\":\"" << json_escape(diagnostic_manager_route) << "\","
           << "\"counts\":{\"armoryTradePosts\":" << trade_posts.size() << ",\"managerCandidates\":" << managers.size() << "},"
           << "\"tradePosts\":[";
        for (std::size_t index = 0; index < trade_posts.size(); ++index)
        {
            if (index > 0) ss << ",";
            append_tradepost_summary(ss, trade_posts[index]);
        }
        ss << "],\"managerCandidates\":[";
        for (std::size_t index = 0; index < managers.size(); ++index)
        {
            if (index > 0) ss << ",";
            append_manager_summary(ss, managers[index]);
        }
        ss << "],\"search\":{\"tradePosts\":";
        append_json_string_array(ss, tradepost_routes);
        ss << ",\"managers\":";
        append_json_string_array(ss, manager_routes);
        ss << "}}";
        return ss.str();
    }

    auto SCUMTraderManager::verify_armory_linked_trader(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        const auto trader_full_name =
            regex_value(command_text, "traderFullName").empty()
                ? regex_value(command_text, "spawnedTrader")
                : regex_value(command_text, "traderFullName");
        const auto expected_tradepost_full_name = regex_value(command_text, "donorTradePostFullName").empty()
            ? regex_value(command_text, "donorTradePost")
            : regex_value(command_text, "donorTradePostFullName");
        const auto expected_personality_guid = regex_value(command_text, "traderPersistentId").empty()
            ? regex_value(command_text, "personalityGuid")
            : regex_value(command_text, "traderPersistentId");
        const auto requested_allow_managerless_linked =
            regex_bool(command_text, "allowManagerlessLinkedTrader", false) ||
            regex_bool(command_text, "allowManagerlessLinked", false);
        const auto require_manager_linked =
            regex_bool(command_text, "requireManagerLinked", regex_bool(command_text, "requireLinkedManager", true));
        const auto allow_managerless_linked = requested_allow_managerless_linked && !require_manager_linked;

        std::vector<std::string> attempts;
        auto* trader = find_object_by_full_name(
            {L"BP_ArmsDealer_01_C", L"Trader", L"ATrader", L"SedentaryNPC", L"Object"},
            trader_full_name,
            attempts,
            "trader");

        auto* trade_post = object_property(trader, L"_tradeOutpostBuilding");
        auto* personality = weak_object_property(trader, L"_traderPersonalityDataAsset");
        const auto personality_guid = guid_to_string(guid_property(personality, L"TraderPersistentId"));
        const auto trader_text = lower_copy(full_name(trader) + " " + full_name(personality));
        const auto tradepost_text = lower_copy(full_name(trade_post));
        const auto outpost_key = outpost_key_from_name(tradepost_text + " " + lower_copy(full_name(personality)));

        std::vector<std::string> manager_route_counts;
        auto managers = outpost_key.empty()
            ? find_trade_outpost_managers(&manager_route_counts)
            : find_trade_outpost_managers_for_key(outpost_key, &manager_route_counts);
        auto* assigned_manager = find_assigned_manager(managers, trade_post);
        std::string manager_route;
        auto* manager = find_manager_for_tradepost(managers, trade_post, &manager_route);
        const auto* assigned_trade_posts = array_property(assigned_manager, L"_assignedTradePosts");
        const auto* spawned_traders = array_property(trade_post, L"_spawnedTraders");
        const auto in_spawned_traders = array_contains_object(spawned_traders, trader);
        const auto tradepost_expected_match =
            expected_tradepost_full_name.empty() ||
            (trade_post != nullptr && full_name(trade_post) == expected_tradepost_full_name);
        const auto personality_expected_match =
            expected_personality_guid.empty() ||
            (!personality_guid.empty() && personality_guid == expected_personality_guid);
        const auto looks_armory =
            trader_text.find("armsdealer") != std::string::npos ||
            trader_text.find("arms_dealer") != std::string::npos ||
            trader_text.find("armory") != std::string::npos;
        const auto managerless_stock_personality_ok = false;
        const auto managerless_marker_link_ok =
            allow_managerless_linked &&
            assigned_manager == nullptr &&
            trade_post != nullptr &&
            in_spawned_traders;
        const auto lifecycle_link_ok = assigned_manager != nullptr || managerless_marker_link_ok;
        const auto ok =
            trader != nullptr &&
            trade_post != nullptr &&
            personality != nullptr &&
            lifecycle_link_ok &&
            in_spawned_traders &&
            looks_armory &&
            tradepost_expected_match &&
            personality_expected_match;

        std::ostringstream ss;
        ss << "{\"ok\":" << (ok ? "true" : "false") << ","
           << "\"route\":\"verify-armory-linked-trader\","
           << "\"version\":\"" << mod_version << "\","
           << "\"requestedTraderFullName\":\"" << json_escape(trader_full_name) << "\","
           << "\"trader\":\"" << json_escape(full_name(trader)) << "\","
           << "\"traderResolved\":" << (trader != nullptr ? "true" : "false") << ","
           << "\"looksArmory\":" << (looks_armory ? "true" : "false") << ","
           << "\"tradePost\":\"" << json_escape(full_name(trade_post)) << "\","
           << "\"expectedTradePost\":\"" << json_escape(expected_tradepost_full_name) << "\","
           << "\"tradePostExpectedMatch\":" << (tradepost_expected_match ? "true" : "false") << ","
           << "\"personality\":\"" << json_escape(full_name(personality)) << "\","
           << "\"personalityGuid\":\"" << json_escape(personality_guid) << "\","
           << "\"expectedPersonalityGuid\":\"" << json_escape(expected_personality_guid) << "\","
           << "\"personalityExpectedMatch\":" << (personality_expected_match ? "true" : "false") << ","
           << "\"outpostKey\":\"" << json_escape(outpost_key) << "\","
           << "\"manager\":\"" << json_escape(full_name(manager)) << "\","
           << "\"managerRoute\":\"" << json_escape(manager_route) << "\","
           << "\"assignedManager\":\"" << json_escape(full_name(assigned_manager)) << "\","
           << "\"managerAssigned\":" << (assigned_manager != nullptr ? "true" : "false") << ","
           << "\"requireManagerLinked\":" << (require_manager_linked ? "true" : "false") << ","
           << "\"allowManagerlessLinkedTrader\":" << (allow_managerless_linked ? "true" : "false") << ","
           << "\"requestedAllowManagerlessLinkedTrader\":" << (requested_allow_managerless_linked ? "true" : "false") << ","
           << "\"managerlessStockPersonalityOk\":" << (managerless_stock_personality_ok ? "true" : "false") << ","
           << "\"managerlessMarkerLinkOk\":" << (managerless_marker_link_ok ? "true" : "false") << ","
           << "\"lifecycleLinkOk\":" << (lifecycle_link_ok ? "true" : "false") << ","
           << "\"managerCount\":" << managers.size() << ","
           << "\"assignedTradePostsCount\":" << (is_reasonable_array(assigned_trade_posts) ? assigned_trade_posts->count : -1) << ","
           << "\"spawnedTradersCount\":" << (is_reasonable_array(spawned_traders) ? spawned_traders->count : -1) << ","
           << "\"inSpawnedTraders\":" << (in_spawned_traders ? "true" : "false") << ","
           << "\"managerSearch\":";
        append_json_string_array(ss, manager_route_counts);
        ss << ",\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << "}";

        return {ok, ss.str()};
    }

    auto SCUMTraderManager::link_existing_summoned_trader(const std::string& command_text) -> std::pair<bool, std::string>
    {
        const auto actor_class = regex_value(command_text, "actorClass").empty()
            ? std::string("BP_ArmsDealer_01_C")
            : regex_value(command_text, "actorClass");
        const auto trader_full_name =
            regex_value(command_text, "traderFullName").empty()
                ? regex_value(command_text, "spawnedTrader")
                : regex_value(command_text, "traderFullName");
        const auto x = regex_number(command_text, "x", 0.0);
        const auto y = regex_number(command_text, "y", 0.0);
        const auto z = regex_number(command_text, "z", 0.0);
        const auto max_link_distance = std::clamp(regex_number(command_text, "maxLinkDistance", 12000.0), 500.0, 100000.0);
        const auto requested_allow_managerless_marker_fallback =
            regex_bool(command_text, "allowManagerlessLinkedTrader", false) ||
            regex_bool(command_text, "allowManagerlessLinked", false);
        const auto require_manager_linked =
            regex_bool(command_text, "requireManagerLinked", regex_bool(command_text, "requireLinkedManager", true));
        const auto allow_manual_spawned_trader_array_mutation_requested =
            regex_bool(command_text, "allowManualSpawnedTraderArrayMutation", false) ||
            regex_bool(command_text, "allowUnsafeSpawnedTraderArrayMutation", false);
        const auto allow_exact_manager_linked_armory_array_mutation =
            regex_bool(command_text, "allowExactManagerLinkedArmoryArrayMutation", false) ||
            regex_bool(command_text, "allowExactStockArmoryArrayMutation", false);
        const auto allow_exact_stock_armory_managerless_no_array_candidate =
            regex_bool(command_text, "allowExactStockArmoryManagerlessNoArrayCandidate", false) ||
            regex_bool(command_text, "allowExactManagerlessArmoryNoArrayCandidate", false);
        auto allow_manual_spawned_trader_array_mutation = false;
        const auto force_stock_personality = regex_bool(command_text, "forceStockPersonality", false);
        const auto requested_personality_path = regex_value(command_text, "personalityPath");
        const auto stock_outpost_key = canonical_outpost_key(regex_value(command_text, "stockOutpostKey"));
        const auto fast_manager_lookup_only =
            regex_bool(command_text, "fastManagerLookupOnly", false) ||
            regex_bool(command_text, "skipSlowManagerFallbacks", false);

        std::vector<std::string> attempts;
        if (std::abs(x) < 1.0 && std::abs(y) < 1.0 && std::abs(z) < 1.0)
        {
            std::ostringstream ss;
            ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
               << "\"ok\":false,"
               << "\"stage\":\"resolve-spawn-location\","
               << "\"route\":\"armory-summon-existing-link\","
               << "\"message\":\"Player/world coordinates are missing; refusing Armory trader link before donor or manager lookup\","
               << "\"x\":" << x << ",\"y\":" << y << ",\"z\":" << z << "}";
            return {false, ss.str()};
        }
        auto* trader = find_object_by_full_name(
            {L"BP_ArmsDealer_01_C", L"Trader", L"ATrader", L"SedentaryNPC", L"Object", L"UObject"},
            trader_full_name,
            attempts,
            "summonedTrader");

        const auto tokens = trader_kind_tokens(actor_class);
        double donor_lookup_x = x;
        double donor_lookup_y = y;
        double donor_selection_distance = std::numeric_limits<double>::max();
        if (force_stock_personality && !stock_outpost_key.empty())
        {
            double forced_x{};
            double forced_y{};
            if (stock_outpost_center(stock_outpost_key, forced_x, forced_y))
            {
                donor_lookup_x = forced_x;
                donor_lookup_y = forced_y;
                attempts.push_back("donor search forced to stock outpost " + stock_outpost_key);
            }
        }
        auto* donor_trade_post = find_donor_tradepost_for_actor(tokens, donor_lookup_x, donor_lookup_y, attempts, donor_selection_distance);
        const auto donor_distance = std::isfinite(donor_selection_distance)
            ? donor_selection_distance
            : (donor_trade_post != nullptr ? tradepost_reference_distance_2d(donor_trade_post, x, y) : donor_selection_distance);
        const auto donor_outpost_key = outpost_key_from_name(lower_copy(full_name(donor_trade_post)));

        std::vector<std::string> manager_route_counts;
        auto managers = donor_outpost_key.empty()
            ? find_trade_outpost_managers(&manager_route_counts)
            : find_trade_outpost_managers_for_key(donor_outpost_key, &manager_route_counts, !fast_manager_lookup_only);
        std::string donor_manager_route;
        auto* donor_manager_candidate = find_manager_for_tradepost(managers, donor_trade_post, &donor_manager_route);
        auto* donor_assigned_manager = find_assigned_manager(managers, donor_trade_post);
        auto* donor_manager = donor_assigned_manager;

        const auto* markers = array_property(donor_trade_post, L"_traderMarkers");
        const auto marker_count = is_reasonable_array(markers) ? markers->count : -1;
        RC::Unreal::UObject* marker_personality = nullptr;
        std::string marker_personality_name;
        std::string marker_personality_guid;
        if (is_reasonable_array(markers) && markers->count > 0 && markers->data != nullptr)
        {
            auto* marker_items = static_cast<FTraderMarkerAbi*>(markers->data);
            marker_personality = marker_items[0].trader_personality;
            marker_personality_name = full_name(marker_personality);
            marker_personality_guid = guid_to_string(guid_property(marker_personality, L"TraderPersistentId"));
        }

        RC::Unreal::UObject* requested_personality = nullptr;
        std::string requested_selected_outpost_key;
        std::string requested_selected_kind;
        std::string requested_selected_path;
        if (force_stock_personality && !requested_personality_path.empty())
        {
            requested_personality = resolve_trader_personality_asset(
                actor_class,
                requested_personality_path,
                x,
                y,
                requested_selected_outpost_key,
                requested_selected_kind,
                requested_selected_path,
                attempts);
        }
        auto* selected_personality = requested_personality != nullptr ? requested_personality : marker_personality;

        RC::Unreal::FVector actual_location{};
        const auto actor_location_ok = actor_location_noexcept(trader, actual_location);
        const auto requested_position_valid = std::abs(x) > 1.0 || std::abs(y) > 1.0 || std::abs(z) > 1.0;
        const auto distance_to_player = requested_position_valid
            ? vector_distance_3d(actual_location, make_vector(x, y, z))
            : 0.0;
        const auto near_player = actor_location_ok && (!requested_position_valid || distance_to_player <= max_link_distance);
        const auto not_origin = actor_location_ok &&
            (std::abs(actual_location.x) > 1.0f || std::abs(actual_location.y) > 1.0f || std::abs(actual_location.z) > 1.0f);
        const auto location_ok = near_player && not_origin;

        const auto managerless_mutation_allowed =
            donor_manager == nullptr &&
            !require_manager_linked &&
            requested_allow_managerless_marker_fallback;
        const auto would_mutate_tradepost_graph = donor_manager != nullptr || managerless_mutation_allowed;
        if (would_mutate_tradepost_graph && !allow_manual_spawned_trader_array_mutation)
        {
            attempts.push_back("blocked: summon-link route would manually mutate trader properties and _spawnedTraders/_spawnedSedentaryNPCs; stock lifecycle API is not proven");
        }
        const auto can_mutate_tradepost_graph =
            trader != nullptr &&
            donor_trade_post != nullptr &&
            selected_personality != nullptr &&
            location_ok &&
            would_mutate_tradepost_graph &&
            allow_manual_spawned_trader_array_mutation;

        const auto mutation_skip_detail = would_mutate_tradepost_graph && !allow_manual_spawned_trader_array_mutation
            ? "skipped:manual-spawned-trader-array-mutation-blocked"
            : "skipped:manager-link-required-before-tradepost-mutation";
        std::string tradepost_detail = can_mutate_tradepost_graph ? "not-run" : mutation_skip_detail;
        const auto tradepost_set = can_mutate_tradepost_graph &&
            set_object_property(trader, L"_tradeOutpostBuilding", donor_trade_post, tradepost_detail);

        std::string personality_detail = can_mutate_tradepost_graph ? "not-run" : mutation_skip_detail;
        const auto personality_set = can_mutate_tradepost_graph &&
            set_weak_object_property(trader, L"_traderPersonalityDataAsset", selected_personality, personality_detail);
        auto* personality_after = weak_object_property(trader, L"_traderPersonalityDataAsset");
        const auto personality_after_guid = guid_to_string(guid_property(personality_after, L"TraderPersistentId"));

        std::string original_location_detail = can_mutate_tradepost_graph ? "not-run" : mutation_skip_detail;
        const auto original_location_set = can_mutate_tradepost_graph && actor_location_ok &&
            set_vector_property(trader, L"_originalLocation", actual_location, original_location_detail);

        std::string net_cull_detail = can_mutate_tradepost_graph ? "not-run" : mutation_skip_detail;
        const auto net_cull_set = can_mutate_tradepost_graph &&
            set_float_property(trader, L"NetCullDistanceSquared", 4000000000000.0f, net_cull_detail, true);
        std::string interaction_distance_detail = can_mutate_tradepost_graph ? "not-run" : mutation_skip_detail;
        const auto interaction_distance_set = can_mutate_tradepost_graph &&
            set_float_property(trader, L"_interactionDistance", 5000.0f, interaction_distance_detail, true);

        auto* spawned_traders = const_cast<TArrayAbi*>(array_property(donor_trade_post, L"_spawnedTraders"));
        const auto before_count = is_reasonable_array(spawned_traders) ? spawned_traders->count : -1;
        const auto before_max = is_reasonable_array(spawned_traders) ? spawned_traders->max : -1;
        std::string add_detail = can_mutate_tradepost_graph ? "not-run" : mutation_skip_detail;
        const auto registered_in_tradepost = can_mutate_tradepost_graph &&
            add_object_to_array(spawned_traders, trader, add_detail);
        const auto after_count = is_reasonable_array(spawned_traders) ? spawned_traders->count : -1;
        const auto after_max = is_reasonable_array(spawned_traders) ? spawned_traders->max : -1;

        auto* spawned_sedentary = const_cast<TArrayAbi*>(array_property(donor_trade_post, L"_spawnedSedentaryNPCs"));
        const auto sedentary_before_count = is_reasonable_array(spawned_sedentary) ? spawned_sedentary->count : -1;
        const auto sedentary_before_max = is_reasonable_array(spawned_sedentary) ? spawned_sedentary->max : -1;
        std::string sedentary_add_detail = can_mutate_tradepost_graph ? "not-run" : mutation_skip_detail;
        const auto registered_in_sedentary = can_mutate_tradepost_graph &&
            add_object_to_array(spawned_sedentary, trader, sedentary_add_detail);
        const auto sedentary_after_count = is_reasonable_array(spawned_sedentary) ? spawned_sedentary->count : -1;
        const auto sedentary_after_max = is_reasonable_array(spawned_sedentary) ? spawned_sedentary->max : -1;

        const auto economy_prepare = prepare_linked_armory_economy(
            command_text,
            can_mutate_tradepost_graph && donor_manager != nullptr,
            selected_personality);
        append_log("link_existing_summoned_trader economyPrepare ok=" + std::string(economy_prepare.ok ? "true" : "false") +
            " allowed=" + std::string(economy_prepare.allowed ? "true" : "false") +
            " detail=" + economy_prepare.detail);

        const auto managerless_marker_link_ok =
            managerless_mutation_allowed &&
            donor_trade_post != nullptr &&
            selected_personality != nullptr &&
            registered_in_tradepost &&
            registered_in_sedentary;
        const auto lifecycle_link_ok = donor_manager != nullptr || managerless_marker_link_ok;
        const auto ok =
            trader != nullptr &&
            donor_trade_post != nullptr &&
            location_ok &&
            lifecycle_link_ok &&
            tradepost_set &&
            personality_set &&
            original_location_set &&
            registered_in_tradepost &&
            registered_in_sedentary &&
            (!economy_prepare.requested || economy_prepare.ok);

        std::ostringstream ss;
        ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
           << "\"ok\":" << (ok ? "true" : "false") << ","
           << "\"stage\":\"link-existing-summoned-trader\","
           << "\"route\":\"armory-summon-existing-link\","
           << "\"version\":\"" << mod_version << "\","
           << "\"note\":\"CheatManager.Summon creates the NPC near the player; native code only links the existing actor to stock Armory TradePost/personality and does not move it.\","
           << "\"requestedTraderFullName\":\"" << json_escape(trader_full_name) << "\","
           << "\"trader\":\"" << json_escape(full_name(trader)) << "\","
           << "\"traderResolved\":" << (trader != nullptr ? "true" : "false") << ","
           << "\"actorLocationOk\":" << (actor_location_ok ? "true" : "false") << ","
           << "\"actualActorLocation\":";
        append_vector_json(ss, actual_location);
        ss << ",\"playerLocation\":";
        append_vector_json(ss, make_vector(x, y, z));
        ss << ",\"distanceToPlayer\":" << distance_to_player << ","
           << "\"maxLinkDistance\":" << max_link_distance << ","
           << "\"nearPlayer\":" << (near_player ? "true" : "false") << ","
           << "\"notOrigin\":" << (not_origin ? "true" : "false") << ","
           << "\"locationOk\":" << (location_ok ? "true" : "false") << ","
           << "\"donorTradePost\":\"" << json_escape(full_name(donor_trade_post)) << "\","
           << "\"donorOutpostKey\":\"" << json_escape(donor_outpost_key) << "\","
           << "\"stockOutpostKey\":\"" << json_escape(stock_outpost_key) << "\","
           << "\"donorDistance2d\":" << donor_distance << ","
           << "\"donorManager\":\"" << json_escape(full_name(donor_manager)) << "\","
           << "\"donorManagerCandidate\":\"" << json_escape(full_name(donor_manager_candidate)) << "\","
           << "\"donorManagerRoute\":\"" << json_escape(donor_manager_route) << "\","
           << "\"managerLinked\":" << (donor_manager != nullptr ? "true" : "false") << ","
           << "\"fastManagerLookupOnly\":" << (fast_manager_lookup_only ? "true" : "false") << ","
           << "\"managerLinkedProof\":\"_assignedTradePosts\","
           << "\"candidateManagerIsOnlyDiagnostic\":" << (donor_manager_candidate != nullptr && donor_manager == nullptr ? "true" : "false") << ","
           << "\"managerlessMarkerLinkOk\":" << (managerless_marker_link_ok ? "true" : "false") << ","
           << "\"requireManagerLinked\":" << (require_manager_linked ? "true" : "false") << ","
           << "\"allowManagerlessMarkerFallback\":" << (requested_allow_managerless_marker_fallback ? "true" : "false") << ","
           << "\"allowManualSpawnedTraderArrayMutation\":" << (allow_manual_spawned_trader_array_mutation ? "true" : "false") << ","
           << "\"lifecycleLinkOk\":" << (lifecycle_link_ok ? "true" : "false") << ","
           << "\"canMutateTradePostGraph\":" << (can_mutate_tradepost_graph ? "true" : "false") << ","
           << "\"markerCount\":" << marker_count << ","
           << "\"markerPersonality\":\"" << json_escape(marker_personality_name) << "\","
           << "\"markerPersonalityGuid\":\"" << json_escape(marker_personality_guid) << "\","
           << "\"requestedPersonalityPath\":\"" << json_escape(requested_personality_path) << "\","
           << "\"requestedPersonalityResolved\":\"" << json_escape(full_name(requested_personality)) << "\","
           << "\"requestedPersonalitySelectedOutpostKey\":\"" << json_escape(requested_selected_outpost_key) << "\","
           << "\"requestedPersonalitySelectedKind\":\"" << json_escape(requested_selected_kind) << "\","
           << "\"requestedPersonalitySelectedPath\":\"" << json_escape(requested_selected_path) << "\","
           << "\"selectedPersonality\":\"" << json_escape(full_name(selected_personality)) << "\","
           << "\"selectedPersonalityGuid\":\"" << json_escape(guid_to_string(guid_property(selected_personality, L"TraderPersistentId"))) << "\","
           << "\"tradeOutpostBuildingSet\":" << (tradepost_set ? "true" : "false") << ","
           << "\"tradeOutpostBuildingDetail\":\"" << json_escape(tradepost_detail) << "\","
           << "\"traderPersonalityWeakSet\":" << (personality_set ? "true" : "false") << ","
           << "\"traderPersonalityWeakDetail\":\"" << json_escape(personality_detail) << "\","
           << "\"traderPersonalityAfter\":\"" << json_escape(full_name(personality_after)) << "\","
           << "\"traderPersonalityAfterGuid\":\"" << json_escape(personality_after_guid) << "\","
           << "\"originalLocationSet\":" << (original_location_set ? "true" : "false") << ","
           << "\"originalLocationDetail\":\"" << json_escape(original_location_detail) << "\","
           << "\"netCullSet\":" << (net_cull_set ? "true" : "false") << ","
           << "\"netCullDetail\":\"" << json_escape(net_cull_detail) << "\","
           << "\"interactionDistanceSet\":" << (interaction_distance_set ? "true" : "false") << ","
           << "\"interactionDistanceDetail\":\"" << json_escape(interaction_distance_detail) << "\","
           << "\"economyPrepare\":";
        append_linked_armory_economy_prepare_json(ss, economy_prepare);
        ss << ",\"spawnedTradersBefore\":{\"count\":" << before_count << ",\"max\":" << before_max << "},"
           << "\"spawnedTradersAfter\":{\"count\":" << after_count << ",\"max\":" << after_max << "},"
           << "\"spawnedSedentaryBefore\":{\"count\":" << sedentary_before_count << ",\"max\":" << sedentary_before_max << "},"
           << "\"spawnedSedentaryAfter\":{\"count\":" << sedentary_after_count << ",\"max\":" << sedentary_after_max << "},"
           << "\"registrationDetail\":\"" << json_escape(add_detail) << "\","
           << "\"sedentaryRegistrationDetail\":\"" << json_escape(sedentary_add_detail) << "\","
           << "\"tokens\":";
        append_json_string_array(ss, tokens);
        ss << ",\"managerCount\":" << managers.size() << ","
           << "\"managerSearch\":";
        append_json_string_array(ss, manager_route_counts);
        ss << ",\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << "}";

        return {ok, ss.str()};
    }

    auto SCUMTraderManager::armory_deep_probe(const std::string& command_text) const -> std::string
    {
        const auto started = std::chrono::steady_clock::now();
        const auto requested_max_objects = static_cast<std::int32_t>(regex_number(command_text, "maxObjects", 750000.0));
        const auto requested_max_samples = static_cast<std::int32_t>(regex_number(command_text, "maxSamples", 48.0));
        const auto max_objects = std::clamp(requested_max_objects, 1000, 2000000);
        const auto max_samples = std::clamp(requested_max_samples, 4, 120);

        std::vector<ObjectScanBucket> buckets{
            {"tradeOutpostManager", {"tradeoutpostmanager", "bp_tradeoutpostmanager"}},
            {"armoryTradePost", {"outpost_armory", "armory_npcinteractionbox", "bp_outpost_armory"}},
            {"tradePost", {"tradepost ", "atradepost", "npcinteractionbox", "interactionbox"}},
            {"traderActor", {"bp_armsdealer", "arms_dealer", "armsdealer", "atrader ", "bp_master_trader", "sedentarynpc"}},
            {"economy", {"economymanager", "trademanagingcomponent", "tradermanagingcomponent", "tradeabledesctable"}},
            {"serverDataRpc", {"server_requestserverdata", "client_receiveserverdata", "client_updatetraderdata", "playerpcchannelserverdata"}},
            {"tradeRpc", {"interactwithobjectonserver", "server_playerpurchasetradeable", "server_playerselltradeables", "client_sendprebuyresponse", "client_sendpostbuyresponse", "tradebuy"}},
            {"economyRequests", {"economymanagerrequest", "economymanagerresponse", "tradeablesresponsedata", "traderforplayeranddepotitem", "playerpcchannelserverrequest", "playerpcchannelserverresponse"}},
            {"serializerStructs", {"netserializer", "serverdatarequestnetserializer", "serverdataresponsenetserializer", "traderuntimeid", "tradeoutpostref"}},
            {"depot", {"depot", "assigneddepot", "depotitem"}},
            {"armoryPersonality", {"armory_personality", "traderpersonalitydataasset", "traderpersistentid"}},
            {"outpostDescription", {"tradeoutpostdescription", "outpostdescription", "tradeoutpostpersistentid"}},
            {"buySellData", {"priceupdatedata", "tradeableclassandquantity", "tradeable", "specialdeals", "prebuy", "postbuy"}}
        };

        const auto gobjects = find_guobject_array();
        TUObjectArrayAbi object_array{};
        std::uintptr_t used_offset{};
        std::string error;
        if (gobjects == 0)
        {
            error = "GUObjectArray unresolved";
        }
        else if (!read_object_array_from_gobjects(gobjects, object_array, used_offset))
        {
            error = "GUObjectArray view unreadable";
        }

        std::unordered_map<std::uintptr_t, std::string> class_name_cache;
        std::int32_t inspected = 0;
        std::int32_t readable_items = 0;
        std::int32_t relevant_class_count = 0;
        std::int32_t full_names_read = 0;
        std::int32_t class_read_errors = 0;
        std::int32_t full_name_errors = 0;
        std::vector<std::string> relevant_class_samples;

        if (error.empty())
        {
            const auto limit = std::min(object_array.num_elements, max_objects);
            for (std::int32_t index = 0; index < limit; ++index)
            {
                ++inspected;
                FUObjectItemAbi item{};
                if (!read_uobject_item_from_array(object_array, index, item) || item.object == 0) continue;
                ++readable_items;

                auto* object = reinterpret_cast<RC::Unreal::UObject*>(item.object);
                auto* object_class = safe_class_private(object);
                if (object_class == nullptr)
                {
                    ++class_read_errors;
                    continue;
                }

                const auto class_address = reinterpret_cast<std::uintptr_t>(object_class);
                auto class_it = class_name_cache.find(class_address);
                if (class_it == class_name_cache.end())
                {
                    class_it = class_name_cache.emplace(class_address, safe_full_name(object_class)).first;
                }
                const auto& class_name = class_it->second;
                if (class_name.empty() || class_name == "<GetFullName exception>")
                {
                    ++class_read_errors;
                    continue;
                }

                const auto class_name_lower = lower_copy(class_name);
                if (!looks_like_scan_relevant_class(class_name_lower)) continue;
                ++relevant_class_count;
                if (relevant_class_samples.size() < static_cast<std::size_t>(max_samples))
                {
                    relevant_class_samples.push_back(class_name);
                }

                const auto object_name = safe_full_name(object);
                if (object_name.empty() || object_name == "<GetFullName exception>")
                {
                    ++full_name_errors;
                    continue;
                }
                ++full_names_read;

                const auto combined_lower = lower_copy(object_name + " " + class_name);
                for (auto& bucket : buckets)
                {
                    if (!bucket_matches(bucket, combined_lower)) continue;
                    ++bucket.count;
                    if (bucket.samples.size() < static_cast<std::size_t>(max_samples))
                    {
                        bucket.samples.push_back(
                            "idx=" + std::to_string(index) +
                            ",flags=" + std::to_string(item.flags) +
                            ",class=" + class_name +
                            ",object=" + object_name);
                    }
                }
            }
        }

        std::ostringstream ss;
        ss << "{\"probe\":\"armory-deep-gobjects\","
           << "\"version\":\"" << mod_version << "\","
           << "\"warning\":\"read-only GUObjectArray name/class scan; no actor spawn, no TradeBuy dispatch, no UObject mutation\","
           << "\"gobjects\":\"" << json_escape(hex_address(gobjects)) << "\","
           << "\"objectArrayOffset\":\"" << json_escape(hex_address(used_offset)) << "\","
           << "\"error\":\"" << json_escape(error) << "\","
           << "\"limits\":{\"maxObjects\":" << max_objects << ",\"maxSamples\":" << max_samples << "},"
           << "\"stats\":{\"numElements\":" << object_array.num_elements
           << ",\"numChunks\":" << object_array.num_chunks
           << ",\"inspected\":" << inspected
           << ",\"readableItems\":" << readable_items
           << ",\"uniqueClasses\":" << class_name_cache.size()
           << ",\"relevantClassCount\":" << relevant_class_count
           << ",\"fullNamesRead\":" << full_names_read
           << ",\"classReadErrors\":" << class_read_errors
           << ",\"fullNameErrors\":" << full_name_errors
           << ",\"durationMs\":" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()
           << "},\"relevantClassSamples\":";
        append_json_string_array(ss, relevant_class_samples);
        ss << ",\"buckets\":{";
        for (size_t index = 0; index < buckets.size(); ++index)
        {
            if (index > 0) ss << ",";
            ss << "\"" << json_escape(buckets[index].key) << "\":";
            append_scan_bucket_json(ss, buckets[index]);
        }
        ss << "}}";
        return ss.str();
    }

    auto SCUMTraderManager::target_armory_interact_native(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        const auto controller_full_name = regex_value(command_text, "controllerFullName");
        const auto target_full_name = regex_value(command_text, "targetFullName");
        const auto target_kind = regex_value(command_text, "targetKind");
        const auto target_class = regex_value(command_text, "targetClass");
        const auto x = static_cast<float>(regex_number(command_text, "targetX", regex_number(command_text, "x", 0.0)));
        const auto y = static_cast<float>(regex_number(command_text, "targetY", regex_number(command_text, "y", 0.0)));
        const auto z = static_cast<float>(regex_number(command_text, "targetZ", regex_number(command_text, "z", 0.0)));

        std::vector<std::string> attempts;
        auto* controller = find_object_by_full_name(
            {L"ConZPlayerController", L"PlayerController", L"Controller", L"AController", L"Object", L"UObject"},
            controller_full_name,
            attempts,
            "controller");
        auto* target = find_object_by_full_name(
            {L"BP_ArmsDealer_01_C", L"BP_Outpost_Armory_NPCInteractionBox_C", L"Trader", L"ATrader", L"TradePost", L"ATradePost", L"SedentaryNPC", L"Object", L"UObject"},
            target_full_name,
            attempts,
            "target");

        const auto target_text = lower_copy(target_class + " " + target_full_name + " " + full_name(target));
        const auto target_looks_armory =
            target_text.find("armsdealer") != std::string::npos ||
            target_text.find("arms_dealer") != std::string::npos ||
            target_text.find("armory") != std::string::npos ||
            target_text.find("bp_armsdealer_01") != std::string::npos ||
            target_text.find("bp_outpost_armory_npcinteractionbox") != std::string::npos;
        if (controller == nullptr || target == nullptr || !target_looks_armory)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"stage\":\"resolve-controller-target\","
               << "\"route\":\"native-target-interactable-interface-interact\","
               << "\"controllerResolved\":" << (controller != nullptr ? "true" : "false") << ","
               << "\"targetResolved\":" << (target != nullptr ? "true" : "false") << ","
               << "\"targetLooksArmory\":" << (target_looks_armory ? "true" : "false") << ","
               << "\"controllerFullName\":\"" << json_escape(controller_full_name) << "\","
               << "\"targetFullName\":\"" << json_escape(target_full_name) << "\","
               << "\"targetClass\":\"" << json_escape(target_class) << "\","
               << "\"targetKind\":\"" << json_escape(target_kind) << "\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        auto* interact_fn = resolve_function_by_name(
            "InteractableInterface",
            "Interact",
            {
                "/Script/SCUM.InteractableInterface:Interact",
                "Function /Script/SCUM.InteractableInterface:Interact",
                "/Script/SCUM.InteractableInterface.Interact",
                "Function /Script/SCUM.InteractableInterface.Interact",
            },
            attempts);

        std::string process_event_detail;
        auto* process_event = process_event_from_vtable(target, process_event_detail);
        attempts.push_back("ProcessEvent=" + process_event_detail);

        if (interact_fn == nullptr || process_event == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"stage\":\"resolve-process-event-or-function\","
               << "\"route\":\"native-target-interactable-interface-interact\","
               << "\"controller\":\"" << json_escape(full_name(controller)) << "\","
               << "\"target\":\"" << json_escape(full_name(target)) << "\","
               << "\"interactFunction\":\"" << json_escape(full_name(interact_fn)) << "\","
               << "\"processEvent\":\"" << json_escape(process_event_detail) << "\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        RC::Unreal::FVector interaction_location{x, y, z};
        bool target_location_resolved = false;
        std::string target_location_detail = "payload";
        if (actor_location_noexcept(target, interaction_location))
        {
            target_location_resolved = true;
            target_location_detail = "K2_GetActorLocation";
        }
        else
        {
            target_location_detail = "payload-after-K2-exception";
        }

        InteractableInterfaceInteractParams params{};
        params.user_controller = controller;
        params.interaction_type = interaction_type_trade_buy;
        params.interaction_data.pointer_data = target;
        params.interaction_data.bool_data = true;
        params.interaction_data.interaction_location = interaction_location;
        params.interaction_data.interaction_normal = RC::Unreal::FVector{0.0f, 0.0f, 1.0f};
        params.interaction_data.vector_data = interaction_location;
        params.interaction_data.base_element_id = 0;

        unsigned long exception_code = 0;
        const auto invoked = invoke_process_event_noexcept(process_event, target, interact_fn, &params, exception_code);
        std::string invoke_error;
        if (!invoked)
        {
            std::ostringstream error;
            error << "seh-exception-code=" << hex_address(static_cast<std::uintptr_t>(exception_code));
            invoke_error = error.str();
        }

        std::ostringstream ss;
        ss << "{\"ok\":" << (invoked ? "true" : "false") << ","
           << "\"stage\":\"native-target-interact-dispatch\","
           << "\"route\":\"native-target-interactable-interface-interact\","
           << "\"interactionType\":" << static_cast<int>(interaction_type_trade_buy) << ","
           << "\"controller\":\"" << json_escape(full_name(controller)) << "\","
           << "\"target\":\"" << json_escape(full_name(target)) << "\","
           << "\"targetKind\":\"" << json_escape(target_kind) << "\","
           << "\"targetClass\":\"" << json_escape(target_class) << "\","
           << "\"interactFunction\":\"" << json_escape(full_name(interact_fn)) << "\","
           << "\"processEvent\":\"" << json_escape(process_event_detail) << "\","
           << "\"targetLocationResolved\":" << (target_location_resolved ? "true" : "false") << ","
           << "\"targetLocationDetail\":\"" << json_escape(target_location_detail) << "\","
           << "\"targetLocation\":";
        append_vector_json(ss, interaction_location);
        ss << ",\"abi\":{\"FText\":24,\"FInteractionData\":" << sizeof(FInteractionDataAbi)
           << ",\"TargetInteractParams\":" << sizeof(InteractableInterfaceInteractParams)
           << ",\"interactionDataOffset\":" << offsetof(InteractableInterfaceInteractParams, interaction_data) << "},"
           << "\"invokeError\":\"" << json_escape(invoke_error) << "\","
           << "\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << "}";
        return {invoked, ss.str()};
    }

    auto SCUMTraderManager::open_armory_tradebuy_native(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        const auto pawn_full_name = regex_value(command_text, "pawnFullName");
        const auto controller_full_name = regex_value(command_text, "controllerFullName");
        const auto target_full_name = regex_value(command_text, "targetFullName");
        const auto target_kind = regex_value(command_text, "targetKind");
        const auto target_class = regex_value(command_text, "targetClass");
        const auto x = static_cast<float>(regex_number(command_text, "targetX", regex_number(command_text, "x", 0.0)));
        const auto y = static_cast<float>(regex_number(command_text, "targetY", regex_number(command_text, "y", 0.0)));
        const auto z = static_cast<float>(regex_number(command_text, "targetZ", regex_number(command_text, "z", 0.0)));

        std::vector<std::string> attempts;
        auto* pawn = find_object_by_full_name(
            {L"BP_Prisoner_C", L"Prisoner", L"APrisoner", L"Object"},
            pawn_full_name,
            attempts,
            "pawn");
        auto* target = find_object_by_full_name(
            {L"BP_ArmsDealer_01_C", L"BP_Outpost_Armory_NPCInteractionBox_C", L"Trader", L"ATrader", L"TradePost", L"ATradePost", L"SedentaryNPC", L"Object", L"UObject"},
            target_full_name,
            attempts,
            "target");

        const auto target_text = lower_copy(target_class + " " + target_full_name + " " + full_name(target));
        const auto target_looks_armory =
            target_text.find("armsdealer") != std::string::npos ||
            target_text.find("arms_dealer") != std::string::npos ||
            target_text.find("armory") != std::string::npos ||
            target_text.find("bp_armsdealer_01") != std::string::npos ||
            target_text.find("bp_outpost_armory_npcinteractionbox") != std::string::npos;
        if (pawn == nullptr || target == nullptr || !target_looks_armory)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"stage\":\"resolve-pawn-target\","
               << "\"route\":\"native-prisoner-client-interact-tradebuy\","
               << "\"pawnResolved\":" << (pawn != nullptr ? "true" : "false") << ","
               << "\"targetResolved\":" << (target != nullptr ? "true" : "false") << ","
               << "\"targetLooksArmory\":" << (target_looks_armory ? "true" : "false") << ","
               << "\"pawnFullName\":\"" << json_escape(pawn_full_name) << "\","
               << "\"controllerFullName\":\"" << json_escape(controller_full_name) << "\","
               << "\"targetFullName\":\"" << json_escape(target_full_name) << "\","
               << "\"targetClass\":\"" << json_escape(target_class) << "\","
               << "\"targetKind\":\"" << json_escape(target_kind) << "\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        auto* client_interact_fn = resolve_function_by_name(
            "Prisoner",
            "Client_Interact",
            {
                "/Script/SCUM.Prisoner:Client_Interact",
                "Function /Script/SCUM.Prisoner:Client_Interact",
                "/Script/SCUM.Prisoner.Client_Interact",
                "Function /Script/SCUM.Prisoner.Client_Interact",
            },
            attempts);

        std::string process_event_detail;
        auto* process_event = process_event_from_ue4ss_log(base_dir(), process_event_detail);
        if (process_event == nullptr)
        {
            std::string vtable_detail;
            process_event = process_event_from_vtable(pawn, vtable_detail);
            process_event_detail += "; fallback-vtable: " + vtable_detail;
        }
        attempts.push_back("ProcessEvent=" + process_event_detail);

        if (client_interact_fn == nullptr || process_event == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"stage\":\"resolve-process-event-or-function\","
               << "\"route\":\"native-prisoner-client-interact-tradebuy\","
               << "\"pawn\":\"" << json_escape(full_name(pawn)) << "\","
               << "\"target\":\"" << json_escape(full_name(target)) << "\","
               << "\"clientInteractFunction\":\"" << json_escape(full_name(client_interact_fn)) << "\","
               << "\"processEvent\":\"" << json_escape(process_event_detail) << "\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        RC::Unreal::FVector interaction_location{x, y, z};
        bool target_location_resolved = false;
        std::string target_location_detail = "payload";
        if (actor_location_noexcept(target, interaction_location))
        {
            target_location_resolved = true;
            target_location_detail = "K2_GetActorLocation";
        }
        else
        {
            target_location_detail = "payload-after-K2-exception";
        }

        PrisonerClientInteractParams params{};
        params.interactable = target;
        params.interaction_type = interaction_type_trade_buy;
        params.interaction_data.pointer_data = target;
        params.interaction_data.bool_data = true;
        params.interaction_data.interaction_location = interaction_location;
        params.interaction_data.interaction_normal = RC::Unreal::FVector{0.0f, 0.0f, 1.0f};
        params.interaction_data.vector_data = interaction_location;
        params.interaction_data.base_element_id = 0;

        bool invoked = false;
        std::string invoke_error;
        unsigned long exception_code = 0;
        invoked = invoke_process_event_noexcept(process_event, pawn, client_interact_fn, &params, exception_code);
        if (!invoked)
        {
            std::ostringstream error;
            error << "seh-exception-code=" << hex_address(static_cast<std::uintptr_t>(exception_code));
            invoke_error = error.str();
        }

        std::ostringstream ss;
        ss << "{\"ok\":" << (invoked ? "true" : "false") << ","
           << "\"stage\":\"native-client-interact-dispatch\","
           << "\"route\":\"native-prisoner-client-interact-tradebuy\","
           << "\"interactionType\":" << static_cast<int>(interaction_type_trade_buy) << ","
           << "\"pawn\":\"" << json_escape(full_name(pawn)) << "\","
           << "\"target\":\"" << json_escape(full_name(target)) << "\","
           << "\"targetKind\":\"" << json_escape(target_kind) << "\","
           << "\"targetClass\":\"" << json_escape(target_class) << "\","
           << "\"clientInteractFunction\":\"" << json_escape(full_name(client_interact_fn)) << "\","
           << "\"processEvent\":\"" << json_escape(process_event_detail) << "\","
           << "\"targetLocationResolved\":" << (target_location_resolved ? "true" : "false") << ","
           << "\"targetLocationDetail\":\"" << json_escape(target_location_detail) << "\","
           << "\"targetLocation\":";
        append_vector_json(ss, interaction_location);
        ss << ",\"abi\":{\"FText\":24,\"FInteractionData\":" << sizeof(FInteractionDataAbi)
           << ",\"ClientInteractParams\":" << sizeof(PrisonerClientInteractParams)
           << ",\"interactionDataOffset\":" << offsetof(PrisonerClientInteractParams, interaction_data) << "},"
           << "\"invokeError\":\"" << json_escape(invoke_error) << "\","
           << "\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << "}";
        return {invoked, ss.str()};
    }

    auto SCUMTraderManager::server_armory_tradebuy_native(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        const auto rpc_full_name = regex_value(command_text, "rpcFullName");
        const auto controller_full_name = regex_value(command_text, "controllerFullName");
        const auto target_full_name = regex_value(command_text, "targetFullName");
        const auto target_kind = regex_value(command_text, "targetKind");
        const auto target_class = regex_value(command_text, "targetClass");
        const auto x = static_cast<float>(regex_number(command_text, "targetX", regex_number(command_text, "x", 0.0)));
        const auto y = static_cast<float>(regex_number(command_text, "targetY", regex_number(command_text, "y", 0.0)));
        const auto z = static_cast<float>(regex_number(command_text, "targetZ", regex_number(command_text, "z", 0.0)));

        std::vector<std::string> attempts;
        auto* rpc = find_object_by_full_name(
            {L"PlayerRpcChannel", L"UPlayerRpcChannel", L"Object", L"UObject"},
            rpc_full_name,
            attempts,
            "rpc");
        auto* controller = find_object_by_full_name(
            {L"ConZPlayerController", L"PlayerController", L"Controller", L"AController", L"Object", L"UObject"},
            controller_full_name,
            attempts,
            "controller");
        auto* target = find_object_by_full_name(
            {L"BP_ArmsDealer_01_C", L"BP_Outpost_Armory_NPCInteractionBox_C", L"Trader", L"ATrader", L"TradePost", L"ATradePost", L"SedentaryNPC", L"Object", L"UObject"},
            target_full_name,
            attempts,
            "target");

        const auto target_text = lower_copy(target_class + " " + target_full_name + " " + full_name(target));
        const auto target_looks_armory =
            target_text.find("armsdealer") != std::string::npos ||
            target_text.find("arms_dealer") != std::string::npos ||
            target_text.find("armory") != std::string::npos ||
            target_text.find("bp_armsdealer_01") != std::string::npos ||
            target_text.find("bp_outpost_armory_npcinteractionbox") != std::string::npos;
        if (rpc == nullptr || controller == nullptr || target == nullptr || !target_looks_armory)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"stage\":\"resolve-rpc-controller-target\","
               << "\"route\":\"native-rpc-server-interact-tradebuy\","
               << "\"rpcResolved\":" << (rpc != nullptr ? "true" : "false") << ","
               << "\"controllerResolved\":" << (controller != nullptr ? "true" : "false") << ","
               << "\"targetResolved\":" << (target != nullptr ? "true" : "false") << ","
               << "\"targetLooksArmory\":" << (target_looks_armory ? "true" : "false") << ","
               << "\"rpcFullName\":\"" << json_escape(rpc_full_name) << "\","
               << "\"controllerFullName\":\"" << json_escape(controller_full_name) << "\","
               << "\"targetFullName\":\"" << json_escape(target_full_name) << "\","
               << "\"targetClass\":\"" << json_escape(target_class) << "\","
               << "\"targetKind\":\"" << json_escape(target_kind) << "\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        auto* server_interact_fn = resolve_function_by_name(
            "PlayerRpcChannel",
            "InteractWithObjectOnServer",
            {
                "/Script/SCUM.PlayerRpcChannel:InteractWithObjectOnServer",
                "Function /Script/SCUM.PlayerRpcChannel:InteractWithObjectOnServer",
                "/Script/SCUM.PlayerRpcChannel.InteractWithObjectOnServer",
                "Function /Script/SCUM.PlayerRpcChannel.InteractWithObjectOnServer",
            },
            attempts);

        std::string process_event_detail;
        auto* process_event = process_event_from_ue4ss_log(base_dir(), process_event_detail);
        if (process_event == nullptr)
        {
            std::string vtable_detail;
            process_event = process_event_from_vtable(rpc, vtable_detail);
            process_event_detail += "; fallback-vtable: " + vtable_detail;
        }
        attempts.push_back("ProcessEvent=" + process_event_detail);

        if (server_interact_fn == nullptr || process_event == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"stage\":\"resolve-process-event-or-function\","
               << "\"route\":\"native-rpc-server-interact-tradebuy\","
               << "\"rpc\":\"" << json_escape(full_name(rpc)) << "\","
               << "\"controller\":\"" << json_escape(full_name(controller)) << "\","
               << "\"target\":\"" << json_escape(full_name(target)) << "\","
               << "\"serverInteractFunction\":\"" << json_escape(full_name(server_interact_fn)) << "\","
               << "\"processEvent\":\"" << json_escape(process_event_detail) << "\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        RC::Unreal::FVector interaction_location{x, y, z};
        bool target_location_resolved = false;
        std::string target_location_detail = "payload";
        if (actor_location_noexcept(target, interaction_location))
        {
            target_location_resolved = true;
            target_location_detail = "K2_GetActorLocation";
        }
        else
        {
            target_location_detail = "payload-after-K2-exception";
        }

        PlayerRpcInteractWithObjectOnServerParams params{};
        params.interactable = target;
        params.user_controller = controller;
        params.interaction_type = interaction_type_trade_buy;
        params.interaction_data.pointer_data = target;
        params.interaction_data.bool_data = true;
        params.interaction_data.interaction_location = interaction_location;
        params.interaction_data.interaction_normal = RC::Unreal::FVector{0.0f, 0.0f, 1.0f};
        params.interaction_data.vector_data = interaction_location;
        params.interaction_data.base_element_id = 0;

        unsigned long exception_code = 0;
        const auto invoked = invoke_process_event_noexcept(process_event, rpc, server_interact_fn, &params, exception_code);
        std::string invoke_error;
        if (!invoked)
        {
            std::ostringstream error;
            error << "seh-exception-code=" << hex_address(static_cast<std::uintptr_t>(exception_code));
            invoke_error = error.str();
        }

        std::ostringstream ss;
        ss << "{\"ok\":" << (invoked ? "true" : "false") << ","
           << "\"stage\":\"native-rpc-server-interact-dispatch\","
           << "\"route\":\"native-rpc-server-interact-tradebuy\","
           << "\"interactionType\":" << static_cast<int>(interaction_type_trade_buy) << ","
           << "\"rpc\":\"" << json_escape(full_name(rpc)) << "\","
           << "\"controller\":\"" << json_escape(full_name(controller)) << "\","
           << "\"target\":\"" << json_escape(full_name(target)) << "\","
           << "\"targetKind\":\"" << json_escape(target_kind) << "\","
           << "\"targetClass\":\"" << json_escape(target_class) << "\","
           << "\"serverInteractFunction\":\"" << json_escape(full_name(server_interact_fn)) << "\","
           << "\"processEvent\":\"" << json_escape(process_event_detail) << "\","
           << "\"targetLocationResolved\":" << (target_location_resolved ? "true" : "false") << ","
           << "\"targetLocationDetail\":\"" << json_escape(target_location_detail) << "\","
           << "\"targetLocation\":";
        append_vector_json(ss, interaction_location);
        ss << ",\"abi\":{\"FText\":24,\"FInteractionData\":" << sizeof(FInteractionDataAbi)
           << ",\"ServerInteractParams\":" << sizeof(PlayerRpcInteractWithObjectOnServerParams)
           << ",\"interactionDataOffset\":" << offsetof(PlayerRpcInteractWithObjectOnServerParams, interaction_data) << "},"
           << "\"invokeError\":\"" << json_escape(invoke_error) << "\","
           << "\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << "}";
        return {invoked, ss.str()};
    }

    auto SCUMTraderManager::base_loot_first_reflected_property_probe(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        const auto started_at = utc_now();
        const auto chest_full_name = first_regex_value(command_text, {"chestFullName", "targetFullName", "objectPath"});
        const auto chest_class = first_regex_value(command_text, {"chestClass", "targetClass"});
        const auto chest_address = first_regex_value(command_text, {"chestAddress", "targetAddress", "objectAddress"});
        const auto component_full_name = first_regex_value(command_text, {"componentFullName", "nameableComponentFullName", "componentPath"});
        const auto component_class = first_regex_value(command_text, {"componentClass", "nameableComponentClass"});
        const auto component_address = first_regex_value(command_text, {"componentAddress", "nameableComponentAddress"});

        std::vector<std::string> attempts;
        auto* chest = find_object_by_full_name(
            {L"ChestItem", L"AChestItem", L"Item", L"AItem", L"Actor", L"Object", L"UObject"},
            chest_full_name,
            attempts,
            "chest",
            false);
        if (chest == nullptr)
        {
            chest = object_from_lua_address_guarded(chest_address, chest_full_name, attempts, "chest");
        }

        RC::Unreal::UObject* component = nullptr;
        bool component_missing = false;
        bool component_exception = false;
        if (!component_full_name.empty())
        {
            component = find_object_by_full_name(
                {L"NameableItemComponent", L"UNameableItemComponent", L"ItemComponent", L"UItemComponent", L"Object", L"UObject"},
                component_full_name,
                attempts,
                "component",
                false);
        }
        if (component == nullptr)
        {
            component = object_from_lua_address_guarded(component_address, component_full_name, attempts, "component");
        }
        if (component == nullptr && chest != nullptr)
        {
            component = object_property_noexcept_raw(chest, L"_nameableItemComponent", component_missing, component_exception);
            attempts.push_back(std::string("chest._nameableItemComponent=") +
                (component_exception ? "exception" : (component_missing ? "missing" : safe_full_name(component))));
        }

        const auto chest_resolved = chest != nullptr;
        const auto component_resolved = component != nullptr;
        const auto component_before = safe_full_name(component);
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

        const auto component_after = safe_full_name(component);
        const auto stale_guard_ok =
            component_resolved &&
            !component_before.empty() &&
            component_before == component_after &&
            component_looks_valid;

        const auto managed_state = base_dir().parent_path().parent_path() / L"Saved" / L"ScumNeDjin" / L"state";
        const auto bridge_status_path = managed_state / L"managed" / L"ue4ss-cppmod-bridge-status.json";
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
        const auto result_path = managed_state / L"managed" / L"first-reflected-property-probe-result.json";
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
              << ",\"objectPath\":\"" << json_escape(safe_full_name(chest)) << "\""
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

        std::ofstream file(result_path, std::ios::binary | std::ios::trunc);
        if (file) file << proof.str();

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

    auto SCUMTraderManager::base_loot_store_item_native(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        const auto rpc_full_name = first_regex_value(command_text, {"rpcFullName", "playerRpcFullName"});
        const auto controller_full_name = first_regex_value(command_text, {"controllerFullName", "playerControllerFullName"});
        const auto item_full_name = first_regex_value(command_text, {"itemFullName", "actorFullName", "sourceItemFullName"});
        const auto item_class = first_regex_value(command_text, {"itemClass", "actorClass", "sourceItemClass"});
        const auto chest_full_name = first_regex_value(command_text, {"chestFullName", "targetFullName", "otherItemFullName"});
        const auto chest_class = first_regex_value(command_text, {"chestClass", "targetClass", "otherItemClass"});
        const auto payload_verified_item = regex_bool(command_text, "verifiedItem", false);
        const auto payload_verified_chest = regex_bool(command_text, "verifiedChest", false);
        const auto x = regex_number(command_text, "x", regex_number(command_text, "targetX", 0.0));
        const auto y = regex_number(command_text, "y", regex_number(command_text, "targetY", 0.0));
        const auto z = regex_number(command_text, "z", regex_number(command_text, "targetZ", 0.0));
        const auto base_element_id = static_cast<std::int64_t>(
            regex_number(command_text, "baseElementId", regex_number(command_text, "baseId", 0.0)));

        std::vector<std::string> attempts;
        auto* rpc = find_object_by_full_name(
            {L"PlayerRpcChannel", L"UPlayerRpcChannel", L"Object", L"UObject"},
            rpc_full_name,
            attempts,
            "rpc",
            false);
        auto* controller = find_object_by_full_name(
            {L"ConZPlayerController", L"PlayerController", L"Controller", L"AController", L"Object", L"UObject"},
            controller_full_name,
            attempts,
            "controller",
            false);
        auto* item = find_object_by_full_name(
            {L"Item", L"AItem", L"Actor", L"Object", L"UObject"},
            item_full_name,
            attempts,
            "item",
            false);
        auto* chest = find_object_by_full_name(
            {L"ChestItem", L"AChestItem", L"Item", L"AItem", L"Actor", L"Object", L"UObject"},
            chest_full_name,
            attempts,
            "chest",
            false);

        const auto item_text = lower_copy(item_class + " " + item_full_name + " " + object_class_full_name_noexcept(item) + " " + safe_full_name(item));
        const auto chest_text = lower_copy(chest_class + " " + chest_full_name + " " + object_class_full_name_noexcept(chest) + " " + safe_full_name(chest));
        // The command file is only a transport; never let caller-supplied
        // verifiedItem/verifiedChest flags override native object validation.
        const auto chest_looks_valid =
            chest_text.find("chestitem") != std::string::npos ||
            chest_text.find("chest") != std::string::npos;
        const auto item_looks_blocked =
            item_text.find("chestitem") != std::string::npos ||
            item_text.find("chest") != std::string::npos ||
            item_text.find("vehicle") != std::string::npos ||
            item_text.find("baseflag") != std::string::npos ||
            item_text.find("conzbase") != std::string::npos;

        if (rpc == nullptr || controller == nullptr || item == nullptr || chest == nullptr || item == chest || !chest_looks_valid || item_looks_blocked)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"stage\":\"resolve-base-loot-store-objects\","
               << "\"route\":\"native-base-loot-store-item\","
               << "\"rpcResolved\":" << (rpc != nullptr ? "true" : "false") << ","
               << "\"controllerResolved\":" << (controller != nullptr ? "true" : "false") << ","
               << "\"itemResolved\":" << (item != nullptr ? "true" : "false") << ","
               << "\"chestResolved\":" << (chest != nullptr ? "true" : "false") << ","
               << "\"sameItemAndChest\":" << (item == chest && item != nullptr ? "true" : "false") << ","
               << "\"chestLooksValid\":" << (chest_looks_valid ? "true" : "false") << ","
               << "\"itemLooksBlocked\":" << (item_looks_blocked ? "true" : "false") << ","
                << "\"payloadVerifiedItemIgnored\":" << (payload_verified_item ? "true" : "false") << ","
                << "\"payloadVerifiedChestIgnored\":" << (payload_verified_chest ? "true" : "false") << ","
               << "\"rpcFullName\":\"" << json_escape(rpc_full_name) << "\","
               << "\"controllerFullName\":\"" << json_escape(controller_full_name) << "\","
               << "\"itemFullName\":\"" << json_escape(item_full_name) << "\","
               << "\"itemClass\":\"" << json_escape(item_class) << "\","
               << "\"chestFullName\":\"" << json_escape(chest_full_name) << "\","
               << "\"chestClass\":\"" << json_escape(chest_class) << "\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        auto* interact_fn = resolve_function_by_name(
            "PlayerRpcChannel",
            "InteractItemWithItemOnServer",
            {
                "/Script/SCUM.PlayerRpcChannel:InteractItemWithItemOnServer",
                "Function /Script/SCUM.PlayerRpcChannel:InteractItemWithItemOnServer",
                "/Script/SCUM.PlayerRpcChannel.InteractItemWithItemOnServer",
                "Function /Script/SCUM.PlayerRpcChannel.InteractItemWithItemOnServer",
            },
            attempts,
            false);

        std::string process_event_detail;
        auto* process_event = process_event_from_ue4ss_log(base_dir(), process_event_detail);
        attempts.push_back("ProcessEvent=" + process_event_detail);

        if (interact_fn == nullptr || process_event == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"stage\":\"resolve-process-event-or-function\","
               << "\"route\":\"native-base-loot-store-item\","
               << "\"rpc\":\"" << json_escape(safe_full_name(rpc)) << "\","
               << "\"controller\":\"" << json_escape(safe_full_name(controller)) << "\","
               << "\"item\":\"" << json_escape(safe_full_name(item)) << "\","
               << "\"chest\":\"" << json_escape(safe_full_name(chest)) << "\","
               << "\"interactFunction\":\"" << json_escape(safe_full_name(interact_fn)) << "\","
               << "\"processEvent\":\"" << json_escape(process_event_detail) << "\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        RC::Unreal::FVector interaction_location = make_vector(x, y, z);
        bool chest_location_resolved = false;
        bool item_location_resolved = false;
        std::string location_detail = "payload";
        if (actor_location_noexcept(chest, interaction_location))
        {
            chest_location_resolved = true;
            location_detail = "chest.K2_GetActorLocation";
        }
        else if (actor_location_noexcept(item, interaction_location))
        {
            item_location_resolved = true;
            location_detail = "item.K2_GetActorLocation";
        }
        else
        {
            location_detail = "payload-after-K2-exception";
        }

        PlayerRpcInteractItemWithItemOnServerParams params{};
        params.item = item;
        params.player_controller = controller;
        params.other_item = chest;
        params.interaction_type = interaction_type_store;
        params.interaction_data.pointer_data = chest;
        params.interaction_data.bool_data = true;
        params.interaction_data.interaction_location = interaction_location;
        params.interaction_data.interaction_normal = RC::Unreal::FVector{0.0f, 0.0f, 1.0f};
        params.interaction_data.vector_data = interaction_location;
        params.interaction_data.base_element_id = base_element_id;

        unsigned long exception_code = 0;
        const auto invoked = invoke_process_event_noexcept(process_event, rpc, interact_fn, &params, exception_code);
        std::string invoke_error;
        if (!invoked)
        {
            std::ostringstream error;
            error << "seh-exception-code=" << hex_address(static_cast<std::uintptr_t>(exception_code));
            invoke_error = error.str();
        }

        std::ostringstream ss;
        ss << "{\"ok\":" << (invoked ? "true" : "false") << ","
           << "\"stage\":\"native-base-loot-store-dispatch\","
           << "\"route\":\"native-base-loot-store-item\","
           << "\"interactionType\":" << static_cast<int>(interaction_type_store) << ","
           << "\"rpc\":\"" << json_escape(safe_full_name(rpc)) << "\","
           << "\"controller\":\"" << json_escape(safe_full_name(controller)) << "\","
           << "\"item\":\"" << json_escape(safe_full_name(item)) << "\","
           << "\"itemClass\":\"" << json_escape(object_class_full_name_noexcept(item)) << "\","
           << "\"chest\":\"" << json_escape(safe_full_name(chest)) << "\","
           << "\"chestClass\":\"" << json_escape(object_class_full_name_noexcept(chest)) << "\","
           << "\"interactFunction\":\"" << json_escape(safe_full_name(interact_fn)) << "\","
           << "\"processEvent\":\"" << json_escape(process_event_detail) << "\","
           << "\"chestLocationResolved\":" << (chest_location_resolved ? "true" : "false") << ","
           << "\"itemLocationResolved\":" << (item_location_resolved ? "true" : "false") << ","
           << "\"locationDetail\":\"" << json_escape(location_detail) << "\","
           << "\"location\":";
        append_vector_json(ss, interaction_location);
        ss << ",\"baseElementId\":" << base_element_id
           << ",\"abi\":{\"FText\":24,\"FInteractionData\":" << sizeof(FInteractionDataAbi)
           << ",\"InteractItemWithItemParams\":" << sizeof(PlayerRpcInteractItemWithItemOnServerParams)
           << ",\"interactionDataOffset\":" << offsetof(PlayerRpcInteractItemWithItemOnServerParams, interaction_data) << "},"
           << "\"invokeError\":\"" << json_escape(invoke_error) << "\","
           << "\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << "}";
        return {invoked, ss.str()};
    }

    auto SCUMTraderManager::placeable_server_place_native(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        const auto placeable_full_name = first_regex_value(command_text, {"placeableFullName", "targetFullName", "actorFullName", "objectPath"});
        const auto placeable_class = first_regex_value(command_text, {"placeableClass", "targetClass", "actorClass", "actorClassFullName"});
        const auto verified_placeable = regex_bool(command_text, "verifiedPlaceable", false);
        const auto x = regex_number(command_text, "x", regex_number(command_text, "targetX", 0.0));
        const auto y = regex_number(command_text, "y", regex_number(command_text, "targetY", 0.0));
        const auto z = regex_number(command_text, "z", regex_number(command_text, "targetZ", 0.0));
        const auto pitch = regex_number(command_text, "pitch", regex_number(command_text, "Pitch", 0.0));
        const auto yaw = regex_number(command_text, "yaw", regex_number(command_text, "Yaw", 0.0));
        const auto roll = regex_number(command_text, "roll", regex_number(command_text, "Roll", 0.0));

        std::vector<std::string> attempts;
        auto* placeable = find_object_by_full_name(
            {L"PlaceableActorBase", L"PlaceableActorBaseBuilding", L"PlaceableActorModularBaseBuilding", L"Actor", L"Object", L"UObject"},
            placeable_full_name,
            attempts,
            "placeable",
            false);

        const auto target_text = lower_copy(placeable_class + " " + placeable_full_name + " " + full_name(placeable));
        const auto target_looks_placeable =
            verified_placeable ||
            target_text.find("placeable") != std::string::npos ||
            target_text.find("basebuilding") != std::string::npos ||
            target_text.find("bp_base_") != std::string::npos ||
            target_text.find("base_") != std::string::npos;
        if (placeable == nullptr || !target_looks_placeable)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"stage\":\"resolve-placeable\","
               << "\"route\":\"native-placeable-server-place\","
               << "\"placeableResolved\":" << (placeable != nullptr ? "true" : "false") << ","
               << "\"targetLooksPlaceable\":" << (target_looks_placeable ? "true" : "false") << ","
               << "\"verifiedPlaceable\":" << (verified_placeable ? "true" : "false") << ","
               << "\"placeableFullName\":\"" << json_escape(placeable_full_name) << "\","
               << "\"placeableClass\":\"" << json_escape(placeable_class) << "\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        auto* server_place_fn = resolve_function_by_name(
            "PlaceableActorBase",
            "Server_Place",
            {
                "/Script/SCUM.PlaceableActorBase:Server_Place",
                "Function /Script/SCUM.PlaceableActorBase:Server_Place",
                "/Script/SCUM.PlaceableActorBase.Server_Place",
                "Function /Script/SCUM.PlaceableActorBase.Server_Place",
            },
            attempts);

        std::string process_event_detail;
        auto* process_event = process_event_from_ue4ss_log(base_dir(), process_event_detail);
        if (process_event == nullptr)
        {
            std::string vtable_detail;
            process_event = process_event_from_vtable(placeable, vtable_detail);
            process_event_detail += "; fallback-vtable: " + vtable_detail;
        }
        attempts.push_back("ProcessEvent=" + process_event_detail);

        RC::Unreal::FVector before_location{};
        RC::Unreal::FRotator before_rotation{};
        const auto before_location_ok = actor_location_noexcept(placeable, before_location);
        const auto before_rotation_ok = actor_rotation_noexcept(placeable, before_rotation);

        if (server_place_fn == nullptr || process_event == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"stage\":\"resolve-process-event-or-function\","
               << "\"route\":\"native-placeable-server-place\","
               << "\"placeable\":\"" << json_escape(full_name(placeable)) << "\","
               << "\"serverPlaceFunction\":\"" << json_escape(full_name(server_place_fn)) << "\","
               << "\"processEvent\":\"" << json_escape(process_event_detail) << "\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        PlaceableServerPlaceParams params{};
        params.location = make_vector(x, y, z);
        params.rotation.pitch = static_cast<float>(pitch);
        params.rotation.yaw = static_cast<float>(normalize_yaw(yaw));
        params.rotation.roll = static_cast<float>(roll);

        unsigned long exception_code = 0;
        const auto invoked = invoke_process_event_noexcept(process_event, placeable, server_place_fn, &params, exception_code);
        std::string invoke_error;
        if (!invoked)
        {
            std::ostringstream error;
            error << "seh-exception-code=" << hex_address(static_cast<std::uintptr_t>(exception_code));
            invoke_error = error.str();
        }

        RC::Unreal::FVector after_location{};
        RC::Unreal::FRotator after_rotation{};
        const auto after_location_ok = actor_location_noexcept(placeable, after_location);
        const auto after_rotation_ok = actor_rotation_noexcept(placeable, after_rotation);

        std::ostringstream ss;
        ss << "{\"ok\":" << (invoked ? "true" : "false") << ","
           << "\"stage\":\"native-placeable-server-place-dispatch\","
           << "\"route\":\"native-placeable-server-place\","
           << "\"placeable\":\"" << json_escape(full_name(placeable)) << "\","
           << "\"placeableClass\":\"" << json_escape(placeable_class) << "\","
           << "\"serverPlaceFunction\":\"" << json_escape(full_name(server_place_fn)) << "\","
           << "\"processEvent\":\"" << json_escape(process_event_detail) << "\","
           << "\"beforeLocationOk\":" << (before_location_ok ? "true" : "false") << ","
           << "\"beforeRotationOk\":" << (before_rotation_ok ? "true" : "false") << ","
           << "\"afterLocationOk\":" << (after_location_ok ? "true" : "false") << ","
           << "\"afterRotationOk\":" << (after_rotation_ok ? "true" : "false") << ","
           << "\"requestedLocation\":";
        append_vector_json(ss, params.location);
        ss << ",\"requestedRotation\":";
        append_rotator_json(ss, params.rotation);
        ss << ",\"beforeLocation\":";
        append_vector_json(ss, before_location);
        ss << ",\"beforeRotation\":";
        append_rotator_json(ss, before_rotation);
        ss << ",\"afterLocation\":";
        append_vector_json(ss, after_location);
        ss << ",\"afterRotation\":";
        append_rotator_json(ss, after_rotation);
        ss << ",\"abi\":{\"ServerPlaceParams\":" << sizeof(PlaceableServerPlaceParams) << "},"
           << "\"invokeError\":\"" << json_escape(invoke_error) << "\","
           << "\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << "}";
        return {invoked, ss.str()};
    }

    auto SCUMTraderManager::placeable_godmode_fill_native(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        const auto rpc_full_name = first_regex_value(command_text, {"rpcFullName", "playerRpcFullName"});
        const auto controller_full_name = first_regex_value(command_text, {"controllerFullName", "playerControllerFullName"});
        const auto pawn_full_name = first_regex_value(command_text, {"pawnFullName", "prisonerFullName", "playerPawnFullName"});
        const auto placeable_full_name = first_regex_value(command_text, {"placeableFullName", "targetFullName", "actorFullName", "objectPath"});
        const auto placeable_class = first_regex_value(command_text, {"placeableClass", "targetClass", "actorClass", "actorClassFullName"});
        const auto verified_placeable = regex_bool(command_text, "verifiedPlaceable", false);
        const auto x = regex_number(command_text, "x", regex_number(command_text, "targetX", 0.0));
        const auto y = regex_number(command_text, "y", regex_number(command_text, "targetY", 0.0));
        const auto z = regex_number(command_text, "z", regex_number(command_text, "targetZ", 0.0));
        const auto dispatch_all = regex_bool(command_text, "dispatchAllRoutes", false) || regex_bool(command_text, "dispatchAll", false);

        std::vector<std::string> attempts;
        auto* rpc = find_object_by_full_name(
            {L"PlayerRpcChannel", L"UPlayerRpcChannel", L"Object", L"UObject"},
            rpc_full_name,
            attempts,
            "rpc");
        auto* controller = find_object_by_full_name(
            {L"ConZPlayerController", L"PlayerController", L"Controller", L"AController", L"Object", L"UObject"},
            controller_full_name,
            attempts,
            "controller");
        auto* pawn = find_object_by_full_name(
            {L"BP_Prisoner_C", L"Prisoner", L"APrisoner", L"Actor", L"Object", L"UObject"},
            pawn_full_name,
            attempts,
            "pawn");
        auto* placeable = find_object_by_full_name(
            {L"PlaceableActorBase", L"PlaceableActorBaseBuilding", L"PlaceableActorModularBaseBuilding", L"Actor", L"Object", L"UObject"},
            placeable_full_name,
            attempts,
            "placeable",
            false);

        const auto target_text = lower_copy(placeable_class + " " + placeable_full_name + " " + full_name(placeable));
        const auto target_looks_placeable =
            verified_placeable ||
            target_text.find("placeable") != std::string::npos ||
            target_text.find("basebuilding") != std::string::npos ||
            target_text.find("bp_base_") != std::string::npos ||
            target_text.find("base_") != std::string::npos;
        if (placeable == nullptr || !target_looks_placeable || (rpc == nullptr && pawn == nullptr) || (rpc != nullptr && controller == nullptr))
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"stage\":\"resolve-placeable-rpc-controller-pawn\","
               << "\"route\":\"native-placeable-godmode-fill\","
               << "\"rpcResolved\":" << (rpc != nullptr ? "true" : "false") << ","
               << "\"controllerResolved\":" << (controller != nullptr ? "true" : "false") << ","
               << "\"pawnResolved\":" << (pawn != nullptr ? "true" : "false") << ","
               << "\"placeableResolved\":" << (placeable != nullptr ? "true" : "false") << ","
               << "\"targetLooksPlaceable\":" << (target_looks_placeable ? "true" : "false") << ","
               << "\"verifiedPlaceable\":" << (verified_placeable ? "true" : "false") << ","
               << "\"rpcFullName\":\"" << json_escape(rpc_full_name) << "\","
               << "\"controllerFullName\":\"" << json_escape(controller_full_name) << "\","
               << "\"pawnFullName\":\"" << json_escape(pawn_full_name) << "\","
               << "\"placeableFullName\":\"" << json_escape(placeable_full_name) << "\","
               << "\"placeableClass\":\"" << json_escape(placeable_class) << "\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        auto* rpc_interact_fn = resolve_function_by_name(
            "PlayerRpcChannel",
            "InteractWithObjectOnServer",
            {
                "/Script/SCUM.PlayerRpcChannel:InteractWithObjectOnServer",
                "Function /Script/SCUM.PlayerRpcChannel:InteractWithObjectOnServer",
                "/Script/SCUM.PlayerRpcChannel.InteractWithObjectOnServer",
                "Function /Script/SCUM.PlayerRpcChannel.InteractWithObjectOnServer",
            },
            attempts);
        auto* prisoner_interact_fn = resolve_function_by_name(
            "Prisoner",
            "InteractWithObjectOnServer",
            {
                "/Script/SCUM.Prisoner:InteractWithObjectOnServer",
                "Function /Script/SCUM.Prisoner:InteractWithObjectOnServer",
                "/Script/SCUM.Prisoner.InteractWithObjectOnServer",
                "Function /Script/SCUM.Prisoner.InteractWithObjectOnServer",
            },
            attempts);

        std::string process_event_detail;
        auto* process_event = process_event_from_ue4ss_log(base_dir(), process_event_detail);
        if (process_event == nullptr)
        {
            std::string vtable_detail;
            process_event = process_event_from_vtable(rpc != nullptr ? rpc : pawn, vtable_detail);
            process_event_detail += "; fallback-vtable: " + vtable_detail;
        }
        attempts.push_back("ProcessEvent=" + process_event_detail);

        if (process_event == nullptr || (rpc_interact_fn == nullptr && prisoner_interact_fn == nullptr))
        {
            std::ostringstream ss;
            ss << "{\"ok\":false,"
               << "\"stage\":\"resolve-process-event-or-function\","
               << "\"route\":\"native-placeable-godmode-fill\","
               << "\"rpc\":\"" << json_escape(full_name(rpc)) << "\","
               << "\"controller\":\"" << json_escape(full_name(controller)) << "\","
               << "\"pawn\":\"" << json_escape(full_name(pawn)) << "\","
               << "\"placeable\":\"" << json_escape(full_name(placeable)) << "\","
               << "\"rpcInteractFunction\":\"" << json_escape(full_name(rpc_interact_fn)) << "\","
               << "\"prisonerInteractFunction\":\"" << json_escape(full_name(prisoner_interact_fn)) << "\","
               << "\"processEvent\":\"" << json_escape(process_event_detail) << "\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        RC::Unreal::FVector interaction_location = make_vector(x, y, z);
        bool target_location_resolved = false;
        std::string target_location_detail = "payload";
        if (actor_location_noexcept(placeable, interaction_location))
        {
            target_location_resolved = true;
            target_location_detail = "K2_GetActorLocation";
        }
        else
        {
            target_location_detail = "payload-after-K2-exception";
        }

        bool rpc_invoked = false;
        bool pawn_invoked = false;
        unsigned long rpc_exception_code = 0;
        unsigned long pawn_exception_code = 0;

        if (rpc != nullptr && controller != nullptr && rpc_interact_fn != nullptr)
        {
            PlayerRpcInteractWithObjectOnServerParams params{};
            params.interactable = placeable;
            params.user_controller = controller;
            params.interaction_type = interaction_type_godmode_fill;
            params.interaction_data.pointer_data = placeable;
            params.interaction_data.bool_data = true;
            params.interaction_data.interaction_location = interaction_location;
            params.interaction_data.interaction_normal = RC::Unreal::FVector{0.0f, 0.0f, 1.0f};
            params.interaction_data.vector_data = interaction_location;
            params.interaction_data.base_element_id = 0;
            rpc_invoked = invoke_process_event_noexcept(process_event, rpc, rpc_interact_fn, &params, rpc_exception_code);
            attempts.push_back(std::string("Rpc.InteractWithObjectOnServer#GodModeFill(23)=") + (rpc_invoked ? "ok" : ("seh:" + hex_address(static_cast<std::uintptr_t>(rpc_exception_code)))));
        }

        if ((dispatch_all || !rpc_invoked) && pawn != nullptr && prisoner_interact_fn != nullptr)
        {
            PrisonerClientInteractParams params{};
            params.interactable = placeable;
            params.interaction_type = interaction_type_godmode_fill;
            params.interaction_data.pointer_data = placeable;
            params.interaction_data.bool_data = true;
            params.interaction_data.interaction_location = interaction_location;
            params.interaction_data.interaction_normal = RC::Unreal::FVector{0.0f, 0.0f, 1.0f};
            params.interaction_data.vector_data = interaction_location;
            params.interaction_data.base_element_id = 0;
            pawn_invoked = invoke_process_event_noexcept(process_event, pawn, prisoner_interact_fn, &params, pawn_exception_code);
            attempts.push_back(std::string("Prisoner.InteractWithObjectOnServer#GodModeFill(23)=") + (pawn_invoked ? "ok" : ("seh:" + hex_address(static_cast<std::uintptr_t>(pawn_exception_code)))));
        }

        const auto invoked = rpc_invoked || pawn_invoked;
        std::ostringstream ss;
        ss << "{\"ok\":" << (invoked ? "true" : "false") << ","
           << "\"stage\":\"native-placeable-godmode-fill-dispatch\","
           << "\"route\":\"native-placeable-godmode-fill\","
           << "\"interactionType\":" << static_cast<int>(interaction_type_godmode_fill) << ","
           << "\"rpcInvoked\":" << (rpc_invoked ? "true" : "false") << ","
           << "\"pawnInvoked\":" << (pawn_invoked ? "true" : "false") << ","
           << "\"dispatchAllRoutes\":" << (dispatch_all ? "true" : "false") << ","
           << "\"rpc\":\"" << json_escape(full_name(rpc)) << "\","
           << "\"controller\":\"" << json_escape(full_name(controller)) << "\","
           << "\"pawn\":\"" << json_escape(full_name(pawn)) << "\","
           << "\"placeable\":\"" << json_escape(full_name(placeable)) << "\","
           << "\"placeableClass\":\"" << json_escape(placeable_class) << "\","
           << "\"rpcInteractFunction\":\"" << json_escape(full_name(rpc_interact_fn)) << "\","
           << "\"prisonerInteractFunction\":\"" << json_escape(full_name(prisoner_interact_fn)) << "\","
           << "\"processEvent\":\"" << json_escape(process_event_detail) << "\","
           << "\"targetLocationResolved\":" << (target_location_resolved ? "true" : "false") << ","
           << "\"targetLocationDetail\":\"" << json_escape(target_location_detail) << "\","
           << "\"targetLocation\":";
        append_vector_json(ss, interaction_location);
        ss << ",\"abi\":{\"FText\":24,\"FInteractionData\":" << sizeof(FInteractionDataAbi)
           << ",\"ServerInteractParams\":" << sizeof(PlayerRpcInteractWithObjectOnServerParams)
           << ",\"PrisonerInteractParams\":" << sizeof(PrisonerClientInteractParams)
           << ",\"rpcInteractionDataOffset\":" << offsetof(PlayerRpcInteractWithObjectOnServerParams, interaction_data)
           << ",\"prisonerInteractionDataOffset\":" << offsetof(PrisonerClientInteractParams, interaction_data) << "},"
           << "\"rpcInvokeError\":\"" << (rpc_invoked ? "" : json_escape(hex_address(static_cast<std::uintptr_t>(rpc_exception_code)))) << "\","
           << "\"pawnInvokeError\":\"" << (pawn_invoked ? "" : json_escape(hex_address(static_cast<std::uintptr_t>(pawn_exception_code)))) << "\","
           << "\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << "}";
        return {invoked, ss.str()};
    }

    auto SCUMTraderManager::prepare_armory_anywhere(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        auto sale_distance = static_cast<float>(regex_number(command_text, "saleDistance", 2000.0));
        auto relevancy_range = static_cast<float>(regex_number(command_text, "relevancyRange", 3000.0));
        auto net_cull_distance = static_cast<float>(regex_number(command_text, "netCullDistance", 15000.0));
        auto update_time = static_cast<float>(regex_number(command_text, "tradeOutpostsUpdateTime", 60.0));
        const auto force_unlimited_stock = regex_bool(command_text, "forceUnlimitedStock", false);
        const auto force_unlimited_funds = regex_bool(command_text, "forceUnlimitedFunds", false);

        sale_distance = std::clamp(sale_distance, 0.0f, 10000000.0f);
        relevancy_range = std::clamp(relevancy_range, 0.0f, 10000000.0f);
        net_cull_distance = std::clamp(net_cull_distance, 10000.0f, 10000000.0f);
        update_time = std::clamp(update_time, 1.0f, 60.0f);

        auto* economy_manager = Unreal::UObjectGlobals::FindFirstOf(L"BP_EconomyManager_C");
        if (economy_manager == nullptr)
        {
            economy_manager = Unreal::UObjectGlobals::FindFirstOf(L"ConZEconomyManager");
        }

        auto* trader_component = object_property(economy_manager, L"_traderManagingComponent");

        std::vector<std::string> manager_route_counts;
        auto managers = find_trade_outpost_managers(&manager_route_counts);
        std::vector<std::string> manager_samples;
        for (size_t index = 0; index < std::min<size_t>(managers.size(), 8); ++index)
        {
            manager_samples.push_back(full_name(managers[index]));
        }

        const auto managers_loaded = !managers.empty();

        std::string sale_detail;
        std::string relevancy_detail;
        std::string update_detail;
        std::string unlimited_stock_detail = "disabled";
        std::string unlimited_funds_detail = "disabled";
        const auto sale_set = set_float_property(economy_manager, L"_maxSaleDistance", sale_distance, sale_detail, true);
        const auto relevancy_set = set_float_property(trader_component, L"_pawnRelevancyRange", relevancy_range, relevancy_detail, true);
        const auto update_set = set_float_property(trader_component, L"_tradeOutpostsUpdateTime", update_time, update_detail, false);
        const auto unlimited_stock_set = force_unlimited_stock && set_bool_property(economy_manager, L"_tradersUnlimitedStock", true, unlimited_stock_detail);
        const auto unlimited_funds_set = force_unlimited_funds && set_bool_property(economy_manager, L"_tradersUnlimitedFunds", true, unlimited_funds_detail);

        std::vector<std::string> target_route_counts;
        auto armory_targets = find_all_unique(
            {L"BP_ArmsDealer_01_C",
             L"BP_Outpost_Armory_NPCInteractionBox_C"},
            &target_route_counts);

        const auto net_cull_squared = std::min<float>(net_cull_distance * net_cull_distance, 30000000000000.0f);
        std::vector<std::string> net_details;
        std::vector<std::string> target_samples;
        int net_cull_set_count = 0;
        for (auto* target : armory_targets)
        {
            if (target == nullptr) continue;
            if (target_samples.size() < 16)
            {
                target_samples.push_back(full_name(target));
            }
            std::string net_detail;
            if (set_float_property(target, L"NetCullDistanceSquared", net_cull_squared, net_detail, true))
            {
                ++net_cull_set_count;
            }
            if (net_details.size() < 16)
            {
                net_details.push_back(full_name(target) + " => " + net_detail);
            }
        }

        const auto ok = economy_manager != nullptr && (sale_set || relevancy_set);
        std::ostringstream ss;
        ss << "{\"ok\":" << (ok ? "true" : "false") << ","
            << "\"stage\":\"armory-anywhere-prepare\","
            << "\"route\":\"stock-economy-relevancy-distance\","
           << "\"message\":\"Expands stock SCUM economy sale distance and trader manager relevancy for /armory proxy TradeBuy. Live TradeOutpostManager actors are diagnostic only; absence no longer blocks preparation. This route does not move donor TradePosts and does not implement a chat shop.\","
           << "\"managersLoaded\":" << (managers_loaded ? "true" : "false") << ","
           << "\"economyManager\":\"" << json_escape(full_name(economy_manager)) << "\","
           << "\"traderManagingComponent\":\"" << json_escape(full_name(trader_component)) << "\","
           << "\"requested\":{\"saleDistance\":" << sale_distance
           << ",\"relevancyRange\":" << relevancy_range
           << ",\"netCullDistance\":" << net_cull_distance
           << ",\"tradeOutpostsUpdateTime\":" << update_time << "},"
           << "\"economy\":{\"maxSaleDistance\":" << float_property(economy_manager, L"_maxSaleDistance", -1.0f)
           << ",\"tradersUnlimitedStock\":" << (bool_property(economy_manager, L"_tradersUnlimitedStock", false) ? "true" : "false")
           << ",\"tradersUnlimitedFunds\":" << (bool_property(economy_manager, L"_tradersUnlimitedFunds", false) ? "true" : "false")
           << ",\"saleSet\":" << (sale_set ? "true" : "false")
           << ",\"saleDetail\":\"" << json_escape(sale_detail) << "\""
           << ",\"unlimitedStockRequested\":" << (force_unlimited_stock ? "true" : "false")
           << ",\"unlimitedStockSet\":" << (unlimited_stock_set ? "true" : "false")
           << ",\"unlimitedStockDetail\":\"" << json_escape(unlimited_stock_detail) << "\""
           << ",\"unlimitedFundsRequested\":" << (force_unlimited_funds ? "true" : "false")
           << ",\"unlimitedFundsSet\":" << (unlimited_funds_set ? "true" : "false")
           << ",\"unlimitedFundsDetail\":\"" << json_escape(unlimited_funds_detail) << "\"},"
           << "\"traderManaging\":{\"pawnRelevancyRange\":" << float_property(trader_component, L"_pawnRelevancyRange", -1.0f)
           << ",\"tradeOutpostsUpdateTime\":" << float_property(trader_component, L"_tradeOutpostsUpdateTime", -1.0f)
           << ",\"relevancySet\":" << (relevancy_set ? "true" : "false")
           << ",\"relevancyDetail\":\"" << json_escape(relevancy_detail)
           << "\",\"updateSet\":" << (update_set ? "true" : "false")
           << ",\"updateDetail\":\"" << json_escape(update_detail) << "\"},"
           << "\"armoryTargets\":{\"count\":" << armory_targets.size()
           << ",\"netCullSetCount\":" << net_cull_set_count
           << ",\"classSearch\":";
        append_json_string_array(ss, target_route_counts);
        ss << ",\"samples\":";
        append_json_string_array(ss, target_samples);
        ss << ",\"netCullDetails\":";
        append_json_string_array(ss, net_details);
        ss << "},\"tradeOutpostManagers\":{\"count\":" << managers.size()
           << ",\"classSearch\":";
        append_json_string_array(ss, manager_route_counts);
        ss << ",\"samples\":";
        append_json_string_array(ss, manager_samples);
        ss << "}}";
        return {ok, ss.str()};
    }

    auto SCUMTraderManager::spawn_linked_trader_actor(const std::string& command_text) -> std::pair<bool, std::string>
    {
        const auto actor_class = regex_value(command_text, "actorClass");
        const auto catalog_path = regex_value(command_text, "catalogPath");
        const auto requested_personality_path = regex_value(command_text, "personalityPath");
        const auto mode = lower_copy(regex_value(command_text, "nativeMode") + regex_value(command_text, "mode"));
        const auto stock_outpost_key = canonical_outpost_key(regex_value(command_text, "stockOutpostKey"));
        const auto force_stock_personality =
            mode.find("stock-personality") != std::string::npos ||
            regex_bool(command_text, "forceStockPersonality", false);
        const auto require_live_manager =
            mode.find("require-manager") != std::string::npos ||
            mode.find("no-personality-fallback") != std::string::npos;
        const auto requested_allow_managerless_marker_fallback =
            mode.find("marker-fallback") != std::string::npos ||
            mode.find("allow-managerless") != std::string::npos ||
            regex_bool(command_text, "allowManagerlessLinkedTrader", false);
        const auto requested_allow_managerless_stock_personality =
            force_stock_personality &&
            (requested_allow_managerless_marker_fallback || regex_bool(command_text, "allowManagerlessStockPersonality", false));
        const auto allow_unsafe_managerless_spawnactor_mutation =
            regex_bool(command_text, "allowUnsafeManagerlessSpawnActorMutation", false);
        const auto allow_manual_spawned_trader_array_mutation_requested =
            regex_bool(command_text, "allowManualSpawnedTraderArrayMutation", false) ||
            regex_bool(command_text, "allowUnsafeSpawnedTraderArrayMutation", false);
        const auto allow_exact_manager_linked_armory_array_mutation =
            regex_bool(command_text, "allowExactManagerLinkedArmoryArrayMutation", false) ||
            regex_bool(command_text, "allowExactStockArmoryArrayMutation", false);
        const auto allow_exact_stock_armory_managerless_no_array_candidate =
            regex_bool(command_text, "allowExactStockArmoryManagerlessNoArrayCandidate", false) ||
            regex_bool(command_text, "allowExactManagerlessArmoryNoArrayCandidate", false);
        auto allow_manual_spawned_trader_array_mutation = allow_manual_spawned_trader_array_mutation_requested;
        const auto allow_managerless_marker_fallback =
            requested_allow_managerless_marker_fallback && allow_unsafe_managerless_spawnactor_mutation;
        const auto allow_managerless_stock_personality =
            requested_allow_managerless_stock_personality && !require_live_manager && allow_unsafe_managerless_spawnactor_mutation;
        const auto require_tradepost =
            requested_allow_managerless_marker_fallback ||
            mode.find("require-tradepost") != std::string::npos;
        const auto tokens = trader_kind_tokens(actor_class);
        const auto x = regex_number(command_text, "x", 0.0);
        const auto y = regex_number(command_text, "y", 0.0);
        const auto z = regex_number(command_text, "z", 0.0);
        const auto yaw = regex_number(command_text, "yaw", 0.0);
        const auto fast_manager_lookup_only =
            regex_bool(command_text, "fastManagerLookupOnly", false) ||
            regex_bool(command_text, "skipSlowManagerFallbacks", false);
        auto spawn_forward_distance = static_cast<float>(regex_number(command_text, "spawnForwardDistance", 350.0));
        auto spawn_lift_z = static_cast<float>(regex_number(command_text, "spawnLiftZ", 85.0));
        spawn_forward_distance = std::clamp(spawn_forward_distance, 150.0f, 1200.0f);
        spawn_lift_z = std::clamp(spawn_lift_z, 35.0f, 250.0f);

        append_log("spawn_linked_trader_actor begin actorClass=" + actor_class +
            " mode=" + mode +
            " x=" + std::to_string(x) +
            " y=" + std::to_string(y) +
            " z=" + std::to_string(z));

        std::vector<std::string> attempts;
        if (std::abs(x) < 1.0 && std::abs(y) < 1.0 && std::abs(z) < 1.0)
        {
            std::ostringstream ss;
            ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
               << "\"ok\":false,"
               << "\"stage\":\"resolve-spawn-location\","
               << "\"route\":\"spawn-linked-trader-actor-exact\","
               << "\"message\":\"Player/world coordinates are missing; refusing Armory trader spawn before donor or manager lookup\","
               << "\"nativeMode\":\"" << json_escape(mode) << "\","
               << "\"x\":" << x << ",\"y\":" << y << ",\"z\":" << z << "}";
            return {false, ss.str()};
        }
        double donor_lookup_x = x;
        double donor_lookup_y = y;
        double donor_selection_distance = std::numeric_limits<double>::max();
        if (force_stock_personality && !stock_outpost_key.empty())
        {
            double forced_x{};
            double forced_y{};
            if (stock_outpost_center(stock_outpost_key, forced_x, forced_y))
            {
                donor_lookup_x = forced_x;
                donor_lookup_y = forced_y;
                attempts.push_back("donor search forced to stock outpost " + stock_outpost_key);
            }
        }
        auto* donor_trade_post = find_donor_tradepost_for_actor(tokens, donor_lookup_x, donor_lookup_y, attempts, donor_selection_distance);
        const auto donor_distance = std::isfinite(donor_selection_distance)
            ? donor_selection_distance
            : (donor_trade_post != nullptr ? tradepost_reference_distance_2d(donor_trade_post, x, y) : donor_selection_distance);
        append_log("spawn_linked_trader_actor donor=" + full_name(donor_trade_post) +
            " donorDistance=" + std::to_string(donor_distance));
        if (donor_trade_post == nullptr)
        {
            if (require_live_manager || require_tradepost)
            {
                std::ostringstream ss;
                ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
                   << "\"ok\":false,"
                   << "\"stage\":\"find-loaded-donor-tradepost\","
                   << "\"route\":\"spawn-linked-trader-actor-marker-fallback\","
                   << "\"message\":\"No loaded donor TradePost was found. Refusing personality-only trader fallback for /armory because a functional Armory NPC must be linked to a real stock TradePost marker/personality.\","
                   << "\"nativeMode\":\"" << json_escape(mode) << "\","
                   << "\"requireLiveManager\":" << (require_live_manager ? "true" : "false") << ","
                   << "\"requireTradePost\":" << (require_tradepost ? "true" : "false") << ","
                   << "\"allowManagerlessMarkerFallback\":" << (allow_managerless_marker_fallback ? "true" : "false") << ","
                   << "\"requestedAllowManagerlessMarkerFallback\":" << (requested_allow_managerless_marker_fallback ? "true" : "false") << ","
                   << "\"allowUnsafeManagerlessSpawnActorMutation\":" << (allow_unsafe_managerless_spawnactor_mutation ? "true" : "false") << ","
                   << "\"tokens\":";
                append_json_string_array(ss, tokens);
                ss << ",\"attempts\":";
                append_json_string_array(ss, attempts);
                ss << "}";
                return {false, ss.str()};
            }

            if (std::abs(x) < 1.0 && std::abs(y) < 1.0 && std::abs(z) < 1.0)
            {
                std::ostringstream ss;
                ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
                   << "\"ok\":false,"
                   << "\"stage\":\"resolve-spawn-location\","
                   << "\"route\":\"spawn-personality-only-trader-actor\","
                   << "\"message\":\"Player/world coordinates are missing; refusing to spawn trader at origin\","
                   << "\"attempts\":";
                append_json_string_array(ss, attempts);
                ss << "}";
                return {false, ss.str()};
            }

            auto* world = static_cast<RC::Unreal::UWorld*>(nullptr);
            if (auto* economy_manager = Unreal::UObjectGlobals::FindFirstOf(L"BP_EconomyManager_C"))
            {
                world = economy_manager->GetWorld();
                attempts.push_back("fallback world from BP_EconomyManager_C=" + full_name(economy_manager));
            }
            if (world == nullptr)
            {
                if (auto* economy_manager = Unreal::UObjectGlobals::FindFirstOf(L"ConZEconomyManager"))
                {
                    world = economy_manager->GetWorld();
                    attempts.push_back("fallback world from ConZEconomyManager=" + full_name(economy_manager));
                }
            }

            auto* trader_class = resolve_trader_actor_class(actor_class, catalog_path, attempts);
            std::string selected_outpost_key;
            std::string selected_kind;
            std::string selected_personality_path;
            auto* personality = resolve_trader_personality_asset(
                actor_class,
                requested_personality_path,
                x,
                y,
                selected_outpost_key,
                selected_kind,
                selected_personality_path,
                attempts);

            if (world == nullptr || trader_class == nullptr || personality == nullptr)
            {
                std::ostringstream ss;
                ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
                   << "\"ok\":false,"
                   << "\"stage\":\"resolve-personality-only-route\","
                   << "\"route\":\"spawn-personality-only-trader-actor\","
                   << "\"message\":\"Exact stock TradePost is not loaded, and the personality-only fallback could not resolve world, ATrader class, or TraderPersonalityDataAsset. Lua must preload the selected personality asset before this native command.\","
                   << "\"worldResolved\":" << (world != nullptr ? "true" : "false") << ","
                   << "\"traderClassResolved\":" << (trader_class != nullptr ? "true" : "false") << ","
                   << "\"personalityResolved\":" << (personality != nullptr ? "true" : "false") << ","
                   << "\"selectedOutpostKey\":\"" << json_escape(selected_outpost_key) << "\","
                   << "\"selectedPersonalityKind\":\"" << json_escape(selected_kind) << "\","
                   << "\"selectedPersonalityPath\":\"" << json_escape(selected_personality_path) << "\","
                   << "\"catalogPath\":\"" << json_escape(catalog_path) << "\","
                   << "\"requestedPersonalityPath\":\"" << json_escape(requested_personality_path) << "\","
                   << "\"tokens\":";
                append_json_string_array(ss, tokens);
                ss << ",\"attempts\":";
                append_json_string_array(ss, attempts);
                ss << "}";
                return {false, ss.str()};
            }

            const auto desired_location = location_in_front_of_player(x, y, z, yaw, spawn_forward_distance, spawn_lift_z);
            const auto trader_yaw = normalize_yaw(yaw + 180.0);
            auto transform = make_transform(desired_location.x, desired_location.y, desired_location.z, trader_yaw);
            append_log("spawn_linked_trader_actor personality-only SpawnActor class=" + full_name(trader_class));
            auto* spawned_actor = world->SpawnActor(trader_class, &transform);
            auto* spawned_trader = static_cast<Unreal::UObject*>(spawned_actor);
            append_log("spawn_linked_trader_actor personality-only spawned=" + full_name(spawned_trader));
            if (spawned_trader == nullptr)
            {
                std::ostringstream ss;
                ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
                   << "\"ok\":false,"
                   << "\"stage\":\"spawn-personality-only-trader\","
                   << "\"route\":\"spawn-personality-only-trader-actor\","
                   << "\"traderClass\":\"" << json_escape(full_name(trader_class)) << "\","
                   << "\"selectedPersonality\":\"" << json_escape(full_name(personality)) << "\","
                   << "\"attempts\":";
                append_json_string_array(ss, attempts);
                ss << "}";
                return {false, ss.str()};
            }

            const auto forced_location = location_in_front_of_player(
                regex_number(command_text, "x", 0.0),
                regex_number(command_text, "y", 0.0),
                regex_number(command_text, "z", 0.0),
                yaw,
                spawn_forward_distance,
                spawn_lift_z);

            spawned_actor->SetActorHiddenInGame(false);
            std::vector<std::string> network_wake_details;
            wake_actor_for_network_trade(spawned_actor, spawned_trader, network_wake_details);
            RC::Unreal::FVector actual_location{};
            std::string location_detail;
            append_log("spawn_linked_trader_actor personality-only place begin actor=" + full_name(spawned_trader));
            const auto location_ok = force_actor_world_location(spawned_actor, forced_location, trader_yaw, actual_location, location_detail);
            append_log("spawn_linked_trader_actor personality-only place ok=" + std::string(location_ok ? "true" : "false") +
                " detail=" + location_detail);
            wake_actor_for_network_trade(spawned_actor, spawned_trader, network_wake_details);

            std::string personality_detail;
            const auto personality_set = set_weak_object_property(spawned_trader, L"_traderPersonalityDataAsset", personality, personality_detail);
            auto* personality_after = weak_object_property(spawned_trader, L"_traderPersonalityDataAsset");
            const auto personality_after_guid = guid_to_string(guid_property(personality_after, L"TraderPersistentId"));

            std::string original_location_detail;
            const auto original_location_set = set_vector_property(spawned_trader, L"_originalLocation", actual_location, original_location_detail);

            std::string net_cull_detail;
            const auto net_cull_set = set_float_property(spawned_trader, L"NetCullDistanceSquared", 4000000000000.0f, net_cull_detail, true);
            std::string interaction_distance_detail;
            const auto interaction_distance_set = set_float_property(spawned_trader, L"_interactionDistance", 5000.0f, interaction_distance_detail, true);

            const auto ok = location_ok && personality_set && original_location_set;
            if (!ok)
            {
                spawned_actor->SetActorHiddenInGame(true);
                spawned_actor->SetActorEnableCollision(false);
            }
            std::ostringstream ss;
            ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
               << "\"ok\":" << (ok ? "true" : "false") << ","
               << "\"stage\":\"spawn-personality-only-trader\","
               << "\"route\":\"spawn-personality-only-trader-actor\","
               << "\"note\":\"Exact stock TradePost was not loaded, so this route spawns a real ATrader-derived actor and sets stock TraderPersonalityDataAsset through WeakObjectProperty. This avoids dummy TradeOutpostManager, relocation, and broad runtime scans. Client trade UI stock/sell must still be proven in-game.\","
               << "\"selectedOutpostKey\":\"" << json_escape(selected_outpost_key) << "\","
               << "\"selectedPersonalityKind\":\"" << json_escape(selected_kind) << "\","
               << "\"selectedPersonalityPath\":\"" << json_escape(selected_personality_path) << "\","
               << "\"selectedPersonality\":\"" << json_escape(full_name(personality)) << "\","
               << "\"selectedPersonalityGuid\":\"" << json_escape(guid_to_string(guid_property(personality, L"TraderPersistentId"))) << "\","
               << "\"traderClass\":\"" << json_escape(full_name(trader_class)) << "\","
               << "\"spawnedTrader\":\"" << json_escape(full_name(spawned_trader)) << "\","
               << "\"spawnOffset\":{\"forwardCm\":" << spawn_forward_distance << ",\"liftZCm\":" << spawn_lift_z << "},"
               << "\"requestedSpawnLocation\":";
            append_vector_json(ss, forced_location);
            ss << ",\"actualSpawnLocation\":";
            append_vector_json(ss, actual_location);
            ss << ",\"spawnLocationOk\":" << (location_ok ? "true" : "false") << ","
               << "\"spawnLocationDetail\":\"" << json_escape(location_detail) << "\","
               << "\"traderPersonalityWeakSet\":" << (personality_set ? "true" : "false") << ","
               << "\"traderPersonalityWeakDetail\":\"" << json_escape(personality_detail) << "\","
               << "\"traderPersonalityAfter\":\"" << json_escape(full_name(personality_after)) << "\","
               << "\"traderPersonalityAfterGuid\":\"" << json_escape(personality_after_guid) << "\","
               << "\"originalLocationSet\":" << (original_location_set ? "true" : "false") << ","
               << "\"originalLocationDetail\":\"" << json_escape(original_location_detail) << "\","
               << "\"netCullSet\":" << (net_cull_set ? "true" : "false") << ","
               << "\"netCullDetail\":\"" << json_escape(net_cull_detail) << "\","
               << "\"interactionDistanceSet\":" << (interaction_distance_set ? "true" : "false") << ","
               << "\"interactionDistanceDetail\":\"" << json_escape(interaction_distance_detail) << "\","
               << "\"networkWakeDetails\":";
            append_json_string_array(ss, network_wake_details);
            ss << ","
               << "\"clientTradeUiConfirmed\":false,"
               << "\"tokens\":";
            append_json_string_array(ss, tokens);
            ss << ",\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {ok, ss.str()};
        }

        const auto donor_outpost_key = outpost_key_from_name(lower_copy(full_name(donor_trade_post)));
        std::vector<std::string> manager_route_counts;
        auto managers = find_trade_outpost_managers_for_key(donor_outpost_key, &manager_route_counts, !fast_manager_lookup_only);
        std::string donor_manager_route;
        auto* donor_manager_candidate = find_manager_for_tradepost(managers, donor_trade_post, &donor_manager_route);
        auto* donor_assigned_manager = find_assigned_manager(managers, donor_trade_post);
        auto* donor_manager = donor_assigned_manager;
        append_log("spawn_linked_trader_actor donorManager=" + full_name(donor_manager) +
            " candidate=" + full_name(donor_manager_candidate) +
            " route=" + donor_manager_route +
            " managerCount=" + std::to_string(managers.size()));

        const auto* markers = array_property(donor_trade_post, L"_traderMarkers");
        const auto marker_count = is_reasonable_array(markers) ? markers->count : -1;
        RC::Unreal::UObject* marker_personality = nullptr;
        std::string marker_personality_name;
        std::string marker_personality_guid;
        if (is_reasonable_array(markers) && markers->count > 0 && markers->data != nullptr)
        {
            auto* marker_items = static_cast<FTraderMarkerAbi*>(markers->data);
            marker_personality = marker_items[0].trader_personality;
            marker_personality_name = full_name(marker_personality);
            marker_personality_guid = guid_to_string(guid_property(marker_personality, L"TraderPersistentId"));
        }

        RC::Unreal::UObject* requested_personality = nullptr;
        std::string requested_selected_outpost_key;
        std::string requested_selected_kind;
        std::string requested_selected_path;
        if (force_stock_personality && !requested_personality_path.empty())
        {
            requested_personality = resolve_trader_personality_asset(
                actor_class,
                requested_personality_path,
                x,
                y,
                requested_selected_outpost_key,
                requested_selected_kind,
                requested_selected_path,
                attempts);
        }
        auto* selected_personality = requested_personality != nullptr ? requested_personality : marker_personality;
        const auto selected_personality_name = full_name(selected_personality);
        const auto selected_personality_guid = guid_to_string(guid_property(selected_personality, L"TraderPersistentId"));
        const auto requested_stock_outpost_key = stock_outpost_key.empty()
            ? std::string(stocked_armory_outpost_key)
            : stock_outpost_key;
        const auto actor_class_lower = lower_copy(actor_class);
        const auto exact_armory_actor =
            actor_class_lower.find("bp_armsdealer_01_c") != std::string::npos;
        const auto donor_tradepost_full_name = full_name(donor_trade_post);
        const auto donor_manager_full_name = full_name(donor_manager);
        const auto exact_stock_a0_tradepost =
            donor_tradepost_full_name == stocked_armory_tradepost_full_name;
        const auto exact_stock_a0_manager =
            donor_manager_full_name == stocked_armory_manager_full_name;
        const auto exact_stock_armory_managerless_no_array_proof_ok =
            donor_manager == nullptr &&
            allow_exact_stock_armory_managerless_no_array_candidate &&
            exact_armory_actor &&
            donor_outpost_key == stocked_armory_outpost_key &&
            requested_stock_outpost_key == stocked_armory_outpost_key &&
            exact_stock_a0_tradepost &&
            marker_personality_guid == stocked_armory_personality_guid &&
            selected_personality_guid == stocked_armory_personality_guid;
        if (donor_manager == nullptr)
        {
           if (!allow_managerless_marker_fallback && !exact_stock_armory_managerless_no_array_proof_ok)
            {
                attempts.push_back("blocked: exact donor TradePost has no linked live TradeOutpostManager and marker fallback is disabled");
                std::ostringstream ss;
                ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
                   << "\"ok\":false,"
                   << "\"stage\":\"find-live-tradeoutpost-manager\","
                   << "\"route\":\"spawn-linked-trader-actor-exact\","
                   << "\"message\":\"Exact donor TradePost was found, but no live TradeOutpostManager is linked to it. Refusing linked ATrader spawn unless explicit marker fallback is enabled.\","
                   << "\"nativeMode\":\"" << json_escape(mode) << "\","
                   << "\"allowManagerlessMarkerFallback\":false,"
                   << "\"allowUnsafeManagerlessSpawnActorMutation\":" << (allow_unsafe_managerless_spawnactor_mutation ? "true" : "false") << ","
                   << "\"allowManualSpawnedTraderArrayMutation\":" << (allow_manual_spawned_trader_array_mutation ? "true" : "false") << ","
                   << "\"allowExactStockArmoryManagerlessNoArrayCandidate\":" << (allow_exact_stock_armory_managerless_no_array_candidate ? "true" : "false") << ","
                   << "\"fastManagerLookupOnly\":" << (fast_manager_lookup_only ? "true" : "false") << ","
                   << "\"donorTradePost\":\"" << json_escape(full_name(donor_trade_post)) << "\","
                   << "\"donorOutpostKey\":\"" << json_escape(donor_outpost_key) << "\","
                   << "\"donorManagerCandidate\":\"" << json_escape(full_name(donor_manager_candidate)) << "\","
                   << "\"donorManagerRoute\":\"" << json_escape(donor_manager_route) << "\","
                   << "\"managerLinkedProof\":\"_assignedTradePosts\","
                   << "\"candidateManagerIsOnlyDiagnostic\":" << (donor_manager_candidate != nullptr ? "true" : "false") << ","
                   << "\"markerPersonalityGuid\":\"" << json_escape(marker_personality_guid) << "\","
                   << "\"selectedPersonalityGuid\":\"" << json_escape(selected_personality_guid) << "\","
                   << "\"expectedPersonalityGuid\":\"" << stocked_armory_personality_guid << "\","
                   << "\"managerCount\":" << managers.size() << ","
                   << "\"managerSearch\":";
                append_json_string_array(ss, manager_route_counts);
                ss << ",\"attempts\":";
                append_json_string_array(ss, attempts);
                ss << "}";
                return {false, ss.str()};
            }
           if (exact_stock_armory_managerless_no_array_proof_ok)
           {
                attempts.push_back("exact stock Armory A_0 managerless no-array candidate enabled; private arrays and economy manager mutation stay skipped");
           }
           else
           {
                attempts.push_back(force_stock_personality
                    ? "unsafe managerless stock-personality SpawnActor mutation explicitly enabled"
                    : "unsafe managerless marker SpawnActor mutation explicitly enabled");
           }
        }

        if (donor_manager != nullptr && !allow_manual_spawned_trader_array_mutation_requested)
        {
            attempts.push_back("manual _spawnedTraders/_spawnedSedentaryNPCs mutation disabled; continuing as exact manager-linked ATrader candidate");
        }

        auto* world = donor_trade_post->GetWorld();
        if (world == nullptr)
        {
            if (auto* economy_manager = Unreal::UObjectGlobals::FindFirstOf(L"BP_EconomyManager_C"))
            {
                world = economy_manager->GetWorld();
            }
        }

        auto* trader_class = resolve_trader_actor_class(actor_class, catalog_path, attempts);
        if (world == nullptr || trader_class == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
               << "\"ok\":false,"
               << "\"stage\":\"resolve-world-or-trader-class\","
               << "\"route\":\"spawn-linked-trader-actor-exact\","
               << "\"donorTradePost\":\"" << json_escape(full_name(donor_trade_post)) << "\","
               << "\"worldResolved\":" << (world != nullptr ? "true" : "false") << ","
               << "\"traderClassResolved\":" << (trader_class != nullptr ? "true" : "false") << ","
               << "\"catalogPath\":\"" << json_escape(catalog_path) << "\","
               << "\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        std::string donor_distance_policy = "within-local-stock-guard";
        if (donor_distance > max_tradepost_donor_distance_2d)
        {
            donor_distance_policy = "allowed-linked-manager-anywhere";
            attempts.push_back("donor distance exceeds local stock guard; continuing linked ATrader route because real TradeOutpostManager is linked and spawned trader stays near player");
        }

        if (std::abs(x) < 1.0 && std::abs(y) < 1.0 && std::abs(z) < 1.0)
        {
            std::ostringstream ss;
            ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
               << "\"ok\":false,"
               << "\"stage\":\"resolve-spawn-location\","
               << "\"message\":\"Player/world coordinates are missing; refusing to spawn trader at origin\","
               << "\"donorTradePost\":\"" << json_escape(full_name(donor_trade_post)) << "\"}";
            return {false, ss.str()};
        }

        const auto exact_manager_linked_armory_proof_ok =
            donor_manager != nullptr &&
            exact_armory_actor &&
            donor_outpost_key == stocked_armory_outpost_key &&
            requested_stock_outpost_key == stocked_armory_outpost_key &&
            donor_manager_route == "_assignedTradePosts" &&
            exact_stock_a0_tradepost &&
            exact_stock_a0_manager &&
            marker_personality_guid == stocked_armory_personality_guid &&
            selected_personality_guid == stocked_armory_personality_guid;
        const auto exact_manager_linked_armory_array_mutation_ok =
            allow_manual_spawned_trader_array_mutation_requested &&
            allow_exact_manager_linked_armory_array_mutation &&
            exact_manager_linked_armory_proof_ok;
        allow_manual_spawned_trader_array_mutation =
            exact_manager_linked_armory_array_mutation_ok ||
            (allow_manual_spawned_trader_array_mutation_requested &&
                allow_unsafe_managerless_spawnactor_mutation &&
                !require_live_manager);
        if (exact_manager_linked_armory_array_mutation_ok)
        {
            attempts.push_back("exact manager-linked Armory A_0 proof passed; private array mutation enabled for this stock route only");
        }
        else if (exact_manager_linked_armory_proof_ok && !allow_manual_spawned_trader_array_mutation)
        {
            attempts.push_back("exact manager-linked Armory A_0 proof passed; private array mutation remains skipped");
        }
        if (donor_manager != nullptr && !allow_manual_spawned_trader_array_mutation && !exact_manager_linked_armory_proof_ok)
        {
            attempts.push_back("blocked: exact Armory array mutation flag was requested, but stock A_0 manager/tradepost/personality proof did not match");
            std::ostringstream ss;
            ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
               << "\"ok\":false,"
               << "\"stage\":\"blocked-exact-manager-linked-armory-proof\","
               << "\"route\":\"spawn-linked-trader-actor-exact\","
               << "\"message\":\"Live TradeOutpostManager is linked, but exact stock Armory proof failed. Refusing manual _spawnedTraders/_spawnedSedentaryNPCs mutation before SpawnActor.\","
               << "\"nativeMode\":\"" << json_escape(mode) << "\","
               << "\"donorTradePost\":\"" << json_escape(full_name(donor_trade_post)) << "\","
               << "\"donorManager\":\"" << json_escape(full_name(donor_manager)) << "\","
               << "\"donorManagerCandidate\":\"" << json_escape(full_name(donor_manager_candidate)) << "\","
               << "\"donorManagerRoute\":\"" << json_escape(donor_manager_route) << "\","
               << "\"donorOutpostKey\":\"" << json_escape(donor_outpost_key) << "\","
               << "\"stockOutpostKey\":\"" << json_escape(requested_stock_outpost_key) << "\","
               << "\"exactArmoryActor\":" << (exact_armory_actor ? "true" : "false") << ","
               << "\"exactStockA0TradePost\":" << (exact_stock_a0_tradepost ? "true" : "false") << ","
               << "\"exactStockA0Manager\":" << (exact_stock_a0_manager ? "true" : "false") << ","
               << "\"markerPersonalityGuid\":\"" << json_escape(marker_personality_guid) << "\","
               << "\"selectedPersonalityGuid\":\"" << json_escape(selected_personality_guid) << "\","
               << "\"expectedPersonalityGuid\":\"" << stocked_armory_personality_guid << "\","
               << "\"allowManualSpawnedTraderArrayMutationRequested\":" << (allow_manual_spawned_trader_array_mutation_requested ? "true" : "false") << ","
               << "\"allowExactManagerLinkedArmoryArrayMutation\":" << (allow_exact_manager_linked_armory_array_mutation ? "true" : "false") << ","
               << "\"exactManagerLinkedArmoryArrayMutationOk\":false,"
               << "\"managerLinked\":true,"
               << "\"managerLinkedProof\":\"_assignedTradePosts\","
               << "\"managerCount\":" << managers.size() << ","
               << "\"managerSearch\":";
            append_json_string_array(ss, manager_route_counts);
            ss << ",\"attempts\":";
            append_json_string_array(ss, attempts);
            ss << "}";
            return {false, ss.str()};
        }

        const auto desired_location = location_in_front_of_player(x, y, z, yaw, spawn_forward_distance, spawn_lift_z);
        const auto trader_yaw = normalize_yaw(yaw + 180.0);
        auto transform = make_transform(desired_location.x, desired_location.y, desired_location.z, trader_yaw);
        append_log("spawn_linked_trader_actor linked SpawnActor class=" + full_name(trader_class) +
            " donor=" + full_name(donor_trade_post));
        auto* spawned_actor = world->SpawnActor(trader_class, &transform);
        auto* spawned_trader = static_cast<Unreal::UObject*>(spawned_actor);
        append_log("spawn_linked_trader_actor linked spawned=" + full_name(spawned_trader));
        if (spawned_trader == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
               << "\"ok\":false,"
               << "\"stage\":\"spawn-trader-actor\","
               << "\"route\":\"spawn-linked-trader-actor-exact\","
               << "\"traderClass\":\"" << json_escape(full_name(trader_class)) << "\","
               << "\"donorTradePost\":\"" << json_escape(full_name(donor_trade_post)) << "\"}";
            return {false, ss.str()};
        }

        const auto forced_location = location_in_front_of_player(
            regex_number(command_text, "x", 0.0),
            regex_number(command_text, "y", 0.0),
            regex_number(command_text, "z", 0.0),
            yaw,
            spawn_forward_distance,
            spawn_lift_z);

        spawned_actor->SetActorHiddenInGame(false);
        std::vector<std::string> network_wake_details;
        wake_actor_for_network_trade(spawned_actor, spawned_trader, network_wake_details);
        RC::Unreal::FVector actual_location{};
        std::string location_detail;
        append_log("spawn_linked_trader_actor linked place begin actor=" + full_name(spawned_trader));
        const auto location_ok = force_actor_world_location(spawned_actor, forced_location, trader_yaw, actual_location, location_detail);
        append_log("spawn_linked_trader_actor linked place ok=" + std::string(location_ok ? "true" : "false") +
            " detail=" + location_detail);
        wake_actor_for_network_trade(spawned_actor, spawned_trader, network_wake_details);

        std::string tradepost_detail;
        const auto tradepost_set = set_object_property(spawned_trader, L"_tradeOutpostBuilding", donor_trade_post, tradepost_detail);

        std::string personality_detail;
        const auto personality_set = selected_personality != nullptr && set_weak_object_property(spawned_trader, L"_traderPersonalityDataAsset", selected_personality, personality_detail);
        auto* personality_after = weak_object_property(spawned_trader, L"_traderPersonalityDataAsset");
        const auto personality_after_guid = guid_to_string(guid_property(personality_after, L"TraderPersistentId"));

        std::string original_location_detail;
        const auto original_location_set = set_vector_property(spawned_trader, L"_originalLocation", actual_location, original_location_detail);

        std::string net_cull_detail;
        const auto net_cull_set = set_float_property(spawned_trader, L"NetCullDistanceSquared", 4000000000000.0f, net_cull_detail, true);
        std::string interaction_distance_detail;
        const auto interaction_distance_set = set_float_property(spawned_trader, L"_interactionDistance", 5000.0f, interaction_distance_detail, true);
        wake_actor_for_network_trade(spawned_actor, spawned_trader, network_wake_details);

        auto* spawned_traders = const_cast<TArrayAbi*>(array_property(donor_trade_post, L"_spawnedTraders"));
        const auto before_count = is_reasonable_array(spawned_traders) ? spawned_traders->count : -1;
        const auto before_max = is_reasonable_array(spawned_traders) ? spawned_traders->max : -1;
        std::string add_detail;
        bool registered_in_tradepost = false;
        append_log("spawn_linked_trader_actor linked add spawnedTraders begin before=" + std::to_string(before_count) +
            "/" + std::to_string(before_max));
        if (!allow_manual_spawned_trader_array_mutation)
        {
            add_detail = "skipped:manual-private-array-mutation-disabled";
        }
        else if (location_ok)
        {
            registered_in_tradepost = add_object_to_array(spawned_traders, spawned_trader, add_detail);
        }
        else
        {
            add_detail = "skipped:spawn-location-not-ok";
        }
        append_log("spawn_linked_trader_actor linked add spawnedTraders ok=" + std::string(registered_in_tradepost ? "true" : "false") +
            " detail=" + add_detail);
        const auto after_count = is_reasonable_array(spawned_traders) ? spawned_traders->count : -1;
        const auto after_max = is_reasonable_array(spawned_traders) ? spawned_traders->max : -1;

        auto* spawned_sedentary = const_cast<TArrayAbi*>(array_property(donor_trade_post, L"_spawnedSedentaryNPCs"));
        const auto sedentary_before_count = is_reasonable_array(spawned_sedentary) ? spawned_sedentary->count : -1;
        const auto sedentary_before_max = is_reasonable_array(spawned_sedentary) ? spawned_sedentary->max : -1;
        std::string sedentary_add_detail;
        bool registered_in_sedentary = false;
        append_log("spawn_linked_trader_actor linked add spawnedSedentary begin before=" + std::to_string(sedentary_before_count) +
            "/" + std::to_string(sedentary_before_max));
        if (!allow_manual_spawned_trader_array_mutation)
        {
            sedentary_add_detail = "skipped:manual-private-array-mutation-disabled";
        }
        else if (location_ok)
        {
            registered_in_sedentary = add_object_to_array(spawned_sedentary, spawned_trader, sedentary_add_detail);
        }
        else
        {
            sedentary_add_detail = "skipped:spawn-location-not-ok";
        }
        append_log("spawn_linked_trader_actor linked add spawnedSedentary ok=" + std::string(registered_in_sedentary ? "true" : "false") +
            " detail=" + sedentary_add_detail);
        const auto sedentary_after_count = is_reasonable_array(spawned_sedentary) ? spawned_sedentary->count : -1;
        const auto sedentary_after_max = is_reasonable_array(spawned_sedentary) ? spawned_sedentary->max : -1;
        const auto linked_candidate_without_private_arrays =
            donor_manager != nullptr &&
            exact_manager_linked_armory_proof_ok &&
            !allow_manual_spawned_trader_array_mutation;
        const auto exact_stock_armory_managerless_no_array_candidate =
            donor_manager == nullptr &&
            exact_stock_armory_managerless_no_array_proof_ok &&
            !allow_manual_spawned_trader_array_mutation;
        const auto tradepost_registration_ok =
            registered_in_tradepost || linked_candidate_without_private_arrays || exact_stock_armory_managerless_no_array_candidate;
        const auto sedentary_registration_ok =
            registered_in_sedentary || linked_candidate_without_private_arrays || exact_stock_armory_managerless_no_array_candidate;

        const auto economy_prepare = prepare_linked_armory_economy(
            command_text,
            donor_manager != nullptr &&
                location_ok &&
                tradepost_set &&
                personality_set &&
                tradepost_registration_ok &&
                sedentary_registration_ok,
            selected_personality);
        append_log("spawn_linked_trader_actor linked economyPrepare ok=" + std::string(economy_prepare.ok ? "true" : "false") +
            " allowed=" + std::string(economy_prepare.allowed ? "true" : "false") +
            " detail=" + economy_prepare.detail);

        network_wake_details.push_back("donorTradePost.ForceNetUpdate=skipped:no-spawn-route-processevent");
        network_wake_details.push_back("donorTradePost.FlushNetDormancy=skipped:no-spawn-route-processevent");
        wake_actor_for_network_trade(spawned_actor, spawned_trader, network_wake_details);

        const auto managerless_marker_link_ok =
            donor_manager == nullptr &&
            allow_managerless_marker_fallback &&
            !force_stock_personality &&
            donor_trade_post != nullptr &&
            selected_personality != nullptr &&
            registered_in_tradepost &&
            registered_in_sedentary;
        const auto managerless_stock_personality_ok =
            donor_manager == nullptr &&
            allow_managerless_stock_personality &&
            donor_outpost_key == stocked_armory_outpost_key &&
            personality_after_guid == stocked_armory_personality_guid;
        const auto lifecycle_link_ok =
            donor_manager != nullptr ||
            managerless_marker_link_ok ||
            managerless_stock_personality_ok ||
            exact_stock_armory_managerless_no_array_candidate;
        const auto economy_prepare_ok =
            exact_stock_armory_managerless_no_array_candidate ||
            (!economy_prepare.requested || economy_prepare.ok);
        const auto ok =
            lifecycle_link_ok &&
            location_ok &&
            tradepost_set &&
            personality_set &&
            original_location_set &&
            tradepost_registration_ok &&
            sedentary_registration_ok &&
            economy_prepare_ok;
        if (!ok)
        {
            spawned_actor->SetActorHiddenInGame(true);
            spawned_actor->SetActorEnableCollision(false);
        }
        std::ostringstream ss;
        ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
           << "\"ok\":" << (ok ? "true" : "false") << ","
           << "\"stage\":\"spawn-link-trader-actor\","
           << "\"route\":\"" << (donor_manager != nullptr ? "spawn-linked-trader-actor-exact" : (exact_stock_armory_managerless_no_array_candidate ? "spawn-linked-trader-actor-exact-stock-managerless-no-arrays" : (managerless_stock_personality_ok ? "spawn-linked-trader-actor-stock-personality-fallback" : "spawn-linked-trader-actor-marker-fallback"))) << "\","
           << "\"note\":\"Spawns a real trader actor and links it to an existing stock TradePost marker/personality without spawning dummy TradeOutpostManager, relocating stock traders, or running broad runtime scans. Functional status still requires client trade UI stock/sell proof.\","
           << "\"donorTradePost\":\"" << json_escape(full_name(donor_trade_post)) << "\","
           << "\"donorManager\":\"" << json_escape(full_name(donor_manager)) << "\","
           << "\"donorManagerCandidate\":\"" << json_escape(full_name(donor_manager_candidate)) << "\","
           << "\"donorManagerRoute\":\"" << json_escape(donor_manager_route) << "\","
           << "\"donorOutpostKey\":\"" << json_escape(donor_outpost_key) << "\","
           << "\"managerLinked\":" << (donor_manager != nullptr ? "true" : "false") << ","
           << "\"fastManagerLookupOnly\":" << (fast_manager_lookup_only ? "true" : "false") << ","
           << "\"managerLinkedProof\":\"_assignedTradePosts\","
           << "\"candidateManagerIsOnlyDiagnostic\":" << (donor_manager_candidate != nullptr && donor_manager == nullptr ? "true" : "false") << ","
           << "\"lifecycleManagerLinkedRequired\":" << (require_live_manager ? "true" : "false") << ","
           << "\"lifecycleLinkOk\":" << (lifecycle_link_ok ? "true" : "false") << ","
           << "\"allowManagerlessMarkerFallback\":" << (allow_managerless_marker_fallback ? "true" : "false") << ","
           << "\"allowManagerlessStockPersonality\":" << (allow_managerless_stock_personality ? "true" : "false") << ","
           << "\"allowUnsafeManagerlessSpawnActorMutation\":" << (allow_unsafe_managerless_spawnactor_mutation ? "true" : "false") << ","
           << "\"allowManualSpawnedTraderArrayMutationRequested\":" << (allow_manual_spawned_trader_array_mutation_requested ? "true" : "false") << ","
           << "\"allowManualSpawnedTraderArrayMutationEffective\":" << (allow_manual_spawned_trader_array_mutation ? "true" : "false") << ","
           << "\"allowExactManagerLinkedArmoryArrayMutation\":" << (allow_exact_manager_linked_armory_array_mutation ? "true" : "false") << ","
           << "\"allowExactStockArmoryManagerlessNoArrayCandidate\":" << (allow_exact_stock_armory_managerless_no_array_candidate ? "true" : "false") << ","
           << "\"exactManagerLinkedArmoryProofOk\":" << (exact_manager_linked_armory_proof_ok ? "true" : "false") << ","
           << "\"exactManagerLinkedArmoryArrayMutationOk\":" << (exact_manager_linked_armory_array_mutation_ok ? "true" : "false") << ","
           << "\"exactStockArmoryManagerlessNoArrayProofOk\":" << (exact_stock_armory_managerless_no_array_proof_ok ? "true" : "false") << ","
           << "\"exactArmoryActor\":" << (exact_armory_actor ? "true" : "false") << ","
           << "\"exactStockA0TradePost\":" << (exact_stock_a0_tradepost ? "true" : "false") << ","
           << "\"exactStockA0Manager\":" << (exact_stock_a0_manager ? "true" : "false") << ","
           << "\"linkedCandidateWithoutPrivateArrays\":" << (linked_candidate_without_private_arrays ? "true" : "false") << ","
           << "\"exactStockArmoryManagerlessNoArrayCandidate\":" << (exact_stock_armory_managerless_no_array_candidate ? "true" : "false") << ","
           << "\"requestedAllowManagerlessMarkerFallback\":" << (requested_allow_managerless_marker_fallback ? "true" : "false") << ","
           << "\"requestedAllowManagerlessStockPersonality\":" << (requested_allow_managerless_stock_personality ? "true" : "false") << ","
           << "\"managerlessMarkerLinkOk\":" << (managerless_marker_link_ok ? "true" : "false") << ","
           << "\"managerlessStockPersonalityOk\":" << (managerless_stock_personality_ok ? "true" : "false") << ","
           << "\"forceStockPersonality\":" << (force_stock_personality ? "true" : "false") << ","
           << "\"stockOutpostKey\":\"" << json_escape(stock_outpost_key) << "\","
           << "\"managerCount\":" << managers.size() << ","
           << "\"managerSearch\":";
        append_json_string_array(ss, manager_route_counts);
        ss << ","
           << "\"donorDistance2d\":" << donor_distance << ","
           << "\"donorDistancePolicy\":\"" << json_escape(donor_distance_policy) << "\","
           << "\"markerCount\":" << marker_count << ","
           << "\"markerPersonality\":\"" << json_escape(marker_personality_name) << "\","
           << "\"markerPersonalityGuid\":\"" << json_escape(marker_personality_guid) << "\","
           << "\"requestedPersonalityPath\":\"" << json_escape(requested_personality_path) << "\","
           << "\"requestedPersonalityResolved\":\"" << json_escape(full_name(requested_personality)) << "\","
           << "\"requestedPersonalitySelectedOutpostKey\":\"" << json_escape(requested_selected_outpost_key) << "\","
           << "\"requestedPersonalitySelectedKind\":\"" << json_escape(requested_selected_kind) << "\","
           << "\"requestedPersonalitySelectedPath\":\"" << json_escape(requested_selected_path) << "\","
           << "\"selectedPersonality\":\"" << json_escape(selected_personality_name) << "\","
           << "\"selectedPersonalityGuid\":\"" << json_escape(selected_personality_guid) << "\","
           << "\"traderClass\":\"" << json_escape(full_name(trader_class)) << "\","
           << "\"spawnedTrader\":\"" << json_escape(full_name(spawned_trader)) << "\","
           << "\"spawnOffset\":{\"forwardCm\":" << spawn_forward_distance << ",\"liftZCm\":" << spawn_lift_z << "},"
           << "\"requestedSpawnLocation\":";
        append_vector_json(ss, forced_location);
        ss << ",\"actualSpawnLocation\":";
        append_vector_json(ss, actual_location);
        ss << ",\"spawnLocationOk\":" << (location_ok ? "true" : "false") << ","
           << "\"spawnLocationDetail\":\"" << json_escape(location_detail) << "\","
           << "\"tradeOutpostBuildingSet\":" << (tradepost_set ? "true" : "false") << ","
           << "\"tradeOutpostBuildingDetail\":\"" << json_escape(tradepost_detail) << "\","
           << "\"traderPersonalityWeakSet\":" << (personality_set ? "true" : "false") << ","
           << "\"traderPersonalityWeakDetail\":\"" << json_escape(personality_detail) << "\","
           << "\"traderPersonalityAfter\":\"" << json_escape(full_name(personality_after)) << "\","
           << "\"traderPersonalityAfterGuid\":\"" << json_escape(personality_after_guid) << "\","
           << "\"originalLocationSet\":" << (original_location_set ? "true" : "false") << ","
           << "\"originalLocationDetail\":\"" << json_escape(original_location_detail) << "\","
           << "\"netCullSet\":" << (net_cull_set ? "true" : "false") << ","
           << "\"netCullDetail\":\"" << json_escape(net_cull_detail) << "\","
           << "\"interactionDistanceSet\":" << (interaction_distance_set ? "true" : "false") << ","
           << "\"interactionDistanceDetail\":\"" << json_escape(interaction_distance_detail) << "\","
           << "\"economyPrepare\":";
        append_linked_armory_economy_prepare_json(ss, economy_prepare);
        ss << ",\"networkWakeDetails\":";
        append_json_string_array(ss, network_wake_details);
        ss << ","
           << "\"spawnedTradersBefore\":{\"count\":" << before_count << ",\"max\":" << before_max << "},"
           << "\"spawnedTradersAfter\":{\"count\":" << after_count << ",\"max\":" << after_max << "},"
           << "\"spawnedSedentaryBefore\":{\"count\":" << sedentary_before_count << ",\"max\":" << sedentary_before_max << "},"
           << "\"spawnedSedentaryAfter\":{\"count\":" << sedentary_after_count << ",\"max\":" << sedentary_after_max << "},"
           << "\"tradepostRegistrationOk\":" << (tradepost_registration_ok ? "true" : "false") << ","
           << "\"sedentaryRegistrationOk\":" << (sedentary_registration_ok ? "true" : "false") << ","
           << "\"registrationDetail\":\"" << json_escape(add_detail) << "\","
           << "\"sedentaryRegistrationDetail\":\"" << json_escape(sedentary_add_detail) << "\","
           << "\"attempts\":";
        append_json_string_array(ss, attempts);
        ss << "}";
        return {ok, ss.str()};
    }

    auto SCUMTraderManager::spawn_functional_tradepost_clone(const std::string& command_text) -> std::pair<bool, std::string>
    {
        const auto actor_class = regex_value(command_text, "actorClass");
        const auto catalog_path = regex_value(command_text, "catalogPath");
        const auto tokens = trader_kind_tokens(actor_class);
        const auto x = regex_number(command_text, "x", 0.0);
        const auto y = regex_number(command_text, "y", 0.0);
        const auto z = regex_number(command_text, "z", 0.0);
        const auto yaw = regex_number(command_text, "yaw", 0.0);

        double donor_distance = std::numeric_limits<double>::max();
        std::vector<std::string> donor_attempts;
        auto* donor_trade_post = find_donor_tradepost_for_actor(tokens, x, y, donor_attempts, donor_distance);

        if (donor_trade_post == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
               << "\"ok\":false,"
               << "\"stage\":\"find-donor-tradepost\","
               << "\"message\":\"No loaded donor TradePost matched requested actor through exact path/name lookup\","
               << "\"tokens\":";
            append_json_string_array(ss, tokens);
            ss << ",\"donorSearch\":";
            append_json_string_array(ss, donor_attempts);
            ss << "}";
            return {false, ss.str()};
        }

        std::vector<std::string> manager_route_counts;
        const auto donor_outpost_key = outpost_key_from_name(lower_copy(full_name(donor_trade_post)));
        auto managers = find_trade_outpost_managers_for_key(donor_outpost_key, &manager_route_counts);
        const auto had_existing_manager = !managers.empty();
        std::string donor_manager_route;
        auto* donor_manager_candidate = find_manager_for_tradepost(managers, donor_trade_post, &donor_manager_route);
        auto* donor_assigned_manager = find_assigned_manager(managers, donor_trade_post);
        auto* donor_manager = donor_assigned_manager;
        std::string donor_distance_policy = "within-local-stock-guard";
        if (donor_distance > max_tradepost_donor_distance_2d)
        {
            donor_distance_policy = "allowed-registered-tradepost-anywhere-live-manager";
        }

        auto* world = donor_trade_post->GetWorld();
        if (world == nullptr)
        {
            if (auto* economy_manager = Unreal::UObjectGlobals::FindFirstOf(L"BP_EconomyManager_C"))
            {
                world = economy_manager->GetWorld();
            }
        }

        auto* donor_class = donor_trade_post->GetClassPrivate();
        if (world == nullptr || donor_class == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
               << "\"ok\":false,"
               << "\"stage\":\"resolve-world-class\","
               << "\"donorTradePost\":\"" << json_escape(full_name(donor_trade_post)) << "\","
               << "\"worldResolved\":" << (world != nullptr ? "true" : "false") << ","
               << "\"classResolved\":" << (donor_class != nullptr ? "true" : "false") << "}";
            return {false, ss.str()};
        }

        if (std::abs(x) < 1.0 && std::abs(y) < 1.0 && std::abs(z) < 1.0)
        {
            std::ostringstream ss;
            ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
               << "\"ok\":false,"
               << "\"stage\":\"resolve-spawn-location\","
               << "\"message\":\"Player/world coordinates are missing; refusing to spawn at origin\","
               << "\"donorTradePost\":\"" << json_escape(full_name(donor_trade_post)) << "\"}";
            return {false, ss.str()};
        }

        bool spawned_manager = false;
        std::string spawned_manager_detail;
        std::string outpost_description_detail;
        std::vector<std::string> manager_spawn_search;
        if (donor_manager == nullptr)
        {
            manager_spawn_search.push_back("blocked: runtime SpawnActor(BP_TradeOutpostManager_C) caused SCUMServer RequestExit on 2026-05-26");
            std::ostringstream ss;
            ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
               << "\"ok\":false,"
               << "\"stage\":\"find-live-tradeoutpost-manager\","
               << "\"route\":\"clone-existing-registered-tradepost-live-manager-only\","
               << "\"message\":\"No live TradeOutpostManager is linked to donor TradePost. Creating BP_TradeOutpostManager at runtime is blocked because the previous native route made SCUMServer request exit. Move/test near a real loaded outpost or use a future proven native lifecycle hook; this command will not create dummy managers.\","
               << "\"donorTradePost\":\"" << json_escape(full_name(donor_trade_post)) << "\","
               << "\"donorOutpostKey\":\"" << json_escape(donor_outpost_key) << "\","
               << "\"donorManagerCandidate\":\"" << json_escape(full_name(donor_manager_candidate)) << "\","
               << "\"donorManagerRoute\":\"" << json_escape(donor_manager_route) << "\","
               << "\"managerLinkedProof\":\"_assignedTradePosts\","
               << "\"candidateManagerIsOnlyDiagnostic\":" << (donor_manager_candidate != nullptr ? "true" : "false") << ","
               << "\"managerCount\":" << managers.size() << ","
               << "\"tradePostCount\":1,"
               << "\"donorSearch\":";
            append_json_string_array(ss, donor_attempts);
            ss << ",\"managerSearch\":";
            append_json_string_array(ss, manager_route_counts);
            ss << ",\"managerSpawnSearch\":";
            append_json_string_array(ss, manager_spawn_search);
            ss << "}";
            return {false, ss.str()};
        }

        const auto desired_tradepost_location = location_in_front_of_player(x, y, z, yaw, 280.0, 20.0);
        auto transform = make_transform(desired_tradepost_location.x, desired_tradepost_location.y, desired_tradepost_location.z, yaw);
        auto* spawned_actor = world->SpawnActor(donor_class, &transform);
        auto* spawned_trade_post = static_cast<Unreal::UObject*>(spawned_actor);
        if (spawned_trade_post == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
               << "\"ok\":false,"
               << "\"stage\":\"spawn-tradepost\","
               << "\"donorTradePost\":\"" << json_escape(full_name(donor_trade_post)) << "\","
               << "\"spawnClass\":\"" << json_escape(full_name(donor_class)) << "\"}";
            return {false, ss.str()};
        }
        spawned_actor->SetActorHiddenInGame(false);
        RC::Unreal::FVector actual_tradepost_location{};
        std::string tradepost_location_detail;
        const auto tradepost_location_ok = force_actor_world_location(spawned_actor, desired_tradepost_location, yaw, actual_tradepost_location, tradepost_location_detail);
        if (!tradepost_location_ok)
        {
            spawned_actor->SetActorHiddenInGame(true);
            spawned_actor->SetActorEnableCollision(false);
            std::ostringstream ss;
            ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
               << "\"ok\":false,"
               << "\"stage\":\"spawn-tradepost-location-proof\","
               << "\"route\":\"clone-existing-registered-tradepost-native-site\","
               << "\"message\":\"Spawned TradePost actor did not remain at the requested player-adjacent world location; refusing to register a hidden/origin tradepost.\","
               << "\"donorTradePost\":\"" << json_escape(full_name(donor_trade_post)) << "\","
               << "\"spawnClass\":\"" << json_escape(full_name(donor_class)) << "\","
               << "\"spawnedTradePost\":\"" << json_escape(full_name(spawned_trade_post)) << "\","
               << "\"requestedTradePostLocation\":";
            append_vector_json(ss, desired_tradepost_location);
            ss << ",\"actualTradePostLocation\":";
            append_vector_json(ss, actual_tradepost_location);
            ss << ",\"tradePostLocationDetail\":\"" << json_escape(tradepost_location_detail) << "\"}";
            return {false, ss.str()};
        }

        std::string tradepost_net_cull_detail;
        const auto tradepost_net_cull_set = set_float_property(spawned_trade_post, L"NetCullDistanceSquared", 4000000000000.0f, tradepost_net_cull_detail, true);

        std::string trader_marker_copy_detail;
        std::string location_marker_copy_detail;
        std::string sedentary_marker_copy_detail;
        const auto trader_markers_copied = copy_raw_array_property(
            spawned_trade_post,
            donor_trade_post,
            L"_traderMarkers",
            sizeof(FTraderMarkerAbi),
            32,
            trader_marker_copy_detail);
        const auto location_markers_copied = copy_raw_array_property(
            spawned_trade_post,
            donor_trade_post,
            L"_locationMarkers",
            sizeof(FTraderLocationMarkerAbi),
            64,
            location_marker_copy_detail);
        const auto sedentary_markers_copied = copy_raw_array_property(
            spawned_trade_post,
            donor_trade_post,
            L"_sedentaryNPCMarkers",
            sizeof(FSedentaryNPCMarkerAbi),
            64,
            sedentary_marker_copy_detail);

        std::string marker_normalization_detail;
        const auto marker_normalization_count = normalize_tradepost_markers(spawned_trade_post, marker_normalization_detail);

        const auto* spawned_markers = array_property(spawned_trade_post, L"_traderMarkers");
        const auto* spawned_sedentary_markers = array_property(spawned_trade_post, L"_sedentaryNPCMarkers");
        const auto has_trader_markers = is_reasonable_array(spawned_markers) && spawned_markers->count > 0;
        const auto has_sedentary_markers = is_reasonable_array(spawned_sedentary_markers) && spawned_sedentary_markers->count > 0;
        const auto* registration_array_name = has_trader_markers ? L"_assignedTradePosts" : L"_otherAssignedTradeOutpostBuildings";
        auto* registration_array = const_cast<TArrayAbi*>(array_property(donor_manager, registration_array_name));
        const auto before_count = is_reasonable_array(registration_array) ? registration_array->count : -1;
        const auto before_max = is_reasonable_array(registration_array) ? registration_array->max : -1;
        std::string add_detail;
        const auto registered = tradepost_location_ok && trader_markers_copied && location_markers_copied && sedentary_markers_copied && (has_trader_markers || has_sedentary_markers) && add_object_to_array(registration_array, spawned_trade_post, add_detail);
        const auto after_count = is_reasonable_array(registration_array) ? registration_array->count : -1;
        const auto after_max = is_reasonable_array(registration_array) ? registration_array->max : -1;

        RC::Unreal::UObject* marker_personality = nullptr;
        std::string marker_personality_name;
        std::string marker_personality_guid;
        if (is_reasonable_array(spawned_markers) && spawned_markers->count > 0 && spawned_markers->data != nullptr)
        {
            auto* marker_items = static_cast<FTraderMarkerAbi*>(spawned_markers->data);
            marker_personality = marker_items[0].trader_personality;
            marker_personality_name = full_name(marker_personality);
            marker_personality_guid = guid_to_string(guid_property(marker_personality, L"TraderPersistentId"));
        }

        std::vector<std::string> trader_spawn_attempts;
        auto* trader_class = resolve_trader_actor_class(actor_class, catalog_path, trader_spawn_attempts);
        const auto initial_trader_location = location_in_front_of_player(x, y, z, yaw, 180.0, 20.0);
        const auto trader_yaw = normalize_yaw(yaw + 180.0);
        auto trader_transform = make_transform(initial_trader_location.x, initial_trader_location.y, initial_trader_location.z, trader_yaw);
        auto* spawned_trader_actor = (registered && trader_class != nullptr && marker_personality != nullptr)
            ? world->SpawnActor(trader_class, &trader_transform)
            : nullptr;
        auto* spawned_trader = static_cast<Unreal::UObject*>(spawned_trader_actor);

        std::string trader_tradepost_detail = "skipped";
        std::string trader_personality_detail = "skipped";
        std::string trader_original_location_detail = "skipped";
        std::string spawned_trader_add_detail = "skipped";
        bool trader_tradepost_set = false;
        bool trader_personality_set = false;
        bool trader_original_location_set = false;
        bool spawned_trader_registered = false;
        bool trader_location_ok = false;
        bool trader_net_cull_set = false;
        bool trader_interaction_distance_set = false;
        RC::Unreal::FVector actual_trader_location{};
        std::string trader_location_detail = "skipped";
        std::string trader_net_cull_detail = "skipped";
        std::string trader_interaction_distance_detail = "skipped";

        if (spawned_trader != nullptr)
        {
            const auto forced_trader_location = location_in_front_of_player(x, y, z, yaw, 180.0, 20.0);

            spawned_trader_actor->SetActorHiddenInGame(false);
            trader_location_ok = force_actor_world_location(spawned_trader_actor, forced_trader_location, trader_yaw, actual_trader_location, trader_location_detail);

            trader_tradepost_set = set_object_property(spawned_trader, L"_tradeOutpostBuilding", spawned_trade_post, trader_tradepost_detail);
            trader_personality_set = set_weak_object_property(spawned_trader, L"_traderPersonalityDataAsset", marker_personality, trader_personality_detail);

            trader_original_location_set = set_vector_property(spawned_trader, L"_originalLocation", actual_trader_location, trader_original_location_detail);
            trader_net_cull_set = set_float_property(spawned_trader, L"NetCullDistanceSquared", 4000000000000.0f, trader_net_cull_detail, true);
            trader_interaction_distance_set = set_float_property(spawned_trader, L"_interactionDistance", 5000.0f, trader_interaction_distance_detail, true);

            auto* spawned_traders_mut = const_cast<TArrayAbi*>(array_property(spawned_trade_post, L"_spawnedTraders"));
            spawned_trader_registered = trader_location_ok && add_object_to_array(spawned_traders_mut, spawned_trader, spawned_trader_add_detail);

            if (!(trader_location_ok && trader_tradepost_set && trader_personality_set && trader_original_location_set && spawned_trader_registered))
            {
                spawned_trader_actor->SetActorHiddenInGame(true);
                spawned_trader_actor->SetActorEnableCollision(false);
            }
        }
        else if (!registered)
        {
            spawned_trader_add_detail = "skipped: tradepost registration failed";
        }
        else if (trader_class == nullptr)
        {
            spawned_trader_add_detail = "skipped: trader class not resolved";
        }
        else if (marker_personality == nullptr)
        {
            spawned_trader_add_detail = "skipped: trader marker personality missing";
        }
        else
        {
            spawned_trader_add_detail = "SpawnActor returned null";
        }

        auto* trader_personality_after = weak_object_property(spawned_trader, L"_traderPersonalityDataAsset");
        const auto trader_personality_after_guid = guid_to_string(guid_property(trader_personality_after, L"TraderPersistentId"));
        const auto* spawned_location_markers = array_property(spawned_trade_post, L"_locationMarkers");
        const auto* spawned_traders = array_property(spawned_trade_post, L"_spawnedTraders");
        const auto* spawned_sedentary = array_property(spawned_trade_post, L"_spawnedSedentaryNPCs");
        const auto spawned_trader_count = is_reasonable_array(spawned_traders) ? spawned_traders->count : -1;
        const auto spawned_sedentary_count = is_reasonable_array(spawned_sedentary) ? spawned_sedentary->count : -1;
        const auto spawned_trader_link_ok =
            trader_location_ok &&
            trader_tradepost_set &&
            trader_personality_set &&
            trader_original_location_set &&
            spawned_trader_registered;
        const auto functional_confirmed = registered && spawned_trader_link_ok;
        const auto requested_trader_location = location_in_front_of_player(x, y, z, yaw, 180.0, 20.0);

        if (registered)
        {
            m_spawned_tradepost_monitors.push_back(
                SpawnedTradePostMonitor{
                    spawned_trade_post,
                    actor_class,
                    full_name(spawned_trade_post),
                    std::chrono::steady_clock::now(),
                    spawned_trader_count,
                    spawned_sedentary_count});
        }

        std::ostringstream ss;
        ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
           << "\"ok\":" << (functional_confirmed ? "true" : "false") << ","
           << "\"stage\":\"spawn-register-tradepost\","
           << "\"route\":\"clone-existing-registered-tradepost-native-site\","
           << "\"note\":\"Creates a new TradePost actor from a stock TradePost class only when a real live TradeOutpostManager is already loaded and linked, then spawns and links a real trader actor to that new TradePost. It refuses to spawn dummy managers. Functional status still requires client trade UI stock/sell proof.\","
           << "\"donorTradePost\":\"" << json_escape(full_name(donor_trade_post)) << "\","
           << "\"donorManager\":\"" << json_escape(full_name(donor_manager)) << "\","
           << "\"donorManagerRoute\":\"" << json_escape(donor_manager_route) << "\","
           << "\"hadExistingManager\":" << (had_existing_manager ? "true" : "false") << ","
           << "\"spawnedManager\":" << (spawned_manager ? "true" : "false") << ","
           << "\"spawnedManagerDetail\":\"" << json_escape(spawned_manager_detail) << "\","
           << "\"donorOutpostKey\":\"" << json_escape(donor_outpost_key) << "\","
           << "\"outpostDescriptionDetail\":\"" << json_escape(outpost_description_detail) << "\","
           << "\"managerSpawnSearch\":";
        append_json_string_array(ss, manager_spawn_search);
        ss << ","
           << "\"donorDistance2d\":" << donor_distance << ","
           << "\"donorDistancePolicy\":\"" << json_escape(donor_distance_policy) << "\","
           << "\"spawnClass\":\"" << json_escape(full_name(donor_class)) << "\","
           << "\"spawnedTradePost\":\"" << json_escape(full_name(spawned_trade_post)) << "\","
           << "\"requestedTradePostLocation\":";
        append_vector_json(ss, desired_tradepost_location);
        ss << ",\"actualTradePostLocation\":";
        append_vector_json(ss, actual_tradepost_location);
        ss << ",\"tradePostLocationOk\":" << (tradepost_location_ok ? "true" : "false") << ","
           << "\"tradePostLocationDetail\":\"" << json_escape(tradepost_location_detail) << "\","
           << "\"tradePostNetCullSet\":" << (tradepost_net_cull_set ? "true" : "false") << ","
           << "\"tradePostNetCullDetail\":\"" << json_escape(tradepost_net_cull_detail) << "\","
           << "\"traderClass\":\"" << json_escape(full_name(trader_class)) << "\","
           << "\"spawnedTrader\":\"" << json_escape(full_name(spawned_trader)) << "\","
           << "\"spawnedTraderImmediate\":" << (spawned_trader != nullptr ? "true" : "false") << ","
           << "\"requestedTraderLocation\":";
        append_vector_json(ss, requested_trader_location);
        ss << ",\"actualTraderLocation\":";
        append_vector_json(ss, actual_trader_location);
        ss << ",\"traderLocationOk\":" << (trader_location_ok ? "true" : "false") << ","
           << "\"traderLocationDetail\":\"" << json_escape(trader_location_detail) << "\","
           << "\"traderNetCullSet\":" << (trader_net_cull_set ? "true" : "false") << ","
           << "\"traderNetCullDetail\":\"" << json_escape(trader_net_cull_detail) << "\","
           << "\"traderInteractionDistanceSet\":" << (trader_interaction_distance_set ? "true" : "false") << ","
           << "\"traderInteractionDistanceDetail\":\"" << json_escape(trader_interaction_distance_detail) << "\","
           << "\"markerPersonality\":\"" << json_escape(marker_personality_name) << "\","
           << "\"markerPersonalityGuid\":\"" << json_escape(marker_personality_guid) << "\","
           << "\"tradeOutpostBuildingSet\":" << (trader_tradepost_set ? "true" : "false") << ","
           << "\"tradeOutpostBuildingDetail\":\"" << json_escape(trader_tradepost_detail) << "\","
           << "\"traderPersonalityWeakSet\":" << (trader_personality_set ? "true" : "false") << ","
           << "\"traderPersonalityWeakDetail\":\"" << json_escape(trader_personality_detail) << "\","
           << "\"traderPersonalityAfter\":\"" << json_escape(full_name(trader_personality_after)) << "\","
           << "\"traderPersonalityAfterGuid\":\"" << json_escape(trader_personality_after_guid) << "\","
           << "\"originalLocationSet\":" << (trader_original_location_set ? "true" : "false") << ","
           << "\"originalLocationDetail\":\"" << json_escape(trader_original_location_detail) << "\","
           << "\"spawnedTraderRegisteredInTradePost\":" << (spawned_trader_registered ? "true" : "false") << ","
           << "\"spawnedTraderAddDetail\":\"" << json_escape(spawned_trader_add_detail) << "\","
           << "\"spawnedTraderAttempts\":";
        append_json_string_array(ss, trader_spawn_attempts);
        ss << ","
           << "\"registrationArray\":\"" << json_escape(narrow(std::wstring(registration_array_name))) << "\","
           << "\"assignedBefore\":{\"count\":" << before_count << ",\"max\":" << before_max << "},"
           << "\"assignedAfter\":{\"count\":" << after_count << ",\"max\":" << after_max << "},"
           << "\"registrationDetail\":\"" << json_escape(add_detail) << "\","
           << "\"markerCopy\":{\"traderMarkers\":" << (trader_markers_copied ? "true" : "false")
           << ",\"traderMarkersDetail\":\"" << json_escape(trader_marker_copy_detail)
           << "\",\"locationMarkers\":" << (location_markers_copied ? "true" : "false")
           << ",\"locationMarkersDetail\":\"" << json_escape(location_marker_copy_detail)
           << "\",\"sedentaryMarkers\":" << (sedentary_markers_copied ? "true" : "false")
           << ",\"sedentaryMarkersDetail\":\"" << json_escape(sedentary_marker_copy_detail) << "\"},"
           << "\"markerNormalization\":{\"adjusted\":" << marker_normalization_count << ",\"detail\":\"" << json_escape(marker_normalization_detail) << "\"},"
           << "\"functionalConfirmed\":" << (functional_confirmed ? "true" : "false") << ","
           << "\"spawnedTraderLinkOk\":" << (spawned_trader_link_ok ? "true" : "false") << ","
           << "\"monitoringSeconds\":180,"
           << "\"spawnedTraderMarkersCount\":" << (is_reasonable_array(spawned_markers) ? spawned_markers->count : -1) << ","
           << "\"spawnedSedentaryMarkersCount\":" << (is_reasonable_array(spawned_sedentary_markers) ? spawned_sedentary_markers->count : -1) << ","
           << "\"spawnedTradersCount\":" << spawned_trader_count << ","
           << "\"spawnedSedentaryNPCsCount\":" << spawned_sedentary_count << ","
           << "\"traderMarkerSamples\":";
        append_trader_marker_samples(ss, spawned_markers, 3);
        ss << ",\"locationMarkerSamples\":";
        append_location_marker_samples(ss, spawned_location_markers, 3);
        ss << "}";
        return {functional_confirmed, ss.str()};
    }

    auto SCUMTraderManager::relocate_registered_tradepost_for_trader(const std::string& command_text) const -> std::pair<bool, std::string>
    {
        const auto actor_class = regex_value(command_text, "actorClass");
        const auto tokens = trader_kind_tokens(actor_class);
        const auto x = regex_number(command_text, "x", 0.0);
        const auto y = regex_number(command_text, "y", 0.0);
        const auto z = regex_number(command_text, "z", 0.0);
        const auto yaw = regex_number(command_text, "yaw", 0.0);

        auto managers = find_all_unique({L"TradeOutpostManager", L"ATradeOutpostManager", L"BP_TradeOutpostManager_C"});
        auto trade_posts = find_all_unique({L"TradePost", L"ATradePost"});

        Unreal::UObject* donor_trade_post = nullptr;
        Unreal::UObject* donor_manager = nullptr;
        std::string donor_reason;

        for (auto* trade_post : trade_posts)
        {
            const auto full = full_name(trade_post);
            const auto full_lower = lower_copy(full);
            if (!tradepost_matches_tokens(full_lower, tokens)) continue;
            const auto* markers = array_property(trade_post, L"_traderMarkers");
            if (!is_reasonable_array(markers) || markers->count <= 0)
            {
                donor_reason = "matched tradepost has no trader markers: " + full;
                continue;
            }
            donor_trade_post = trade_post;
            donor_reason = "matched " + full;
            break;
        }

        if (donor_trade_post == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
               << "\"ok\":false,"
               << "\"stage\":\"find-registered-donor-tradepost\","
               << "\"message\":\"No donor TradePost with trader markers matched requested actor\","
               << "\"tokens\":";
            append_json_string_array(ss, tokens);
            ss << ",\"lastReason\":\"" << json_escape(donor_reason) << "\"}";
            return {false, ss.str()};
        }

        donor_manager = find_assigned_manager(managers, donor_trade_post);

        if (donor_manager == nullptr)
        {
            std::ostringstream ss;
            ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
               << "\"ok\":false,"
               << "\"stage\":\"find-manager\","
               << "\"donorTradePost\":\"" << json_escape(full_name(donor_trade_post)) << "\","
               << "\"message\":\"Donor TradePost has markers but is not assigned to any live TradeOutpostManager; refusing teleport route\"}";
            return {false, ss.str()};
        }

        if (std::abs(x) < 1.0 && std::abs(y) < 1.0 && std::abs(z) < 1.0)
        {
            std::ostringstream ss;
            ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
               << "\"ok\":false,"
               << "\"stage\":\"resolve-spawn-location\","
               << "\"message\":\"Player/world coordinates are missing; refusing to teleport registered TradePost to origin\","
               << "\"donorTradePost\":\"" << json_escape(full_name(donor_trade_post)) << "\"}";
            return {false, ss.str()};
        }

        auto* donor_actor = static_cast<Unreal::AActor*>(donor_trade_post);
        const auto before_location = donor_actor->K2_GetActorLocation();
        const auto before_rotation = donor_actor->K2_GetActorRotation();

        Unreal::FVector destination{};
        destination.x = static_cast<float>(x + 250.0);
        destination.y = static_cast<float>(y);
        destination.z = static_cast<float>(z + 20.0);
        Unreal::FRotator destination_rotation{};
        destination_rotation.pitch = 0.0f;
        destination_rotation.yaw = static_cast<float>(yaw);
        destination_rotation.roll = 0.0f;

        donor_actor->SetActorHiddenInGame(false);
        donor_actor->SetActorEnableCollision(true);
        const auto teleported = donor_actor->K2_TeleportTo(destination, destination_rotation);
        const auto after_location = donor_actor->K2_GetActorLocation();
        const auto after_rotation = donor_actor->K2_GetActorRotation();

        const auto* markers = array_property(donor_trade_post, L"_traderMarkers");
        const auto* spawned_traders = array_property(donor_trade_post, L"_spawnedTraders");

        std::ostringstream ss;
        ss << "{\"actorClass\":\"" << json_escape(actor_class) << "\","
           << "\"ok\":" << (teleported ? "true" : "false") << ","
           << "\"stage\":\"teleport-registered-tradepost\","
           << "\"route\":\"relocate-existing-registered-tradepost-native\","
           << "\"note\":\"Moves an existing stock TradePost already registered with TradeOutpostManager/economy; this tests the real trader lifecycle path instead of spawning an unregistered NPC ghost\","
           << "\"donorTradePost\":\"" << json_escape(full_name(donor_trade_post)) << "\","
           << "\"donorManager\":\"" << json_escape(full_name(donor_manager)) << "\","
           << "\"beforeLocation\":";
        append_vector_json(ss, before_location);
        ss << ",\"beforeRotation\":";
        append_rotator_json(ss, before_rotation);
        ss << ",\"requestedLocation\":";
        append_vector_json(ss, destination);
        ss << ",\"requestedRotation\":";
        append_rotator_json(ss, destination_rotation);
        ss << ",\"afterLocation\":";
        append_vector_json(ss, after_location);
        ss << ",\"afterRotation\":";
        append_rotator_json(ss, after_rotation);
        ss << ",\"traderMarkersCount\":" << (is_reasonable_array(markers) ? markers->count : -1)
           << ",\"spawnedTradersCount\":" << (is_reasonable_array(spawned_traders) ? spawned_traders->count : -1)
           << ",\"traderMarkerSamples\":";
        append_trader_marker_samples(ss, markers, 3);
        ss << "}";
        return {teleported, ss.str()};
    }
}
