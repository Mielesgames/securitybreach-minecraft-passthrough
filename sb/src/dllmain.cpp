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
#include <fstream>
#include <set>
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
// A bigger gap than this between where Minecraft has its player and where
// Gregory is means the game moved him somewhere else (a door, a cutscene):
// then the game wins and Minecraft is put on him. Anything smaller is
// Minecraft's to win, Gregory is simply put back.
static constexpr double kMaxStep = 700.0;
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
static constexpr int kFenceBreakHits = 8;
static constexpr int kStatueBreakHits = 15;
// Half the size (Unreal units) above which something is part of the
// building, not a prop that can be broken.
static constexpr float kMaxPropExtent = 500.0f;
// Doors: how often their state is looked at, how often the list of doors is
// made anew, how far from the player they are watched (Unreal units), how
// long one takes to move, and how far around it Steve's geometry is renewed
// (blocks).
static constexpr ULONGLONG kDoorCheckMs = 150;
static constexpr ULONGLONG kDoorListMs = 2000;
static constexpr double kDoorWatchRange = 1800.0;
static constexpr ULONGLONG kDoorMoveMs = 900;
static constexpr double kDoorRescanRadius = 3.0;
// Powered redstone within this distance of a door opens it (Unreal units: 3.5
// blocks, on purpose not only right next to it: the two worlds don't line up exactly).
static constexpr double kRedstoneReach = 245.0;
// Winged characters: how far out and how high a phantom circles, how fast
// it goes round (radians per second), flies and dives (units per second),
// and how long it waits between dives.
static constexpr double kWingRadius = 350.0;
static constexpr double kWingHeight = 420.0;
static constexpr double kWingTurn = 0.9;
static constexpr double kWingSpeed = 650.0;
static constexpr double kWingDiveSpeed = 1100.0;
static constexpr ULONGLONG kWingDiveEveryMs = 8000;
static constexpr ULONGLONG kWingDiveMs = 2500;
// Wings Minecraft hasn't confirmed for this long are gone (it repeats every 2 s).
static constexpr ULONGLONG kWingTimeoutMs = 6000;
// How hard a slime block throws a character up (units per second).
static constexpr float kSlimeLaunch = 950.0f;
// The middle button let go within this long is a click (pick), not a hold (carry).
static constexpr ULONGLONG kPickClickMs = 250;
// A shield stops characters this close in front of it (2 blocks), at most this often.
static constexpr double kShieldReach = 140.0;
static constexpr ULONGLONG kShieldBashMs = 600;
// UE's MOVE_None: a character in a bucket doesn't move.
static constexpr std::uint8_t kMoveNone = 0;
// A lead Minecraft hasn't mentioned for this long is gone.
static constexpr ULONGLONG kLeadTimeoutMs = 1000;
// A lead is this long (Unreal units: 6 blocks, where Minecraft starts pulling).
static constexpr double kLeadLength = 420.0;
// A captive Minecraft hasn't mentioned for this long is free again.
static constexpr ULONGLONG kCaptiveTimeoutMs = 600;
// How high above Freddy's feet the rider is if it couldn't be measured.
static constexpr double kDefaultSeat = 105.0;
// Creative grab: held at least this far from the camera, and how hard a
// loose object is pulled to that spot (per second, as a fraction of the gap).
static constexpr double kGrabMinDistance = 120.0;
static constexpr double kGrabPull = 12.0;
// With the button held, one more hit this often.
static constexpr ULONGLONG kHoldSwingMs = 250;
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
    bool holds_flip_tag = false;
    // Gamerule skycraft:allow_opening_doors is off.
    bool doors_locked = false;
    // The broken-object paper in Minecraft's main hand (0: none).
    std::uint32_t held_token = 0;

    // Broken objects and how to make them again. The first time a paper is
    // used the original comes back; after that (creative keeps the paper)
    // each use makes a copy of its class, with the same mesh.
    struct Token
    {
        std::uint32_t id = 0;
        RC::Unreal::FWeakObjectPtr object;
        bool taken_away = true;
        // A character in a bucket, not a broken object.
        bool character = false;
        RC::Unreal::FWeakObjectPtr object_class;
        RC::Unreal::FWeakObjectPtr mesh;
    };

    std::vector<Token> tokens;
    std::uint32_t next_token = 1;

    // Characters on a lead, and ones stuck in a Minecraft boat or minecart.
    bool holds_lead = false;
    // The middle button: when it went down, to tell a click from a hold.
    bool grab_was_down = false;
    ULONGLONG grab_down_tick = 0;

    // Minecraft's shield is up; whether that switched the game's jumpscares
    // off (so they go back on), and when it last knocked someone back.
    bool shield_up = false;
    bool shield_took_scare = false;
    ULONGLONG shield_bash_tick = 0;

    bool holds_shears = false;

    // Who of the game's cast is around, for "... joined the game" (by object,
    // with the name the chat uses). The first look after the player appears
    // only takes stock: whoever is there already was there.
    struct Present
    {
        RC::Unreal::FWeakObjectPtr object;
        std::wstring name;
    };

    std::vector<Present> present;
    RC::Unreal::UObject* roster_player = nullptr;
    bool roster_taken = false;
    ULONGLONG roster_tick = 0;

    // Characters' sight shortened while the player wears a pumpkin, with
    // how far each saw before.
    struct Dimmed
    {
        RC::Unreal::FWeakObjectPtr sight;
        float end_distance = 0.0f;
    };

    std::vector<Dimmed> dimmed;
    ULONGLONG pumpkin_tick = 0;

    // Things squashed a little by a blow, put back after a moment.
    struct Squashed
    {
        RC::Unreal::FWeakObjectPtr object;
        float scale[3]{};
        ULONGLONG until = 0;
    };

    std::vector<Squashed> squashed;

    // Minecraft's spyglass is at the eye.
    bool spyglass_up = false;

    // A water bucket in Minecraft's hand scoops up a character.
    bool holds_bucket = false;

    // Characters a dispenser gave an elytra: they fly. Freddy keeps the
    // player company like an allay; the others circle and dive like phantoms.
    struct Winged
    {
        RC::Unreal::FWeakObjectPtr object;
        ULONGLONG seen = 0;
        double angle = 0.0;
        bool diving = false;
        ULONGLONG next_dive = 0;
        ULONGLONG dive_until = 0;
    };

    std::vector<Winged> winged;
    ULONGLONG wings_tick = 0;

    // What the game's subtitle widgets showed last, to tell a new line.
    struct Spoken
    {
        RC::Unreal::FWeakObjectPtr widget;
        std::vector<std::wstring> lines;
        // The widget's text block: which text it had, and its lines.
        const void* rich_data = nullptr;
        std::vector<std::wstring> rich_lines;
        float time = 0.0f;
    };

    std::vector<Spoken> spoken;
    ULONGLONG subtitles_tick = 0;
    std::uint16_t chat_line = 0;

    // A character on Minecraft's lead: where the other end is (the player's
    // hand or a knot on a fence), and when Minecraft last said so.
    struct Lead
    {
        RC::Unreal::FWeakObjectPtr object;
        double post[3]{};
        ULONGLONG seen = 0;
    };

    std::vector<Lead> leashed;

    struct Captive
    {
        RC::Unreal::FWeakObjectPtr object;
        ULONGLONG seen = 0;
    };

    std::vector<Captive> captives;

    // Freddy as a mule: who he is, whether the player sits on him, and how
    // high above his feet Minecraft has the rider.
    RC::Unreal::FWeakObjectPtr freddy;
    bool riding_freddy = false;
    double seat_height = 0.0;

    // What the middle button holds (creative), and how far in front of the camera.
    RC::Unreal::FWeakObjectPtr grabbed_pawn;
    RC::Unreal::FWeakObjectPtr grabbed_part;
    double grab_distance = 0.0;
    bool grab_tried = false;
    // The game's doors near the player (see update_doors).
    struct Door
    {
        RC::Unreal::FWeakObjectPtr object;
        bool seen = false;
        bool open = false;
        bool powered = false;
    };

    struct DoorRescan
    {
        double x, y, z;
        ULONGLONG when;
    };

    std::vector<Door> doors;
    std::vector<DoorRescan> door_rescans;
    ULONGLONG doors_checked = 0;
    ULONGLONG doors_listed = 0;
    // Redstone sources that are on, as Minecraft blocks.
    std::vector<std::array<int, 3>> redstone;
    // Blueprint classes of the characters met so far (see remember_characters).
    std::set<std::wstring> known_characters;
    bool characters_loaded = false;
    // Characters that are upside down: where their mesh belongs, and where it is held now.
    struct Flipped
    {
        RC::Unreal::FWeakObjectPtr object;
        float home[3]{};
        float lifted = 0.0f;
    };

    std::vector<Flipped> flipped;
    RC::Unreal::Hook::GlobalCallbackId post_tick_callback = RC::Unreal::Hook::ERROR_ID;
    ULONGLONG swing_repeat_tick = 0;
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
    // Minecraft mode starts by itself once Minecraft and the game are ready
    // (the player's wish, 14 Oct); F6 turns it off and on, and that choice
    // holds until the next F6.
    bool f6_waiting = true;
    bool steve_wanted = true;
    // The game shows its mouse pointer: one of its own screens is up.
    bool game_screen_open = false;
    // The game is letting the player walk (see where it is set).
    bool game_playable = false;
    ULONGLONG playable_since = 0;
    // Minecraft mode went off because Gregory went into a hiding spot.
    bool back_after_hiding = false;
    // The seat Minecraft gives Steve in a hiding spot: when it was asked
    // for, whether he has sat on it, and whether he already asked to leave.
    ULONGLONG hide_seat_tick = 0;
    bool hide_seated = false;
    bool hide_leaving = false;
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

        if (post_tick_callback != RC::Unreal::Hook::ERROR_ID)
        {
            RC::Unreal::Hook::UnregisterCallback(post_tick_callback);
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

        // After the game's own tick: for what the game would otherwise undo.
        post_tick_callback = RC::Unreal::Hook::RegisterEngineTickPostCallback(
            [this](auto&, RC::Unreal::UEngine*, float, bool)
            {
                try
                {
                    hold_flipped();
                }
                catch (...)
                {
                }
            },
            { false, true, STR("SecurityBreachMinecraftPassthrough"), STR("AfterGameTick") }
        );

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

        skycraft::proto::McState mc_state{};
        const bool have_mc_state = g_link.read_mc_state(mc_state);
        const ULONGLONG now = ::GetTickCount64();

        const bool f6_down =
            (::GetAsyncKeyState(VK_F6) & 0x8000) != 0;

        // Minecraft has to be in its world, and the game has to be playing:
        // on a loading screen or in a cutscene Gregory isn't the player's yet,
        // and taking him over there ended the game once.
        const bool minecraft_ready =
            have_mc_state &&
            (mc_state.flags & skycraft::proto::kMcInWorld) != 0 &&
            game_playable;

        // F6 pressed while Minecraft was still starting: it counts once
        // Minecraft is in its world (F6 again cancels).
        const bool waiting_f6_now_ready = f6_waiting && minecraft_ready && !minecraft_mode;

        if ((f6_down && !f6_was_down) || waiting_f6_now_ready)
        {
            if (!minecraft_mode && !minecraft_ready)
            {
                f6_waiting = !f6_waiting;
                steve_wanted = f6_waiting;

                // Minecraft takes a while to start and open its world.
                RC::Output::send<RC::LogLevel::Default>(
                    f6_waiting
                        ? STR("SBMP Minecraft or the game isn't ready yet, Minecraft mode starts when both are\n")
                        : STR("SBMP Minecraft mode will not start after all\n")
                );
            }
            else
            {
                f6_waiting = false;

                if (back_after_hiding)
                {
                    back_after_hiding = false;
                    g_link.push_input(skycraft::proto::kInHide, 0);
                }

                minecraft_mode = !minecraft_mode;

                // Only a press is the player's choice, not the start by itself.
                if (!waiting_f6_now_ready)
                {
                    steve_wanted = minecraft_mode;
                }

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
            steve_wanted = false;
            f6_waiting = false;

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
                // Back by itself once Minecraft is, unless F6 turned it off.
                f6_waiting = steve_wanted;

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

            // Hiding is the game's doing, but the player is still Steve.
            const bool as_steve = minecraft_mode || back_after_hiding;

            shown.show.store(as_steve && in_world && !game_paused, std::memory_order_relaxed);
            shown.crosshair.store(
                as_steve && in_world && !screen_open && mc_state.cameraMode == 0,
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

        // One of the game's own screens (the upgrade terminal, a mission
        // screen): it shows its mouse pointer for those. Same as a pause:
        // nothing of Minecraft's is drawn and the mouse and keys are the game's.
        // bShowMouseCursor is the first bit of its group of flags.
        if (auto* pointer = controller->GetValuePtrByPropertyNameInChain<std::uint8_t>(STR("bShowMouseCursor")))
        {
            const bool game_screen = (*pointer & 1) != 0;

            if (game_screen != game_screen_open)
            {
                game_screen_open = game_screen;

                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP the game's own screen: {}\n"),
                    game_screen ? STR("open, Minecraft steps aside") : STR("closed")
                );
            }

            game_paused = game_paused || game_screen;
        }

        // While the game itself ignores the player's walking (loading screen,
        // cutscene) it isn't playable. Only asked while Minecraft mode is
        // off: with it on, that ignore is our own.
        if (!minecraft_mode)
        {
            bool ignored = false;

            call_getter(controller, STR("IsMoveInputIgnored"), ignored);

            // A second of being playable, so a save that has just loaded has settled.
            if (ignored || game_paused)
            {
                playable_since = 0;
                game_playable = false;
            }
            else
            {
                if (playable_since == 0)
                {
                    playable_since = now;
                }

                game_playable = now - playable_since > 1000;
            }
        }

        if (game_paused)
        {
            overlay::state().show.store(false, std::memory_order_relaxed);
        }

        input::update(
            g_link,
            overlay::state().window.load(std::memory_order_relaxed),
            (minecraft_mode || back_after_hiding) && !game_paused,
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

        // Seen 13 Oct: with Gregory walking behind the sick Freddy, the above
        // gave FreddyGuideSick_C, and the mod moved Freddy while Gregory stood
        // still. If that isn't a Gregory but the camera looks from one, the
        // camera is right.
        const auto is_gregory = [](RC::Unreal::UObject* who)
        {
            return who != nullptr && lower(who->GetClassPrivate()->GetName()).rfind(L"gregory", 0) == 0;
        };

        if (!is_gregory(first_person_character))
        {
            RC::Unreal::UObject* viewed = nullptr;

            if (call_getter(controller, STR("GetViewTarget"), viewed) && is_gregory(viewed))
            {
                if (viewed != last_character)
                {
                    RC::Output::send<RC::LogLevel::Default>(
                        STR("SBMP the controller points at {}, but the camera looks from {}: that one is the player\n"),
                        first_person_character != nullptr ? first_person_character->GetFullName() : STR("nothing"),
                        viewed->GetFullName()
                    );
                }

                first_person_character = viewed;
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

        // A hiding spot (locker, under a table): the game moves Gregory in and
        // out of it and he leaves with its own key, so while he hides it is
        // plain Security Breach. Minecraft mode comes back when he is out.
        {
            auto* in_hiding = first_person_character->GetValuePtrByPropertyNameInChain<bool>(STR("IsHiding"));
            auto* moving = first_person_character->GetValuePtrByPropertyNameInChain<bool>(STR("Hide/UnhideTransitionActive"));
            const bool hiding = (in_hiding != nullptr && *in_hiding) || (moving != nullptr && *moving);

            if (hiding && minecraft_mode)
            {
                minecraft_mode = false;
                back_after_hiding = true;
                hide_seat_tick = 0;
                hide_seated = false;
                hide_leaving = false;

                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP hiding: the game has Gregory until he comes out\n")
                );
            }
            else if (!hiding && back_after_hiding)
            {
                back_after_hiding = false;
                minecraft_mode = true;
                watch_tick = now;
                g_link.push_input(skycraft::proto::kInHide, 0);
                request_sync(STR("out of the hiding spot"));
            }
            else if (back_after_hiding && in_hiding != nullptr && *in_hiding && (moving == nullptr || !*moving))
            {
                // Inside. To Steve a hiding spot is a seat: Minecraft sits him
                // on one nobody sees, and getting off it (Shift) is coming out.
                const bool seated = have_mc_state && (mc_state.flags & skycraft::proto::kMcHiding) != 0;

                if (hide_seat_tick == 0)
                {
                    const auto spot = reinterpret_cast<RC::Unreal::AActor*>(first_person_character)->K2_GetActorLocation();
                    const double feet = spot.Z() - capsule_half_height(first_person_character);

                    hide_seat_tick = now;

                    g_link.push_input(
                        skycraft::proto::kInHide,
                        1,
                        static_cast<std::int32_t>(sb_to_mc_x(spot.X()) * 100.0),
                        static_cast<std::int32_t>(sb_to_mc_y(feet) * 100.0),
                        static_cast<std::int32_t>(sb_to_mc_z(spot.Y()) * 100.0)
                    );
                }

                if (seated && !hide_seated)
                {
                    hide_seated = true;

                    RC::Output::send<RC::LogLevel::Default>(
                        STR("SBMP hiding: Steve sits in the hiding spot, Shift gets him out\n")
                    );
                }

                // Off the seat, or the seat never came (then Shift itself counts).
                const bool got_off = hide_seated && !seated;
                const bool no_seat = !hide_seated && now - hide_seat_tick > 1500 && (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

                if (!hide_leaving && (got_off || no_seat))
                {
                    hide_leaving = true;

                    auto* leave = first_person_character->GetFunctionByNameInChain(STR("LeaveHidingSpace"));

                    RC::Output::send<RC::LogLevel::Default>(
                        STR("SBMP hiding: Steve {}, leaving the hiding spot: {}\n"),
                        got_off ? STR("got off the seat") : STR("pressed Shift without a seat"),
                        leave != nullptr ? STR("asked the game") : STR("the game has no LeaveHidingSpace")
                    );

                    if (leave != nullptr)
                    {
                        first_person_character->ProcessEvent(leave, nullptr);
                    }
                }
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

        // Read even when Minecraft mode is off: what is switched on in
        // Minecraft has to stay known.
        handle_events();

        update_roster(first_person_character);

        // Inverted blocks placed or broken: the game's collision there changes.
        {
            std::vector<std::array<int, 3>> changed;

            world::take_dug_changes(changed);

            for (const auto& section : changed)
            {
                collision::invalidate(section[0] * 16 + 8.0, section[1] * 16 + 8.0, section[2] * 16 + 8.0, 12.0);
            }

            if (!changed.empty())
            {
                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP holes: {} sections changed, their collision is felt out again\n"),
                    changed.size()
                );
            }
        }
        update_pumpkin(minecraft_mode && (mc_state.flags & skycraft::proto::kMcPumpkin) != 0);
        update_wobbles();

        creative_mode = (mc_state.flags & skycraft::proto::kMcCreative) != 0;
        holds_sword = (mc_state.flags & skycraft::proto::kMcHoldsSword) != 0;
        holds_flip_tag = (mc_state.flags & skycraft::proto::kMcHoldsFlipTag) != 0;
        doors_locked = (mc_state.flags & skycraft::proto::kMcDoorsLocked) != 0;
        holds_lead = (mc_state.flags & skycraft::proto::kMcHoldsLead) != 0;
        holds_bucket = (mc_state.flags & skycraft::proto::kMcHoldsBucket) != 0;
        holds_shears = (mc_state.flags & skycraft::proto::kMcHoldsShears) != 0;
        held_token = mc_state.heldToken;

        if (back_after_hiding)
        {
            // In a hiding spot nothing of the game's is in reach: clicks are Minecraft's.
            if (input::take_use_click())
            {
                input::forward_use_click();
            }

            input::take_attack_click();
        }

        if (minecraft_mode)
        {
            handle_clicks(controller, first_person_character);
            actors::update(g_link, first_person_character);
            remember_characters();
            update_doors(first_person_character);
            update_grab(first_person_character);
            update_ride(mc_state, control_rotation->Yaw);
            update_leads(first_person_character);
            release_captives();
            update_wings(first_person_character);
            update_spyglass(controller, (mc_state.flags & skycraft::proto::kMcScoping) != 0);
            update_shield(first_person_character, (mc_state.flags & skycraft::proto::kMcBlocking) != 0);
            update_subtitles();

            // Fire, lava and magma: a character that stands in it jumps out
            // of it, stunned. Lava throws harder.
            actors::take_burning(
                [](RC::Unreal::UObject* burnt, int hazard, void* context)
                {
                    static_cast<SecurityBreachMinecraftPassthrough*>(context)->hit_character(
                        burnt,
                        0.0,
                        0.0,
                        hazard == skycraft::proto::kHazardLava ? 2.0 : 1.0
                    );
                },
                this
            );
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
                hide_extras(first_person_character, wanted != nullptr);
            }
            else if (wanted != nullptr && log_now)
            {
                // The game shows these again on its own (seen 13 Oct: Gregory's
                // watch arm floating next to Steve), so once a second.
                hide_extras(wanted, true);
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
            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP drift: Minecraft at ({}, {}, {}), Gregory at ({}, {}, {})\n"),
                sb_x,
                sb_y,
                sb_z,
                location.X(),
                location.Y(),
                location.Z()
            );

            request_sync(STR("positions drifted apart"));
            return;
        }

        // Minecraft only knows the geometry that has been scanned. Where it
        // doesn't, its player falls through the floor.
        const bool supported =
            (mc_state.flags & (
                skycraft::proto::kMcOnGround | skycraft::proto::kMcFlying | skycraft::proto::kMcSwimming |
                skycraft::proto::kMcRidingHost  // in the saddle: it is the mule that stands on the ground
            )) != 0;

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
        const ULONGLONG click_tick = ::GetTickCount64();

        // Holding the button keeps hacking at whatever can be broken, like
        // mining a block. It doesn't keep hitting characters.
        const bool repeat = !attack && input::attack_held() && click_tick - swing_repeat_tick > kHoldSwingMs;

        if (attack || repeat)
        {
            swing_repeat_tick = click_tick;
        }

        if (!attack && !use && !repeat)
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

        if (attack || repeat)
        {
            double to[3]{};

            reach(kAttackReach, to);

            RC::Unreal::UObject* part = nullptr;
            auto* target = collision::trace(character, view.position, to, kAttackThickness, &part);

            // Never the player's own body (seen 13 Oct: Steve hit Gregory forward).
            if (target != nullptr && (target == character || lower(target->GetClassPrivate()->GetName()).rfind(L"gregory", 0) == 0))
            {
                target = nullptr;
                part = nullptr;
            }

            if (repeat && (target == nullptr || break_hits(target, part) == 0))
            {
                // Nothing to keep hacking at.
            }
            else if (target != nullptr && target->GetFunctionByNameInChain(STR("GetController")) != nullptr)
            {
                swing_target = target;
                swing_tick = ::GetTickCount64();
                hit_character(target, view.forward[0], view.forward[1], 1.0);
            }
            else if (!repeat && target != nullptr && break_hits(target, part) == 0 &&
                     target->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(STR("SkeletalMeshComponent")) != nullptr)
            {
                // A figure that only stands there (the staff bots behind a
                // counter are SkeletalMeshActors, not characters): it has no
                // movement of its own, so it is slid away like the wet floor bots.
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

                if (repeat)
                {
                    g_link.push_input(skycraft::proto::kInSwing, 0);
                }

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

                    take_away(target, spot.X(), spot.Y(), spot.Z());

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
                // It can't break, but it gives a little under the blow.
                if (!repeat)
                {
                    wobble(target);
                }

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

        // A water bucket: the character under the crosshair goes in it, like a fish.
        if (holds_bucket)
        {
            double far_end[3]{};

            reach(kUseReach, far_end);

            auto* who = collision::trace(character, view.position, far_end, kAttackThickness);

            if (who != nullptr && who->GetFunctionByNameInChain(STR("GetController")) != nullptr)
            {
                const auto at = reinterpret_cast<RC::Unreal::AActor*>(who)->K2_GetActorLocation();

                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP bucket: scooped up {}\n"),
                    who->GetFullName()
                );

                take_away(who, at.X(), at.Y(), at.Z(), true);

                g_link.push_input(skycraft::proto::kInSwing, 0);
                input::consume_use_click();
                return;
            }
        }

        // Shears on a character with wings: they come off (Minecraft drops them).
        if (holds_shears)
        {
            double far_end[3]{};

            reach(kUseReach, far_end);

            auto* who = collision::trace(character, view.position, far_end, kAttackThickness);
            const std::uint32_t id = who != nullptr ? actors::id_of(who) : 0;

            if (id != 0)
            {
                g_link.push_input(skycraft::proto::kInShear, 0, static_cast<std::int32_t>(id));
                g_link.push_input(skycraft::proto::kInSwing, 0);
                input::consume_use_click();

                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP shears: on {}\n"),
                    who->GetFullName()
                );

                return;
            }
        }

        // A lead is Minecraft's own (it draws it and ties it to fences): the
        // mod only says which character it goes on. One that is on a lead
        // comes off it with any click, like an animal.
        if (holds_lead || !leashed.empty())
        {
            double far_end[3]{};

            reach(kUseReach, far_end);

            auto* who = collision::trace(character, view.position, far_end, kAttackThickness);

            if (who != nullptr && who->GetFunctionByNameInChain(STR("GetController")) != nullptr)
            {
                const bool on_lead = std::any_of(leashed.begin(), leashed.end(), [who](const Lead& l) { return l.object.Get() == who; });
                const std::uint32_t id = actors::id_of(who);

                if (id != 0 && (on_lead || holds_lead))
                {
                    g_link.push_input(skycraft::proto::kInLeash, on_lead ? 0 : 1, static_cast<std::int32_t>(id));

                    RC::Output::send<RC::LogLevel::Default>(
                        STR("SBMP lead: asked Minecraft to take {} {} the lead\n"),
                        who->GetFullName(),
                        on_lead ? STR("off") : STR("on")
                    );

                    g_link.push_input(skycraft::proto::kInSwing, 0);
                    input::consume_use_click();
                    return;
                }
            }
        }

        // Freddy is a mule: using him is getting on. (While riding, Minecraft's
        // inventory key opens his saddle and chest.)
        if (!riding_freddy)
        {
            double far_end[3]{};

            reach(kUseReach, far_end);

            auto* who = collision::trace(character, view.position, far_end, kAttackThickness);

            if (who != nullptr && who->GetFunctionByNameInChain(STR("GetController")) != nullptr && is_freddy(who))
            {
                const auto at = reinterpret_cast<RC::Unreal::AActor*>(who)->K2_GetActorLocation();
                const double feet = at.Z() - capsule_half_height(who);

                freddy = who;

                g_link.push_input(
                    skycraft::proto::kInMountFreddy,
                    0,
                    static_cast<std::int32_t>(sb_to_mc_x(at.X()) * 100.0),
                    static_cast<std::int32_t>(sb_to_mc_y(feet) * 100.0),
                    static_cast<std::int32_t>(sb_to_mc_z(at.Y()) * 100.0)
                );
                g_link.push_input(skycraft::proto::kInSwing, 0);
                input::consume_use_click();

                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP ride: getting on {}\n"),
                    who->GetFullName()
                );

                return;
            }
        }

        // The paper of a broken object: it goes back where the player points.
        if (held_token != 0 && put_back(held_token, character))
        {
            input::consume_use_click();
            return;
        }

        // A bucket with a character in it never reaches Minecraft: there it
        // is a water bucket, and emptying it would lose the character.
        if (held_token != 0 && std::any_of(tokens.begin(), tokens.end(), [this](const Token& t) { return t.id == held_token && t.character; }))
        {
            input::consume_use_click();
            return;
        }

        // A name tag called Dinnerbone or Grumm on a character: upside down,
        // as with Minecraft's mobs. Again puts it back.
        if (holds_flip_tag)
        {
            double far_end[3]{};

            reach(kUseReach, far_end);

            auto* who = collision::trace(character, view.position, far_end, kAttackThickness);

            if (who != nullptr && who->GetFunctionByNameInChain(STR("GetController")) != nullptr && flip_character(who))
            {
                g_link.push_input(skycraft::proto::kInSwing, 0);
                input::consume_use_click();
                return;
            }
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
        // Gamerule skycraft:allow_opening_doors off: doors are the game's
        // business (its own interaction below still works, with its locks).
        if (!doors_locked && target->GetFunctionByNameInChain(STR("ForceDoorOpen")) != nullptr)
        {
            bool is_open = false;

            call_getter(target, STR("IsDoorOpen"), is_open);
            set_door(target, !is_open);
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

            if (name.find(wanted) != std::wstring::npos && name.find(L"enterexit") == std::wstring::npos && (existing == nullptr || name.size() < existing_length))
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

        // A new one needs the character's class: from one that walks around,
        // else a class the game has loaded, else one remembered from an
        // earlier meeting, which is then loaded from the game's files.
        RC::Unreal::UObject* character_class = existing != nullptr ? existing->GetClassPrivate() : nullptr;

        if (character_class == nullptr)
        {
            static auto* pawn_class = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(
                nullptr, nullptr, STR("/Script/Engine.Pawn"));

            std::vector<RC::Unreal::UObject*> classes;

            RC::Unreal::UObjectGlobals::FindAllOf(STR("BlueprintGeneratedClass"), classes);

            std::size_t best = 0;

            for (auto* candidate : classes)
            {
                if (candidate == nullptr || pawn_class == nullptr)
                {
                    continue;
                }

                const auto name = lower(candidate->GetName());

                if (
                    name.find(wanted) != std::wstring::npos &&
                    // FreddyEnterExitPawn is a helper of the game, not a Freddy.
                    name.find(L"enterexit") == std::wstring::npos &&
                    (character_class == nullptr || name.size() < best) &&
                    static_cast<RC::Unreal::UClass*>(candidate)->IsChildOf(pawn_class)
                )
                {
                    character_class = candidate;
                    best = name.size();
                }
            }
        }

        if (character_class == nullptr)
        {
            std::wstring path;

            for (const auto& known : known_characters)
            {
                // The name after the last slash: "Roxy.Roxy_C".
                const auto slash = known.find_last_of(L'/');
                const auto name = lower(slash == std::wstring::npos ? known : known.substr(slash + 1));

                if (name.find(wanted) != std::wstring::npos && name.find(L"enterexit") == std::wstring::npos && (path.empty() || known.size() < path.size()))
                {
                    path = known;
                }
            }

            if (!path.empty())
            {
                character_class = load_class(player, path);

                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP command: loading {} from the game's files: {}\n"),
                    path,
                    character_class != nullptr ? STR("done") : STR("failed")
                );
            }
        }

        if (character_class == nullptr)
        {
            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP command: no character class for '{}' is loaded, and none was met before to load\n"),
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

    // Loads a blueprint class by its asset path (UKismetSystemLibrary::LoadClassAsset_Blocking).
    static auto load_class(RC::Unreal::UObject* world_context, const std::wstring& path) -> RC::Unreal::UObject*
    {
        static auto* library = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UObject*>(
            nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        static auto* function = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(
            nullptr, nullptr, STR("/Script/Engine.KismetSystemLibrary:LoadClassAsset_Blocking"));

        if (library == nullptr || function == nullptr)
        {
            return nullptr;
        }

        auto* asset = function->FindProperty(RC::Unreal::FName(STR("AssetClass"), RC::Unreal::FNAME_Find));
        auto* result = function->GetReturnProperty();

        if (asset == nullptr || result == nullptr)
        {
            return nullptr;
        }

        // A soft class pointer: a weak pointer (unset), a tag, then the path
        // as a name plus an empty sub-path string.
        struct
        {
            std::int32_t object_index{ -1 };
            std::int32_t serial_number{ 0 };
            std::int32_t tag{ 0 };
            std::int32_t padding{ 0 };
            RC::Unreal::FName asset_path;
            void* sub_path_data{ nullptr };
            std::int32_t sub_path_num{ 0 };
            std::int32_t sub_path_max{ 0 };
        } soft;

        soft.asset_path = RC::Unreal::FName(path.c_str(), RC::Unreal::FNAME_Add);

        std::vector<std::uint8_t> params(function->GetParmsSize() + 16, 0);

        std::memcpy(params.data() + asset->GetOffset_Internal(), static_cast<void*>(&soft), sizeof(soft));

        library->ProcessEvent(function, params.data());

        RC::Unreal::UObject* loaded = nullptr;

        std::memcpy(&loaded, params.data() + result->GetOffset_Internal(), sizeof(loaded));

        return loaded;
    }

    // Where the remembered character classes are kept: next to the mod's dll folder.
    static auto characters_file() -> std::wstring
    {
        HMODULE module = nullptr;

        ::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&characters_file),
            &module
        );

        wchar_t path[MAX_PATH * 2]{};

        ::GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));

        std::wstring folder = path;

        // ...<mod>/dlls/main.dll -> ...<mod>/characters.txt
        for (int i = 0; i < 2; ++i)
        {
            const auto slash = folder.find_last_of(L"\\/");

            if (slash != std::wstring::npos)
            {
                folder.resize(slash);
            }
        }

        return folder + L"\\characters.txt";
    }

    // Keeps the list of character classes across sessions, so one met
    // yesterday can be summoned today.
    auto remember_characters() -> void
    {
        if (!characters_loaded)
        {
            characters_loaded = true;

            std::wifstream in(characters_file());
            std::wstring line;

            while (std::getline(in, line))
            {
                if (line.rfind(L"/Game/", 0) == 0)
                {
                    known_characters.insert(line);
                }
            }
        }

        const auto before = known_characters.size();

        for (const auto& path : actors::known_classes())
        {
            known_characters.insert(path);
        }

        if (known_characters.size() != before)
        {
            std::wofstream out(characters_file(), std::ios::trunc);

            for (const auto& path : known_characters)
            {
                out << path << L"\n";
            }
        }
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

    // Opens or closes one of the game's doors for good, lock or no lock.
    static auto set_door(RC::Unreal::UObject* target, bool open) -> void
    {
        auto* force = target->GetFunctionByNameInChain(open ? STR("ForceDoorOpen") : STR("ForceDoorClose"));

        if (force == nullptr)
        {
            return;
        }

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

        alignas(16) std::uint8_t no_params[16]{};

        target->ProcessEvent(force, no_params);
    }

    // Keeps an eye on the game's doors near the player:
    //  - one that opens or closes changes what Steve can walk through, so the
    //    geometry around it is scanned again at once (and once more when the
    //    door has finished moving);
    //  - one with powered redstone within a few blocks opens, and closes
    //    again when the power goes.
    auto update_doors(RC::Unreal::UObject* player) -> void
    {
        const ULONGLONG now = ::GetTickCount64();

        // Scans that were put off until a door stopped moving.
        for (auto it = door_rescans.begin(); it != door_rescans.end();)
        {
            if (now >= it->when)
            {
                collision::invalidate(it->x, it->y, it->z, kDoorRescanRadius);
                it = door_rescans.erase(it);
            }
            else
            {
                ++it;
            }
        }

        if (now - doors_checked < kDoorCheckMs)
        {
            return;
        }

        doors_checked = now;

        if (now - doors_listed > kDoorListMs)
        {
            doors_listed = now;

            std::vector<RC::Unreal::UObject*> found;

            RC::Unreal::UObjectGlobals::FindAllOf(STR("BP_DoorMaster_C"), found);

            std::vector<Door> listed;

            for (auto* object : found)
            {
                if (object == nullptr)
                {
                    continue;
                }

                Door door;

                door.object = object;

                // Keep what was known about it.
                for (const auto& old : doors)
                {
                    if (old.object.Get() == object)
                    {
                        door = old;
                        break;
                    }
                }

                listed.push_back(door);
            }

            doors = std::move(listed);
        }

        const auto here = reinterpret_cast<RC::Unreal::AActor*>(player)->K2_GetActorLocation();

        for (auto& door : doors)
        {
            auto* object = door.object.Get();

            if (object == nullptr)
            {
                continue;
            }

            const auto at = reinterpret_cast<RC::Unreal::AActor*>(object)->K2_GetActorLocation();
            const double dx = at.X() - here.X();
            const double dy = at.Y() - here.Y();
            const double dz = at.Z() - here.Z();

            if (dx * dx + dy * dy + dz * dz > kDoorWatchRange * kDoorWatchRange)
            {
                continue;
            }

            // Redstone: any powered source within reach of the door.
            bool powered = false;

            for (const auto& source : redstone)
            {
                const double sx = mc_to_sb_x(source[0] + 0.5) - at.X();
                const double sy = mc_to_sb_z(source[2] + 0.5) - at.Y();
                const double sz = mc_to_sb_y(source[1] + 0.5) - at.Z();

                if (sx * sx + sy * sy + sz * sz < kRedstoneReach * kRedstoneReach)
                {
                    powered = true;
                    break;
                }
            }

            if (powered != door.powered && !doors_locked)
            {
                door.powered = powered;
                set_door(object, powered);

                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP redstone: {} door {}\n"),
                    powered ? STR("opened") : STR("closed"),
                    object->GetFullName()
                );
            }

            bool open = false;

            call_getter(object, STR("IsDoorOpen"), open);

            if (!door.seen || open != door.open)
            {
                const bool changed = door.seen;

                door.seen = true;
                door.open = open;

                if (changed)
                {
                    const double x = sb_to_mc_x(at.X());
                    const double y = sb_to_mc_y(at.Z());
                    const double z = sb_to_mc_z(at.Y());

                    collision::invalidate(x, y, z, kDoorRescanRadius);
                    door_rescans.push_back({ x, y, z, now + kDoorMoveMs });
                }
            }
        }
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

    // What hiding Gregory leaves in view: the arm with his watch (a part of
    // him the game keeps showing) and whatever he holds (its own actor).
    static auto hide_extras(RC::Unreal::UObject* gregory, bool hide) -> void
    {
        if (auto* arm = gregory->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(STR("RIG_FazWatchHand")); arm != nullptr && *arm != nullptr)
        {
            // SetHiddenInGame(bool NewHidden, bool bPropagateToChildren): two bytes.
            call_with(*arm, STR("SetHiddenInGame"), static_cast<std::uint16_t>(hide ? 0x0101 : 0x0100));
        }

        if (auto* item = gregory->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(STR("EquippedItem")); item != nullptr && *item != nullptr)
        {
            call_with(*item, STR("SetActorHiddenInGame"), hide);
        }
    }

    // Minecraft's world moves a character: water carries it, a bubble column
    // lifts it or drags it down, slime throws it up, and a hostile one is
    // walked towards a villager. x, y, z in blocks, Minecraft's axes.
    auto push_character(RC::Unreal::UObject* target, double x, double y, double z, std::uint32_t flags) -> void
    {
        if ((flags & 1) != 0)
        {
            if (auto* launch = target->GetFunctionByNameInChain(STR("LaunchCharacter")))
            {
                struct
                {
                    float LaunchVelocity[3];
                    bool bXYOverride;
                    bool bZOverride;
                } params{ { 0.0f, 0.0f, kSlimeLaunch }, false, true };

                target->ProcessEvent(launch, &params);
            }

            return;
        }

        auto* actor = reinterpret_cast<RC::Unreal::AActor*>(target);
        const auto at = actor->K2_GetActorLocation();

        if (y != 0.0)
        {
            // Going up or down in water: gravity would undo it between ticks.
            // Same list as the boats: let go when Minecraft stops saying so.
            const ULONGLONG tick = ::GetTickCount64();
            auto known = std::find_if(captives.begin(), captives.end(), [target](const Captive& c) { return c.object.Get() == target; });

            if (known == captives.end())
            {
                Captive afloat;

                afloat.object = target;
                captives.push_back(afloat);
                known = captives.end() - 1;

                set_movement_mode(target, kMoveFlying);

                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP water: {} is carried {}\n"),
                    target->GetFullName(),
                    y > 0.0 ? STR("up") : STR("down")
                );
            }

            known->seen = tick;
        }

        RC::Unreal::FVector to{
            static_cast<float>(at.X() + x * skycraft::proto::kUnitsPerBlock),
            static_cast<float>(at.Y() + kZSign * z * skycraft::proto::kUnitsPerBlock),
            static_cast<float>(at.Z() + y * skycraft::proto::kUnitsPerBlock)
        };

        RC::Unreal::FHitResult sweep_hit{};

        actor->K2_SetActorLocation(to, true, sweep_hit, false);

        if ((flags & 2) != 0)
        {
            // Walking there: it looks where it goes.
            face(target, static_cast<float>(std::atan2(kZSign * z, x) * 180.0 / 3.14159265358979323846));

            static ULONGLONG last_log = 0;

            if (::GetTickCount64() - last_log > 5000)
            {
                last_log = ::GetTickCount64();

                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP villager: {} goes after one\n"),
                    target->GetFullName()
                );
            }
        }
    }

    static auto face(RC::Unreal::UObject* target, float yaw) -> void
    {
        struct
        {
            float NewRotation[3];
            bool bTeleportPhysics;
            bool ReturnValue;
        } turn{ { 0.0f, yaw, 0.0f }, true, false };

        if (auto* rotate = target->GetFunctionByNameInChain(STR("K2_SetActorRotation")))
        {
            target->ProcessEvent(rotate, &turn);
        }
    }

    // The chat name of one of the game's cast, from its class: "Moon",
    // "Glamrock Chica"... Empty for everyone else (bots, helpers).
    static auto cast_name(RC::Unreal::UObject* who) -> std::wstring
    {
        static const std::pair<const wchar_t*, const wchar_t*> kCast[] = {
            { L"gregory", L"Gregory" }, { L"vanessa", L"Vanessa" }, { L"vanny", L"Vanny" },
            { L"musicman", L"DJ Music Man" }, { L"moon", L"Moon" }, { L"sun", L"Sun" },
            { L"freddy", L"Glamrock Freddy" }, { L"chica", L"Glamrock Chica" }, { L"monty", L"Montgomery Gator" },
            { L"roxy", L"Roxanne Wolf" }, { L"endo", L"Endo" }, { L"blob", L"The Blob" }, { L"burntrap", L"Burntrap" },
        };

        const auto name = lower(who->GetClassPrivate()->GetName());

        if (name.find(L"enterexit") != std::wstring::npos || name.find(L"jumpscare") != std::wstring::npos)
        {
            return {};
        }

        for (const auto& [word, shown] : kCast)
        {
            if (name.find(word) != std::wstring::npos)
            {
                return shown;
            }
        }

        return {};
    }

    // One line in Minecraft's chat, in yellow when `yellow`.
    auto say_in_chat(const std::wstring& message, bool yellow) -> void
    {
        const std::uint16_t code = chat_line++;

        for (std::size_t at = 0; at < message.size() && at < 240; at += 12)
        {
            std::int32_t words[3]{};

            for (std::size_t i = 0; i < 12 && at + i < message.size(); ++i)
            {
                const wchar_t c = message[at + i];

                words[i / 4] |= static_cast<std::int32_t>((c < 256 ? c : L'?') & 0xFF) << ((i % 4) * 8);
            }

            g_link.push_input(skycraft::proto::kInChatText, code, words[0], words[1], words[2]);
        }

        g_link.push_input(skycraft::proto::kInChatSay, code, yellow ? 1 : 0);
    }

    // "Moon joined the game", "Moon left the game", like players in
    // Minecraft. Once a second; the player's own Gregory joins too.
    auto update_roster(RC::Unreal::UObject* player) -> void
    {
        const ULONGLONG tick = ::GetTickCount64();

        if (tick - roster_tick < 1000)
        {
            return;
        }

        roster_tick = tick;

        if (player != roster_player)
        {
            // A new Gregory (start, respawn, another level): take stock again.
            roster_player = player;
            roster_taken = false;
            present.clear();

            if (player != nullptr && lower(player->GetClassPrivate()->GetName()).rfind(L"gregory", 0) == 0)
            {
                say_in_chat(L"Gregory joined the game", true);
            }
        }

        if (player == nullptr)
        {
            return;
        }

        std::vector<RC::Unreal::UObject*> pawns;

        RC::Unreal::UObjectGlobals::FindAllOf(STR("Pawn"), pawns);

        for (auto it = present.begin(); it != present.end();)
        {
            auto* object = it->object.Get();

            if (object == nullptr || std::find(pawns.begin(), pawns.end(), object) == pawns.end())
            {
                say_in_chat(it->name + L" left the game", true);
                it = present.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto* pawn : pawns)
        {
            if (pawn == nullptr || pawn == player)
            {
                continue;
            }

            if (std::any_of(present.begin(), present.end(), [pawn](const Present& p) { return p.object.Get() == pawn; }))
            {
                continue;
            }

            auto name = cast_name(pawn);

            if (name.empty())
            {
                continue;
            }

            if (roster_taken)
            {
                say_in_chat(name + L" joined the game", true);

                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP roster: {} joined ({})\n"),
                    name,
                    pawn->GetFullName()
                );
            }

            Present fresh;

            fresh.object = pawn;
            fresh.name = std::move(name);
            present.push_back(std::move(fresh));
        }

        roster_taken = true;
    }

    // A carved pumpkin on Steve's head: like an enderman, the game's
    // characters see him less well. Their sight reaches a third as far
    // (the game's own USightComponent), and back when the pumpkin comes off.
    auto update_pumpkin(bool wearing) -> void
    {
        const ULONGLONG tick = ::GetTickCount64();

        if (!wearing)
        {
            if (!dimmed.empty())
            {
                for (auto& d : dimmed)
                {
                    if (auto* sight = d.sight.Get())
                    {
                        if (auto* end = sight->GetValuePtrByPropertyNameInChain<float>(STR("EndDistance")))
                        {
                            *end = d.end_distance;
                        }
                    }
                }

                dimmed.clear();

                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP pumpkin: off, the characters see as far as before\n")
                );
            }

            return;
        }

        if (tick - pumpkin_tick < 1000)
        {
            return;
        }

        pumpkin_tick = tick;

        static auto* sight_class = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(
            nullptr, nullptr, STR("/Script/fnaf9.SightComponent"));

        if (sight_class == nullptr)
        {
            return;
        }

        struct Pumpkin
        {
            SecurityBreachMinecraftPassthrough* self;
            RC::Unreal::UClass* sight_class;
            int newly;
        } pumpkin{ this, sight_class, 0 };

        actors::for_each(
            [](RC::Unreal::UObject* character, void* context)
            {
                auto& pumpkin = *static_cast<Pumpkin*>(context);
                auto* get = character->GetFunctionByNameInChain(STR("GetComponentByClass"));

                if (get == nullptr)
                {
                    return;
                }

                RC::Output::send<RC::LogLevel::Verbose>(
                    STR("SBMP pumpkin: looking at the sight of {}\n"),
                    character->GetFullName()
                );

                // (TSubclassOf<UActorComponent> ComponentClass, UActorComponent* ReturnValue)
                struct
                {
                    RC::Unreal::UClass* ComponentClass;
                    RC::Unreal::UObject* ReturnValue;
                } query{ pumpkin.sight_class, nullptr };

                character->ProcessEvent(get, &query);

                auto* sight = query.ReturnValue;

                if (sight == nullptr || std::any_of(pumpkin.self->dimmed.begin(), pumpkin.self->dimmed.end(), [sight](const Dimmed& d) { return d.sight.Get() == sight; }))
                {
                    return;
                }

                auto* end = sight->GetValuePtrByPropertyNameInChain<float>(STR("EndDistance"));

                if (end == nullptr)
                {
                    return;
                }

                Dimmed d;

                d.sight = sight;
                d.end_distance = *end;
                pumpkin.self->dimmed.push_back(d);

                *end = d.end_distance / 3.0f;
                ++pumpkin.newly;
            },
            &pumpkin
        );

        if (pumpkin.newly != 0)
        {
            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP pumpkin: {} more characters see a third as far\n"),
                pumpkin.newly
            );
        }
    }

    // A blow on something that can't break squashes it a little for a
    // moment, if it is one that may move at all (most of the level is fixed
    // in place, and stays as it is).
    auto wobble(RC::Unreal::UObject* target) -> void
    {
        auto* root = target->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(STR("RootComponent"));

        if (root == nullptr || *root == nullptr)
        {
            return;
        }

        // EComponentMobility: 2 = Movable.
        auto* mobility = (*root)->GetValuePtrByPropertyNameInChain<std::uint8_t>(STR("Mobility"));

        if (mobility == nullptr || *mobility != 2 || std::any_of(squashed.begin(), squashed.end(), [target](const Squashed& s) { return s.object.Get() == target; }))
        {
            return;
        }

        Squashed squash;

        squash.object = target;
        squash.until = ::GetTickCount64() + 160;

        if (!call_getter(target, STR("GetActorScale3D"), squash.scale))
        {
            return;
        }

        squashed.push_back(squash);

        RC::Output::send<RC::LogLevel::Default>(
            STR("SBMP hit: {} gives a little under the blow\n"),
            target->GetFullName()
        );

        const float flat[3] = { squash.scale[0] * 1.06f, squash.scale[1] * 1.06f, squash.scale[2] * 0.9f };

        call_with(target, STR("SetActorScale3D"), *reinterpret_cast<const std::array<float, 3>*>(flat));
    }

    auto update_wobbles() -> void
    {
        const ULONGLONG tick = ::GetTickCount64();

        for (auto it = squashed.begin(); it != squashed.end();)
        {
            auto* target = it->object.Get();

            if (target == nullptr)
            {
                it = squashed.erase(it);
            }
            else if (tick >= it->until)
            {
                call_with(target, STR("SetActorScale3D"), *reinterpret_cast<const std::array<float, 3>*>(it->scale));
                it = squashed.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // A raised shield: the game is told Gregory can't be jumpscared (its own
    // switch, CanBeJumpScared), and a character that comes within two
    // blocks in front of the shield bounces off it, stunned.
    auto update_shield(RC::Unreal::UObject* player, bool blocking) -> void
    {
        auto* scareable = player->GetValuePtrByPropertyNameInChain<bool>(STR("CanBeJumpScared"));

        if (blocking != shield_up)
        {
            shield_up = blocking;

            if (scareable != nullptr)
            {
                if (blocking)
                {
                    // Only handed back if it was on: a cutscene may have it off.
                    shield_took_scare = *scareable;
                    *scareable = false;
                }
                else if (shield_took_scare)
                {
                    *scareable = true;
                }
            }

            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP shield: {}\n"),
                blocking ? STR("up") : STR("down")
            );
        }

        if (!blocking || !view.valid)
        {
            return;
        }

        // The game may switch it back on while the shield is still up.
        if (scareable != nullptr && shield_took_scare)
        {
            *scareable = false;
        }

        const ULONGLONG tick = ::GetTickCount64();

        if (tick - shield_bash_tick < kShieldBashMs)
        {
            return;
        }

        struct Shield
        {
            SecurityBreachMinecraftPassthrough* self;
            double at[3];
            double facing[2];
            bool bashed;
        } shield{ this, { view.position[0], view.position[1], view.position[2] }, { view.forward[0], view.forward[1] }, false };

        actors::for_each(
            [](RC::Unreal::UObject* character, void* context)
            {
                auto& shield = *static_cast<Shield*>(context);

                // Freddy is a friend.
                if (is_freddy(character))
                {
                    return;
                }

                const auto location = reinterpret_cast<RC::Unreal::AActor*>(character)->K2_GetActorLocation();
                const double dx = location.X() - shield.at[0];
                const double dy = location.Y() - shield.at[1];
                const double dz = location.Z() - shield.at[2];
                const double flat = std::sqrt(dx * dx + dy * dy);

                // In front of the shield, not behind the player's back.
                if (flat > kShieldReach || std::abs(dz) > kShieldReach || dx * shield.facing[0] + dy * shield.facing[1] < flat * 0.3)
                {
                    return;
                }

                shield.self->hit_character(character, dx, dy, 1.5);
                shield.bashed = true;
            },
            &shield
        );

        if (shield.bashed)
        {
            shield_bash_tick = tick;
            g_link.push_input(skycraft::proto::kInSwing, 0);
        }
    }

    // Minecraft's spyglass narrows Minecraft's own view to a tenth; the
    // picture is the game's, so the game's camera has to do the same.
    auto update_spyglass(RC::Unreal::UObject* controller, bool scoping) -> void
    {
        if (scoping == spyglass_up)
        {
            return;
        }

        auto* manager = controller->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(STR("PlayerCameraManager"));

        if (manager == nullptr || *manager == nullptr)
        {
            return;
        }

        spyglass_up = scoping;

        if (scoping)
        {
            float fov = 90.0f;

            call_getter(*manager, STR("GetFOVAngle"), fov);
            call_with(*manager, STR("SetFOV"), fov * 0.1f);
        }
        else if (auto* unlock = (*manager)->GetFunctionByNameInChain(STR("UnlockFOV")))
        {
            alignas(16) std::uint8_t no_params[16]{};

            (*manager)->ProcessEvent(unlock, no_params);
        }

        RC::Output::send<RC::LogLevel::Default>(
            STR("SBMP spyglass: {}\n"),
            scoping ? STR("up, the game's camera zooms in") : STR("down")
        );
    }

    // An elytra from a dispenser went on a character's stand-in, or came off.
    auto set_wings(RC::Unreal::UObject* target, bool on) -> void
    {
        const ULONGLONG tick = ::GetTickCount64();
        auto known = std::find_if(winged.begin(), winged.end(), [target](const Winged& w) { return w.object.Get() == target; });

        if (!on)
        {
            if (known != winged.end())
            {
                set_movement_mode(target, kMoveFalling);
                winged.erase(known);

                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP wings: {} lost them\n"),
                    target->GetFullName()
                );
            }

            return;
        }

        if (known == winged.end())
        {
            Winged fresh;

            fresh.object = target;
            fresh.next_dive = tick + kWingDiveEveryMs;
            winged.push_back(fresh);
            known = winged.end() - 1;

            set_movement_mode(target, kMoveFlying);

            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP wings: {} flies, as {}\n"),
                target->GetFullName(),
                is_freddy(target) ? STR("an allay (stays with the player)") : STR("a phantom (circles and dives)")
            );
        }

        known->seen = tick;
    }

    auto update_wings(RC::Unreal::UObject* player) -> void
    {
        const ULONGLONG tick = ::GetTickCount64();
        const double dt = wings_tick == 0 ? 0.0 : (std::min)(static_cast<double>(tick - wings_tick) / 1000.0, 0.1);

        wings_tick = tick;

        if (winged.empty())
        {
            return;
        }

        const auto here = reinterpret_cast<RC::Unreal::AActor*>(player)->K2_GetActorLocation();

        for (auto it = winged.begin(); it != winged.end();)
        {
            auto* target = it->object.Get();

            if (target == nullptr || tick - it->seen > kWingTimeoutMs)
            {
                if (target != nullptr)
                {
                    set_movement_mode(target, kMoveFalling);
                }

                it = winged.erase(it);
                continue;
            }

            auto* actor = reinterpret_cast<RC::Unreal::AActor*>(target);
            const auto at = actor->K2_GetActorLocation();

            double goal[3]{};
            double speed = kWingSpeed;

            it->angle += kWingTurn * dt;

            if (is_freddy(target))
            {
                // An allay. Minecraft flies a real one for him and says where
                // it is (kEvActorAt, the captives): then that is where he is.
                if (std::any_of(captives.begin(), captives.end(), [target](const Captive& c) { return c.object.Get() == target; }))
                {
                    ++it;
                    continue;
                }

                // Without one: a wide, slow circle round the player.
                goal[0] = here.X() + std::cos(it->angle * 0.5) * 320.0;
                goal[1] = here.Y() + std::sin(it->angle * 0.5) * 320.0;
                goal[2] = here.Z() + 120.0 + std::sin(it->angle * 2.5) * 15.0;
            }
            else if (it->diving)
            {
                goal[0] = here.X();
                goal[1] = here.Y();
                goal[2] = here.Z();
                speed = kWingDiveSpeed;

                const double dx = here.X() - at.X();
                const double dy = here.Y() - at.Y();
                const double dz = here.Z() - at.Z();

                if (tick > it->dive_until || dx * dx + dy * dy + dz * dz < 90.0 * 90.0)
                {
                    it->diving = false;
                    it->next_dive = tick + kWingDiveEveryMs + (tick % 3000);
                }
            }
            else
            {
                goal[0] = here.X() + std::cos(it->angle) * kWingRadius;
                goal[1] = here.Y() + std::sin(it->angle) * kWingRadius;
                goal[2] = here.Z() + kWingHeight;

                if (tick > it->next_dive)
                {
                    it->diving = true;
                    it->dive_until = tick + kWingDiveMs;
                }
            }

            const double dx = goal[0] - at.X();
            const double dy = goal[1] - at.Y();
            const double dz = goal[2] - at.Z();
            const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);

            if (distance > 1.0)
            {
                const double step = (std::min)(distance, speed * dt) / distance;

                move_actor(actor, at.X() + dx * step, at.Y() + dy * step, at.Z() + dz * step);

                if (dx * dx + dy * dy > 4.0)
                {
                    face(target, static_cast<float>(std::atan2(dy, dx) * 180.0 / 3.14159265358979323846));
                }
            }

            ++it;
        }
    }

    // What the game's characters say is shown by its subtitle widgets
    // (WI_Subtitles_C). Twice a second they are read; a line that wasn't
    // there before goes to Minecraft's chat.
    auto update_subtitles() -> void
    {
        const ULONGLONG tick = ::GetTickCount64();

        if (tick - subtitles_tick < 500)
        {
            return;
        }

        subtitles_tick = tick;

        // FString and TArray are a pointer and two counts.
        struct RawArray
        {
            const std::uint8_t* data;
            std::int32_t count;
            std::int32_t room;
        };

        const auto text_of = [](const RawArray& string) -> std::wstring
        {
            if (string.data == nullptr || string.count <= 1 || string.count > 2048)
            {
                return {};
            }

            // The count includes the closing zero.
            return std::wstring(reinterpret_cast<const wchar_t*>(string.data), static_cast<std::size_t>(string.count - 1));
        };

        std::vector<RC::Unreal::UObject*> widgets;

        RC::Unreal::UObjectGlobals::FindAllOf(STR("WI_Subtitles_C"), widgets);

        int widgets_seen = 0;
        int widgets_with_text = 0;

        for (auto* widget : widgets)
        {
            if (widget == nullptr)
            {
                continue;
            }

            auto* lines = widget->GetValuePtrByPropertyNameInChain<RawArray>(STR("P_Lines"));
            auto* source = widget->GetValuePtrByPropertyNameInChain<RawArray>(STR("P_Source"));
            auto* time = widget->GetValuePtrByPropertyNameInChain<float>(STR("P_Time"));

            auto known = std::find_if(spoken.begin(), spoken.end(), [widget](const Spoken& s) { return s.widget.Get() == widget; });

            if (known == spoken.end())
            {
                Spoken fresh;

                fresh.widget = widget;
                spoken.push_back(fresh);
                known = spoken.end() - 1;
            }

            // What is on screen is the surest: the widget's own text block.
            // Its text is only turned into a string when it was set anew.
            std::vector<std::wstring> shown;
            bool said_again = false;

            static auto* get_text = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(
                nullptr, nullptr, STR("/Script/UMG.RichTextBlock:GetText"));
            static auto* to_string = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(
                nullptr, nullptr, STR("/Script/Engine.KismetTextLibrary:Conv_TextToString"));
            static auto* text_library = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UObject*>(
                nullptr, nullptr, STR("/Script/Engine.Default__KismetTextLibrary"));

            if (auto* block = widget->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(STR("RichText"));
                block != nullptr && *block != nullptr && get_text != nullptr && to_string != nullptr && text_library != nullptr)
            {
                // (FText InText, FString ReturnValue): an FText is 0x18 bytes,
                // its first pointer is the text itself.
                alignas(16) std::uint8_t params[0x40]{};

                (*block)->ProcessEvent(get_text, params);

                const void* text_data = nullptr;

                std::memcpy(&text_data, params, sizeof(text_data));

                if (text_data != nullptr && text_data != known->rich_data)
                {
                    known->rich_data = text_data;

                    text_library->ProcessEvent(to_string, params);

                    RawArray string{};

                    std::memcpy(&string, params + 0x18, sizeof(string));

                    std::vector<std::wstring> fresh_lines;
                    std::wstring line;

                    for (const wchar_t c : text_of(string) + L"\n")
                    {
                        if (c == L'\n' || c == L'\r')
                        {
                            if (!line.empty())
                            {
                                fresh_lines.push_back(line);
                                line.clear();
                            }
                        }
                        else
                        {
                            line += c;
                        }
                    }

                    // Set anew to the very same words: the line was said again.
                    said_again = !fresh_lines.empty() && fresh_lines == known->rich_lines;
                    known->rich_lines = std::move(fresh_lines);
                }

                shown = known->rich_lines;
            }

            // Otherwise S_SubtitleLine: a start time, then the text. The widget
            // shows a few lines at a time and drops the ones that were said.
            if (shown.empty() && lines != nullptr && lines->data != nullptr && lines->count > 0 && lines->count <= 64)
            {
                for (std::int32_t i = 0; i < lines->count; ++i)
                {
                    RawArray string{};

                    std::memcpy(&string, lines->data + i * 0x18 + 0x8, sizeof(string));

                    if (auto line = text_of(string); !line.empty())
                    {
                        shown.push_back(std::move(line));
                    }
                }
            }

            // Short ones may only have the plain list of strings.
            if (auto* strings = widget->GetValuePtrByPropertyNameInChain<RawArray>(STR("P_LineStrings"));
                shown.empty() && strings != nullptr && strings->data != nullptr && strings->count > 0 && strings->count <= 64)
            {
                for (std::int32_t i = 0; i < strings->count; ++i)
                {
                    RawArray string{};

                    std::memcpy(&string, strings->data + i * 0x10, sizeof(string));

                    if (auto line = text_of(string); !line.empty())
                    {
                        shown.push_back(std::move(line));
                    }
                }
            }

            ++widgets_seen;

            if (shown.empty())
            {
                continue;
            }

            ++widgets_with_text;

            const float now_at = time != nullptr ? *time : 0.0f;
            // The same words again count when the widget's clock started over.
            const bool again = said_again || now_at + 0.25f < known->time;

            known->time = now_at;

            // Only the lines that weren't there the last time.
            std::wstring text;

            for (const auto& line : shown)
            {
                if (again || std::find(known->lines.begin(), known->lines.end(), line) == known->lines.end())
                {
                    text += text.empty() ? L"" : L" ";
                    text += line;
                }
            }

            known->lines = std::move(shown);

            if (text.empty())
            {
                continue;
            }

            // Rich text marks (<Italic>...</>) aren't words.
            std::wstring plain;
            bool in_mark = false;

            for (const wchar_t c : text)
            {
                if (c == L'<')
                {
                    in_mark = true;
                }
                else if (c == L'>' && in_mark)
                {
                    in_mark = false;
                }
                else if (!in_mark)
                {
                    plain += c;
                }
            }

            const auto who = source != nullptr ? text_of(*source) : std::wstring{};
            const std::wstring message = (who.empty() ? L"" : L"<" + who + L"> ") + plain;

            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP said: {}\n"),
                message
            );

            // Twelve characters per message, latin-1.
            const std::uint16_t code = chat_line++;

            for (std::size_t at = 0; at < message.size() && at < 240; at += 12)
            {
                std::int32_t words[3]{};

                for (std::size_t i = 0; i < 12 && at + i < message.size(); ++i)
                {
                    wchar_t c = message[at + i];

                    // Curly quotes and the long dash have plain cousins.
                    c = c == 0x2019 || c == 0x2018 ? L'\'' : c == 0x201C || c == 0x201D ? L'"' : c == 0x2014 || c == 0x2013 ? L'-' : c == 0x2026 ? L'.' : c;

                    words[i / 4] |= static_cast<std::int32_t>((c < 256 ? c : L'?') & 0xFF) << ((i % 4) * 8);
                }

                g_link.push_input(skycraft::proto::kInChatText, code, words[0], words[1], words[2]);
            }

            g_link.push_input(skycraft::proto::kInChatSay, code);
        }

        spoken.erase(
            std::remove_if(spoken.begin(), spoken.end(), [](const Spoken& s) { return s.widget.Get() == nullptr; }),
            spoken.end()
        );

        // To learn why a character's line didn't make it: was there a widget
        // with its words at all?
        static ULONGLONG last_count = 0;

        if (tick - last_count > 10000)
        {
            last_count = tick;

            RC::Output::send<RC::LogLevel::Verbose>(
                STR("SBMP subtitles: {} widgets, {} with text\n"),
                widgets_seen,
                widgets_with_text
            );
        }
    }

    // Characters on a lead follow the player: further than the lead is
    // long and they are pulled along, stopping at walls.
    auto update_leads(RC::Unreal::UObject* player) -> void
    {
        const ULONGLONG tick = ::GetTickCount64();

        for (auto it = leashed.begin(); it != leashed.end();)
        {
            auto* target = it->object.Get();

            // Minecraft repeats where the other end is every tick; silence
            // means the lead is gone.
            if (target == nullptr || tick - it->seen > kLeadTimeoutMs)
            {
                it = leashed.erase(it);
                continue;
            }

            auto* actor = reinterpret_cast<RC::Unreal::AActor*>(target);
            const auto at = actor->K2_GetActorLocation();

            const double dx = it->post[0] - at.X();
            const double dy = it->post[1] - at.Y();
            const double dz = it->post[2] - at.Z();
            const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);

            if (distance > kLeadLength)
            {
                // Onto the end of the lead. Height only follows when the
                // other end is well above (the player flies): then it dangles.
                const double pull = (distance - kLeadLength) / distance;
                const bool lifted = dz > kLeadLength;

                RC::Unreal::FVector to{
                    static_cast<float>(at.X() + dx * pull),
                    static_cast<float>(at.Y() + dy * pull),
                    static_cast<float>(lifted ? at.Z() + dz * pull : at.Z())
                };

                RC::Unreal::FHitResult sweep_hit{};

                actor->K2_SetActorLocation(to, true, sweep_hit, false);
            }

            ++it;
        }
    }

    // A character whose stand-in sits in a boat or minecart stays where
    // that is, for as long as Minecraft keeps saying so.
    auto hold_captive(RC::Unreal::UObject* target, double x, double y, double z) -> void
    {
        const ULONGLONG tick = ::GetTickCount64();
        auto known = std::find_if(captives.begin(), captives.end(), [target](const Captive& c) { return c.object.Get() == target; });

        if (known == captives.end())
        {
            Captive captive;

            captive.object = target;
            captives.push_back(captive);
            known = captives.end() - 1;

            set_movement_mode(target, kMoveFlying);

            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP captive: Minecraft carries {} (a boat, a minecart, an allay or a chicken)\n"),
                target->GetFullName()
            );
        }

        known->seen = tick;

        move_actor(
            reinterpret_cast<RC::Unreal::AActor*>(target),
            mc_to_sb_x(x),
            mc_to_sb_z(z),
            mc_to_sb_y(y) + capsule_half_height(target)
        );
    }

    // Lets go of the ones Minecraft stopped reporting (the boat broke).
    auto release_captives() -> void
    {
        const ULONGLONG tick = ::GetTickCount64();

        for (auto it = captives.begin(); it != captives.end();)
        {
            auto* target = it->object.Get();

            if (target == nullptr || tick - it->seen > kCaptiveTimeoutMs)
            {
                if (target != nullptr)
                {
                    set_movement_mode(target, kMoveFalling);
                }

                it = captives.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    static auto is_freddy(RC::Unreal::UObject* who) -> bool
    {
        return lower(who->GetClassPrivate()->GetName()).find(L"freddy") != std::wstring::npos;
    }

    // While the player rides Freddy's mule in Minecraft, Freddy is kept
    // under the rider: Minecraft's mule does the moving, Freddy is its body.
    auto update_ride(const skycraft::proto::McState& mc, float yaw) -> void
    {
        const bool riding = minecraft_mode && driving && (mc.flags & skycraft::proto::kMcRidingHost) != 0;
        auto* target = freddy.Get();

        // Minecraft's player sits on the mule but which Freddy it is got
        // lost (a new level, a restart): the one that is around.
        if (riding && target == nullptr)
        {
            RC::Unreal::UObject* found = nullptr;

            actors::for_each(
                [](RC::Unreal::UObject* character, void* context)
                {
                    if (is_freddy(character))
                    {
                        *static_cast<RC::Unreal::UObject**>(context) = character;
                    }
                },
                &found
            );

            if (found != nullptr)
            {
                freddy = found;
                target = found;
            }
        }

        if (!riding || target == nullptr)
        {
            if (riding_freddy)
            {
                riding_freddy = false;

                // Back on his own feet, his own movement.
                if (target != nullptr)
                {
                    set_movement_mode(target, kMoveFalling);
                }

                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP ride: got off Freddy\n")
                );
            }

            return;
        }

        auto* actor = reinterpret_cast<RC::Unreal::AActor*>(target);
        const double half = capsule_half_height(target);

        if (!riding_freddy)
        {
            riding_freddy = true;
            set_movement_mode(target, kMoveFlying);

            // How far above Freddy's feet Minecraft seats its rider: measured
            // once, while he still stands where the mule was put.
            // Kept for the next time: after the first ride Freddy is where
            // the rider put him, not where the mule was put.
            const auto at = actor->K2_GetActorLocation();
            const double measured = mc_to_sb_y(mc.y) - (at.Z() - half);

            if (seat_height < 30.0 && measured > 30.0)
            {
                seat_height = measured;
            }
            else if (seat_height < 30.0)
            {
                seat_height = kDefaultSeat;
            }

            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP ride: on Freddy, the seat is {} units up\n"),
                seat_height
            );
        }

        move_actor(actor, mc_to_sb_x(mc.x), mc_to_sb_z(mc.z), mc_to_sb_y(mc.y) - seat_height + half);

        // He faces where the rider looks.
        struct
        {
            float NewRotation[3];
            bool bTeleportPhysics;
            bool ReturnValue;
        } turn{ { 0.0f, yaw, 0.0f }, true, false };

        if (auto* rotate = target->GetFunctionByNameInChain(STR("K2_SetActorRotation")))
        {
            target->ProcessEvent(rotate, &turn);
        }
    }

    // A broken object isn't destroyed: it is hidden and stops colliding, and
    // Minecraft drops a paper with its name. Using that paper on the floor
    // puts the very same object back (put_back), so nothing about it is lost.
    auto take_away(RC::Unreal::UObject* target, double x, double y, double z, bool in_bucket = false) -> void
    {
        call_with(target, STR("SetActorHiddenInGame"), true);
        call_with(target, STR("SetActorEnableCollision"), false);

        if (in_bucket)
        {
            // A character would walk on unseen: it stops thinking and moving,
            // and Minecraft keeps no stand-in for it.
            call_with(target, STR("SetActorTickEnabled"), false);
            set_movement_mode(target, kMoveNone);
            actors::ignore(target, true);
        }

        const std::uint32_t id = make_token(target, in_bucket, true);

        if (in_bucket)
        {
            g_link.push_input(skycraft::proto::kInBucketToken, static_cast<std::uint16_t>(id));
            return;
        }

        g_link.push_input(
            skycraft::proto::kInDropToken,
            static_cast<std::uint16_t>(id),
            static_cast<std::int32_t>(sb_to_mc_x(x) * 100.0),
            static_cast<std::int32_t>(sb_to_mc_y(z) * 100.0),
            static_cast<std::int32_t>(sb_to_mc_z(y) * 100.0)
        );
    }

    // Remembers an object or character under a new number and tells
    // Minecraft its name. `taken`: it is gone from the level until it is put
    // back; otherwise the number only stands for copies of it.
    auto make_token(RC::Unreal::UObject* target, bool is_character, bool taken) -> std::uint32_t
    {
        Token token;

        token.character = is_character;
        token.taken_away = taken;
        token.id = next_token++;
        token.object = target;
        token.object_class = target->GetClassPrivate();

        // A plain static mesh actor is nothing without its mesh.
        if (auto* component = target->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(STR("StaticMeshComponent")); component != nullptr && *component != nullptr)
        {
            if (auto* mesh = (*component)->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(STR("StaticMesh")); mesh != nullptr)
            {
                token.mesh = *mesh;
            }
        }
        tokens.push_back(token);

        // "MOD_SpaceFreddy_Cutout4" -> "SpaceFreddy Cutout"
        std::wstring name = target->GetName();

        for (const wchar_t* prefix : { L"PRE_MOD_", L"MOD_", L"PRE_", L"SM_", L"BP_" })
        {
            if (name.rfind(prefix, 0) == 0)
            {
                name.erase(0, std::wcslen(prefix));
                break;
            }
        }

        while (!name.empty() && (std::iswdigit(name.back()) || name.back() == L'_'))
        {
            name.pop_back();
        }

        for (auto& c : name)
        {
            if (c == L'_')
            {
                c = L' ';
            }
        }

        // Twelve characters per message, latin-1.
        for (std::size_t at = 0; at < name.size() && at < 48; at += 12)
        {
            std::int32_t words[3]{};

            for (std::size_t i = 0; i < 12 && at + i < name.size(); ++i)
            {
                const wchar_t c = name[at + i];

                words[i / 4] |= static_cast<std::int32_t>((c < 256 ? c : L'?') & 0xFF) << ((i % 4) * 8);
            }

            g_link.push_input(skycraft::proto::kInTokenName, static_cast<std::uint16_t>(token.id), words[0], words[1], words[2]);
        }

        return token.id;
    }

    // Creative, a short click of the middle button: Minecraft's "pick block"
    // for the game's things. A character gives its spawn egg, something that
    // can be broken gives its paper; both make copies and leave it standing.
    auto pick_target(RC::Unreal::UObject* character) -> void
    {
        if (g_link.minecraft_targets_block())
        {
            return;
        }

        double to[3]{};

        for (int i = 0; i < 3; ++i)
        {
            to[i] = view.position[i] + view.forward[i] * kUseReach;
        }

        RC::Unreal::UObject* part = nullptr;
        double impact[3]{};
        auto* target = collision::trace(character, view.position, to, kAttackThickness, &part, impact);

        if (target == nullptr)
        {
            return;
        }

        const bool is_character = target->GetFunctionByNameInChain(STR("GetController")) != nullptr;

        if (!is_character && break_hits(target, part) == 0)
        {
            return;
        }

        const std::uint32_t id = make_token(target, is_character, false);

        g_link.push_input(skycraft::proto::kInPickToken, static_cast<std::uint16_t>(id), is_character ? 1 : 0);

        RC::Output::send<RC::LogLevel::Default>(
            STR("SBMP pick: {} of {}\n"),
            is_character ? STR("the spawn egg") : STR("the paper"),
            target->GetFullName()
        );
    }

    // The paper of a broken object, used on the floor: the object stands
    // there again, facing the player.
    auto put_back(std::uint32_t id, RC::Unreal::UObject* character) -> bool
    {
        auto token = std::find_if(tokens.begin(), tokens.end(), [id](const Token& t) { return t.id == id; });

        if (token == tokens.end())
        {
            return false;
        }

        double to[3]{};

        for (int i = 0; i < 3; ++i)
        {
            to[i] = view.position[i] + view.forward[i] * kUseReach;
        }

        double impact[3]{};

        if (collision::trace(character, view.position, to, 0.0, nullptr, impact) == nullptr)
        {
            return false;
        }

        auto* target = token->object.Get();
        const bool original = token->taken_away && target != nullptr;

        if (original)
        {
            // A character stands on the spot; an object's own origin goes there.
            const double lift = token->character ? capsule_half_height(target) + 5.0 : 0.0;

            move_actor(reinterpret_cast<RC::Unreal::AActor*>(target), impact[0], impact[1], impact[2] + lift);
            token->taken_away = false;

            if (token->character)
            {
                call_with(target, STR("SetActorTickEnabled"), true);
                set_movement_mode(target, kMoveFalling);
                actors::ignore(target, false);
            }
        }
        else
        {
            // A copy: the original stands somewhere already, or is gone with
            // the part of the level it was in.
            auto* object_class = token->object_class.Get();

            // A character's origin is its middle: up it goes, or it spawns in the floor.
            const double spot[3] = { impact[0], impact[1], impact[2] + (token->character ? 100.0 : 0.0) };

            target = object_class != nullptr ? spawn_actor(character, object_class, spot) : nullptr;

            if (target == nullptr)
            {
                RC::Output::send<RC::LogLevel::Default>(
                    STR("SBMP token: couldn't make another of paper {}\n"),
                    id
                );

                return false;
            }

            if (auto* mesh = token->mesh.Get())
            {
                if (auto* component = target->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(STR("StaticMeshComponent")); component != nullptr && *component != nullptr)
                {
                    // A level's static meshes can't change; a movable one can.
                    call_with(*component, STR("SetMobility"), std::uint8_t{ 2 });
                    call_with(*component, STR("SetStaticMesh"), mesh);
                }
            }
        }

        // Turned to face the player, like a placed block that has a front.
        const float yaw = static_cast<float>(std::atan2(-view.forward[1], -view.forward[0]) * 180.0 / 3.14159265358979323846);

        struct
        {
            float NewRotation[3];
            bool bTeleportPhysics;
            bool ReturnValue;
        } turn{ { 0.0f, yaw, 0.0f }, true, false };

        if (auto* rotate = target->GetFunctionByNameInChain(STR("K2_SetActorRotation")))
        {
            target->ProcessEvent(rotate, &turn);
        }

        call_with(target, STR("SetActorEnableCollision"), true);
        call_with(target, STR("SetActorHiddenInGame"), false);

        g_link.push_input(skycraft::proto::kInUseToken, static_cast<std::uint16_t>(id));
        g_link.push_input(skycraft::proto::kInSwing, 0);

        RC::Output::send<RC::LogLevel::Default>(
            STR("SBMP token: {} {}\n"),
            original ? STR("put back") : STR("made a copy of"),
            target->GetFullName()
        );

        return true;
    }

    // Creative: the middle button picks up a character or a loose object and
    // carries it in front of the camera; letting go drops (or throws) it.
    auto update_grab(RC::Unreal::UObject* character) -> void
    {
        // A short click picks (like Minecraft's pick block); holding carries.
        const bool down = creative_mode && driving && view.valid && input::grab_held();
        const ULONGLONG grab_now = ::GetTickCount64();

        if (down && !grab_was_down)
        {
            grab_down_tick = grab_now;
        }
        else if (!down && grab_was_down && grab_now - grab_down_tick < kPickClickMs && creative_mode && view.valid)
        {
            pick_target(character);
        }

        grab_was_down = down;

        const bool held = down && grab_now - grab_down_tick >= kPickClickMs;

        if (!held)
        {
            if (auto* pawn = grabbed_pawn.Get())
            {
                set_movement_mode(pawn, kMoveFalling);
            }

            grabbed_pawn = RC::Unreal::FWeakObjectPtr{};
            grabbed_part = RC::Unreal::FWeakObjectPtr{};
            grab_tried = false;
            return;
        }

        if (grabbed_pawn.Get() == nullptr && grabbed_part.Get() == nullptr)
        {
            if (grab_tried)
            {
                return;
            }

            // One look per press.
            grab_tried = true;

            double to[3]{};

            for (int i = 0; i < 3; ++i)
            {
                to[i] = view.position[i] + view.forward[i] * kUseReach;
            }

            RC::Unreal::UObject* part = nullptr;
            double impact[3]{};
            auto* target = collision::trace(character, view.position, to, kAttackThickness, &part, impact);

            if (target == nullptr)
            {
                return;
            }

            const double dx = impact[0] - view.position[0];
            const double dy = impact[1] - view.position[1];
            const double dz = impact[2] - view.position[2];

            grab_distance = (std::max)(std::sqrt(dx * dx + dy * dy + dz * dz), kGrabMinDistance);

            if (target->GetFunctionByNameInChain(STR("GetController")) != nullptr)
            {
                grabbed_pawn = target;
                set_movement_mode(target, kMoveFlying);
            }
            else if (part != nullptr && is_simulating(part))
            {
                grabbed_part = part;
            }
            else
            {
                return;
            }

            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP grab: picked up {}\n"),
                target->GetFullName()
            );
        }

        const double hold[3] = {
            view.position[0] + view.forward[0] * grab_distance,
            view.position[1] + view.forward[1] * grab_distance,
            view.position[2] + view.forward[2] * grab_distance,
        };

        if (auto* pawn = grabbed_pawn.Get())
        {
            // A character: hangs there, its feet off the ground.
            move_actor(reinterpret_cast<RC::Unreal::AActor*>(pawn), hold[0], hold[1], hold[2]);
            return;
        }

        if (auto* part = grabbed_part.Get())
        {
            // A physics object: pulled towards the spot, so it still bumps
            // into things, and flies on when let go.
            float where[3]{};

            if (!call_getter(part, STR("K2_GetComponentLocation"), where))
            {
                return;
            }

            auto* push = part->GetFunctionByNameInChain(STR("SetPhysicsLinearVelocity"));

            if (push == nullptr)
            {
                return;
            }

            // (FVector NewVel, bool bAddToCurrent, FName BoneName)
            struct
            {
                float NewVel[3];
                bool bAddToCurrent;
                std::uint8_t padding[3];
                std::uint32_t BoneName[2];
            } params{
                {
                    static_cast<float>((hold[0] - where[0]) * kGrabPull),
                    static_cast<float>((hold[1] - where[1]) * kGrabPull),
                    static_cast<float>((hold[2] - where[2]) * kGrabPull)
                },
                false,
                {},
                { 0, 0 }
            };

            part->ProcessEvent(push, &params);
        }
    }

    // UPrimitiveComponent::IsSimulatingPhysics(None).
    static auto is_simulating(RC::Unreal::UObject* component) -> bool
    {
        auto* simulating = component->GetFunctionByNameInChain(STR("IsSimulatingPhysics"));

        if (simulating == nullptr)
        {
            return false;
        }

        struct
        {
            std::uint32_t BoneName[2];
            bool ReturnValue;
        } query{};

        component->ProcessEvent(simulating, &query);

        return query.ReturnValue;
    }

    // The building itself is never broken, only things that stand in it.
    // The level's big merged meshes, floors, walls and colliders are out by
    // name, and so is anything larger than a prop (over ~14 blocks across).
    static auto is_loose_prop(RC::Unreal::UObject* actor) -> bool
    {
        static constexpr const wchar_t* kBuilding[] = {
            L"merged", L"floor", L"wall", L"ceiling", L"collider", L"blockingvolume", L"landscape", L"stair",
            L"platform", L"ramp", L"pillar", L"column", L"ground", L"roof", L"bridge", L"elevator"
        };

        const auto name = lower(actor->GetFullName());

        for (const auto* word : kBuilding)
        {
            if (name.find(word) != std::wstring::npos)
            {
                return false;
            }
        }

        auto* bounds = actor->GetFunctionByNameInChain(STR("GetActorBounds"));

        if (bounds == nullptr)
        {
            return false;
        }

        struct
        {
            bool bOnlyCollidingComponents;
            float Origin[3];
            float BoxExtent[3];
            bool bIncludeFromChildActors;
        } query{};

        actor->ProcessEvent(bounds, &query);

        return query.BoxExtent[0] < kMaxPropExtent && query.BoxExtent[1] < kMaxPropExtent && query.BoxExtent[2] < kMaxPropExtent;
    }

    // How many hits it takes in survival to break this, 0 if it can't be
    // broken: the cardboard cutouts, fences, and statues and doors (a lot
    // more work).
    static auto break_hits(RC::Unreal::UObject* actor, RC::Unreal::UObject* component) -> int
    {
        if (is_cutout(actor, component))
        {
            return kBreakHits;
        }

        if (is_fence(actor, component))
        {
            return kFenceBreakHits;
        }

        if (is_statue(actor, component))
        {
            return kStatueBreakHits;
        }

        if (is_furniture(actor, component))
        {
            return kFenceBreakHits;
        }

        return actor->GetFunctionByNameInChain(STR("ForceDoorOpen")) != nullptr ? kDoorBreakHits : 0;
    }

    // The cardboard cutouts: recognised by the name of the actor or of the
    // mesh it shows. Only things that stand in the level on their own.
    static auto is_cutout(RC::Unreal::UObject* actor, RC::Unreal::UObject* component) -> bool
    {
        static constexpr const wchar_t* kWords[] = { L"cutout", L"cut_out", L"standee", L"cardboard" };

        return is_named(actor, component, kWords);
    }

    // Fences, railings and rope barriers that stand in the level as their
    // own object. A guess by name, like the cutouts.
    static auto is_fence(RC::Unreal::UObject* actor, RC::Unreal::UObject* component) -> bool
    {
        static constexpr const wchar_t* kWords[] = { L"fence", L"railing", L"barricade", L"stanchion", L"rope_barrier", L"ropebarrier", L"barrier" };

        return is_named(actor, component, kWords) && is_loose_prop(actor);
    }

    static auto is_statue(RC::Unreal::UObject* actor, RC::Unreal::UObject* component) -> bool
    {
        static constexpr const wchar_t* kWords[] = { L"statue", L"sculpture", L"mannequin", L"bust_" };

        return is_named(actor, component, kWords) && is_loose_prop(actor);
    }

    // Furniture and decoration: plants, tables, chairs, bins, signs,
    // balloons, arcade machines. Also a guess by name; nothing a player
    // stands on or needs (no counters, platforms, presents or boxes).
    static auto is_furniture(RC::Unreal::UObject* actor, RC::Unreal::UObject* component) -> bool
    {
        static constexpr const wchar_t* kWords[] = {
            L"palmtree", L"plant", L"table", L"chair", L"stool", L"bench", L"trash", L"sign", L"balloon",
            L"arcade", L"stroller", L"cone", L"poster", L"banner"
        };

        return is_named(actor, component, kWords) && is_loose_prop(actor);
    }

    // The name of the actor, or of the mesh it shows, has one of the words.
    template <std::size_t N>
    static auto is_named(RC::Unreal::UObject* actor, RC::Unreal::UObject* component, const wchar_t* const (&words)[N]) -> bool
    {
        const auto named = [&words](RC::Unreal::UObject* object)
        {
            if (object == nullptr)
            {
                return false;
            }

            const auto name = lower(object->GetFullName());

            for (const auto* word : words)
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

    // (FVector or FRotator, bool bSweep, FHitResult& SweepHitResult, bool bTeleport):
    // K2_SetRelativeLocation and K2_SetRelativeRotation of a scene component.
    static auto set_relative(RC::Unreal::UObject* component, const wchar_t* name, float a, float b, float c) -> void
    {
        auto* function = component->GetFunctionByNameInChain(name);

        if (function == nullptr)
        {
            return;
        }

        struct
        {
            float value[3];
            bool bSweep;
            std::uint8_t SweepHitResult[0x88];
            bool bTeleport;
            std::uint8_t padding[0x40];
        } params{};

        params.value[0] = a;
        params.value[1] = b;
        params.value[2] = c;
        params.bTeleport = true;

        component->ProcessEvent(function, &params);
    }

    // Turns a character's body upside down (or back): its mesh is rolled
    // over and lifted by its own height, the collision capsule stays as it is.
    auto flip_character(RC::Unreal::UObject* target) -> bool
    {
        auto* mesh = target->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(STR("Mesh"));

        if (mesh == nullptr || *mesh == nullptr)
        {
            return false;
        }

        auto* place = (*mesh)->GetValuePtrByPropertyNameInChain<float>(STR("RelativeLocation"));
        auto* turn = (*mesh)->GetValuePtrByPropertyNameInChain<float>(STR("RelativeRotation"));

        if (place == nullptr || turn == nullptr)
        {
            return false;
        }

        // Already upside down: put it back.
        for (auto it = flipped.begin(); it != flipped.end(); ++it)
        {
            if (it->object.Get() != target)
            {
                continue;
            }

            // FRotator is pitch, yaw, roll.
            set_relative(*mesh, STR("K2_SetRelativeRotation"), turn[0], turn[1], turn[2] - 180.0f);
            set_relative(*mesh, STR("K2_SetRelativeLocation"), it->home[0], it->home[1], it->home[2]);
            flipped.erase(it);

            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP name tag: {} is the right way up again\n"),
                target->GetFullName()
            );

            return true;
        }

        // The mesh hangs at the character's feet, so rolled over it would
        // point into the floor: mirror its place in the capsule, which puts
        // the feet where the head was.
        Flipped flip;

        flip.object = target;
        flip.home[0] = place[0];
        flip.home[1] = place[1];
        flip.home[2] = place[2];
        flip.lifted = place[2] < 0.0f ? -place[2] : place[2] + static_cast<float>(capsule_half_height(target) * 2.0);

        set_relative(*mesh, STR("K2_SetRelativeRotation"), turn[0], turn[1], turn[2] + 180.0f);
        set_relative(*mesh, STR("K2_SetRelativeLocation"), flip.home[0], flip.home[1], flip.lifted);

        RC::Output::send<RC::LogLevel::Default>(
            STR("SBMP name tag: {} is upside down (mesh height {} -> {})\n"),
            target->GetFullName(),
            flip.home[2],
            flip.lifted
        );

        flipped.push_back(flip);

        return true;
    }

    // The game puts a character's mesh back in its usual place while it
    // animates, which sinks an upside-down one into the floor. This runs
    // after the game's own tick and lifts them again.
    auto hold_flipped() -> void
    {
        for (auto it = flipped.begin(); it != flipped.end();)
        {
            auto* target = it->object.Get();

            if (target == nullptr)
            {
                it = flipped.erase(it);
                continue;
            }

            if (auto* mesh = target->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(STR("Mesh")); mesh != nullptr && *mesh != nullptr)
            {
                auto* place = (*mesh)->GetValuePtrByPropertyNameInChain<float>(STR("RelativeLocation"));

                if (place != nullptr && std::abs(place[2] - it->lifted) > 0.5f)
                {
                    set_relative(*mesh, STR("K2_SetRelativeLocation"), it->home[0], it->home[1], it->lifted);
                }
            }

            ++it;
        }
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

        // Carried by Minecraft (a chicken jockey, a boat, an allay): it is
        // Minecraft that throws the carrier, and the character rides along.
        // Thrown here as well, it would fly off and leave its chicken behind.
        if (std::any_of(captives.begin(), captives.end(), [target](const Captive& c) { return c.object.Get() == target; }))
        {
            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP hit: {} is carried by Minecraft, its ride is thrown instead\n"),
                target->GetFullName()
            );

            return;
        }

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
            if (event.type == skycraft::proto::kEvRedstone)
            {
                const std::array<int, 3> block{
                    static_cast<int>(std::lround(event.a)),
                    static_cast<int>(std::lround(event.b)),
                    static_cast<int>(std::lround(event.c))
                };

                const auto known = std::find(redstone.begin(), redstone.end(), block);

                if (event.flags != 0 && known == redstone.end())
                {
                    redstone.push_back(block);
                }
                else if (event.flags == 0 && known != redstone.end())
                {
                    redstone.erase(known);
                }

                continue;
            }

            if (event.type == skycraft::proto::kEvActorAt)
            {
                // Stuck in a boat or minecart in Minecraft: that is where it is.
                if (auto* target = actors::find(event.formId))
                {
                    hold_captive(target, event.a, event.b, event.c);
                }

                continue;
            }

            if (event.type == skycraft::proto::kEvLeash)
            {
                // Minecraft's lead on a character: where its other end is
                // (the player's hand or a knot on a fence), or that it is off.
                auto* target = actors::find(event.formId);
                auto known = std::find_if(leashed.begin(), leashed.end(), [target](const Lead& l) { return l.object.Get() == target; });

                if (target == nullptr || (event.flags & 1) == 0)
                {
                    if (target != nullptr && known != leashed.end())
                    {
                        leashed.erase(known);

                        RC::Output::send<RC::LogLevel::Default>(
                            STR("SBMP lead: {} is off the lead\n"),
                            target->GetFullName()
                        );
                    }

                    continue;
                }

                if (known == leashed.end())
                {
                    Lead lead;

                    lead.object = target;
                    leashed.push_back(lead);
                    known = leashed.end() - 1;

                    RC::Output::send<RC::LogLevel::Default>(
                        STR("SBMP lead: {} is on the lead\n"),
                        target->GetFullName()
                    );
                }

                known->post[0] = mc_to_sb_x(event.a);
                known->post[1] = mc_to_sb_z(event.c);
                known->post[2] = mc_to_sb_y(event.b);
                known->seen = ::GetTickCount64();

                continue;
            }

            // Everything else only means something while the player is Steve.
            if (!minecraft_mode)
            {
                continue;
            }

            if (event.type == skycraft::proto::kEvActorPush)
            {
                if (auto* target = actors::find(event.formId))
                {
                    push_character(target, event.a, event.b, event.c, event.flags);
                }

                continue;
            }

            if (event.type == skycraft::proto::kEvActorWings)
            {
                if (auto* target = actors::find(event.formId))
                {
                    set_wings(target, (event.flags & 1) != 0);
                }

                continue;
            }

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

                break_with_blast(blast.centre, blast.reach * 0.6);
            }
        }
    }

    // An explosion breaks what a hand can break (cutouts, doors, fences,
    // statues) and what it can see: rays go out from its centre in 26
    // directions, and the first thing each one meets breaks if it is of
    // that kind. So a wall shelters what is behind it, and the building
    // itself is never touched.
    auto break_with_blast(const double centre[3], double reach) -> void
    {
        auto* player = last_character;

        if (player == nullptr)
        {
            return;
        }

        std::vector<RC::Unreal::UObject*> broken;

        for (int dx = -1; dx <= 1; ++dx)
        {
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dz = -1; dz <= 1; ++dz)
                {
                    if (dx == 0 && dy == 0 && dz == 0)
                    {
                        continue;
                    }

                    const double length = std::sqrt(static_cast<double>(dx * dx + dy * dy + dz * dz));
                    const double to[3] = {
                        centre[0] + dx / length * reach,
                        centre[1] + dy / length * reach,
                        centre[2] + dz / length * reach,
                    };

                    RC::Unreal::UObject* part = nullptr;
                    auto* target = collision::trace(player, centre, to, kAttackThickness, &part);

                    if (
                        target == nullptr ||
                        target->GetFunctionByNameInChain(STR("GetController")) != nullptr ||
                        break_hits(target, part) == 0 ||
                        std::find(broken.begin(), broken.end(), target) != broken.end()
                    )
                    {
                        continue;
                    }

                    broken.push_back(target);

                    const auto spot = reinterpret_cast<RC::Unreal::AActor*>(target)->K2_GetActorLocation();

                    RC::Output::send<RC::LogLevel::Default>(
                        STR("SBMP explosion: broke {}\n"),
                        target->GetFullName()
                    );

                    g_link.push_input(
                        skycraft::proto::kInBreakEffect,
                        0,
                        static_cast<std::int32_t>(sb_to_mc_x(spot.X()) * 100.0),
                        static_cast<std::int32_t>(sb_to_mc_y(spot.Z()) * 100.0),
                        static_cast<std::int32_t>(sb_to_mc_z(spot.Y()) * 100.0)
                    );

                    take_away(target, spot.X(), spot.Y(), spot.Z());
                }
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