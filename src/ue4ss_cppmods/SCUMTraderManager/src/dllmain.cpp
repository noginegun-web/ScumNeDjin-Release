#include "SCUMTraderManager.hpp"

#include <memory>

namespace
{
    auto lua_host_instance() -> RC::SCUMTraderManager::SCUMTraderManager&
    {
        static std::unique_ptr<RC::SCUMTraderManager::SCUMTraderManager> lua_host_mod{};
        if (!lua_host_mod)
        {
            lua_host_mod = std::make_unique<RC::SCUMTraderManager::SCUMTraderManager>();
        }
        return *lua_host_mod;
    }
}

extern "C"
{
    __declspec(dllexport) RC::CppUserModBase* start_mod()
    {
        return new RC::SCUMTraderManager::SCUMTraderManager();
    }

    __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }

    __declspec(dllexport) int scum_trader_native_tick(void*)
    {
        lua_host_instance().tick_from_lua_host();
        return 0;
    }

    __declspec(dllexport) int scum_trader_native_tick_inline(void*)
    {
        lua_host_instance().tick_inline_from_lua_host();
        return 0;
    }
}
