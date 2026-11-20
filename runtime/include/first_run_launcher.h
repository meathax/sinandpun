#pragma once

#include <filesystem>

// First-run launcher: shown instead of the fatal "No DVD root is configured" error. Asks for the user's disc
// image, imports it into <UserData>/Game (see disc_import.h) and points paths.dvd_root at it.
namespace first_run_launcher {

// <ApplicationData>/Game, the app-owned extraction target.
std::filesystem::path ManagedGameDirectory();

// True when paths.dvd_root resolves to a usable extracted game (sys/{fst.bin,main.dol} + files/).
bool GameDataAvailable();

// Runs the launcher in the already-initialised Aurora window until the import has finished and the user chose
// to continue. Returns true when game data is ready. Closing the window exits the process.
// Call after aurora_initialize and before the GX worker starts.
bool Run();

}  // namespace first_run_launcher
