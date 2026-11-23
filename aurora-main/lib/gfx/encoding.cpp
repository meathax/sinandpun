#include "encoding.hpp"

#include "frame.hpp"
#include "depth_peek.hpp"

#include "clear.hpp"
#include "pipeline_cache.hpp"
#include "readback/gpu.hpp"
#include "tex_copy_conv.hpp"
#include "tex_palette_conv.hpp"
#include "../gx/gx.hpp"
#include "../gx/pipeline.hpp"
#include "../gx/native_scene.hpp"
#ifdef AURORA_ENABLE_RMLUI
#include "../rmlui/pipeline.hpp"
#endif
#include "../webgpu/dlss.hpp"
#include "../webgpu/gpu.hpp"
#include "../webgpu/upscale.hpp"
#include "../webgpu/gpu_prof.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>

#include <tracy/Tracy.hpp>

namespace aurora::gfx {
using namespace detail;
using webgpu::g_device;
using webgpu::g_queue;

namespace {
constexpr Module Log{"aurora::gfx"};
WGPURenderPipeline g_currentPipeline = nullptr;
TextureHandle g_hudSceneCopy;

void apply_viewport(const wgpu::RenderPassEncoder& pass, const Viewport& vp, bool suppressDlssJitter) {
  const float minDepth = gx::UseReversedZ ? 1.f - vp.zfar : vp.znear;
  const float maxDepth = gx::UseReversedZ ? 1.f - vp.znear : vp.zfar;
  // DLSS sub-pixel jitter: shifting the viewport origin shifts the whole rasterised picture (zero unless DLSS is on).
  float jitterX = 0.f;
  float jitterY = 0.f;
  if (!suppressDlssJitter) {
    webgpu::dlss::current_jitter(jitterX, jitterY);
  }
  pass.SetViewport(vp.left + jitterX, vp.top + jitterY, vp.width, vp.height, std::clamp(minDepth, 0.f, 1.f),
                   std::clamp(maxDepth, 0.f, 1.f));
}

void apply_scissor(const wgpu::RenderPassEncoder& pass, const ClipRect& sc, const wgpu::Extent3D& size) {
  // Clamp in signed space: a negative origin must shrink the rectangle, not wrap around.
  const int64_t width = size.width;
  const int64_t height = size.height;
  const int64_t left = std::clamp<int64_t>(sc.x, 0, width);
  const int64_t top = std::clamp<int64_t>(sc.y, 0, height);
  const int64_t right = std::clamp<int64_t>(static_cast<int64_t>(sc.x) + sc.width, left, width);
  const int64_t bottom = std::clamp<int64_t>(static_cast<int64_t>(sc.y) + sc.height, top, height);
  pass.SetScissorRect(static_cast<uint32_t>(left), static_cast<uint32_t>(top), static_cast<uint32_t>(right - left),
                      static_cast<uint32_t>(bottom - top));
}

DrawContext make_draw_context(const RenderPass& passInfo) {
  auto& res = resources();
  return {
      .device = g_device,
      .queue = g_queue,
      .vertexBuffer = res.vertexBuffer,
      .indexBuffer = res.indexBuffer,
      .uniformBuffer = res.uniformBuffer,
      .storageBuffer = res.storageBuffer,
      .layout = passInfo.target_layout(),
  };
}

void render_custom_draw(const CustomDrawCommand& draw, const wgpu::RenderPassEncoder& pass,
                        const RenderPass& passInfo) {
  const auto drawType = find_runtime_draw_type(draw.type);
  if (!drawType) {
    // Unregistered between record and replay; the command is a no-op.
    return;
  }

  const auto context = make_draw_context(passInfo);
  drawType->draw(context, pass, draw.payload.data(), draw.payloadSize, drawType->userdata);
}

void execute_encoder_task(wgpu::CommandEncoder& cmd, FramePacket& frame, const EncoderTask& task) {
  const auto taskType = find_runtime_encoder_task_type(task.type);
  if (!taskType) {
    // Unregistered between record and encode; the task is a no-op.
    return;
  }

  auto& res = resources();
  const EncoderTaskContext context{
      .device = g_device,
      .queue = g_queue,
      .vertexBuffer = res.vertexBuffer,
      .indexBuffer = res.indexBuffer,
      .uniformBuffer = res.uniformBuffer,
      .storageBuffer = res.storageBuffer,
  };
  taskType->callback(context, cmd, task.payload.data(), task.payloadSize, taskType->userdata);
  if (taskType->afterSubmit != nullptr) {
    const auto payload = task.payload;
    const auto payloadSize = task.payloadSize;
    const auto callback = taskType->afterSubmit;
    const auto userdata = taskType->userdata;
    frame.afterSubmitCallbacks.emplace_back([payload, payloadSize, callback, userdata] {
      const EncoderTaskCompletionContext completionContext{
          .device = g_device,
          .queue = g_queue,
      };
      callback(completionContext, payload.data(), payloadSize, userdata);
    });
  }
}

void render_pass(const wgpu::RenderPassEncoder& pass, FramePacket& frame, RenderPass& passInfo) {
  ZoneScoped;
  g_currentPipeline = nullptr;
  const RequireReadyPipelinesScope requireReady{passInfo.requireReadyPipelines};
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> lastDebugGroupStack;
#endif
  Viewport currentViewport{};
  ClipRect currentScissor{};
  bool hasViewport = false;
  bool hasScissor = false;

  // Bind bind group for the whole pass
  pass.SetBindGroup(0, resources().staticBindGroup);
  pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);

