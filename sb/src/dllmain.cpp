#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/FHitResult.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/Hooks/Hooks.hpp>

#include <UE4SS_SDK/Script/CoreUObject/Rotator.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Actors.h"
#include "Collision.h"
#include "Input.h"
#include "Link.h"
#include "Overlay.h"
#include "World.h"

#include <algorithm>
#include <array>
#include <exception>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <string>
#include <vector>

static SecurityBreachLink g_link;

// Sign of Unreal Y when converting to Minecraft Z.
//  +1.0: geometrically correct (Unreal is left-handed, Minecraft right-handed),
//        no mirror image. This is what you want when Minecraft blocks get
//        drawn inside Security Breach later.
//  -1.0: the original SkyCraft (Skyrim) convention. Gives a mirror image here.
// Yaw below is derived from this sign so position and direction always agree.
static constexpr double kZSign = 1.0;

// All in Unreal units (70 per Minecraft block).
// Minecraft takes over once it stands this close to Gregory after a sync.
static constexpr double kEngageDistance = 35.0;
// A bigger gap than this in one frame is not walking: sync again instead.
static constexpr double kMaxStep = 210.0;
// Minecraft's player in the air (not flying) for this long has fallen through
// geometry Minecraft didn't know yet: put both back where it last stood.
static constexpr ULONGLONG kMaxFallMs = 3000;
// How far Steve reaches: to use things (4.5 blocks) and to hit (3 blocks).
static constexpr double kUseReach = 315.0;
static constexpr double kAttackReach = 210.0;
// /freddy puts him this far in front of the player, and this much higher.
static constexpr double kSummonDistance = 160.0;
static constexpr double kSummonLift = 70.0;
// Half the width of the wider look for something to use.
static constexpr double kUseThickness = 12.0;
// Half the width of the swing that looks for someone to hit.
static constexpr double kAttackThickness = 14.0;
// What a hit does to a character, in Unreal units per second.
static constexpr float kKnockbackSpeed = 1100.0f;
static constexpr float kKnockbackLift = 420.0f;
// Hits it takes to break a cutout in survival, and the pause after which the count starts over.
static constexpr int kBreakHits = 5;
static constexpr int kDoorBreakHits = 12;
static constexpr ULONGLONG kBreakPauseMs = 3000;
// How long a bot that can't be launched keeps sliding.
static constexpr ULONGLONG kShoveMs = 300;

// A hit that Minecraft reports for a character the swing above already hit is skipped for this long.
static constexpr ULONGLONG kSameHitMs = 400;

// EMovementMode.
static constexpr std::uint8_t kMoveFalling = 3;
static constexpr std::uint8_t kMoveFlying = 5;
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
    return kZSign * y / skycraft::proto::kUnitsPerBlock;
}

