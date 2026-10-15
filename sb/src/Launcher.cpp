#include "Launcher.h"

#include <DynamicOutput/DynamicOutput.hpp>

#include <Windows.h>
#include <TlHelp32.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <string>

// The Minecraft a release ships (SkyCraft's way): FNAFSBCraft-Minecraft.zip next to the mod's
// dll folder holds a portable Prism Launcher with a ready "FNAFSBCraft" instance (Minecraft 26.3,
// Fabric, Fabric API, the FNAFSBCraft mod). It is unpacked to %LOCALAPPDATA%\FNAFSBCraft, outside
// the game, and started into that instance; Prism asks for the Microsoft account once and
// downloads Minecraft and Java itself.
namespace sbmp::launcher
{
    namespace
    {
        std::wstring expand(const std::wstring& path)
        {
            wchar_t buffer[MAX_PATH * 2];
            const DWORD length = ::ExpandEnvironmentStringsW(path.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));

            return length > 0 && length <= std::size(buffer) ? std::wstring(buffer) : path;
        }

        std::filesystem::path install_dir()
        {
            return expand(L"%LOCALAPPDATA%\\FNAFSBCraft");
        }

        // Unpacks the bundle the first time, and again whenever the mod brings a different one.
        // Prism's own data there (the signed-in account, Minecraft and Java, the worlds) is kept;
        // the instance and its mods are replaced, so both halves always match.
        std::filesystem::path ensure_bundle(const std::filesystem::path& bundle)
        {
            const auto dir = install_dir();
            const auto prism = dir / "Prism" / "prismlauncher.exe";
            std::error_code error;
            const auto stamp = std::format(
                "{} {}",
                std::filesystem::file_size(bundle, error),
                std::filesystem::last_write_time(bundle, error).time_since_epoch().count()
            );
            std::string installed;

            if (std::ifstream in{ dir / "bundle.stamp" }; in)
            {
                std::getline(in, installed);
            }

            if (installed == stamp && std::filesystem::exists(prism))
            {
                return prism;
            }

            RC::Output::send<RC::LogLevel::Default>(STR("SBMP launcher: unpacking the bundled Minecraft to {}\n"), dir.wstring());

            std::filesystem::create_directories(dir, error);

            for (const auto& entry : std::filesystem::directory_iterator(dir / "Prism" / "instances" / "FNAFSBCraft" / ".minecraft" / "mods", error))
            {
                const auto name = entry.path().filename().string();

                if (name.starts_with("skycraft-") || name.starts_with("fnafsbcraft-") || name.starts_with("fabric-api-"))
                {
                    std::filesystem::remove(entry.path(), error);
                }
            }

            std::wstring command = L"\"" + expand(L"%SystemRoot%\\System32\\tar.exe") + L"\" -xf \"" + bundle.wstring() + L"\" -C \"" + dir.wstring() + L"\"";
            STARTUPINFOW startup{ sizeof(startup) };
            PROCESS_INFORMATION process{};
            DWORD code = 1;

            if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &startup, &process))
            {
                ::WaitForSingleObject(process.hProcess, 5 * 60 * 1000);
                ::GetExitCodeProcess(process.hProcess, &code);
                ::CloseHandle(process.hThread);
                ::CloseHandle(process.hProcess);
            }

            if (code != 0 || !std::filesystem::exists(prism))
            {
                RC::Output::send<RC::LogLevel::Error>(STR("SBMP launcher: unpacking failed (tar exit code {})\n"), code);
                return {};
            }

            // Prism's settings: only the first time (after that they're the player's).
            const auto settings = dir / "Prism" / "prismlauncher.cfg";

            if (!std::filesystem::exists(settings))
            {
                std::filesystem::copy_file(dir / "defaults" / "prismlauncher.cfg", settings, error);
            }

            std::ofstream(dir / "bundle.stamp") << stamp;

            return prism;
        }
    }

    bool minecraft_running()
    {
        // A Minecraft with the mod holds this mutex while it runs (SkyLink.announceRunning).
        if (HANDLE mutex = ::OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\SkyCraft_v1_minecraft"))
        {
            ::CloseHandle(mutex);
            return true;
        }

        return false;
    }

    std::filesystem::path bundle_next_to(const std::filesystem::path& mod_dir)
    {
        const auto bundle = mod_dir / "FNAFSBCraft-Minecraft.zip";

        return std::filesystem::exists(bundle) ? bundle : std::filesystem::path{};
    }

    bool start_bundled(const std::filesystem::path& bundle)
    {
        const auto prism = ensure_bundle(bundle);

        if (prism.empty())
        {
            return false;
        }

        if (!std::filesystem::exists(prism.parent_path() / "accounts.json"))
        {
            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP launcher: first start: Prism Launcher asks for your Microsoft account (alt-tab to it)\n")
            );
        }

        std::wstring command = L"\"" + prism.wstring() + L"\" --launch FNAFSBCraft";
        const std::wstring dir = prism.parent_path().wstring();
        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION process{};

        if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &startup, &process))
        {
            RC::Output::send<RC::LogLevel::Error>(STR("SBMP launcher: couldn't start Prism Launcher (error {})\n"), ::GetLastError());
            return false;
        }

        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);

        RC::Output::send<RC::LogLevel::Default>(STR("SBMP launcher: started the bundled Minecraft ({})\n"), prism.wstring());
        return true;
    }
}