  for (auto& cmd : passInfo.commands) {
#ifdef AURORA_GFX_DEBUG_GROUPS
    {
      size_t firstDiff = lastDebugGroupStack.size();
      for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
        if (i >= cmd.debugGroupStack.size() || cmd.debugGroupStack[i] != lastDebugGroupStack[i]) {
          firstDiff = i;
          break;
        }
      }
      for (size_t i = firstDiff; i < lastDebugGroupStack.size(); ++i) {
        pass.PopDebugGroup();
      }
      for (size_t i = firstDiff; i < cmd.debugGroupStack.size(); ++i) {
        pass.PushDebugGroup(cmd.debugGroupStack[i].c_str());
      }
      lastDebugGroupStack = cmd.debugGroupStack;
    }
#endif
    switch (cmd.type) {
    case CommandType::SetViewport: {
      const auto& vp = cmd.data.setViewport;
      apply_viewport(pass, vp, passInfo.suppressDlssJitter);
      currentViewport = vp;
      hasViewport = true;
    } break;
    case CommandType::SetScissor: {
      const auto& sc = cmd.data.setScissor;
      apply_scissor(pass, sc, passInfo.colorAttachments[SceneColorAttachmentIndex].size);
      currentScissor = sc;
      hasScissor = true;
    } break;
    case CommandType::Draw: {
      auto& draw = cmd.data.draw;
      if (draw.encoder != nullptr) {
        draw.encoder(draw.payload.data(), pass, passInfo);
      }
    } break;
    case CommandType::CustomDraw: {
      render_custom_draw(cmd.data.customDraw, pass, passInfo);
      g_currentPipeline = nullptr;
      pass.SetBindGroup(0, resources().staticBindGroup);
      pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);
      if (hasViewport) {
        apply_viewport(pass, currentViewport, passInfo.suppressDlssJitter);
      }
      if (hasScissor) {
        apply_scissor(pass, currentScissor, passInfo.colorAttachments[SceneColorAttachmentIndex].size);
      }
    } break;
    case CommandType::DebugMarker: {
#if defined(AURORA_GFX_DEBUG_GROUPS)
      pass.InsertDebugMarker(wgpu::StringView(frame.debugMarkers[cmd.data.debugMarkerIndex]));
#endif
    } break;
    }
  }

#ifdef AURORA_GFX_DEBUG_GROUPS
  for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
    pass.PopDebugGroup();
  }
#endif
}

