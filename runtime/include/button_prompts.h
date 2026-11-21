#pragma once

namespace ButtonPrompts {
// Call on the GX worker before loading textures. Returns true after replacing
// the atlas so the caller can invalidate its redundant-bind suppression.
bool Update();
}
