#pragma once

#include <filesystem>

// Starting the Minecraft a release ships (a portable Prism Launcher, see Launcher.cpp).
namespace sbmp::launcher
{
    // A Minecraft with the mod is running already.
    bool minecraft_running();

    // FNAFSBCraft-Minecraft.zip in the mod's folder, or empty if there is none (a build from source).
    std::filesystem::path bundle_next_to(const std::filesystem::path& mod_dir);

    // Unpacks the bundle if needed and starts its Minecraft instance. Off the game thread.
    bool start_bundled(const std::filesystem::path& bundle);
}