void render(wgpu::CommandEncoder& cmd, FramePacket& frame, RenderPass& passInfo, uint32_t passIndex, bool renderOnly = false) {
  ZoneScoped;
  if (!passInfo.sealed) {
    return;
  }

  for (const auto& conv : passInfo.paletteConvs) {
    tex_palette_conv::run(cmd, conv);
  }
  if (passInfo.discardable) {
    // This pass has no effect and can be safely discarded (e.g. an empty EFB segment between two back-to-back pass
    // breaks, or an unresolved offscreen pass).
    return;
  }

  std::array<wgpu::RenderPassColorAttachment, MaxColorAttachments> attachments{};
  for (uint32_t i = 0; i < passInfo.colorAttachmentCount; ++i) {
    const auto& source = passInfo.colorAttachments[i];
    attachments[i] = {
        .view = source.view,
        .resolveTarget = source.resolveView,
        .loadOp = source.loadOp != wgpu::LoadOp::Undefined ? source.loadOp
                                                           : (source.clear ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load),
        .storeOp = source.storeOp,
        .clearValue =
            {
                .r = source.clearValue.x(),
                .g = source.clearValue.y(),
                .b = source.clearValue.z(),
                .a = source.clearValue.w(),
            },
    };
  }
  wgpu::RenderPassDepthStencilAttachment depthStencilAttachment{};
  const wgpu::RenderPassDepthStencilAttachment* depthStencilAttachmentPtr = nullptr;
  if (passInfo.depthStencilView) {
    depthStencilAttachment = {
        .view = passInfo.depthStencilView,
        .depthLoadOp = passInfo.hasDepth ? (passInfo.depthLoadOp != wgpu::LoadOp::Undefined
                                                ? passInfo.depthLoadOp
                                                : (passInfo.clearDepth ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load))
                                         : wgpu::LoadOp::Undefined,
        .depthStoreOp = passInfo.hasDepth ? passInfo.depthStoreOp : wgpu::StoreOp::Undefined,
        .depthClearValue = passInfo.clearDepthValue,
        .stencilLoadOp = passInfo.hasStencil ? passInfo.stencilLoadOp : wgpu::LoadOp::Undefined,
        .stencilStoreOp = passInfo.hasStencil ? passInfo.stencilStoreOp : wgpu::StoreOp::Undefined,
        .stencilClearValue = passInfo.stencilClearValue,
    };
    depthStencilAttachmentPtr = &depthStencilAttachment;
  }
  const auto label = passInfo.label.empty() ? fmt::format("Render pass {}", passIndex)
                                            : fmt::format("{} {}", passInfo.label, passIndex);
  const wgpu::RenderPassDescriptor renderPassDescriptor{
      .label = label.c_str(),
      .colorAttachmentCount = passInfo.colorAttachmentCount,
      .colorAttachments = attachments.data(),
      .depthStencilAttachment = depthStencilAttachmentPtr,
      .timestampWrites = webgpu::gpu_prof::pass_writes(label),
  };

  auto pass = cmd.BeginRenderPass(&renderPassDescriptor);
  render_pass(pass, frame, passInfo);
  pass.End();

  // Shade the main world geometry before later GX copies, particles, post effects
  // and HUD draws. Those passes then consume the lit scene through their normal paths.
  if (passInfo.giDepth.view) {
    const auto& attachment = passInfo.colorAttachments[SceneColorAttachmentIndex];
    const auto size = attachment.size;
    const auto& depth = passInfo.giDepth.view;
    tex_copy_conv::snapshot_depth(cmd, passInfo.copySourceDepthView, passInfo.msaaSamples, depth);
    const webgpu::TextureWithSampler scene{.texture = passInfo.copySourceTexture, .view = passInfo.copySourceView,
      .size = size, .format = attachment.format, .sampler = webgpu::present_source().sampler};
    const webgpu::Viewport full{0,0,float(size.width),float(size.height),0,1};
    const bool rayTraced = !passInfo.giNodes.empty();
    if (rayTraced) {
      const webgpu::Viewport quarter{0,0,float((size.width+3)/4),float((size.height+3)/4),0,1};
      if (!passInfo.giUnretainedNodes.empty() && webgpu::upscale::culled_comparison_pending()) {
        webgpu::resample_present_source(cmd, quarter, scene, SAMPLER_BILINEAR, false, depth,
                                       webgpu::BouncePass::RayGather, passInfo.giProjection,
                                       passInfo.giUnretained, passInfo.giUnretainedNodes, passInfo.giScreenCenter, true);
        webgpu::resample_present_source(cmd, quarter, scene, SAMPLER_BILINEAR, false, depth,
                                       webgpu::BouncePass::RayFilter, passInfo.giProjection);
        const auto& before = webgpu::resample_present_source(cmd, full, scene, SAMPLER_BILINEAR, false, depth,
                                               webgpu::BouncePass::RayComposite, passInfo.giProjection);
        webgpu::upscale::capture_culled_before(cmd, before);
      }
      webgpu::resample_present_source(cmd, quarter, scene, SAMPLER_BILINEAR, false, depth,
                                       webgpu::BouncePass::RayGather, passInfo.giProjection,
                                       passInfo.giTriangles, passInfo.giNodes, passInfo.giScreenCenter);
      webgpu::resample_present_source(cmd, quarter, scene, SAMPLER_BILINEAR, false, depth,
                                       webgpu::BouncePass::RayFilter, passInfo.giProjection);
    }
    auto lit = scene;
    if (passInfo.applyGlobalIllumination && (!rayTraced || webgpu::upscale::gi_comparison_pending())) {
      const webgpu::Viewport half{0,0,float((size.width+1)/2),float((size.height+1)/2),0,1};
      const webgpu::Viewport coarse{0,0,float((size.width+31)/32),float((size.height+31)/32),0,1};
      webgpu::resample_present_source(cmd, coarse, scene, SAMPLER_BILINEAR, false, depth,
                                       webgpu::BouncePass::Radiance, passInfo.giProjection);
      webgpu::resample_present_source(cmd, half, scene, SAMPLER_BILINEAR, false, depth,
                                       webgpu::BouncePass::Gather, passInfo.giProjection);
      lit = webgpu::resample_present_source(cmd, full, scene, SAMPLER_BILINEAR, false, depth,
                                               webgpu::BouncePass::Composite, passInfo.giProjection);
      webgpu::upscale::capture_gi_pair(cmd, scene, lit, passInfo.giDepth, passInfo.giProjection);
    }
    if (rayTraced) {
      lit = webgpu::resample_present_source(cmd, full, scene, SAMPLER_BILINEAR, false, depth,
                                               webgpu::BouncePass::RayComposite, passInfo.giProjection);
      webgpu::upscale::capture_rtgi(cmd, lit);
    }
    if (passInfo.applyAmbientOcclusion) {
      lit = webgpu::resample_present_source(cmd, full, lit, SAMPLER_BILINEAR, false, depth);
    }
    if (passInfo.msaaSamples == 1 && lit.texture.Get() != scene.texture.Get()) {
      const wgpu::TexelCopyTextureInfo src{.texture=lit.texture}, dst{.texture=scene.texture};
      cmd.CopyTextureToTexture(&src, &dst, &size);
    } else if (passInfo.msaaSamples > 1 && lit.texture.Get() != scene.texture.Get()) {
      const wgpu::RenderPassColorAttachment dst{.view=attachment.view, .resolveTarget=attachment.resolveView,
                                                .loadOp=wgpu::LoadOp::Clear, .storeOp=wgpu::StoreOp::Store};
      const wgpu::RenderPassDescriptor descriptor{.label="GI multisample restore", .colorAttachmentCount=1, .colorAttachments=&dst};
      auto restore = cmd.BeginRenderPass(&descriptor);
      auto sampled = lit;
      sampled.sampler = scene.sampler;
      restore.SetPipeline(webgpu::g_GiRestorePipeline);
      restore.SetBindGroup(0, webgpu::create_copy_bind_group(sampled));
      restore.Draw(3);
      restore.End();
    }
  }

  if (passInfo.captureDepthSnapshot && !renderOnly) {
    depth_peek::encode_frame_snapshot(cmd, passInfo.copySourceDepthView,
        passInfo.colorAttachments[SceneColorAttachmentIndex].size, passInfo.msaaSamples);
  }

  if (passInfo.resolveTarget) {
    const auto& dstSize = passInfo.resolveTarget->size;
    const bool needsConversion = tex_copy_conv::needs_conversion(passInfo.resolveFormat);
    // A destination texture larger than the copy source is a partial update of its top-left corner (the GX destination
    // width is only the row stride), not an upscale.
    const bool partialDst = dstSize.width > static_cast<uint32_t>(passInfo.resolveRect.width) &&
                            dstSize.height > static_cast<uint32_t>(passInfo.resolveRect.height);
    const bool needsScaling = !partialDst && (dstSize.width != static_cast<uint32_t>(passInfo.resolveRect.width) ||
                              dstSize.height != static_cast<uint32_t>(passInfo.resolveRect.height));
    const bool isDepth = gx::is_depth_format(passInfo.resolveFormat);
    if (isDepth && passInfo.msaaSamples > 1) {
      Log.fatal("Depth tex copies from multisampled EFB targets are not supported");
    }
    const tex_copy_conv::ConvRequest convReq{
        .fmt = passInfo.resolveFormat,
        .srcFmt = passInfo.resolveSourceFormat,
        .srcView = isDepth ? passInfo.copySourceDepthView : passInfo.copySourceView,
        .uniformRange = passInfo.resolveUniformRange,
        .dst = passInfo.resolveTarget,
        // Filter taps must land on exact source rows.
        .sampleFilter = needsScaling && !passInfo.resolveCopyFilterActive ? tex_copy_conv::SampleFilter::Linear
                                                                           : tex_copy_conv::SampleFilter::Nearest,
        .partialDst = partialDst,
        .dstRectW = static_cast<uint32_t>(passInfo.resolveRect.width),
        .dstRectH = static_cast<uint32_t>(passInfo.resolveRect.height),
    };
    const bool needsOpaqueAlpha = !isDepth && !gx::efb_has_alpha(convReq.srcFmt);
    const bool sameFormat =
        passInfo.resolveTarget->format == passInfo.colorAttachments[SceneColorAttachmentIndex].format;
    if (needsConversion) {
      tex_copy_conv::run(cmd, convReq);
    } else if (needsScaling || partialDst || needsOpaqueAlpha || !sameFormat || passInfo.resolveCopyFilterActive) {
      tex_copy_conv::blit(cmd, convReq);
    } else {
      const webgpu::gpu_prof::Zone zone{cmd, "EFB copy"};
      const wgpu::TexelCopyTextureInfo src{
          .texture = passInfo.copySourceTexture,
          .origin =
              wgpu::Origin3D{
                  .x = static_cast<uint32_t>(passInfo.resolveRect.x),
                  .y = static_cast<uint32_t>(passInfo.resolveRect.y),
              },
      };
      const wgpu::TexelCopyTextureInfo dst{
          .texture = passInfo.resolveTarget->texture,
      };
      const wgpu::Extent3D size{
          .width = static_cast<uint32_t>(passInfo.resolveRect.width),
          .height = static_cast<uint32_t>(passInfo.resolveRect.height),
          .depthOrArrayLayers = 1,
      };
      cmd.CopyTextureToTexture(&src, &dst, &size);
    }
  }

  if (passInfo.snapshotColorDst) {
    const webgpu::gpu_prof::Zone zone{cmd, "Pass snapshot"};
    const wgpu::TexelCopyTextureInfo src{
        .texture = passInfo.copySourceTexture,
    };
    const wgpu::TexelCopyTextureInfo dst{
        .texture = passInfo.snapshotColorDst,
    };
    const wgpu::Extent3D size{
        .width = passInfo.colorAttachments[SceneColorAttachmentIndex].size.width,
        .height = passInfo.colorAttachments[SceneColorAttachmentIndex].size.height,
        .depthOrArrayLayers = 1,
    };
    cmd.CopyTextureToTexture(&src, &dst, &size);
  }
  if (passInfo.snapshotDepthDst) {
    tex_copy_conv::snapshot_depth(cmd, passInfo.copySourceDepthView, passInfo.msaaSamples, passInfo.snapshotDepthDst);
  }
  if (passInfo.snapshotNormalDst) {
    const webgpu::gpu_prof::Zone zone{cmd, "Normal snapshot"};
    const wgpu::TexelCopyTextureInfo src{
        .texture = passInfo.copySourceNormalTexture,
    };
    const wgpu::TexelCopyTextureInfo dst{
        .texture = passInfo.snapshotNormalDst,
    };
    const wgpu::Extent3D size{
        .width = passInfo.colorAttachments[SceneColorAttachmentIndex].size.width,
        .height = passInfo.colorAttachments[SceneColorAttachmentIndex].size.height,
        .depthOrArrayLayers = 1,
    };
    cmd.CopyTextureToTexture(&src, &dst, &size);
  }
}

