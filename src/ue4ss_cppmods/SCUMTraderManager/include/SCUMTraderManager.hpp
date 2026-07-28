#pragma once

#include "ue4ss_abi_shim.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace RC::SCUMTraderManager
{
    class SCUMTraderManager final : public CppUserModBase
    {
      public:
        SCUMTraderManager();
        ~SCUMTraderManager() override;

        auto on_update() -> void override;
        auto on_unreal_init() -> void override;
        auto on_ui_init() -> void override;
        auto on_program_start() -> void override;
        auto on_cpp_mods_loaded() -> void override;
        auto on_dll_load(StringViewType dll_name) -> void override;
        auto render_tab() -> void override {}
        auto tick_from_lua_host() -> void;
        auto tick_inline_from_lua_host() -> void;

        auto on_lua_start(StringViewType, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, std::vector<LuaMadeSimple::Lua*>&) -> void override {}
        auto on_lua_start(LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, std::vector<LuaMadeSimple::Lua*>&) -> void override {}
        auto on_lua_stop(StringViewType, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, std::vector<LuaMadeSimple::Lua*>&) -> void override {}
        auto on_lua_stop(LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, std::vector<LuaMadeSimple::Lua*>&) -> void override {}
        auto on_lua_start(StringViewType, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua*) -> void override {}
        auto on_lua_start(LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua*) -> void override {}
        auto on_lua_stop(StringViewType, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua*) -> void override {}
        auto on_lua_stop(LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua*) -> void override {}

      private:
        struct SpawnedTradePostMonitor
        {
            Unreal::UObject* trade_post{};
            std::string actor_class{};
            std::string full_name{};
            std::chrono::steady_clock::time_point created_at{};
            int last_spawned_traders{-1};
            int last_spawned_sedentary{-1};
        };

        std::chrono::steady_clock::time_point m_last_poll{};
        std::chrono::steady_clock::time_point m_last_monitor_check{};
        std::chrono::steady_clock::time_point m_next_outpost_pin_attempt{};
        std::chrono::steady_clock::time_point m_next_manager_cache_refresh{};
        std::string m_last_command_id{};
        std::string m_last_inline_command_id{};
        std::vector<SpawnedTradePostMonitor> m_spawned_tradepost_monitors{};
        bool m_unreal_initialized{false};
        bool m_cppmods_loaded{false};
        bool m_outpost_runtime_pinned{false};
        mutable bool m_inline_game_thread_command{false};
        mutable std::filesystem::path m_result_path_override{};
        int m_outpost_pin_attempts{0};
        int m_outpost_pin_object_count{0};
        int m_manager_cache_refresh_attempts{0};
        int m_manager_cache_complete_count{0};
        std::string m_last_outpost_pin_result{};

        auto base_dir() const -> std::filesystem::path;
        auto log_path() const -> std::filesystem::path;
        auto command_path() const -> std::filesystem::path;
        auto inline_command_path() const -> std::filesystem::path;
        auto result_path() const -> std::filesystem::path;
        auto inline_result_path() const -> std::filesystem::path;
        auto append_log(const std::string& line) const -> void;
        auto write_result(const std::string& id, bool ok, const std::string& message, const std::string& data_json = "{}") const -> void;
        auto poll_command_file() -> void;
        auto poll_inline_command_file() -> void;
        auto handle_command(const std::string& text) -> void;
        auto read_only_trader_probe() const -> std::string;
        auto world_edit_server_only_probe() const -> std::string;
        auto scum_mod_manager_probe() const -> std::pair<bool, std::string>;
        auto cfcore_native_status_probe() const -> std::pair<bool, std::string>;
        auto cfcore_editor_settings_probe() const -> std::pair<bool, std::string>;
        auto cfcore_configure_server_runtime(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto cfcore_api_mods_summary_probe(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto cfcore_api_files_summary_probe(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto cfcore_server_mod_sync_probe(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto cfcore_client_file_sync_probe(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto raw_byte_stream_probe(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto armory_runtime_probe() const -> std::string;
        auto armory_trade_session_probe(const std::string& command_text) const -> std::string;
        auto armory_identity_probe(const std::string& command_text) const -> std::string;
        auto armory_deep_probe(const std::string& command_text) const -> std::string;
        auto verify_armory_linked_trader(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto link_existing_summoned_trader(const std::string& command_text) -> std::pair<bool, std::string>;
        auto prepare_armory_anywhere(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto target_armory_interact_native(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto open_armory_tradebuy_native(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto server_armory_tradebuy_native(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto placeable_server_place_native(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto placeable_godmode_fill_native(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto base_loot_first_reflected_property_probe(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto base_loot_store_item_native(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto editor_actor_probe(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto editor_actor_target_probe(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto editor_actor_trace_select(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto editor_actor_select(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto editor_actor_move(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto editor_actor_nudge(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto editor_actor_copy(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto editor_actor_hide(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto server_actor_spawn_persistent(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto spawn_linked_trader_actor(const std::string& command_text) -> std::pair<bool, std::string>;
        auto spawn_functional_tradepost_clone(const std::string& command_text) -> std::pair<bool, std::string>;
        auto relocate_registered_tradepost_for_trader(const std::string& command_text) const -> std::pair<bool, std::string>;
        auto pin_loaded_outpost_runtime(const std::string& reason) -> std::pair<bool, std::string>;
        auto maybe_pin_loaded_outpost_runtime() -> void;
        auto maybe_refresh_manager_cache_fast() -> void;
        auto check_spawned_tradepost_monitors() -> void;
    };
}
