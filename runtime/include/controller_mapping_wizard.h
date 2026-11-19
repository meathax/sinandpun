#pragma once

#include "game_ui.h"

#include <SDL3/SDL_events.h>

#include <cstdint>
#include <string>
#include <vector>

// Press-to-bind setup for joysticks SDL either doesn't recognize as gamepads or
// recognizes with a mapping that lacks the analog stick (e.g. raphnet adapters).
// The wizard produces a standard SDL gamepad mapping, applies it live, and
// persists it to gamecontrollerdb.txt in the user data directory.
namespace controller_mapping_wizard {

void LoadPersistedMappings();
void HandleSdlEvent(const SDL_Event& event);

// A connected device that needs setup (no gamepad mapping, or a mapping without an analog stick).
struct Candidate {
    uint32_t id = 0;
    std::string name;
    bool incompleteMapping = false;
};
std::vector<Candidate> Candidates();
void Start(uint32_t id);

// Draws the wizard dialog when active; call once per overlay frame after the menu.
void Draw(game_ui::Frame& input);

bool IsActive();

} // namespace controller_mapping_wizard