constexpr uint64_t VertexStagingOffset = 0;
constexpr uint64_t UniformStagingOffset = VertexStagingOffset + VertexBufferSize;
constexpr uint64_t IndexStagingOffset = UniformStagingOffset + UniformBufferSize;
constexpr uint64_t StorageStagingOffset = IndexStagingOffset + IndexBufferSize;
constexpr uint64_t TextureUploadStagingOffset = StorageStagingOffset + StorageBufferSize;

constexpr uint32_t align_down_copy_offset(uint32_t value) noexcept { return value & ~3u; }

void copy_staging_buffer_range(wgpu::CommandEncoder& cmd, const FramePacket& frame, uint32_t& copied,
                               uint32_t highWater, uint64_t stagingOffset, const wgpu::Buffer& dst) {
  if (highWater <= copied) {
    return;
  }
  const uint32_t copyStart = align_down_copy_offset(copied);
  const uint32_t copyEnd = AURORA_ALIGN(highWater, 4);
  cmd.CopyBufferToBuffer(staging_buffer(frame.stagingBuffer), stagingOffset + copyStart, dst, copyStart,
                         copyEnd - copyStart);
  copied = highWater;
}

bool needs_staging_copy(const FramePacket& frame, const FrameOp& op) {
  const auto& highWater = op.highWater;
  if (highWater.verts > frame.copied.verts || highWater.uniforms > frame.copied.uniforms ||
      highWater.indices > frame.copied.indices || highWater.storage > frame.copied.storage) {
    return true;
  }
  if constexpr (UseTextureBuffer) {
    return highWater.textureUploadCount > frame.copied.textureUploadCount;
  }
  return false;
}

