#pragma once

#include <cstdint>

namespace aurora::gfx::pixel_perfect {

enum class GuiProjectionTransition {
  None,
  BeginGui,
  BeginThreeD,
};

struct GuiProjectionTracker {
  bool known = false;
  bool orthographic = false;

  GuiProjectionTransition observe(bool isOrthographic) noexcept {
    if (!known) {
      known = true;
      orthographic = isOrthographic;
      return isOrthographic ? GuiProjectionTransition::BeginGui : GuiProjectionTransition::BeginThreeD;
    }
    if (orthographic == isOrthographic) {
      return GuiProjectionTransition::None;
    }
    orthographic = isOrthographic;
    return isOrthographic ? GuiProjectionTransition::BeginGui : GuiProjectionTransition::BeginThreeD;
  }

  void reset() noexcept {
    known = false;
    orthographic = false;
  }
};

struct GuiCopyMapping {
  float sourceLeft = 0.f;
  float sourceTop = 0.f;
  float sourceWidth = 1.f;
  float sourceHeight = 1.f;
  float destinationLeft = 0.f;
  float destinationTop = 0.f;
  float destinationWidth = 1.f;
  float destinationHeight = 1.f;
};

constexpr GuiCopyMapping display_copy_mapping(uint32_t sceneWidth, uint32_t sceneHeight, int32_t left, int32_t top,
                                              uint32_t width, uint32_t height, uint32_t displayWidth,
                                              uint32_t displayHeight, bool partialDestination) noexcept {
  const auto sceneW = sceneWidth != 0 ? static_cast<float>(sceneWidth) : 1.f;
  const auto sceneH = sceneHeight != 0 ? static_cast<float>(sceneHeight) : 1.f;
  const auto displayW = displayWidth != 0 ? static_cast<float>(displayWidth) : 1.f;
  const auto displayH = displayHeight != 0 ? static_cast<float>(displayHeight) : 1.f;
  return {
      .sourceLeft = static_cast<float>(left) / sceneW,
      .sourceTop = static_cast<float>(top) / sceneH,
      .sourceWidth = static_cast<float>(width) / sceneW,
      .sourceHeight = static_cast<float>(height) / sceneH,
      .destinationLeft = 0.f,
      .destinationTop = 0.f,
      .destinationWidth = partialDestination ? static_cast<float>(width) / displayW : 1.f,
      .destinationHeight = partialDestination ? static_cast<float>(height) / displayH : 1.f,
  };
}

constexpr bool integer_scale(uint32_t source, uint32_t output) noexcept {
  return source != 0 && output >= source && output % source == 0;
}

} // namespace aurora::gfx::pixel_perfect
