#pragma once

#include <filesystem>
#include <string>

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
using PrepareFolder = bool (*)(const std::filesystem::path&, std::string&);
bool Run(PrepareFolder prepareFolder);

// A disc image or folder to import as soon as Run starts (the launcher's --game for a game that was never prepared).
void SetInitialImage(const std::filesystem::path& image);

}  // namespace first_run_launcher