void copy_staging_to_high_water(wgpu::CommandEncoder& cmd, FramePacket& frame, const FrameOp& op) {
  if (!needs_staging_copy(frame, op)) {
    return;
  }
  const webgpu::gpu_prof::Zone zone{cmd, "Staging copies"};
  const auto& highWater = op.highWater;
  auto& res = resources();
  copy_staging_buffer_range(cmd, frame, frame.copied.verts, highWater.verts, VertexStagingOffset, res.vertexBuffer);
  copy_staging_buffer_range(cmd, frame, frame.copied.uniforms, highWater.uniforms, UniformStagingOffset,
                            res.uniformBuffer);
  copy_staging_buffer_range(cmd, frame, frame.copied.indices, highWater.indices, IndexStagingOffset, res.indexBuffer);
  copy_staging_buffer_range(cmd, frame, frame.copied.storage, highWater.storage, StorageStagingOffset,
                            res.storageBuffer);

  if constexpr (UseTextureBuffer) {
    const size_t first = highWater.textureUploadCount - op.textureUploads.size();
    for (size_t i = frame.copied.textureUploadCount; i < highWater.textureUploadCount; ++i) {
      const auto& item = *op.textureUploads.at(i - first);
      const wgpu::TexelCopyBufferInfo buf{
          .layout =
              wgpu::TexelCopyBufferLayout{
                  .offset = item.buffer ? item.layout.offset : item.layout.offset + TextureUploadStagingOffset,
                  .bytesPerRow = AURORA_ALIGN(item.layout.bytesPerRow, 256),
                  .rowsPerImage = item.layout.rowsPerImage,
              },
          .buffer = item.buffer ? item.buffer : staging_buffer(frame.stagingBuffer),
      };
      cmd.CopyBufferToTexture(&buf, &item.tex, &item.size);
    }
    frame.copied.textureUpload = highWater.textureUpload;
    frame.copied.textureUploadCount = highWater.textureUploadCount;
  }
}
} // namespace

