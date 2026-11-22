#pragma once

#include <cstdint>

struct CpuContext;
bool ObserveSceneAnchor(uint32_t address, CpuContext* ctx);
void UpdateSceneLighting(uint8_t stage);
