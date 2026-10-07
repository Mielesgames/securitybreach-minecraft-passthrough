#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/AActor.hpp>

#include <UE4SS_SDK/Script/CoreUObject/Rotator.hpp>

#include "Link.h"

#include <cstdint>
#include <vector>

static SecurityBreachLink g_link;

static double sb_to_mc_x(double x)
{
    return x / skycraft::proto::kUnitsPerBlock;
}

static double sb_to_mc_y(double z)
{
    return z / skycraft::proto::kUnitsPerBlock;
}

static double sb_to_mc_z(double y)
{
    return -y / skycraft::proto::kUnitsPerBlock;
}

static float sb_to_mc_yaw(float yaw)
{
    float result = 90.0f - yaw;

    while (result > 180.0f)
    {
        result -= 360.0f;
    }

    while (result < -180.0f)
    {
        result += 360.0f;
    }

    return result;
}

static float sb_to_mc_pitch(float pitch)
{
    float result = -pitch;

    while (result > 180.0f)
    {
        result -= 360.0f;
    }

    while (result < -180.0f)
    {
        result += 360.0f;
    }

    return result;
}

static double mc_to_sb_x(double x)
{
    return x * skycraft::proto::kUnitsPerBlock;
}

static double mc_to_sb_y(double y)
{
    return y * skycraft::proto::kUnitsPerBlock;
}

static double mc_to_sb_z(double z)
{
    return -z * skycraft::proto::kUnitsPerBlock;
}

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

        if (!g_link.valid() && !g_link.create())
        {
            return;
        }

        std::vector<RC::Unreal::UObject*> controllers;

        RC::Unreal::UObjectGlobals::FindAllOf(
            STR("MainGamePC_C"),
            controllers
        );

        if (controllers.empty())
        {
            return;
        }

        static int controller_log_counter = 0;

        if (controller_log_counter++ % 60 == 0)
        {
            RC::Output::send<RC::LogLevel::Verbose>(
                STR("SBMP MainGamePC_C count: {}\n"),
                controllers.size()
            );
        }

        auto* controller = controllers[0];

        if (controller == nullptr)
        {
            return;
        }

        if (controller_log_counter % 60 == 0)
        {
            RC::Output::send<RC::LogLevel::Verbose>(
                STR("SBMP MainGamePC_C: {}\n"),
                controller->GetFullName()
            );
        }

        const std::uintptr_t controller_address =
            reinterpret_cast<std::uintptr_t>(controller);

        auto* first_person_character =
            *reinterpret_cast<RC::Unreal::UObject**>(
                controller_address + static_cast<std::uintptr_t>(0x6F0)
            );

        if (first_person_character == nullptr)
        {
            return;
        }

        if (controller_log_counter % 60 == 0)
        {
            RC::Output::send<RC::LogLevel::Verbose>(
                STR("SBMP FirstPersonCharacter: {}\n"),
                first_person_character->GetFullName()
            );
        }

        auto* actor =
            reinterpret_cast<RC::Unreal::AActor*>(
                first_person_character
            );

        auto location = actor->K2_GetActorLocation();

        if (controller_log_counter % 60 == 0)
        {
            RC::Output::send<RC::LogLevel::Verbose>(
                STR(
                    "SBMP FirstPersonCharacter location: X={}, Y={}, Z={}\n"
                ),
                location.X(),
                location.Y(),
                location.Z()
            );
        }

        auto* control_rotation =
            reinterpret_cast<WSDK::FRotator*>(
                controller_address + static_cast<std::uintptr_t>(0x290)
            );

        float pitch = control_rotation->Pitch;

        if (pitch > 180.0f)
        {
            pitch -= 360.0f;
        }

        if (controller_log_counter % 60 == 0)
        {
            RC::Output::send<RC::LogLevel::Verbose>(
                STR(
                    "SBMP view: Pitch={}, Yaw={}, Roll={}\n"
                ),
                pitch,
                control_rotation->Yaw,
                control_rotation->Roll
            );
        }

        if (!gregory_found)
        {
            auto gregory = RC::Unreal::UObjectGlobals::FindFirstOf(
                STR("Gregory_C")
            );

            if (gregory != nullptr)
            {
                gregory_found = true;

                RC::Output::send<RC::LogLevel::Verbose>(
                    STR("SBMP Gregory found: {}\n"),
                    gregory->GetFullName()
                );

                if (gregory == first_person_character)
                {
                    RC::Output::send<RC::LogLevel::Verbose>(
                        STR("SBMP FirstPersonCharacter IS Gregory!\n")
                    );
                }
            }
        }

        const double mc_x = sb_to_mc_x(location.X());
        const double mc_y = sb_to_mc_y(location.Z());
        const double mc_z = sb_to_mc_z(location.Y());

        const float mc_yaw =
            sb_to_mc_yaw(control_rotation->Yaw);

        const float mc_pitch =
            sb_to_mc_pitch(pitch);

        g_link.write_sky_state(
            skycraft::proto::kSkyInGame,
            1,
            1,
            mc_x,
            mc_y,
            mc_z,
            mc_yaw,
            mc_pitch,
            1,
            1920,
            1080,
            12.0f
        );

        static int mc_log_counter = 0;

        skycraft::proto::McState mc_state{};

        if (g_link.read_mc_state(mc_state))
        {
            if ((mc_state.flags & skycraft::proto::kMcInWorld) != 0)
            {
                const double sb_x = mc_to_sb_x(mc_state.x);
                const double sb_y = mc_to_sb_y(mc_state.y);
                const double sb_z = mc_to_sb_z(mc_state.z);

                RC::Unreal::FVector new_location{
                    static_cast<float>(sb_x),
                    static_cast<float>(sb_z),
                    static_cast<float>(sb_y)
                };

                RC::Unreal::FHitResult sweep_hit{};

                actor->K2_SetActorLocation(
                    new_location,
                    false,
                    sweep_hit,
                    true
                );
            }

            if (mc_log_counter++ % 60 == 0)
            {
                RC::Output::send<RC::LogLevel::Verbose>(
                    STR(
                        "SBMP MC state: X={}, Y={}, Z={}, Yaw={}, Pitch={}, Flags={}, Frame={}\n"
                    ),
                    mc_state.x,
                    mc_state.y,
                    mc_state.z,
                    mc_state.yaw,
                    mc_state.pitch,
                    mc_state.flags,
                    mc_state.frameCounter
                );
            }
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

    SECURITY_BREACH_MINECRAFT_PASSTHROUGH_API void uninstall_mod(
        RC::CppUserModBase* mod
    )
    {
        delete mod;
    }
}