namespace detail {
namespace {
std::unordered_map<size_t, std::vector<SceneTextureState>> sceneTexturePools;
void copy_scene_texture(wgpu::CommandEncoder& cmd, const wgpu::Texture& srcTexture, const wgpu::Texture& dstTexture) {
  for (uint32_t mip=0; mip<srcTexture.GetMipLevelCount(); ++mip) {
    const wgpu::TexelCopyTextureInfo src{.texture=srcTexture,.mipLevel=mip};
    const wgpu::TexelCopyTextureInfo dst{.texture=dstTexture,.mipLevel=mip};
    const wgpu::Extent3D extent{std::max(1u,srcTexture.GetWidth()>>mip),std::max(1u,srcTexture.GetHeight()>>mip),srcTexture.GetDepthOrArrayLayers()};
    cmd.CopyTextureToTexture(&src,&dst,&extent);
  }
}
void capture_scene_texture_before(wgpu::CommandEncoder& cmd, FramePacket& frame, const wgpu::Texture& texture) {
  if (!texture) return;
  for (const auto& state : frame.sceneTextures) if (state.texture.Get()==texture.Get()) return;
  const wgpu::TextureUsage required=wgpu::TextureUsage::CopySrc|wgpu::TextureUsage::CopyDst;
  if (!static_cast<bool>(texture.GetUsage()&wgpu::TextureUsage::CopySrc) || !static_cast<bool>(texture.GetUsage()&wgpu::TextureUsage::CopyDst) || texture.GetSampleCount()!=1 || texture.GetDimension()!=wgpu::TextureDimension::e2D) {
    static unsigned warnings=0;
    if(warnings++<8) std::fprintf(stderr,"[scene-texture] unsupported snapshot %ux%u usage=%u samples=%u dimension=%u format=%u\n",texture.GetWidth(),texture.GetHeight(),unsigned(texture.GetUsage()),texture.GetSampleCount(),unsigned(texture.GetDimension()),unsigned(texture.GetFormat()));
    frame.sceneTexturesReady=false;
    return;
  }
  auto& pool=sceneTexturePools[frame.stagingBuffer];
  const size_t index=frame.sceneTextures.size();
  if (pool.size()<=index) pool.resize(index+1);
  auto& cached=pool[index];
  if (!cached.before || cached.before.GetWidth()!=texture.GetWidth() || cached.before.GetHeight()!=texture.GetHeight() ||
      cached.before.GetFormat()!=texture.GetFormat() || cached.before.GetMipLevelCount()!=texture.GetMipLevelCount() ||
      cached.before.GetDepthOrArrayLayers()!=texture.GetDepthOrArrayLayers()) {
    const wgpu::TextureDescriptor desc{
      .label="Immutable scene texture state", .usage=required, .dimension=wgpu::TextureDimension::e2D,
      .size={texture.GetWidth(),texture.GetHeight(),texture.GetDepthOrArrayLayers()}, .format=texture.GetFormat(),
      .mipLevelCount=texture.GetMipLevelCount(), .sampleCount=1,
    };
    cached.before=g_device.CreateTexture(&desc); cached.after=g_device.CreateTexture(&desc);
  }
  cached.texture=texture;
  frame.sceneTextures.push_back(cached);
  copy_scene_texture(cmd,texture,cached.before);
}
}
void clear_scene_texture_pool() {
  sceneTexturePools.clear();
  g_hudSceneCopy.reset();
}
bool capture_replay_textures(wgpu::CommandEncoder& cmd, FramePacket& frame) {
  for (const auto& op : frame.ops) {
    if (op.renderPass && op.renderPass->resolveTarget)
      capture_scene_texture_before(cmd, frame, op.renderPass->resolveTarget->texture);
    if (op.textureCopy)
      capture_scene_texture_before(cmd, frame, op.textureCopy->dst.texture);
  }
  return frame.sceneTexturesReady;
}
void capture_scene_texture_after(wgpu::CommandEncoder& cmd, FramePacket& frame) {
  for (const auto& state : frame.sceneTextures) copy_scene_texture(cmd,state.texture,state.after);
  frame.sceneAfterCaptured=true;
}
void restore_scene_textures(wgpu::CommandEncoder& cmd, FramePacket& frame, bool after) {
  if (after && !frame.sceneAfterCaptured) return;
  for (const auto& state : frame.sceneTextures) copy_scene_texture(cmd,after?state.after:state.before,state.texture);
}
// Copies everything the frame staged into the device buffers, independent of what the
// incrementally encoded native frame has copied so far.
void encode_full_staging_copy(wgpu::CommandEncoder& cmd, FramePacket& frame) {
  if (frame.ops.empty()) {
    return;
  }
  FrameOp op{};
  op.highWater = {
      .verts = static_cast<uint32_t>(frame.verts.size()),
      .uniforms = static_cast<uint32_t>(frame.uniforms.size()),
      .indices = static_cast<uint32_t>(frame.indices.size()),
      .storage = static_cast<uint32_t>(frame.storage.size()),
      .textureUpload = static_cast<uint32_t>(frame.textureUpload.size()),
      .textureUploadCount = frame.textureUploads.size(),
  };
  op.textureUploads.clear();
  for (const auto& upload : frame.textureUploads) {
    op.textureUploads.push_back(&upload);
  }
  auto copied = frame.copied;
  frame.copied = {};
  copy_staging_to_high_water(cmd, frame, op);
  frame.copied = copied;
}

// Re-encodes an op for an interpolated slot: render passes and texture copies only, so
// readbacks and guest-visible tasks run once, with the native frame.
void encode_replay_op(wgpu::CommandEncoder& cmd, FramePacket& frame, const FrameOp& op) {
  switch (op.type) {
  case FrameOpType::RenderPass:
    if (op.renderPass != nullptr) {
      render(cmd, frame, *op.renderPass, op.index, true);
    }
    break;
  case FrameOpType::TextureCopy:
    if (op.textureCopy != nullptr) {
      cmd.CopyTextureToTexture(&op.textureCopy->src, &op.textureCopy->dst, &op.textureCopy->size);
    }
    break;
  default:
    break;
  }
}

void encode_op(wgpu::CommandEncoder& cmd, FramePacket& frame, const FrameOp& op) {
  copy_staging_to_high_water(cmd, frame, op);
  if (gx::native_scene_enabled()) {
    if (op.renderPass && op.renderPass->resolveTarget)
      capture_scene_texture_before(cmd,frame,op.renderPass->resolveTarget->texture);
    if (op.textureCopy) capture_scene_texture_before(cmd,frame,op.textureCopy->dst.texture);
  }
  switch (op.type) {
  case FrameOpType::RenderPass:
    if (op.renderPass != nullptr) {
      render(cmd, frame, *op.renderPass, op.index);
    }
    break;
  case FrameOpType::TextureCopy:
    if (op.textureCopy != nullptr) {
      const webgpu::gpu_prof::Zone zone{cmd, "Texture copy"};
      cmd.CopyTextureToTexture(&op.textureCopy->src, &op.textureCopy->dst, &op.textureCopy->size);
    }
    break;
  case FrameOpType::EncoderTask:
    if (op.encoderTask != nullptr) {
      execute_encoder_task(cmd, frame, *op.encoderTask);
    }
    break;
  case FrameOpType::Readback:
    if (op.readbackTask != nullptr) {
      readback::gpu::encode_job(cmd, frame, op.readbackTask->job);
    }
    break;
  }
}
} // namespace detail

