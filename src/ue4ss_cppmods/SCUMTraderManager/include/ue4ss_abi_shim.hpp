#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace RC
{
    using CharType = wchar_t;
    using StringType = std::basic_string<CharType>;
    using StringViewType = std::basic_string_view<CharType>;
}

#define STR(value) L##value

namespace RC::GUI
{
    class GUITab;
}

namespace RC::LuaMadeSimple
{
    class Lua;
}

namespace RC
{
    class __declspec(dllimport) CppUserModBase
    {
      protected:
        std::vector<std::shared_ptr<GUI::GUITab>> GUITabs{};

      public:
        StringType ModName{};
        StringType ModVersion{};
        StringType ModDescription{};
        StringType ModAuthors{};
        StringType ModIntendedSDKVersion{};

      public:
        CppUserModBase();
        virtual ~CppUserModBase();

      public:
        virtual auto on_update() -> void {}
        virtual auto on_unreal_init() -> void {}
        virtual auto on_ui_init() -> void {}
        virtual auto on_program_start() -> void {}
        virtual auto on_lua_start(StringViewType, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, std::vector<LuaMadeSimple::Lua*>&) -> void {}
        virtual auto on_lua_start(LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, std::vector<LuaMadeSimple::Lua*>&) -> void {}
        virtual auto on_lua_stop(StringViewType, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, std::vector<LuaMadeSimple::Lua*>&) -> void {}
        virtual auto on_lua_stop(LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, std::vector<LuaMadeSimple::Lua*>&) -> void {}
        virtual auto on_dll_load(StringViewType) -> void {}
        virtual auto render_tab() -> void {}
        virtual auto on_lua_start(StringViewType, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua*) -> void {}
        virtual auto on_lua_start(LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua*) -> void {}
        virtual auto on_lua_stop(StringViewType, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua*) -> void {}
        virtual auto on_lua_stop(LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua*) -> void {}
        virtual auto on_cpp_mods_loaded() -> void {}
    };
}

namespace RC::Unreal
{
    struct FVector
    {
        float x{};
        float y{};
        float z{};
    };

    struct FRotator
    {
        float pitch{};
        float yaw{};
        float roll{};
    };

    struct FQuat
    {
        float x{};
        float y{};
        float z{};
        float w{1.0f};
    };

    struct alignas(16) FTransform
    {
        FQuat rotation{};
        FVector translation{};
        float translation_padding{};
        FVector scale3d{1.0f, 1.0f, 1.0f};
        float scale3d_padding{};
    };

    class AActor;
    class UClass;
    class UObject;
    class UWorld;

    struct FWeakObjectPtr
    {
        std::int32_t object_index{};
        std::int32_t object_serial_number{};

        __declspec(dllimport) auto operator=(UObject* object) -> void;
        __declspec(dllimport) auto Get() const -> UObject*;
    };

    class UObjectBase
    {
      public:
        __declspec(dllimport) auto GetClassPrivate() -> UClass*&;
    };

    class UObject : public UObjectBase
    {
      public:
        __declspec(dllimport) auto GetFullName(UObject* stop_outer = nullptr) const -> std::wstring;
        __declspec(dllimport) auto GetValuePtrByPropertyNameInChain(const wchar_t* property_name) -> void*;
        __declspec(dllimport) auto GetWorld() const -> UWorld*;
    };

    class AActor : public UObject
    {
      public:
        __declspec(dllimport) auto K2_GetActorLocation() -> FVector;
        __declspec(dllimport) auto K2_GetActorRotation() -> FRotator;
        __declspec(dllimport) auto K2_TeleportTo(FVector destination_location, FRotator destination_rotation) -> bool;
        __declspec(dllimport) auto SetActorEnableCollision(bool new_actor_enable_collision) -> void;
        __declspec(dllimport) auto SetActorHiddenInGame(bool new_hidden) -> void;
    };

    class UClass : public UObject
    {
    };

    class UWorld : public UObject
    {
      public:
        __declspec(dllimport) auto SpawnActor(UClass* actor_class, const FTransform* transform) -> AActor*;
    };

    namespace UObjectGlobals
    {
        __declspec(dllimport) auto FindFirstOf(const wchar_t* class_name) -> UObject*;
        __declspec(dllimport) auto FindAllOf(const wchar_t* class_name, std::vector<UObject*>& objects) -> void;
        __declspec(dllimport) auto FindObject(const wchar_t* class_name, const wchar_t* object_name, int required_flags, int banned_flags) -> UObject*;
        __declspec(dllimport) auto FindObjects(const wchar_t* class_name, const wchar_t* object_name, std::vector<UObject*>& objects, int required_flags, int banned_flags, bool exact_class) -> void;
    }

    class FMemory
    {
      public:
        __declspec(dllimport) static auto Realloc(void* original, std::size_t count, std::uint32_t alignment) -> void*;
    };
}
