#pragma once

#include "frame_packet.hpp"

namespace aurora::gfx {

bool bind_pipeline(PipelineRef ref, const wgpu::RenderPassEncoder& pass);
void bind_pipeline(const wgpu::RenderPipeline& pipeline, const wgpu::RenderPassEncoder& pass);
bool presentation_hud_ready(const PixelPerfectGuiPresent& hud);
webgpu::TextureWithSampler prepare_presentation_hud_scene(const wgpu::CommandEncoder& encoder,
                                                         const PixelPerfectGuiPresent& hud);
void render_presentation_hud(const wgpu::CommandEncoder& encoder, const PixelPerfectGuiPresent& hud,
                             const webgpu::TextureWithSampler& target);

namespace detail {
void encode_op(wgpu::CommandEncoder& encoder, FramePacket& frame, const FrameOp& op);
void encode_full_staging_copy(wgpu::CommandEncoder& cmd, FramePacket& frame);
void encode_replay_op(wgpu::CommandEncoder& cmd, FramePacket& frame, const FrameOp& op);
void capture_scene_texture_after(wgpu::CommandEncoder& cmd, FramePacket& frame);
bool capture_replay_textures(wgpu::CommandEncoder& cmd, FramePacket& frame);
void restore_scene_textures(wgpu::CommandEncoder& cmd, FramePacket& frame, bool after);
void clear_scene_texture_pool();
}

} // namespace aurora::gfx