void bind_pipeline(const wgpu::RenderPipeline& pipeline, const wgpu::RenderPassEncoder& pass) {
  if (pipeline.Get() == g_currentPipeline) {
    return;
  }
  pass.SetPipeline(pipeline);
  g_currentPipeline = pipeline.Get();
}

bool presentation_hud_ready(const PixelPerfectGuiPresent& hud) {
  if (!hud.pass || !hud.pass->resolveTarget) {
    return false;
  }
  PipelineRef lastPipeline{};
  for (const auto& command : hud.pass->commands) {
    if (command.type == detail::CommandType::Draw && command.data.draw.presentationPipeline != lastPipeline) {
      lastPipeline = command.data.draw.presentationPipeline;
      CompiledPipeline pipeline;
      if (!get_pipeline(lastPipeline, pipeline)) {
        return false;
      }
    }
  }
  return true;
}

webgpu::TextureWithSampler prepare_presentation_hud_scene(const wgpu::CommandEncoder& encoder,
                                                         const PixelPerfectGuiPresent& hud) {
  const auto& copy = *hud.pass;
  const auto& size = copy.resolveTarget->size;
  if (!g_hudSceneCopy || g_hudSceneCopy->size.width != size.width || g_hudSceneCopy->size.height != size.height ||
      g_hudSceneCopy->format != copy.resolveTarget->format) {
    g_hudSceneCopy = new_render_texture(size.width, size.height, GX_TF_RGBA8, "HUD-free display copy");
  }
  const bool scaled = size.width != static_cast<uint32_t>(copy.resolveRect.width) ||
                      size.height != static_cast<uint32_t>(copy.resolveRect.height);
  const auto& background = hud.background;
  tex_copy_conv::blit(encoder, {
      .fmt = copy.resolveFormat,
      .srcFmt = copy.resolveSourceFormat,
      .srcView = background.view,
      .uniformRange = copy.resolveUniformRange,
      .dst = g_hudSceneCopy,
      .sampleFilter = scaled ? tex_copy_conv::SampleFilter::Linear : tex_copy_conv::SampleFilter::Nearest,
  });
  return {.texture = g_hudSceneCopy->texture, .view = g_hudSceneCopy->sampleTextureView, .sampler = hud.background.sampler,
          .size = size, .format = g_hudSceneCopy->format};
}

