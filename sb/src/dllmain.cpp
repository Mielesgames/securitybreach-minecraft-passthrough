#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>

class SecurityBreachMinecraftPassthrough : public RC::CppUserModBase
{
public:
    SecurityBreachMinecraftPassthrough() : CppUserModBase()
    {
        ModName = STR("SecurityBreachMinecraftPassthrough");
        ModVersion = STR("0.1.0");
        ModDescription = STR("Minecraft passthrough for FNAF: Security Breach");
        ModAuthors = STR("Mielesgames");

        RC::Output::send<RC::LogLevel::Verbose>(
            STR("SecurityBreachMinecraftPassthrough loaded!\n")
        );
    }

    ~SecurityBreachMinecraftPassthrough() override
    {
    }

    bool gregory_found = false;

    auto on_update() -> void override
    {
        static bool update_logged = false;

        if (!update_logged)
        {
            update_logged = true;

            RC::Output::send<RC::LogLevel::Verbose>(
                STR("SBMP on_update is running!\n")
            );
        }

        if (gregory_found)
        {
            return;
        }

        auto gregory = RC::Unreal::UObjectGlobals::FindFirstOf(
            STR("Gregory_C")
        );

        if (gregory != nullptr)
        {
            gregory_found = true;

            RC::Output::send<RC::LogLevel::Verbose>(
                STR("SBMP found Gregory instance: {}\n"),
                gregory->GetFullName()
            );

            return;
        }

        auto first_person_character = RC::Unreal::UObjectGlobals::FindFirstOf(
            STR("FirstPersonCharacter_C")
        );

        if (first_person_character != nullptr)
        {
            gregory_found = true;

            RC::Output::send<RC::LogLevel::Verbose>(
                STR("SBMP found FirstPersonCharacter instance: {}\n"),
                first_person_character->GetFullName()
            );
        }
    }

    auto on_unreal_init() -> void override
    {
    }
};

#define SECURITY_BREACH_MINECRAFT_PASSTHROUGH_API __declspec(dllexport)

extern "C"
{
    SECURITY_BREACH_MINECRAFT_PASSTHROUGH_API RC::CppUserModBase* start_mod()
    {
        return new SecurityBreachMinecraftPassthrough();
    }

    SECURITY_BREACH_MINECRAFT_PASSTHROUGH_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}