static float sb_to_mc_yaw(float yaw)
{
    // UE yaw 0 = +X = Minecraft east (-90). Turning right in Unreal (+yaw)
    // goes towards +Y, which maps to Minecraft Z with sign kZSign.
    float result = static_cast<float>(kZSign) * yaw - 90.0f;

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
    return kZSign * z * skycraft::proto::kUnitsPerBlock;
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

    bool gregory_found = false;
    bool no_character_logged = false;
    // We set Gregory's crouch last, so it has to be released when Minecraft mode ends.
    bool crouch_managed = false;
    // The game's pause menu is up (as of the last tick).
    bool game_paused = false;
    // Gregory's movement component is in flying mode because Minecraft drives.
    bool flying_applied = false;
    // The cutout being broken, and how far along.
    RC::Unreal::FWeakObjectPtr breaking;
    int breaking_hits = 0;
    ULONGLONG breaking_tick = 0;
    // Minecraft's player is in creative mode (as of the last state read).
    bool creative_mode = false;
    bool holds_sword = false;
    // The bot that is sliding away from a hit.
    RC::Unreal::FWeakObjectPtr shoved;
    double shove_direction[2]{};
    ULONGLONG shove_until = 0;
    ULONGLONG shove_tick = 0;
    // Who the mod's own swing last hit, so Minecraft's report of the same hit isn't a second one.
    RC::Unreal::UObject* swing_target = nullptr;
    ULONGLONG swing_tick = 0;
    // Third person (F5): the camera is pulled back, and has to be put back afterwards.
    bool third_person = false;
    bool third_person_failed = false;
    // Gregory is not drawn while the player is Steve.
    RC::Unreal::UObject* hidden_character = nullptr;
    // The game's camera this tick.
    world::Camera view;
    // How far the camera sits above the middle of Gregory's capsule (smoothed),
    // and how far Gregory is shifted to put it at Steve's eye height.
    double eye_above_actor = 0.0;
    bool have_eye_above_actor = false;
    double eye_shift = 0.0;
    // Where Minecraft's player last stood on something (Unreal units), and when.
    double safe_spot[3]{};
    bool have_safe_spot = false;
    ULONGLONG supported_tick = 0;
    // Only compared, never dereferenced.
    RC::Unreal::UObject* last_character = nullptr;
    bool minecraft_mode = false;
    bool minecraft_launch_started = false;
    bool f6_was_down = false;
    HANDLE minecraft_job = nullptr;
    std::uint32_t teleport_seq = 1;
    std::uint64_t last_mc_frame = 0;
    RC::Unreal::UObject* move_input_controller = nullptr;
    bool move_input_ignored = false;
    RC::Unreal::FWeakObjectPtr controller_ref;
    int controller_search_cooldown = 0;
    RC::Unreal::Hook::GlobalCallbackId tick_callback = RC::Unreal::Hook::ERROR_ID;
    // True while Minecraft's position is applied to Gregory. False right
    // after a sync request, until Minecraft has arrived on Gregory.
    bool driving = false;
    ULONGLONG sync_started = 0;

    // UGameplayStatics::IsGamePaused.
    static auto is_game_paused(RC::Unreal::UObject* world_context) -> bool
    {
        static auto* statics = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UObject*>(
            nullptr,
            nullptr,
            STR("/Script/Engine.Default__GameplayStatics")
        );

        static auto* function = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(
            nullptr,
            nullptr,
            STR("/Script/Engine.GameplayStatics:IsGamePaused")
        );

        if (statics == nullptr || function == nullptr)
        {
            return false;
        }

        struct
        {
            RC::Unreal::UObject* WorldContextObject;
            bool ReturnValue;
        } params{ world_context, false };

        statics->ProcessEvent(function, &params);

        return params.ReturnValue;
    }

    // Calls a function without parameters that returns T.
    template <class T>
    static auto call_getter(RC::Unreal::UObject* object, const wchar_t* name, T& out) -> bool
    {
        auto* function = object->GetFunctionByNameInChain(name);

        if (function == nullptr)
        {
            return false;
        }

        alignas(16) std::uint8_t result[64]{};

        object->ProcessEvent(function, result);
        std::memcpy(&out, result, sizeof(T));

        return true;
    }

    // Tells the block renderer where the game's camera is this frame.
    auto update_camera(RC::Unreal::UObject* controller) -> void
    {
        auto* manager = controller->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(
            STR("PlayerCameraManager")
        );

        if (manager == nullptr || *manager == nullptr)
        {
            return;
        }

        float location[3]{};
        float rotation[3]{};  // pitch, yaw, roll in degrees
        float fov = 0.0f;     // horizontal, degrees

        if (
            !call_getter(*manager, STR("GetCameraLocation"), location) ||
            !call_getter(*manager, STR("GetCameraRotation"), rotation) ||
            !call_getter(*manager, STR("GetFOVAngle"), fov) ||
            fov <= 1.0f ||
            fov >= 179.0f
        )
        {
            return;
        }

        constexpr double kDegrees = 3.14159265358979323846 / 180.0;

        const double sp = std::sin(rotation[0] * kDegrees);
        const double cp = std::cos(rotation[0] * kDegrees);
        const double sy = std::sin(rotation[1] * kDegrees);
        const double cy = std::cos(rotation[1] * kDegrees);
        const double sr = std::sin(rotation[2] * kDegrees);
        const double cr = std::cos(rotation[2] * kDegrees);

        world::Camera camera;

        camera.valid = true;

        for (int i = 0; i < 3; ++i)
        {
            camera.position[i] = location[i];
        }

        // The axes of Unreal's rotation matrix.
        camera.forward[0] = static_cast<float>(cp * cy);
        camera.forward[1] = static_cast<float>(cp * sy);
        camera.forward[2] = static_cast<float>(sp);

        camera.right[0] = static_cast<float>(sr * sp * cy - cr * sy);
        camera.right[1] = static_cast<float>(sr * sp * sy + cr * cy);
        camera.right[2] = static_cast<float>(-sr * cp);

        camera.up[0] = static_cast<float>(-(cr * sp * cy + sr * sy));
        camera.up[1] = static_cast<float>(cy * sr - cr * sp * sy);
        camera.up[2] = static_cast<float>(cr * cp);

        camera.tan_half_fov = static_cast<float>(std::tan(fov * 0.5 * kDegrees));

        world::set_camera(camera);
        view = camera;
    }

    // Half the height of the character's collision capsule, 0 if it has none.
    static auto capsule_half_height(RC::Unreal::UObject* character) -> double
    {
        auto* capsule = character->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(
            STR("CapsuleComponent")
        );

        if (capsule == nullptr || *capsule == nullptr)
        {
            return 0.0;
        }

        auto* half_height = (*capsule)->GetValuePtrByPropertyNameInChain<float>(
            STR("CapsuleHalfHeight")
        );

        return half_height != nullptr ? *half_height : 0.0;
    }

    // UCharacterMovementComponent::SetMovementMode on the character's
    // movement component. Does nothing for a pawn without one.
    static auto set_movement_mode(RC::Unreal::UObject* character, std::uint8_t mode) -> void
    {
        auto* movement = character->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(
            STR("CharacterMovement")
        );

        if (movement == nullptr || *movement == nullptr)
        {
            return;
        }

        auto* function = (*movement)->GetFunctionByNameInChain(STR("SetMovementMode"));

        if (function == nullptr)
        {
            return;
        }

        struct
        {
            std::uint8_t NewMovementMode;
            std::uint8_t NewCustomMode;
        } params{ mode, 0 };

        (*movement)->ProcessEvent(function, &params);
    }

    // ACharacter::Crouch / UnCrouch. Does nothing for a pawn that isn't a
    // character.
    static auto set_crouch(RC::Unreal::UObject* character, bool crouch) -> void
    {
        auto* function = character->GetFunctionByNameInChain(
            crouch ? STR("Crouch") : STR("UnCrouch")
        );

        if (function == nullptr)
        {
            return;
        }

        struct
        {
            bool bClientSimulation;
        } params{ false };

        character->ProcessEvent(function, &params);
    }

    // Stops the game's own WASD from moving Gregory while Minecraft drives
    // him. Otherwise Gregory gets moved twice (game input + Minecraft).
    auto set_ignore_move_input(RC::Unreal::UObject* controller, bool ignore) -> void
    {
        auto* function =
            RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(
                nullptr,
                nullptr,
                STR("/Script/Engine.Controller:SetIgnoreMoveInput")
            );

        if (function == nullptr)
        {
            RC::Output::send<RC::LogLevel::Error>(
                STR("SBMP could not find Controller:SetIgnoreMoveInput\n")
            );
            return;
        }

        struct
        {
            bool bNewMoveInput;
        } params{ ignore };

        controller->ProcessEvent(function, &params);

        RC::Output::send<RC::LogLevel::Default>(
            STR("SBMP game move input ignored: {}\n"),
            ignore ? STR("YES") : STR("NO")
        );
    }

    ~SecurityBreachMinecraftPassthrough() override
    {
        if (tick_callback != RC::Unreal::Hook::ERROR_ID)
        {
            RC::Unreal::Hook::UnregisterCallback(tick_callback);
        }

        input::uninstall();
        overlay::uninstall();

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

        // Set the environment variable SBMP_FABRIC_DIR to the fabric folder,
        // so renaming the repo folder doesn't break this.
        std::wstring fabric_dir =
            L"D:\\Github\\miside-minecraft-passthrough\\fabric";

        // Tells the Minecraft side there is no Skyrim collision feed.
        ::SetEnvironmentVariableW(L"SKYCRAFT_PASSTHROUGH", L"1");

        wchar_t env_buffer[MAX_PATH * 2]{};

        const DWORD env_len = ::GetEnvironmentVariableW(
            L"SBMP_FABRIC_DIR",
            env_buffer,
            static_cast<DWORD>(std::size(env_buffer))
        );

        if (env_len > 0 && env_len < std::size(env_buffer))
        {
            fabric_dir.assign(env_buffer, env_len);
        }

        std::wstring command_line =
            L"\"C:\\Windows\\System32\\cmd.exe\" /C call \"" +
            fabric_dir +
            L"\\gradlew.bat\" runClient";

        STARTUPINFOW startup_info{};
        startup_info.cb = sizeof(startup_info);

        PROCESS_INFORMATION process_info{};

        const BOOL started = ::CreateProcessW(
            L"C:\\Windows\\System32\\cmd.exe",
            command_line.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED,
            nullptr,
            fabric_dir.c_str(),
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

    // Everything below touches Unreal objects, so it runs on the game thread
    // (engine tick), not in on_update. on_update is called from UE4SS's own
    // thread; moving Gregory from there runs his overlap events (Blueprint) on
    // the wrong thread and crashed the mod thread on the first move.
    auto on_unreal_init() -> void override
    {
        tick_callback = RC::Unreal::Hook::RegisterEngineTickPreCallback(
            [this](auto&, RC::Unreal::UEngine*, float, bool)
            {
                on_game_tick();
            },
            { false, true, STR("SecurityBreachMinecraftPassthrough"), STR("GameTick") }
        );

        if (tick_callback == RC::Unreal::Hook::ERROR_ID)
        {
            RC::Output::send<RC::LogLevel::Error>(
                STR("SBMP could not hook the engine tick, the mod will do nothing\n")
            );
        }

        overlay::install(g_link);
    }

    auto on_game_tick() -> void
    {
        try
        {
            update_impl();
        }
        catch (const std::exception& e)
        {
            static int errors = 0;

            if (errors++ < 10)
            {
                const std::string what = e.what();
                const std::wstring what_w(what.begin(), what.end());

                RC::Output::send<RC::LogLevel::Error>(
                    STR("SBMP exception in game tick: {}\n"),
                    what_w
                );
            }
        }
        catch (...)
        {
            static int errors = 0;

            if (errors++ < 10)
            {
                RC::Output::send<RC::LogLevel::Error>(
                    STR("SBMP unknown exception in game tick\n")
                );
            }
        }
    }

    // Failsafe state: if Minecraft stops rendering frames, give control back.
    std::uint64_t watch_frame = 0;
    ULONGLONG watch_tick = 0;

    // Asks Minecraft to jump to Gregory. Gregory is not moved again until
    // Minecraft has confirmed it and is actually standing on him.
    auto request_sync(const wchar_t* reason) -> void
    {
        ++teleport_seq;
        driving = false;
        sync_started = ::GetTickCount64();

        RC::Output::send<RC::LogLevel::Default>(
            STR("SBMP syncing Minecraft to Gregory (seq {}): {}\n"),
            teleport_seq,
            reason
        );
    }

    // The player controller, looked up at most twice a second and otherwise
    // reused through a weak pointer (searching all objects every frame on the
    // game thread costs framerate, a raw pointer dangles after a level change).
    auto find_controller() -> RC::Unreal::UObject*
    {
        if (auto* cached = controller_ref.Get())
        {
            return cached;
        }

        if (controller_search_cooldown > 0)
        {
            --controller_search_cooldown;
            return nullptr;
        }

        controller_search_cooldown = 30;

        std::vector<RC::Unreal::UObject*> controllers;

        RC::Unreal::UObjectGlobals::FindAllOf(
            STR("MainGamePC_C"),
            controllers
        );

        if (controllers.empty() || controllers[0] == nullptr)
        {
            return nullptr;
        }

        controller_ref = controllers[0];

        RC::Output::send<RC::LogLevel::Verbose>(
            STR("SBMP MainGamePC_C: {}\n"),
            controllers[0]->GetFullName()
        );

        return controllers[0];
    }

    auto update_impl() -> void
    {
        static bool update_logged = false;

        if (!update_logged)
        {
            update_logged = true;

            RC::Output::send<RC::LogLevel::Verbose>(
                STR("SBMP game tick is running!\n")
            );
        }

        if (!g_link.valid() && !g_link.create())
        {
            return;
        }

        start_minecraft();

        // Minecraft only opens its world once it sees this, and drops the
        // link when it stops. Keep it going while there is no Gregory yet.
        g_link.heartbeat();
        if (!minecraft_mode)
        {
            g_link.discard_unused_rings();
        }

        skycraft::proto::McState mc_state{};
        const bool have_mc_state = g_link.read_mc_state(mc_state);
        const ULONGLONG now = ::GetTickCount64();

        const bool f6_down =
            (::GetAsyncKeyState(VK_F6) & 0x8000) != 0;

        if (f6_down && !f6_was_down)
        {
            const bool minecraft_ready =
                have_mc_state &&
                (mc_state.flags & skycraft::proto::kMcInWorld) != 0;

            if (!minecraft_mode && !minecraft_ready)
            {
                // Minecraft takes a while to start and open its world.
                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP Minecraft is not in its world yet, F6 ignored\n")
                );
            }
            else
            {
                minecraft_mode = !minecraft_mode;

                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP Minecraft mode: {}\n"),
                    minecraft_mode ? STR("ON") : STR("OFF")
                );

                if (minecraft_mode)
                {
                    watch_tick = now;
                    request_sync(STR("F6"));
                }
            }
        }

        f6_was_down = f6_down;

        // Emergency off.
        if (minecraft_mode && (::GetAsyncKeyState(VK_F7) & 0x8000) != 0)
        {
            minecraft_mode = false;

            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP Minecraft mode: OFF (F7)\n")
            );
        }

        // Watchdog: Minecraft that doesn't produce frames can't drive Gregory.
        if (have_mc_state)
        {
            if (mc_state.frameCounter != watch_frame)
            {
                // Gregory stands still for as long as Minecraft does.
                if (driving && now - watch_tick > 150)
                {
                    RC::Output::send<RC::LogLevel::Default>(
                        STR("SBMP Minecraft hitch: no frame for {} ms\n"),
                        now - watch_tick
                    );
                }

                watch_frame = mc_state.frameCounter;
                watch_tick = now;
            }
            else if (minecraft_mode && now - watch_tick > 3000)
            {
                minecraft_mode = false;

                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP Minecraft stopped responding, mode OFF\n")
                );
            }
        }

        if (!minecraft_mode)
        {
            driving = false;
        }

        // Minecraft's hand and HUD are only drawn while Minecraft mode is on:
        // without it this is plain Security Breach.
        {
            const bool in_world =
                have_mc_state &&
                (mc_state.flags & skycraft::proto::kMcInWorld) != 0;
            const bool screen_open =
                (mc_state.flags & skycraft::proto::kMcScreenOpen) != 0;

            auto& shown = overlay::state();

            shown.show.store(minecraft_mode && in_world && !game_paused, std::memory_order_relaxed);
            shown.crosshair.store(
                minecraft_mode && in_world && !screen_open && mc_state.cameraMode == 0,
                std::memory_order_relaxed
            );
            shown.gui_scale.store(static_cast<int>(mc_state.guiScale), std::memory_order_relaxed);
        }

        auto* controller = find_controller();

        if (controller == nullptr)
        {
            return;
        }

        update_camera(controller);
        update_shove();

        // The game's pause menu: Minecraft's picture would cover it and
        // Minecraft would eat its clicks. Hand everything back while paused.
        game_paused = is_game_paused(controller);

        if (game_paused)
        {
            overlay::state().show.store(false, std::memory_order_relaxed);
        }

        input::update(
            g_link,
            overlay::state().window.load(std::memory_order_relaxed),
            minecraft_mode && !game_paused,
            have_mc_state && (mc_state.flags & skycraft::proto::kMcScreenOpen) != 0,
            overlay::state().viewport_w.load(std::memory_order_relaxed),
            overlay::state().viewport_h.load(std::memory_order_relaxed)
        );

        static int log_counter = 0;
        const bool log_now = log_counter++ % 60 == 0;

        // SetIgnoreMoveInput counts: call it once per change, and never undo
        // an ignore we didn't set ourselves (cutscenes use the same counter).
        if (controller != move_input_controller)
        {
            move_input_controller = controller;
            move_input_ignored = false;
        }

        if (move_input_ignored != minecraft_mode)
        {
            move_input_ignored = minecraft_mode;
            set_ignore_move_input(controller, minecraft_mode);
        }

        const std::uintptr_t controller_address =
            reinterpret_cast<std::uintptr_t>(controller);

        // MainGamePC's own Gregory pointer. After New Game it stays empty,
        // so fall back to whatever the controller possesses.
        auto* first_person_character =
            *reinterpret_cast<RC::Unreal::UObject**>(
                controller_address + static_cast<std::uintptr_t>(0x6F0)
            );

        if (first_person_character == nullptr)
        {
            if (auto* pawn = controller->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(STR("Pawn")))
            {
                first_person_character = *pawn;
            }
        }

        if (first_person_character != last_character)
        {
            last_character = first_person_character;
            gregory_found = false;
        }

        // No Gregory to walk with: main menu, the intro (he sits inside
        // Freddy), cutscenes. Minecraft keeps waiting in its world.
        if (first_person_character == nullptr)
        {
            driving = false;

            if (!no_character_logged)
            {
                no_character_logged = true;

                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP no character to walk with (menu or cutscene?), waiting\n")
                );
            }

            return;
        }

        auto* actor =
            reinterpret_cast<RC::Unreal::AActor*>(
                first_person_character
            );

        const auto location = actor->K2_GetActorLocation();

        // Minecraft's position is the player's feet, Unreal's is the middle
        // of the capsule. Without this Minecraft's blocks float half a
        // Gregory above the floor.
        const double half_height = capsule_half_height(first_person_character);

        auto* control_rotation =
            reinterpret_cast<WSDK::FRotator*>(
                controller_address + static_cast<std::uintptr_t>(0x290)
            );

        float pitch = control_rotation->Pitch;

        if (pitch > 180.0f)
        {
            pitch -= 360.0f;
        }

        if (log_now)
        {
            RC::Output::send<RC::LogLevel::Verbose>(
                STR("SBMP Gregory: X={}, Y={}, Z={}, Pitch={}, Yaw={}\n"),
                location.X(),
                location.Y(),
                location.Z(),
                pitch,
                control_rotation->Yaw
            );
        }

        if (!gregory_found)
        {
            gregory_found = true;
            no_character_logged = false;

            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP FirstPersonCharacter: {}\n"),
                first_person_character->GetFullName()
            );

            if (minecraft_mode)
            {
                request_sync(STR("Gregory is back"));
            }
        }

        // Minecraft renders its picture at the size of the game's back buffer.
        const int viewport_w = overlay::state().viewport_w.load(std::memory_order_relaxed);
        const int viewport_h = overlay::state().viewport_h.load(std::memory_order_relaxed);

        const bool front_view = third_person && have_mc_state && mc_state.cameraMode == 2;

        // In Minecraft mode Minecraft's physics move the player and the game's
        // geometry is sent over as collision. Otherwise the game moves
        // Gregory itself and Minecraft's player is pinned to him.
        if (minecraft_mode)
        {
            collision::update(
                g_link,
                first_person_character,
                sb_to_mc_x(location.X()),
                sb_to_mc_y(location.Z() - half_height - eye_shift),
                sb_to_mc_z(location.Y())
            );
        }

        g_link.write_sky_state(
            skycraft::proto::kSkyInGame | (minecraft_mode ? 0u : skycraft::proto::kSkyHostDrives),
            1,
            1,
            sb_to_mc_x(location.X()),
            sb_to_mc_y(location.Z() - half_height - eye_shift),
            sb_to_mc_z(location.Y()),
            // F5's front view: the camera looks back at Steve, so Steve
            // looks the other way.
            sb_to_mc_yaw(front_view ? control_rotation->Yaw + 180.0f : control_rotation->Yaw),
            sb_to_mc_pitch(front_view ? -pitch : pitch),
            teleport_seq,
            viewport_w > 0 ? static_cast<std::uint32_t>(viewport_w) : 1920u,
            viewport_h > 0 ? static_cast<std::uint32_t>(viewport_h) : 1080u,
            12.0f
        );

        if (!have_mc_state)
        {
            return;
        }

        // Crouching is Minecraft's in Minecraft mode: Shift (sneak) crouches
        // Gregory. The game's own crouch key is Ctrl, which is Minecraft's
        // sprint, so that crouch is undone every frame.
        // While Minecraft drives, Unreal's own movement (gravity, floors) has
        // to let go of Gregory: its "flying" mode leaves him where he is put.
        const bool flying = minecraft_mode && driving;

        if (flying != flying_applied)
        {
            flying_applied = flying;
            set_movement_mode(first_person_character, flying ? kMoveFlying : kMoveFalling);

            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP Unreal's movement for Gregory: {}\n"),
                flying ? STR("OFF (Minecraft's physics)") : STR("ON")
            );
        }

        if (minecraft_mode)
        {
            // While flying in Minecraft, Shift is "down", not sneaking.
            set_crouch(
                first_person_character,
                (mc_state.flags & skycraft::proto::kMcSneaking) != 0 &&
                (mc_state.flags & skycraft::proto::kMcFlying) == 0
            );

            crouch_managed = true;
        }
        else if (crouch_managed)
        {
            crouch_managed = false;
            set_crouch(first_person_character, false);
        }

        creative_mode = (mc_state.flags & skycraft::proto::kMcCreative) != 0;
        holds_sword = (mc_state.flags & skycraft::proto::kMcHoldsSword) != 0;

        if (minecraft_mode)
        {
            handle_clicks(controller, first_person_character);
            actors::update(g_link, first_person_character);
            handle_events();
        }
        else
        {
            eye_shift = 0.0;
            actors::clear(g_link);
        }

        world::set_player_position(mc_state.x, mc_state.y, mc_state.z);
        world::set_night_vision(
            minecraft_mode && (mc_state.flags & skycraft::proto::kMcNightVision) != 0
        );

        // The player is Steve while Minecraft drives: Gregory's body and
        // hands would be in the way of Steve's.
        {
            auto* wanted = minecraft_mode && driving ? first_person_character : nullptr;

            if (wanted != hidden_character)
            {
                if (hidden_character != nullptr && hidden_character == first_person_character)
                {
                    call_with(hidden_character, STR("SetActorHiddenInGame"), false);
                }

                if (wanted != nullptr)
                {
                    call_with(wanted, STR("SetActorHiddenInGame"), true);
                }

                hidden_character = wanted;
            }
        }

        update_third_person(
            controller,
            minecraft_mode && driving && mc_state.cameraMode != 0,
            mc_state,
            mc_to_sb_y(mc_state.y) + half_height + eye_shift
        );

        if (log_now)
        {
            RC::Output::send<RC::LogLevel::Verbose>(
                STR("SBMP MC state: X={}, Y={}, Z={}, Flags={}, Frame={}, Ack={}, Driving={}\n"),
                mc_state.x,
                mc_state.y,
                mc_state.z,
                mc_state.flags,
                mc_state.frameCounter,
                mc_state.teleportAck,
                driving ? STR("YES") : STR("NO")
            );

            if (minecraft_mode)
            {
                RC::Output::send<RC::LogLevel::Verbose>(
                    STR("SBMP collision: {} regions sent to Minecraft\n"),
                    collision::regions_sent()
                );
            }
        }

        if (
            !minecraft_mode ||
            (mc_state.flags & skycraft::proto::kMcInWorld) == 0 ||
            mc_state.frameCounter == last_mc_frame
        )
        {
            return;
        }

        last_mc_frame = mc_state.frameCounter;

        // Minecraft's player position, as where Gregory's capsule has to be.
        const double sb_x = mc_to_sb_x(mc_state.x);
        const double sb_y = mc_to_sb_z(mc_state.z);

        const double gap_x = sb_x - location.X();
        const double gap_y = sb_y - location.Y();
        const double gap = std::sqrt(gap_x * gap_x + gap_y * gap_y);

        if (!driving)
        {
            // Minecraft still stands where it was before the sync (or is
            // gliding towards Gregory). Following it now would yank Gregory
            // back through walls, so wait until it has really arrived.
            if (mc_state.teleportAck == teleport_seq && gap < kEngageDistance)
            {
                driving = true;
                supported_tick = ::GetTickCount64();
                have_safe_spot = false;

                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP Minecraft is on Gregory ({} units), Minecraft drives now\n"),
                    gap
                );
            }
            else if (::GetTickCount64() - sync_started > 10000)
            {
                request_sync(STR("Minecraft did not arrive"));
            }

            return;
        }

        // A jump this big is never walking: Minecraft teleported or died, or
        // Unreal moved Gregory (cutscene, door, respawn). Unreal wins.
        const double sb_z = mc_to_sb_y(mc_state.y) + half_height;
        const double gap_z = sb_z - location.Z();

        if (std::sqrt(gap * gap + gap_z * gap_z) > kMaxStep)
        {
            request_sync(STR("positions drifted apart"));
            return;
        }

        // Minecraft only knows the geometry that has been scanned. Where it
        // doesn't, its player falls through the floor.
        const bool supported =
            (mc_state.flags & (skycraft::proto::kMcOnGround | skycraft::proto::kMcFlying | skycraft::proto::kMcSwimming)) != 0;

        if (supported)
        {
            supported_tick = now;
            have_safe_spot = true;
            safe_spot[0] = sb_x;
            safe_spot[1] = sb_y;
            safe_spot[2] = sb_z;
        }
        else if (have_safe_spot && now - supported_tick > kMaxFallMs)
        {
            move_actor(actor, safe_spot[0], safe_spot[1], safe_spot[2]);
            request_sync(STR("Minecraft fell through the world"));
            return;
        }

        // The camera hangs on Gregory, who is smaller than Steve and can't
        // crouch while Unreal's movement is off. Shift him so the camera is
        // where Minecraft's eyes are: same height as Minecraft aims from, and
        // it drops when sneaking.
        if (view.valid && !third_person)
        {
            const double measured = view.position[2] - location.Z();

            eye_above_actor = have_eye_above_actor
                ? eye_above_actor + (measured - eye_above_actor) * 0.1
                : measured;
            have_eye_above_actor = true;

            eye_shift = std::clamp(
                mc_to_sb_y(mc_state.eyeY) - eye_above_actor - sb_z,
                -150.0,
                150.0
            );
        }

        // Minecraft has already collided with everything: no sweep.
        move_actor(actor, sb_x, sb_y, sb_z + eye_shift);
    }

    // Left click hits whoever stands in front of Steve, right click uses
    // what the crosshair is on in the game (doors, buttons).
    auto handle_clicks(RC::Unreal::UObject* controller, RC::Unreal::UObject* character) -> void
    {
        const bool attack = input::take_attack_click();
        const bool use = input::take_use_click();

        if (!attack && !use)
        {
            return;
        }

        if (!driving || !view.valid)
        {
            input::forward_use_click();
            return;
        }

        const auto reach = [this](double distance, double out[3])
        {
            for (int i = 0; i < 3; ++i)
            {
                out[i] = view.position[i] + view.forward[i] * distance;
            }
        };

        if (attack)
        {
            double to[3]{};

            reach(kAttackReach, to);

            RC::Unreal::UObject* part = nullptr;
            auto* target = collision::trace(character, view.position, to, kAttackThickness, &part);

            if (target != nullptr && target->GetFunctionByNameInChain(STR("GetController")) != nullptr)
            {
                swing_target = target;
                swing_tick = ::GetTickCount64();
                hit_character(target, view.forward[0], view.forward[1], 1.0);
            }
            else if (target != nullptr && break_hits(target, part) > 0)
            {
                const int needed = break_hits(target, part);

                // A sword in creative breaks nothing, as with Minecraft's blocks.
                if (creative_mode && holds_sword)
                {
                    RC::Output::send<RC::LogLevel::Verbose>(
                        STR("SBMP hit: a sword in creative breaks nothing\n")
                    );
                }
                else
                {
                // Like a block: one hit in creative, a few in survival. The
                // count starts over on another cutout or after a pause.
                const ULONGLONG hit_tick = ::GetTickCount64();

                if (breaking.Get() != target || hit_tick - breaking_tick > kBreakPauseMs)
                {
                    breaking = target;
                    breaking_hits = 0;
                }

                breaking_tick = hit_tick;
                ++breaking_hits;

                if (creative_mode || breaking_hits >= needed)
                {
                    RC::Output::send<RC::LogLevel::Default>(
                        STR("SBMP hit: broke {}\n"),
                        target->GetFullName()
                    );

                    // Minecraft's break particles and sound, where the cutout stood.
                    const auto spot = reinterpret_cast<RC::Unreal::AActor*>(target)->K2_GetActorLocation();

                    g_link.push_input(
                        skycraft::proto::kInBreakEffect,
                        0,
                        static_cast<std::int32_t>(sb_to_mc_x(spot.X()) * 100.0),
                        static_cast<std::int32_t>(sb_to_mc_y(spot.Z()) * 100.0),
                        static_cast<std::int32_t>(sb_to_mc_z(spot.Y()) * 100.0)
                    );

                    alignas(16) std::uint8_t no_params[16]{};

                    if (auto* destroy = target->GetFunctionByNameInChain(STR("K2_DestroyActor")))
                    {
                        target->ProcessEvent(destroy, no_params);
                    }

                    breaking = RC::Unreal::FWeakObjectPtr{};
                }
                else
                {
                    RC::Output::send<RC::LogLevel::Verbose>(
                        STR("SBMP hit: {} of {} on {}\n"),
                        breaking_hits,
                        needed,
                        target->GetFullName()
                    );
                }
                }
            }
            else if (part != nullptr && hit_loose_object(part))
            {
                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP hit: knocked {} away\n"),
                    target->GetFullName()
                );
            }
            else if (target != nullptr)
            {
                // What the swing reached instead, to learn what the game's bots are made of.
                RC::Output::send<RC::LogLevel::Verbose>(
                    STR("SBMP hit: the swing only reached {}\n"),
                    target->GetFullName()
                );
            }
        }

        if (!use)
        {
            return;
        }

        // A Minecraft block under the crosshair: the click places or uses that.
        if (g_link.minecraft_targets_block())
        {
            input::forward_use_click();
            return;
        }

        double to[3]{};

        reach(kUseReach, to);

        // First what would stop a body (doors), then what the eye lands on
        // (buttons and other things you can walk through).
        auto* target = collision::trace(character, view.position, to, 0.0);

        if (target != nullptr && use_object(target, character, controller))
        {
            input::consume_use_click();
            return;
        }

        auto* seen = collision::trace_visible(character, view.position, to, 0.0);

        if (seen != nullptr && seen != target && use_object(seen, character, controller))
        {
            input::consume_use_click();
            return;
        }

        // Small things (presents, levers): a wider look.
        auto* near_miss = collision::trace_visible(character, view.position, to, kUseThickness);

        if (near_miss != nullptr && near_miss != target && near_miss != seen && use_object(near_miss, character, controller))
        {
            input::consume_use_click();
        }
        else
        {
            input::forward_use_click();
        }
    }

    // Doors open and close like Minecraft doors, locked or not. Anything
    // else the game lets the player use is used the game's way.
    auto use_object(RC::Unreal::UObject* target, RC::Unreal::UObject* character, RC::Unreal::UObject* controller) -> bool
    {
        auto* open = target->GetFunctionByNameInChain(STR("ForceDoorOpen"));
        auto* close = target->GetFunctionByNameInChain(STR("ForceDoorClose"));

        if (open != nullptr && close != nullptr)
        {
            // The game's doors are automatic: they open for whoever may pass
            // and shut again right away for whoever may not. Take the lock
            // off and switch the automatic part off for the player, so the
            // door stays the way Steve left it.
            if (auto* door = target->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(STR("Door")); door != nullptr && *door != nullptr)
            {
                call_with(*door, STR("SetLockedForPlayer"), false);
                call_with(*door, STR("SetSecurityLevel"), std::int32_t{ 0 });
                call_with(*door, STR("SetAutomaticDoorEnableForPlayer"), false);
            }

            // No key or pass needed any more (an empty name), and nothing
            // invisible left standing in the doorway.
            call_with(target, STR("SetDoorRequiredItem"), std::uint64_t{ 0 });
            call_with(target, STR("ClearPlayerBlocker"), std::uint64_t{ 0 });

            bool is_open = false;

            call_getter(target, STR("IsDoorOpen"), is_open);

            alignas(16) std::uint8_t no_params[16]{};

            target->ProcessEvent(is_open ? close : open, no_params);
            g_link.push_input(skycraft::proto::kInSwing, 0);

            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP use: {} door {}\n"),
                is_open ? STR("closed") : STR("opened"),
                target->GetFullName()
            );

            return true;
        }

        auto* interact = target->GetFunctionByNameInChain(STR("OnPlayerInteract"));

        if (interact == nullptr)
        {
            RC::Output::send<RC::LogLevel::Verbose>(
                STR("SBMP use: nothing to do with {}\n"),
                target->GetFullName()
            );

            return false;
        }

        // Things the game makes you hold the button for (presents, some
        // switches): Steve doesn't wait. Starting the hold and finishing it
        // by hand made it finish a second time when the game's own timer ran
        // out, so only the finish is called.
        bool hold = false;

        if (auto* type = target->GetFunctionByNameInChain(STR("GetInteractibleType")))
        {
            // (out Type, out PressType): one byte each; PressType 1 is "hold".
            alignas(16) std::uint8_t types[16]{};

            target->ProcessEvent(type, types);
            hold = types[1] == 1;
        }

        if (!hold || !complete_hold(target))
        {
            struct
            {
                RC::Unreal::UObject* PlayerPawn;
                RC::Unreal::UObject* PlayerController;
            } params{ character, controller };

            target->ProcessEvent(interact, &params);
        }

        g_link.push_input(skycraft::proto::kInSwing, 0);

        RC::Output::send<RC::LogLevel::Default>(
            STR("SBMP use: interacted with {}\n"),
            target->GetFullName()
        );

        return true;
    }

    // Lower case, plain letters.
    static auto lower(const std::wstring& text) -> std::wstring
    {
        std::wstring out = text;

        for (auto& c : out)
        {
            c = static_cast<wchar_t>(std::towlower(c));
        }

        return out;
    }

    // "/freddy", "/tp chica", "/summon monty": brings one of the game's
    // characters to the player, or makes a new one there.
    auto host_command(std::uint32_t who, bool spawn) -> void
    {
        // What the class of each character has in its name (HostCharacter - 1).
        static constexpr const wchar_t* kNames[] = {
            L"freddy", L"chica", L"monty", L"roxy", L"vanny", L"vanessa", L"moon", L"sun", L"bot", L"endo", L"musicman"
        };

        auto* player = last_character;

        if (who == 0 || who > std::size(kNames) || player == nullptr || !view.valid)
        {
            return;
        }

        const std::wstring wanted = kNames[who - 1];

        // Two steps in front of the player, a little above his height: the
        // character's own movement drops it onto the floor.
        const auto here = reinterpret_cast<RC::Unreal::AActor*>(player)->K2_GetActorLocation();

        double x = view.forward[0];
        double y = view.forward[1];
        const double length = std::sqrt(x * x + y * y);

        if (length > 1.0e-3)
        {
            x /= length;
            y /= length;
        }

        const double spot[3] = {
            here.X() + x * kSummonDistance,
            here.Y() + y * kSummonDistance,
            here.Z() + kSummonLift
        };

        // One that walks around already: the shortest matching class name
        // ("Chica_C" rather than "ChicaRummagePoint_C").
        std::vector<RC::Unreal::UObject*> pawns;

        RC::Unreal::UObjectGlobals::FindAllOf(STR("Pawn"), pawns);

        RC::Unreal::UObject* existing = nullptr;
        std::size_t existing_length = 0;

        for (auto* pawn : pawns)
        {
            if (pawn == nullptr || pawn == player)
            {
                continue;
            }

            const auto name = lower(pawn->GetClassPrivate()->GetName());

            if (name.find(wanted) != std::wstring::npos && (existing == nullptr || name.size() < existing_length))
            {
                existing = pawn;
                existing_length = name.size();
            }
        }

        if (!spawn)
        {
            if (existing == nullptr)
            {
                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP command: nobody called '{}' is in the level right now\n"),
                    wanted
                );

                return;
            }

            move_actor(reinterpret_cast<RC::Unreal::AActor*>(existing), spot[0], spot[1], spot[2]);

            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP command: moved {} to the player\n"),
                existing->GetFullName()
            );

            return;
        }

        // A new one needs the character's class. Take it from one that exists,
        // else look for a loaded blueprint class by name.
        RC::Unreal::UObject* character_class = existing != nullptr ? existing->GetClassPrivate() : nullptr;

        if (character_class == nullptr)
        {
            std::vector<RC::Unreal::UObject*> classes;

            RC::Unreal::UObjectGlobals::FindAllOf(STR("BlueprintGeneratedClass"), classes);

            std::size_t best = 0;

            for (auto* candidate : classes)
            {
                if (candidate == nullptr)
                {
                    continue;
                }

                const auto name = lower(candidate->GetName());

                // Characters only: their class has the pawn's functions.
                if (
                    name.find(wanted) != std::wstring::npos &&
                    (character_class == nullptr || name.size() < best) &&
                    static_cast<RC::Unreal::UClass*>(candidate)->GetFunctionByNameInChain(STR("LaunchCharacter")) != nullptr
                )
                {
                    character_class = candidate;
                    best = name.size();
                }
            }
        }

        if (character_class == nullptr)
        {
            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP command: the game has no character class loaded for '{}'\n"),
                wanted
            );

            return;
        }

        auto* spawned = spawn_actor(player, character_class, spot);

        RC::Output::send<RC::LogLevel::Default>(
            STR("SBMP command: summoning {}: {}\n"),
            character_class->GetFullName(),
            spawned != nullptr ? STR("done") : STR("the game refused")
        );
    }

    // UGameplayStatics' two-step actor spawn, then a controller so its AI runs.
    static auto spawn_actor(RC::Unreal::UObject* world_context, RC::Unreal::UObject* actor_class, const double spot[3]) -> RC::Unreal::UObject*
    {
        static auto* statics = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UObject*>(
            nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
        static auto* begin = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(
            nullptr, nullptr, STR("/Script/Engine.GameplayStatics:BeginDeferredActorSpawnFromClass"));
        static auto* finish = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(
            nullptr, nullptr, STR("/Script/Engine.GameplayStatics:FinishSpawningActor"));

        if (statics == nullptr || begin == nullptr || finish == nullptr)
        {
            return nullptr;
        }

        // FTransform: rotation (quaternion), translation, scale, each on 16 bytes.
        struct alignas(16) Transform
        {
            float rotation[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
            float translation[4]{};
            float scale[4]{ 1.0f, 1.0f, 1.0f, 0.0f };
        } transform;

        for (int i = 0; i < 3; ++i)
        {
            transform.translation[i] = static_cast<float>(spot[i]);
        }

        const auto offset = [](RC::Unreal::UFunction* function, const wchar_t* name) -> int
        {
            auto* property = function->FindProperty(RC::Unreal::FName(name, RC::Unreal::FNAME_Find));

            return property != nullptr ? property->GetOffset_Internal() : -1;
        };

        const int begin_context = offset(begin, STR("WorldContextObject"));
        const int begin_class = offset(begin, STR("ActorClass"));
        const int begin_transform = offset(begin, STR("SpawnTransform"));
        const int begin_handling = offset(begin, STR("CollisionHandlingOverride"));
        const int begin_result = offset(begin, STR("ReturnValue"));
        const int finish_actor = offset(finish, STR("Actor"));
        const int finish_transform = offset(finish, STR("SpawnTransform"));

        if (
            begin_context < 0 || begin_class < 0 || begin_transform < 0 || begin_handling < 0 ||
            begin_result < 0 || finish_actor < 0 || finish_transform < 0
        )
        {
            return nullptr;
        }

        std::vector<std::uint8_t> params(begin->GetParmsSize() + 16, 0);

        std::memcpy(params.data() + begin_context, &world_context, sizeof(world_context));
        std::memcpy(params.data() + begin_class, &actor_class, sizeof(actor_class));
        std::memcpy(params.data() + begin_transform, &transform, sizeof(transform));
        params[begin_handling] = 2;  // move it if something is in the way, but always spawn

        statics->ProcessEvent(begin, params.data());

        RC::Unreal::UObject* actor = nullptr;

        std::memcpy(&actor, params.data() + begin_result, sizeof(actor));

        if (actor == nullptr)
        {
            return nullptr;
        }

        params.assign(finish->GetParmsSize() + 16, 0);

        std::memcpy(params.data() + finish_actor, &actor, sizeof(actor));
        std::memcpy(params.data() + finish_transform, &transform, sizeof(transform));

        statics->ProcessEvent(finish, params.data());

        if (auto* controller = actor->GetFunctionByNameInChain(STR("SpawnDefaultController")))
        {
            alignas(16) std::uint8_t no_params[16]{};

            actor->ProcessEvent(controller, no_params);
        }

        return actor;
    }

    // Finishes a hold-to-use interaction by calling what the game calls when
    // the hold bar is full. False if the object has no such thing.
    static auto complete_hold(RC::Unreal::UObject* target) -> bool
    {
        alignas(16) std::uint8_t no_params[16]{};

        // The handler's name carries a number that differs per blueprint.
        for (int i = 0; i < 6; ++i)
        {
            const std::wstring name =
                L"BndEvt__PlayerInteractHoldComponent_K2Node_ComponentBoundEvent_" +
                std::to_wstring(i) +
                L"_OnHoldCompleted__DelegateSignature";

            if (auto* done = target->GetFunctionByNameInChain(name.c_str()))
            {
                target->ProcessEvent(done, no_params);
                return true;
            }
        }

        // Collectibles without that handler: collect directly.
        if (auto* collect = target->GetFunctionByNameInChain(STR("OnCollect")))
        {
            target->ProcessEvent(collect, no_params);
            return true;
        }

        return false;
    }

    // How many hits it takes in survival to break this, 0 if it can't be
    // broken: the cardboard cutouts, and doors (a lot more work).
    static auto break_hits(RC::Unreal::UObject* actor, RC::Unreal::UObject* component) -> int
    {
        if (is_cutout(actor, component))
        {
            return kBreakHits;
        }

        return actor->GetFunctionByNameInChain(STR("ForceDoorOpen")) != nullptr ? kDoorBreakHits : 0;
    }

    // The cardboard cutouts: recognised by the name of the actor or of the
    // mesh it shows. Only things that stand in the level on their own.
    static auto is_cutout(RC::Unreal::UObject* actor, RC::Unreal::UObject* component) -> bool
    {
        static constexpr const wchar_t* kWords[] = { L"cutout", L"cut_out", L"standee", L"cardboard" };

        const auto named = [](RC::Unreal::UObject* object)
        {
            if (object == nullptr)
            {
                return false;
            }

            const auto name = lower(object->GetFullName());

            for (const auto* word : kWords)
            {
                if (name.find(word) != std::wstring::npos)
                {
                    return true;
                }
            }

            return false;
        };

        if (named(actor))
        {
            return true;
        }

        // A plain static mesh actor: look at which mesh it is.
        if (component != nullptr)
        {
            auto* mesh = component->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(STR("StaticMesh"));

            return mesh != nullptr && named(*mesh);
        }

        return false;
    }

    // Loose things the game simulates (balls, cups, toys) fly off when hit.
    // False for anything that is fixed in place.
    auto hit_loose_object(RC::Unreal::UObject* component) -> bool
    {
        auto* simulating = component->GetFunctionByNameInChain(STR("IsSimulatingPhysics"));
        auto* impulse = component->GetFunctionByNameInChain(STR("AddImpulse"));

        if (simulating == nullptr || impulse == nullptr)
        {
            return false;
        }

        // (FName BoneName) -> bool
        struct
        {
            std::uint32_t BoneName[2];
            bool ReturnValue;
        } query{};

        component->ProcessEvent(simulating, &query);

        if (!query.ReturnValue)
        {
            return false;
        }

        // (FVector Impulse, FName BoneName, bool bVelChange): as a change of
        // velocity, so a bowling ball flies as far as a paper cup.
        struct
        {
            float Impulse[3];
            std::uint32_t BoneName[2];
            bool bVelChange;
        } params{
            {
                view.forward[0] * kKnockbackSpeed,
                view.forward[1] * kKnockbackSpeed,
                view.forward[2] * kKnockbackSpeed + kKnockbackLift
            },
            { 0, 0 },
            true
        };

        component->ProcessEvent(impulse, &params);

        return true;
    }

    // Knocks a character or bot away along (x, y) in Unreal's axes, and
    // stuns it if the game can. `strength` 1 is a normal hit.
    auto hit_character(RC::Unreal::UObject* target, double x, double y, double strength) -> void
    {
        if (auto* stun = target->GetFunctionByNameInChain(STR("Stun")))
        {
            alignas(16) std::uint8_t no_params[16]{};

            target->ProcessEvent(stun, no_params);
        }

        const double length = std::sqrt(x * x + y * y);

        if (length > 1.0e-3)
        {
            x /= length;
            y /= length;
        }

        strength = std::clamp(strength, 0.5, 3.0);

        if (auto* launch = target->GetFunctionByNameInChain(STR("LaunchCharacter")))
        {
            // A character: its own movement throws it, with a hop.
            struct
            {
                float LaunchVelocity[3];
                bool bXYOverride;
                bool bZOverride;
            } params{
                {
                    static_cast<float>(x * kKnockbackSpeed * strength),
                    static_cast<float>(y * kKnockbackSpeed * strength),
                    static_cast<float>(kKnockbackLift * strength)
                },
                true,
                true
            };

            target->ProcessEvent(launch, &params);
        }
        else
        {
            // A plain pawn (the wet floor bots) has nothing to throw it with:
            // slide it away ourselves over the next ticks.
            shoved = target;
            shove_direction[0] = x * strength;
            shove_direction[1] = y * strength;
            shove_until = ::GetTickCount64() + kShoveMs;
            shove_tick = ::GetTickCount64();
        }

        RC::Output::send<RC::LogLevel::Default>(
            STR("SBMP hit: {}\n"),
            target->GetFullName()
        );
    }

    // What happened in Minecraft to the game's characters: hits on their
    // stand-ins (weapons, arrows, iron golems, fire) and explosions.
    auto handle_events() -> void
    {
        skycraft::proto::McEvent event{};

        for (int i = 0; i < 64 && g_link.read_event(event); ++i)
        {
            if (event.type == skycraft::proto::kEvHitActor)
            {
                auto* target = actors::find(event.formId);

                // The swing in handle_clicks got there first.
                if (target == nullptr || (target == swing_target && ::GetTickCount64() - swing_tick < kSameHitMs))
                {
                    continue;
                }

                // b, c: knockback direction in Minecraft's x and z. Without
                // one (fire, magic) the character is pushed away from the player.
                double x = event.b;
                double y = kZSign * event.c;

                if (std::abs(x) + std::abs(y) < 1.0e-3)
                {
                    x = view.forward[0];
                    y = view.forward[1];
                }

                hit_character(target, x, y, 0.6 + event.a / 10.0);
            }
            else if (event.type == skycraft::proto::kEvCommand)
            {
                host_command(event.formId, event.flags != 0);
            }
            else if (event.type == skycraft::proto::kEvExplosion)
            {
                // a, b, c: the centre in Minecraft coordinates; d: radius in blocks.
                struct Blast
                {
                    SecurityBreachMinecraftPassthrough* self;
                    double centre[3];
                    double reach;
                } blast{
                    this,
                    { mc_to_sb_x(event.a), mc_to_sb_z(event.c), mc_to_sb_y(event.b) },
                    std::max(double(event.d), 1.0) * 2.0 * skycraft::proto::kUnitsPerBlock
                };

                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP explosion at ({}, {}, {}), radius {} blocks\n"),
                    event.a,
                    event.b,
                    event.c,
                    event.d
                );

                actors::for_each(
                    [](RC::Unreal::UObject* character, void* context)
                    {
                        auto& blast = *static_cast<Blast*>(context);
                        const auto location = reinterpret_cast<RC::Unreal::AActor*>(character)->K2_GetActorLocation();

                        const double dx = location.X() - blast.centre[0];
                        const double dy = location.Y() - blast.centre[1];
                        const double dz = location.Z() - blast.centre[2];
                        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);

                        if (distance < blast.reach)
                        {
                            // Harder the closer it stood.
                            blast.self->hit_character(character, dx, dy, 3.0 * (1.0 - distance / blast.reach) + 0.5);
                        }
                    },
                    &blast
                );
            }
        }
    }

    // F5: Minecraft's camera sits behind its player. Unreal's camera manager
    // has a mode for that ("FreeCam": behind whoever is being viewed, at a
    // distance, pulled in by walls), so it is switched to that rather than
    // moving Gregory's own camera, which he puts back every frame.
    auto update_third_person(RC::Unreal::UObject* controller, bool wanted, const skycraft::proto::McState& mc, double actor_z) -> void
    {
        auto* manager = controller->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(STR("PlayerCameraManager"));

        if (manager == nullptr || *manager == nullptr)
        {
            return;
        }

        auto* distance = (*manager)->GetValuePtrByPropertyNameInChain<float>(STR("FreeCamDistance"));
        auto* offset = (*manager)->GetValuePtrByPropertyNameInChain<float>(STR("FreeCamOffset"));

        if (distance == nullptr || offset == nullptr || controller->GetFunctionByNameInChain(STR("ClientSetCameraMode")) == nullptr)
        {
            if (wanted && !third_person_failed)
            {
                third_person_failed = true;

                RC::Output::send<RC::LogLevel::Error>(
                    STR("SBMP third person: the game's camera has no free camera mode to switch to\n")
                );
            }

            return;
        }

        if (!wanted)
        {
            if (third_person)
            {
                third_person = false;
                call_with(controller, STR("ClientSetCameraMode"), RC::Unreal::FName(STR("Default"), RC::Unreal::FNAME_Add));
            }

            return;
        }

        if (!third_person)
        {
            third_person = true;
            call_with(controller, STR("ClientSetCameraMode"), RC::Unreal::FName(STR("FreeCam"), RC::Unreal::FNAME_Add));

            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP third person: camera distance {} blocks\n"),
                mc.cameraDistance
            );
        }

        // Behind Minecraft's eye, as far as Minecraft's own camera is (that
        // distance already stops at Minecraft's blocks).
        *distance = (std::max)(mc.cameraDistance, 0.5f) * static_cast<float>(skycraft::proto::kUnitsPerBlock);

        offset[0] = 0.0f;
        offset[1] = 0.0f;
        offset[2] = static_cast<float>(mc_to_sb_y(mc.eyeY) - actor_z);
    }

    // Moves the bot that was hit a bit further, stopping at walls.
    auto update_shove() -> void
    {
        auto* target = shoved.Get();
        const ULONGLONG now = ::GetTickCount64();

        if (target == nullptr || now >= shove_until)
        {
            shoved = RC::Unreal::FWeakObjectPtr{};
            return;
        }

        const double seconds = (now - shove_tick) / 1000.0;

        shove_tick = now;

        auto* actor = reinterpret_cast<RC::Unreal::AActor*>(target);
        const auto location = actor->K2_GetActorLocation();

        RC::Unreal::FVector moved{
            static_cast<float>(location.X() + shove_direction[0] * kKnockbackSpeed * seconds),
            static_cast<float>(location.Y() + shove_direction[1] * kKnockbackSpeed * seconds),
            location.Z()
        };

        RC::Unreal::FHitResult sweep_hit{};

        actor->K2_SetActorLocation(moved, true, sweep_hit, false);
    }

    static auto move_actor(RC::Unreal::AActor* actor, double x, double y, double z) -> void
    {
        RC::Unreal::FVector target{
            static_cast<float>(x),
            static_cast<float>(y),
            static_cast<float>(z)
        };

        // Unused by UE4SS, the parameter only has to exist.
        RC::Unreal::FHitResult sweep_hit{};

        actor->K2_SetActorLocation(target, false, sweep_hit, true);
    }

    // Calls a function that takes one parameter. Does nothing if the object doesn't have it.
    template <class T>
    static auto call_with(RC::Unreal::UObject* object, const wchar_t* name, T value) -> void
    {
        auto* function = object->GetFunctionByNameInChain(name);

        if (function == nullptr)
        {
            return;
        }

        alignas(16) std::uint8_t params[64]{};

        std::memcpy(params, &value, sizeof(T));
        object->ProcessEvent(function, params);
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