void render_presentation_hud(const wgpu::CommandEncoder& encoder, const PixelPerfectGuiPresent& hud,
                             const webgpu::TextureWithSampler& target) {
  const auto& source = hud.pass->resolveRect;
  if (static const bool trace = std::getenv("SIN_HUD_TRACE") != nullptr; trace) {
    static bool reported = false;
    if (!reported) {
      reported = true;
      Log.info("[hud-present] replayed GX HUD from {}x{} at {}x{}", source.width, source.height,
               target.size.width, target.size.height);
    }
  }
  const float scaleX = static_cast<float>(target.size.width) / static_cast<float>(source.width);
  const float scaleY = static_cast<float>(target.size.height) / static_cast<float>(source.height);
  const auto edge = [](int64_t coordinate, float scale) {
    // Scissors include pixels whose centers lie inside the scaled source rectangle.
    return static_cast<int32_t>(std::ceil(static_cast<double>(coordinate) * scale - 0.5));
  };
  const wgpu::RenderPassColorAttachment color{
      .view = target.view,
      .loadOp = wgpu::LoadOp::Load,
      .storeOp = wgpu::StoreOp::Store,
  };
  const wgpu::RenderPassDescriptor desc{
      .label = "GX HUD at presentation resolution",
      .colorAttachmentCount = 1,
      .colorAttachments = &color,
      .timestampWrites = webgpu::gpu_prof::pass_writes("GX HUD presentation"),
  };
  const auto pass = encoder.BeginRenderPass(&desc);
  g_currentPipeline = nullptr;
  pass.SetBindGroup(0, detail::resources().staticBindGroup);
  pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);
  for (const auto& command : hud.pass->commands) {
    switch (command.type) {
    case detail::CommandType::SetViewport: {
      auto vp = command.data.setViewport;
      vp.left *= scaleX;
      vp.top *= scaleY;
      vp.width *= scaleX;
      vp.height *= scaleY;
      apply_viewport(pass, vp, true);
      break;
    }
    case detail::CommandType::SetScissor: {
      const auto& sc = command.data.setScissor;
      const auto left = edge(sc.x, scaleX);
      const auto top = edge(sc.y, scaleY);
      const auto right = edge(static_cast<int64_t>(sc.x) + sc.width, scaleX);
      const auto bottom = edge(static_cast<int64_t>(sc.y) + sc.height, scaleY);
      apply_scissor(pass, {left, top, right - left, bottom - top}, target.size);
      break;
    }
    case detail::CommandType::Draw: {
      gx::DrawData draw;
      std::memcpy(&draw, command.data.draw.payload.data(), sizeof(draw));
      draw.pipeline = command.data.draw.presentationPipeline;
      gx::render(draw, pass);
      break;
    }
    case detail::CommandType::CustomDraw:
    case detail::CommandType::DebugMarker:
      break;
    }
  }
  pass.End();
}

bool bind_pipeline(PipelineRef ref, const wgpu::RenderPassEncoder& pass) {
  CompiledPipeline pipeline;
  if (!get_pipeline(ref, pipeline)) {
    return false;
  }
  AURORA_ASSERT(!pipeline.prepass, "Prepass pipelines require individual binding");
  bind_pipeline(pipeline.main, pass);
  return true;
}
} // namespace aurora::gfx
