#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/AActor.hpp>

#include <UE4SS_SDK/Script/CoreUObject/Rotator.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Link.h"

#include <array>
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

        // register_keydown_event(
        //     RC::Input::Key::F6,
        //     [this]()
        //     {
        //         minecraft_mode = !minecraft_mode;

        //         if (minecraft_mode)
        //         {
        //             ++teleport_seq;
        //         }

        //         RC::Output::send<RC::LogLevel::Verbose>(
        //             STR(
        //                 "SBMP Minecraft mode: {}\n"
        //             ),
        //             minecraft_mode ? STR("ON") : STR("OFF")
        //         );
        //     }
        // );

        RC::Output::send<RC::LogLevel::Verbose>(
            STR("SecurityBreachMinecraftPassthrough loaded!\n")
        );
    }

    struct MinecraftKey
    {
        int virtual_key;
        std::uint16_t scancode;
        bool was_down;
    };

    bool gregory_found = false;
    bool minecraft_mode = false;
    bool minecraft_launch_started = false;
    bool f6_was_down = false;
    HANDLE minecraft_job = nullptr;
    std::uint32_t teleport_seq = 1;
    std::uint64_t last_mc_frame = 0;

    std::array<MinecraftKey, 8> minecraft_keys{
        MinecraftKey{ 'W', 26, false },
        MinecraftKey{ 'A', 4, false },
        MinecraftKey{ 'S', 22, false },
        MinecraftKey{ 'D', 7, false },
        MinecraftKey{ VK_SPACE, 44, false },
        MinecraftKey{ VK_LSHIFT, 225, false },
        MinecraftKey{ VK_LCONTROL, 224, false },
        MinecraftKey{ 'E', 8, false }
    };

    auto sync_minecraft_keyboard() -> void
    {
        if (!minecraft_mode)
        {
            for (auto& key : minecraft_keys)
            {
                if (key.was_down)
                {
                    g_link.push_input(
                        skycraft::proto::kInKey,
                        key.scancode,
                        0
                    );

                    RC::Output::send<RC::LogLevel::Verbose>(
                        STR(
                            "SBMP input: scancode={}, state=UP\n"
                        ),
                        key.scancode
                    );

                    key.was_down = false;
                }
            }

            g_link.push_input(
                skycraft::proto::kInReleaseAll,
                0
            );

            return;
        }

        for (auto& key : minecraft_keys)
        {
            const bool down =
                (::GetAsyncKeyState(key.virtual_key) & 0x8000) != 0;

            if (down == key.was_down)
            {
                continue;
            }

            g_link.push_input(
                skycraft::proto::kInKey,
                key.scancode,
                down ? 1 : 0
            );

            RC::Output::send<RC::LogLevel::Verbose>(
                STR(
                    "SBMP input: VK={}, scancode={}, state={}\n"
                ),
                key.virtual_key,
                key.scancode,
                down ? STR("DOWN") : STR("UP")
            );

            key.was_down = down;
        }
    }

    ~SecurityBreachMinecraftPassthrough() override
    {
        if (minecraft_job != nullptr)
        {
            ::CloseHandle(minecraft_job);
            minecraft_job = nullptr;
        }
    }

    auto start_minecraft() -> void
    {
        if (minecraft_launch_started)
        {
            return;
        }

        if (minecraft_job == nullptr)
        {
            minecraft_job = ::CreateJobObjectW(
                nullptr,
                nullptr
            );

            if (minecraft_job == nullptr)
            {
                RC::Output::send<RC::LogLevel::Error>(
                    STR(
                        "SBMP failed to create Minecraft job. Windows error: {}\n"
                    ),
                    ::GetLastError()
                );

                return;
            }

            JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info{};

            job_info.BasicLimitInformation.LimitFlags =
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

            if (!::SetInformationJobObject(
                minecraft_job,
                JobObjectExtendedLimitInformation,
                &job_info,
                sizeof(job_info)
            ))
            {
                RC::Output::send<RC::LogLevel::Error>(
                    STR(
                        "SBMP failed to configure Minecraft job. Windows error: {}\n"
                    ),
                    ::GetLastError()
                );

                ::CloseHandle(minecraft_job);
                minecraft_job = nullptr;

                return;
            }
        }

        wchar_t command_line[] =
            L"\"C:\\Windows\\System32\\cmd.exe\" /C call "
            L"\"D:\\Github\\miside-minecraft-passthrough\\fabric\\gradlew.bat\" "
            L"runClient";

        STARTUPINFOW startup_info{};
        startup_info.cb = sizeof(startup_info);

        PROCESS_INFORMATION process_info{};

        const BOOL started = ::CreateProcessW(
            L"C:\\Windows\\System32\\cmd.exe",
            command_line,
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED,
            nullptr,
            L"D:\\Github\\miside-minecraft-passthrough\\fabric",
            &startup_info,
            &process_info
        );

        if (!started)
        {
            RC::Output::send<RC::LogLevel::Error>(
                STR(
                    "SBMP failed to start Minecraft. Windows error: {}\n"
                ),
                ::GetLastError()
            );

            return;
        }

        if (!::AssignProcessToJobObject(
            minecraft_job,
            process_info.hProcess
        ))
        {
            const DWORD error = ::GetLastError();

            RC::Output::send<RC::LogLevel::Error>(
                STR(
                    "SBMP failed to assign Minecraft to job. Windows error: {}\n"
                ),
                error
            );

            ::TerminateProcess(
                process_info.hProcess,
                1
            );

            ::CloseHandle(process_info.hThread);
            ::CloseHandle(process_info.hProcess);

            return;
        }

        minecraft_launch_started = true;

        ::ResumeThread(process_info.hThread);

        ::CloseHandle(process_info.hThread);
        ::CloseHandle(process_info.hProcess);

        RC::Output::send<RC::LogLevel::Verbose>(
            STR("SBMP started Minecraft automatically.\n")
        );
    }

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

        if (!g_link.valid())
        {
            return;
        }

        start_minecraft();

        const bool f6_down =
            (::GetAsyncKeyState(VK_F6) & 0x8000) != 0;

        if (f6_down && !f6_was_down)
        {
            minecraft_mode = !minecraft_mode;

            if (minecraft_mode)
            {
                ++teleport_seq;
            }

            RC::Output::send<RC::LogLevel::Verbose>(
                STR(
                    "SBMP Minecraft mode: {}\n"
                ),
                minecraft_mode ? STR("ON") : STR("OFF")
            );
        }

        f6_was_down = f6_down;
        sync_minecraft_keyboard();

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
            teleport_seq,
            1920,
            1080,
            12.0f
        );

        static int mc_log_counter = 0;

        skycraft::proto::McState mc_state{};

        if (g_link.read_mc_state(mc_state))
        {
            if (
                minecraft_mode &&
                (mc_state.flags & skycraft::proto::kMcInWorld) != 0 &&
                mc_state.frameCounter != last_mc_frame
            )
            {
                last_mc_frame = mc_state.frameCounter;

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

                // float sb_yaw = 90.0f - mc_state.yaw;
                // float sb_pitch = -mc_state.pitch;

                // while (sb_yaw > 180.0f)
                // {
                //     sb_yaw -= 360.0f;
                // }

                // while (sb_yaw < -180.0f)
                // {
                //     sb_yaw += 360.0f;
                // }

                // while (sb_pitch > 180.0f)
                // {
                //     sb_pitch -= 360.0f;
                // }

                // while (sb_pitch < -180.0f)
                // {
                //     sb_pitch += 360.0f;
                // }

                // control_rotation->Yaw = sb_yaw;
                // control_rotation->Pitch = sb_pitch;
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