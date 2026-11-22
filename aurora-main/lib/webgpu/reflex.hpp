#pragma once
#include <cstdint>

namespace aurora::reflex {
enum class Marker : unsigned { SimulationStart, SimulationEnd, SubmitStart, SubmitEnd, PresentStart, PresentEnd,
                               ExtraSubmitStart = 9, ExtraSubmitEnd, ExtraPresentStart, ExtraPresentEnd };
void attach(void* device);
void detach();
void request(bool enabled);
bool active();
void begin();
uint64_t end();
void mark(uint64_t frame, Marker marker);
}
