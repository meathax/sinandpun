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

// A disc image to import as soon as Run starts (the launcher's --game for a disc whose region was never prepared).
void SetInitialImage(const std::filesystem::path& image);

// Starts the other regional executable for a disc or extracted folder of that region, waits for it and exits this
// process with its status. Returns false (with error) only when the other executable cannot be started.
using RegionHandOff = bool (*)(const std::string& gameId, const std::filesystem::path& game, std::string& error);
void SetRegionHandOff(RegionHandOff handOff);

}  // namespace first_run_launcher
