#pragma once
#include <cstdint>
struct CpuContext;
double SinCoopBaseLife(double base);
uint32_t SinCoopChange(uint32_t sda, uint32_t stage, uint32_t actor, uint32_t original);
uint32_t SinCoopPad(uint32_t sda, uint32_t actor, uint32_t firstPad);
bool SinCoopDecodePad(CpuContext* ctx, uint32_t stack, uint32_t input, uint32_t device);
uint32_t SinCoopPollMask(uint32_t original);
uint32_t SinCoopSightCharacter(uint32_t sda, uint32_t owner, uint32_t original);
float SinCoopMoveAxis(uint32_t sda, uint32_t actor, uint32_t pad, uint32_t offset);
void SinCoopFinishStageSeven(CpuContext* ctx);
uint32_t SinCoopFinaleCharacter(uint32_t actor, uint32_t storyCharacter);
void SinCoopEndActor(uint32_t actor);
void SinCoopSeedTestCharacter(CpuContext* ctx);
