#pragma once
// Capture-only autoplay ("trailer bot"). SIN_TRAILER_BOT=1 replaces both remotes' input with an
// enemy-aware player that aims like a person: it reads the game's own lock-on targets (ObjHit
// records), projects them with the gameplay camera and steers an absolute pointer with spring
// dynamics, reaction delay and target stickiness. Off unless the variable is set.

namespace TrailerBot {

struct Output {
    bool a = false, b = false, plus = false, c = false, z = false;
    bool pointer = true;       // false: leave the pointer to the remote's relative path (menus)
    float u = 0.5f, v = 0.5f;  // absolute pointer, screen fractions
    float lx = 0.f, ly = 0.f;  // Nunchuk stick, SDL convention (+y down)
};

bool Enabled();
// port 0 = P1, 1 = P2. Returns false when the bot is off.
bool Sample(int port, Output& out);

} // namespace TrailerBot
