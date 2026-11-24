#include "gx/native_scene.hpp"
#include <aurora/aurora.h>
#include <aurora/time.hpp>
#include "audio.hpp"
#include "video.hpp"

#ifdef AURORA_ENABLE_GX
#include "gfx/resources.hpp"
#include "gfx/presentation.hpp"
#include <dolphin/gx.h>
#include <aurora/texture.hpp>
#include "gfx/frame.hpp"
#include "gfx/depth_peek.hpp"
#include "gfx/encoding.hpp"
#include "gfx/recording.hpp"
#include "gfx/render_worker.hpp"
#include "gx/command_processor.hpp"
#include "gx/fifo.hpp"
#include "gx/frame_interpolation.hpp"
#include "gx/gx.hpp"
#include "gx/texture.hpp"
#include "imgui.hpp"
#include "webgpu/gpu.hpp"
#include "webgpu/gpu_prof.hpp"
#include "webgpu/dlss.hpp"
#include "webgpu/reflex.hpp"
#include "webgpu/metalfx.hpp"
#include "webgpu/upscale.hpp"
#include <webgpu/webgpu_cpp.h>
#endif

#ifdef AURORA_ENABLE_RMLUI
#include "rmlui.hpp"
#endif

#include "gamepad.hpp"
#include "internal.hpp"
#include "io.hpp"
#include "thread.hpp"
#include "window.hpp"

#include <SDL3/SDL_filesystem.h>
#include <magic_enum.hpp>

#include <array>
#include <cstdio>
#include <mutex>
#include <optional>

#include "system_info.hpp"
#include "tracy/Tracy.hpp"

static std::atomic<bool> s_enhancedBloom{false};
extern "C" bool aurora_enhanced_bloom_enabled() { return s_enhancedBloom.load(std::memory_order_relaxed); }
extern "C" void aurora_set_enhanced_bloom(bool enabled) { s_enhancedBloom.store(enabled); }

namespace aurora {
AuroraConfig g_config;
uint32_t g_sdlCustomEventsStart;
char g_gameName[4];

namespace {
constexpr Module Log{"aurora"};

#ifdef AURORA_ENABLE_GX
// GPU
using webgpu::g_device;
using webgpu::g_queue;
using webgpu::g_surface;

// Last presented picture rectangle as fractions of the surface (left, top, width, height); mouse mapping reads it.
std::mutex g_presentVpMutex;
std::optional<std::array<float, 4>> g_presentVp;

std::atomic<bool> g_metalfxRequested{false};
std::atomic<bool> g_metalfxSupported{false};
std::atomic<AuroraMetalFXStatus> g_metalfxStatus{AURORA_METALFX_DISABLED};

struct MetalFXSlot {
  webgpu::metalfx::Size size{};
  std::unique_ptr<webgpu::metalfx::SpatialScaler> scaler;
  webgpu::TextureWithSampler output;
  wgpu::BindGroup bindGroup;
};
std::array<MetalFXSlot, gx::MaxInterpolatedFrames + 1> g_metalfxSlots;
size_t g_metalfxNextSlot = 0;
webgpu::metalfx::SpatialScaler* g_metalfxPendingOutput = nullptr;
const webgpu::TextureWithSampler* g_metalfxPendingImage = nullptr;
bool g_metalfxFailed = false;

void metalfx_failed(const std::string& reason) {
  Log.warn("MetalFX spatial upscaling disabled: {}; using normal presentation", reason);
  g_metalfxFailed = true;
  g_metalfxStatus.store(AURORA_METALFX_ERROR);
  g_metalfxPendingOutput = nullptr;
  g_metalfxPendingImage = nullptr;
  g_metalfxSlots = {};
}

// ---- NVIDIA DLSS (docs/DLSS-NOTES.md) ---------------------------------------------------------------------------
// Runs between the scene submit and the present blit, like MetalFX; any failure falls back to the FSR1 / plain path.
const bool g_dlssLogSink = [] {
  webgpu::dlss::set_log([](int level, const char* message) {
    if (level >= 2) {
      Log.error("{}", message);
    } else if (level == 1) {
      Log.warn("{}", message);
    } else {
      Log.info("{}", message);
    }
  });
  return true;
}();

struct DlssPresentState {
  webgpu::dlss::Output output;
  webgpu::TextureWithSampler image;
  wgpu::BindGroup bindGroup;
  wgpu::TextureView boundView;
  bool wasActive = false;
  bool hadSeparateHud = false;
  bool hadVelocity = false;
  std::array<float,4> lastVelocityRect{};
};
DlssPresentState g_dlss;

// Window-size side of the upscaler selection: internal resolution divisor, and DLAA's native-resolution lock. Also
// called when DLSS becomes ready, which happens asynchronously after start-up.
bool temporal_ready(AuroraUpscaler mode) {
  return (mode == AURORA_UPSCALER_FSR3 && webgpu::dlss::fsr_ready()) ||
         (mode == AURORA_UPSCALER_DLSS && webgpu::dlss::ready());
}

void sync_upscale_window() {
  const bool jitter = temporal_ready(webgpu::upscale::settings().mode);
  if (!jitter) {
    webgpu::dlss::set_jitter_enabled(false, 0, 0);
  }
  window::set_upscale_divisor(webgpu::upscale::resolution_divisor(), webgpu::upscale::locks_native_resolution());
}

webgpu::upscale::Result dlss_dispatch(wgpu::CommandEncoder& encoder, const webgpu::TextureWithSampler& source,
                                      const webgpu::Viewport& viewport, bool separateHud, const gfx::PixelPerfectGuiPresent& hud) {
  const auto settings = webgpu::upscale::settings();
  if (!temporal_ready(settings.mode) || !source.view) {
    g_dlss.wasActive = false;
    webgpu::dlss::set_jitter_enabled(false, 0, 0);
    return {};
  }
  const uint32_t outW = std::max(1u, static_cast<uint32_t>(std::lround(std::max(viewport.width, 1.f))));
  const uint32_t outH = std::max(1u, static_cast<uint32_t>(std::lround(std::max(viewport.height, 1.f))));
  webgpu::dlss::DispatchParams params;
  params.fsr = settings.mode == AURORA_UPSCALER_FSR3;
  params.color = source.view;
  params.depth = separateHud && hud.depth ? hud.depth : hud.temporalDepth;
  // Both display paths apply the GX copy rectangle before temporal upscaling.
  params.depthRect = hud.depthRect;
  params.velocity = separateHud ? hud.hudVelocity : hud.velocity;
  if (gx::g_velocityWeight == 1.f && gx::g_nativeSceneWeight < 0.f &&
      gx::g_velocityFrame == gx::g_presentedVelocityFrame)
    params.previousNativeWeight = gx::g_presentedVelocityWeight;
  params.velocityRect = params.depthRect;
  params.reversedDepth = gx::UseReversedZ;
  params.renderWidth = source.size.width;
  params.renderHeight = source.size.height;
  params.outputWidth = outW;
  params.outputHeight = outH;
  params.mode = webgpu::dlss::mode_for_scale_factor(settings.scaleFactor);
  // The Wii can crop the EFB before presentation. Preserve the selected DLSS
  // ratio on that image, then fit the completed image to the viewport.
  if (!params.fsr) {
    const float ratio = webgpu::dlss::mode_scale_factor(params.mode);
    params.outputWidth = static_cast<uint32_t>(std::lround(params.renderWidth * ratio));
    params.outputHeight = static_cast<uint32_t>(std::lround(params.renderHeight * ratio));
  }
  params.sharpness = settings.sharpness;
  const bool skippedHistory = params.velocity && gx::g_presentedVelocityFrame != UINT64_MAX &&
      gx::g_velocityFrame != gx::g_presentedVelocityFrame && gx::g_velocityFrame != gx::g_presentedVelocityFrame + 1;
  const bool changedVelocity = g_dlss.hadVelocity != static_cast<bool>(params.velocity) ||
      (params.velocity && params.velocityRect != g_dlss.lastVelocityRect);
  params.reset = !g_dlss.wasActive || g_dlss.hadSeparateHud != separateHud || skippedHistory || changedVelocity;
  webgpu::dlss::set_jitter_enabled(true, params.renderWidth, params.outputWidth);
  if (!webgpu::dlss::dispatch(encoder, params, g_dlss.output)) {
    g_dlss.wasActive = false;
    webgpu::dlss::set_jitter_enabled(false, 0, 0);
    return {};
  }
  gx::g_presentedVelocityFrame = gx::g_velocityFrame;
  gx::g_presentedVelocityWeight = gx::g_velocityWeight;
  g_dlss.wasActive = true;
  g_dlss.hadSeparateHud = separateHud;
  g_dlss.hadVelocity = static_cast<bool>(params.velocity);
  g_dlss.lastVelocityRect = params.velocityRect;
  if (!g_dlss.bindGroup || g_dlss.boundView.Get() != g_dlss.output.view.Get()) {
    g_dlss.image = webgpu::TextureWithSampler{.texture = g_dlss.output.texture,
                                              .view = g_dlss.output.view,
                                              .size = g_dlss.output.size,
                                              .format = g_dlss.output.format,
                                              .sampler = g_dlss.output.sampler};
    g_dlss.bindGroup = webgpu::create_copy_bind_group(g_dlss.image);
    g_dlss.boundView = g_dlss.output.view;
  }
  webgpu::upscale::note_present(params.renderWidth, params.renderHeight, outW, outH, settings.mode,
                                settings.sharpness > 0.01f);
  return {g_dlss.bindGroup, &g_dlss.image};
}

wgpu::BindGroup upscale_presentation(wgpu::CommandEncoder& encoder,
                                     const webgpu::TextureWithSampler& source,
                                     const webgpu::Viewport& viewport, bool enabled) {
  g_metalfxPendingImage = nullptr;
  if (!enabled) {
    g_metalfxSlots = {};
    g_metalfxFailed = false;
    g_metalfxStatus.store(AURORA_METALFX_DISABLED);
    return {};
  }
  if (!g_metalfxSupported.load()) {
    g_metalfxStatus.store(AURORA_METALFX_UNSUPPORTED);
    return {};
  }
  if (g_metalfxFailed) return {};
  const webgpu::metalfx::Size size{
      source.size.width, source.size.height,
      static_cast<uint32_t>(viewport.width), static_cast<uint32_t>(viewport.height),
      webgpu::g_graphicsConfig.surfaceConfiguration.format,
  };
  // The existing copy path samples perceptual values from unorm game images.
  // Do not introduce implicit sRGB decoding or downscaling into MetalFX.
  if (!size.inputWidth || !size.inputHeight || size.inputWidth >= size.outputWidth ||
      size.inputHeight >= size.outputHeight ||
      (source.format != wgpu::TextureFormat::RGBA8Unorm && source.format != wgpu::TextureFormat::BGRA8Unorm)) {
    g_metalfxStatus.store(AURORA_METALFX_NOT_UPSCALING);
    return {};
  }
  auto& slot = g_metalfxSlots[g_metalfxNextSlot++ % g_metalfxSlots.size()];
  if (!slot.scaler || !(slot.size == size)) {
    slot = {};
    std::string error;
    slot.scaler = webgpu::metalfx::create(webgpu::g_instance, g_device, size, error);
    if (!slot.scaler) {
      if (error.empty()) g_metalfxStatus.store(AURORA_METALFX_NOT_UPSCALING);
      else metalfx_failed(error);
      return {};
    }
    slot.size = size;
    wgpu::SamplerDescriptor samplerDescriptor{};
    samplerDescriptor.magFilter = wgpu::FilterMode::Linear;
    samplerDescriptor.minFilter = wgpu::FilterMode::Linear;
    webgpu::TextureWithSampler output{};
    output.size = {size.outputWidth, size.outputHeight, 1};
    output.format = size.format;
    output.texture = slot.scaler->output_texture();
    output.view = slot.scaler->output_view();
    output.sampler = g_device.CreateSampler(&samplerDescriptor);
    slot.output = std::move(output);
    slot.bindGroup = webgpu::create_copy_bind_group(slot.output);
    Log.info("MetalFX spatial slot: {}x{} -> {}x{}", size.inputWidth, size.inputHeight,
             size.outputWidth, size.outputHeight);
  }
  if (!slot.scaler->begin_input()) {
    metalfx_failed(slot.scaler->error());
    return {};
  }
  const wgpu::RenderPassColorAttachment attachment{
      .view = slot.scaler->input_view(),
      .loadOp = wgpu::LoadOp::Clear,
      .storeOp = wgpu::StoreOp::Store,
  };
  const wgpu::RenderPassDescriptor descriptor{
      .label = "MetalFX input copy",
      .colorAttachmentCount = 1,
      .colorAttachments = &attachment,
  };
  auto pass = encoder.BeginRenderPass(&descriptor);
  pass.SetPipeline(webgpu::g_CopyPipeline);
  pass.SetBindGroup(0, webgpu::create_copy_bind_group(source));
  pass.SetViewport(0, 0, static_cast<float>(size.inputWidth), static_cast<float>(size.inputHeight), 0, 1);
  pass.Draw(3);
  pass.End();
  // Submit the sealed scene and input copy before crossing to the native queue.
  // The replacement encoder composites the upscaled image and ImGui normally.
  auto buffer = encoder.Finish();
  g_queue.Submit(1, &buffer);
  encoder = g_device.CreateCommandEncoder();
  if (!slot.scaler->upscale()) {
    metalfx_failed(slot.scaler->error());
    return {};
  }
  g_metalfxPendingOutput = slot.scaler.get();
  g_metalfxPendingImage = &slot.output;
  g_metalfxStatus.store(AURORA_METALFX_ACTIVE);
  return slot.bindGroup;
}


uint32_t clamp_scissor_coord(double value, uint32_t maximum) noexcept {
  if (!std::isfinite(value)) {
    return 0;
  }

  return static_cast<uint32_t>(std::clamp(value, 0.0, static_cast<double>(maximum)));
}

void set_present_viewport(const wgpu::RenderPassEncoder& pass, const gfx::Viewport& viewport, uint32_t surfaceWidth,
                          uint32_t surfaceHeight) noexcept {
  pass.SetViewport(viewport.left, viewport.top, viewport.width, viewport.height, viewport.znear, viewport.zfar);
  const auto scissorX = clamp_scissor_coord(std::floor(viewport.left), surfaceWidth);
  const auto scissorY = clamp_scissor_coord(std::floor(viewport.top), surfaceHeight);
  const auto scissorRight = clamp_scissor_coord(std::ceil(viewport.left + viewport.width), surfaceWidth);
  const auto scissorBottom = clamp_scissor_coord(std::ceil(viewport.top + viewport.height), surfaceHeight);
  pass.SetScissorRect(scissorX, scissorY, scissorRight - scissorX, scissorBottom - scissorY);
}
#endif

#ifdef AURORA_ENABLE_GX
constexpr std::array PreferredBackendOrder{
#ifdef ENABLE_BACKEND_WEBGPU
    BACKEND_WEBGPU,
#endif
#ifdef DAWN_ENABLE_BACKEND_D3D12
    BACKEND_D3D12,
#endif
#ifdef DAWN_ENABLE_BACKEND_METAL
    BACKEND_METAL,
#endif
#ifdef DAWN_ENABLE_BACKEND_VULKAN
    BACKEND_VULKAN,
#endif
#ifdef DAWN_ENABLE_BACKEND_D3D11
    BACKEND_D3D11,
#endif
// #ifdef DAWN_ENABLE_BACKEND_DESKTOP_GL
//     BACKEND_OPENGL,
// #endif
#ifdef DAWN_ENABLE_BACKEND_OPENGLES
    BACKEND_OPENGLES,
#endif
#ifdef DAWN_ENABLE_BACKEND_NULL
    BACKEND_NULL,
#endif
};
#else
constexpr std::array<AuroraBackend, 0> PreferredBackendOrder{};
#endif

bool g_initialFrame = false;

constexpr const char* backend_name(AuroraBackend backend) noexcept {
  switch (backend) {
  case BACKEND_AUTO:
    return "Auto";
  case BACKEND_D3D11:
    return "D3D11";
  case BACKEND_D3D12:
    return "D3D12";
  case BACKEND_METAL:
    return "Metal";
  case BACKEND_VULKAN:
    return "Vulkan";
  case BACKEND_OPENGL:
    return "OpenGL";
  case BACKEND_OPENGLES:
    return "OpenGLES";
  case BACKEND_WEBGPU:
    return "WebGPU";
  case BACKEND_NULL:
    return "Null";
  }
  return "Unknown";
}
#ifdef AURORA_ENABLE_GX
texture::ReplacementGroup g_textureReplacements;
#endif

AuroraInfo initialize(int argc, char* argv[], const AuroraConfig& config) noexcept {
  g_config = config;
  Log.info("Aurora initializing");
  log_system_information();

  if (g_config.appName == nullptr) {
    g_config.appName = "Aurora";
  } else {
    g_config.appName = strdup(g_config.appName);
  }

  if (g_config.userPath == nullptr) {
    g_config.userPath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.userPath = strdup(g_config.userPath);
  }

  if (g_config.cachePath == nullptr) {
    g_config.cachePath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.cachePath = strdup(g_config.cachePath);
  }

  if (g_config.resourcesPath == nullptr) {
    g_config.resourcesPath = SDL_GetBasePath();
  } else {
    g_config.resourcesPath = strdup(g_config.resourcesPath);
  }

  if (g_config.msaa == 0) {
    g_config.msaa = 1;
  }

  if (g_config.maxTextureAnisotropy == 0) {
    g_config.maxTextureAnisotropy = 16;
  }

  AURORA_ASSERT(window::initialize(), "Error initializing window");

  if (!audio::initialize()) {
    Log.warn("Error initializing audio: {}", SDL_GetError());
  }

  g_sdlCustomEventsStart = SDL_RegisterEvents(2);
  AURORA_ASSERT(g_sdlCustomEventsStart, "Failed to allocate user events: {}", SDL_GetError());
  AURORA_ASSERT(window::initialize_event_watch(), "Error initializing SDL event watch");

#ifdef AURORA_ENABLE_GX
  /* Attempt to create a window using the calling application's desired backend */
  const AuroraBackend requestedBackend = config.desiredBackend;
  AuroraBackend selectedBackend = requestedBackend;
  bool windowCreated = false;
  std::string firstGraphicsError;
  const auto rememberGraphicsError = [&] {
    if (firstGraphicsError.empty() && *SDL_GetError() != '\0') {
      firstGraphicsError = SDL_GetError();
    }
  };

  if (selectedBackend != BACKEND_AUTO) {
    Log.info("Requested graphics backend: {}", backend_name(selectedBackend));
    if (window::create_window(selectedBackend)) {
      if (webgpu::initialize(selectedBackend, config.allowCpuAdapter)) {
        windowCreated = true;
      } else {
        rememberGraphicsError();
        window::destroy_window();
      }
    } else {
      rememberGraphicsError();
    }
    if (!windowCreated) {
      // An unavailable requested backend falls back to the automatic search instead of aborting.
      Log.error("Requested graphics backend {} is unavailable; falling back to automatic selection",
                backend_name(requestedBackend));
    }
  }

  if (!windowCreated) {
    for (const auto backendType : PreferredBackendOrder) {
      selectedBackend = backendType;

      if (!window::create_window(selectedBackend)) {
        rememberGraphicsError();
        continue;
      }

      if (webgpu::initialize(selectedBackend, config.allowCpuAdapter)) {
        windowCreated = true;
        break;
      } else {
        rememberGraphicsError();
        window::destroy_window();
      }
    }
  }

  if (!windowCreated) {
    if (firstGraphicsError.empty()) {
      firstGraphicsError = "No supported graphics backend is available";
    }
    SDL_SetError("%s", firstGraphicsError.c_str());
    Log.error("Graphics initialization failed: {}", firstGraphicsError);
    return {
        .initializationStatus = AURORA_INITIALIZATION_GRAPHICS_UNAVAILABLE,
        .initializationError = SDL_GetError(),
    };
  }
  if (requestedBackend != BACKEND_AUTO && selectedBackend != requestedBackend) {
    Log.error("Graphics backend fallback in effect: requested {}, running on {}",
              backend_name(requestedBackend), backend_name(selectedBackend));
  }

  // Initialize SDL_Renderer for ImGui when we can't use a Dawn backend
  if (webgpu::g_backendType == wgpu::BackendType::Null) {
    AURORA_ASSERT(window::create_renderer(), "Failed to initialize SDL renderer: {}", SDL_GetError());
  }
#else
  AuroraBackend selectedBackend = BACKEND_NULL;
  AURORA_ASSERT(window::create_window(BACKEND_NULL), "Error creating window: {}", SDL_GetError());
  AURORA_ASSERT(window::create_renderer(), "Failed to initialize SDL renderer: {}", SDL_GetError());
#endif

  if (!g_config.startHidden) {
    window::show_window();
  }
  thread::set_current({
      .name = "Main thread",
      .affinity = thread::Affinity::SharedCache,
  });

#ifdef AURORA_ENABLE_GX
  g_metalfxSupported.store(webgpu::metalfx::supported(g_device, webgpu::g_backendType));
  g_metalfxStatus.store(AURORA_METALFX_DISABLED);
  gfx::initialize();
  gx::fifo::init();
  imgui::create_context();
#endif
  const auto size = window::get_window_size();
  Log.info("Using framebuffer size {}x{} scale {}", size.fb_width, size.fb_height, size.scale);
#ifdef AURORA_ENABLE_GX
  if (g_config.imGuiInitCallback != nullptr) {
    g_config.imGuiInitCallback(&size);
  }

  imgui::initialize();

  if (g_config.allowTextureReplacements) {
    g_textureReplacements =
        texture::load_replacement_directory(io::fs_path_from_string(g_config.userPath) / "texture_replacements");
  }
#endif

#ifdef AURORA_ENABLE_RMLUI
  rmlui::initialize(size);
#endif

  g_initialFrame = true;
  g_config.desiredBackend = selectedBackend;
  return {
      .backend = selectedBackend,
      .userPath = g_config.userPath,
      .cachePath = g_config.cachePath,
      .window = window::get_sdl_window(),
      .windowSize = size,
  };
}

void shutdown() noexcept {
  video::shutdown();
  audio::shutdown();
#ifdef AURORA_ENABLE_GX
  gx::fifo::shutdown();
  gfx::render_worker::synchronize();
#ifdef AURORA_ENABLE_RMLUI
  rmlui::shutdown();
#endif
  g_metalfxSlots = {};
  g_metalfxPendingOutput = nullptr;
  g_metalfxNextSlot = 0;
  g_metalfxFailed = false;
  g_metalfxRequested.store(false);
  g_metalfxSupported.store(false);
  g_metalfxStatus.store(AURORA_METALFX_DISABLED);
  g_dlss = {};
  imgui::shutdown();
  gfx::shutdown();
  webgpu::upscale::shutdown();
  webgpu::shutdown();
#endif
  gamepad::shutdown();
  window::shutdown();
}

const AuroraEvent* update() noexcept {
  ZoneScoped;

  if (g_initialFrame) {
    g_initialFrame = false;
    gamepad::initialize();
  }
  const auto* events = window::poll_events();
  gamepad::update();
#ifdef AURORA_ENABLE_GX
  {
    // DLSS becomes ready (or fails) a couple of seconds after start-up; re-evaluate the internal resolution then.
    static bool dlssReadyLast = false;
    if (const bool ready = webgpu::dlss::ready(); ready != dlssReadyLast) {
      dlssReadyLast = ready;
      sync_upscale_window();
    }
  }
#endif
  return events;
}

bool begin_frame_window() noexcept {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  {
    if (!window::is_presentable()) {
      webgpu::release_surface();
      return false;
    }

    if (window::is_paused()) {
      return false;
    }

    if (!g_surface) {
      webgpu::refresh_surface(true);

      if (!g_surface) {
        return false;
      }
    }
  }

  reflex::begin();
  imgui::new_frame(window::get_window_size());
#endif
  return true;
}

bool begin_frame_gx() noexcept {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  gx::update();
  window::apply_pending_surface_changes();
  if (!gfx::begin_frame()) {
    return false;
  }

  gx::fifo::begin_frame();
  gx::flush_texture_destructions();
#endif
  return true;
}

bool begin_frame() noexcept { return begin_frame_window() && begin_frame_gx(); }

void end_frame_gx(imgui::DrawData imguiDrawData, uint64_t reflexFrame) noexcept {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  gx::flush_texture_destructions();
  gx::fifo::drain();
  gx::fifo::end_frame();
  gx::texture::end_frame();
  gfx::finish();
  const auto pixelPerfectGui = gfx::pixel_perfect_gui_present();

  // A display copy (the XFB) is what reaches the screen; post-copy EFB draws must not leak in.
  webgpu::TextureWithSampler presentSource = webgpu::present_source();
  gfx::TextureHandle displayCopy;
  if (gx::g_gxState.displayCopyProduced && gx::g_gxState.displayCopyTexture) {
    displayCopy = gx::g_gxState.displayCopyTexture;
    presentSource.texture = displayCopy->texture;
    presentSource.view = displayCopy->sampleTextureView;
    presentSource.size = displayCopy->size;
    presentSource.format = displayCopy->format;
  }
  gx::g_gxState.displayCopyProduced = false;
  const auto surfaceWidth = webgpu::g_graphicsConfig.surfaceConfiguration.width;
  const auto surfaceHeight = webgpu::g_graphicsConfig.surfaceConfiguration.height;
  float presentAspect = 0.f;
  auto viewport =
      window::get_present_aspect_ratio(presentAspect)
          ? webgpu::calculate_present_viewport_for_aspect(surfaceWidth, surfaceHeight, presentAspect)
          : webgpu::calculate_present_viewport(surfaceWidth, surfaceHeight, presentSource.size.width,
                                               presentSource.size.height);
  const bool metalfxEnabled = g_metalfxRequested.load();
  const auto requestedSampler = webgpu::get_resampler();
  const bool integerPresentation = requestedSampler == SAMPLER_INTEGER && !metalfxEnabled &&
      webgpu::upscale::settings().mode == AURORA_UPSCALER_OFF;
  const auto sampler = requestedSampler == SAMPLER_INTEGER && !integerPresentation ? SAMPLER_BILINEAR : requestedSampler;
  if (integerPresentation) {
    viewport = webgpu::calculate_integer_present_viewport(surfaceWidth, surfaceHeight, presentSource.size.width,
                                                          presentSource.size.height, viewport);
  }
  if (surfaceWidth > 0 && surfaceHeight > 0 && viewport.width > 0.f && viewport.height > 0.f) {
    const std::lock_guard lock{g_presentVpMutex};
    g_presentVp = std::array{viewport.left / static_cast<float>(surfaceWidth),
                             viewport.top / static_cast<float>(surfaceHeight),
                             viewport.width / static_cast<float>(surfaceWidth),
                             viewport.height / static_cast<float>(surfaceHeight)};
  }

  wgpu::BindGroup rmlBindGroup;
  bool rmlOverlay = false;
#if AURORA_ENABLE_RMLUI
  if (rmlui::is_initialized()) {
    auto rmlFrame = rmlui::record_frame(viewport);
    rmlBindGroup = std::move(rmlFrame.bindGroup);
    rmlOverlay = rmlFrame.overlay;
  }
#endif

  const auto schedule = gfx::presentation::present_schedule();
  const auto sealedAt = gfx::presentation::now_nanos();
  const bool black = video::is_black();
  const float brightness = video::brightness();
  gfx::end_frame([reflexFrame, black, brightness, metalfxEnabled, integerPresentation, sampler,
                  rmlBindGroup = std::move(rmlBindGroup), rmlOverlay, viewport,
                  presentSource, displayCopy = std::move(displayCopy), pixelPerfectGui, schedule, sealedAt,
                  imguiDrawData = std::move(imguiDrawData)](
                     wgpu::CommandEncoder& encoder, std::vector<gfx::AfterSubmitCallback> afterSubmitCallbacks,
                     const gfx::InterpolationReplay& replay) {
    const auto presentMode = webgpu::g_graphicsConfig.surfaceConfiguration.presentMode;
    const bool blockingPresent = presentMode == wgpu::PresentMode::Fifo || presentMode == wgpu::PresentMode::FifoRelaxed;
    if (replay.nativeUncapped) gfx::presentation::request_native_surface();
    const auto present = [&](wgpu::CommandEncoder& encoder, bool finalPresent, bool show) {
      webgpu::dlss::fg_render_frame(reflexFrame);
      using M = reflex::Marker;
      const bool extra = replay.scenePresentOnly || !finalPresent;
      reflex::mark(reflexFrame, extra ? M::ExtraSubmitStart : M::SubmitStart);
      const uint64_t phaseStart = gfx::presentation::now_nanos();
      uint64_t acquireNanos=0, submitNanos=0, presentNanos=0;
      if (show && blockingPresent) {
        const wgpu::CommandBufferDescriptor sceneDescriptor{.label = "Scene command buffer"};
        const auto sceneCommands = encoder.Finish(&sceneDescriptor);
        g_queue.Submit(1, &sceneCommands);

        const wgpu::CommandEncoderDescriptor presentDescriptor{.label = "Present encoder"};
        encoder = g_device.CreateCommandEncoder(&presentDescriptor);
      }

      wgpu::Texture currentTexture;
      wgpu::TextureView currentView;
      wgpu::Surface presentSurface;
      bool acquiredAsync=false;
      auto surfaceStatus = wgpu::SurfaceGetCurrentTextureStatus::Error;
      uint64_t surfaceWaitNanos = 0;
      const auto& surfaceConfig = webgpu::g_graphicsConfig.surfaceConfiguration;
      if (show && window::is_presentable())
        currentTexture = webgpu::dlss::fg_target(surfaceConfig.width, surfaceConfig.height, surfaceConfig.format);
      const bool nativePresenter = static_cast<bool>(currentTexture);
      if (nativePresenter) {
        currentView = currentTexture.CreateView();
      } else if (show) {
        auto acquired=gfx::presentation::take_native_surface(!replay.nativeUncapped);
        if (acquired) {
          acquiredAsync=true;
          presentSurface=std::move(acquired->surface);
          surfaceWaitNanos=acquireNanos=acquired->waitedNanos;
          surfaceStatus=acquired->texture.status;
          if(surfaceStatus==wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal && acquired->texture.texture) {
            currentTexture=std::move(acquired->texture.texture);
            currentView=currentTexture.CreateView();
          }
        } else if (!replay.nativeUncapped) {
          window::SurfaceLock surfaceLock;
          if (window::is_presentable() && g_surface) {
          ZoneScopedN("Acquire texture");
          wgpu::SurfaceTexture surfaceTexture;
          const uint64_t acquireStart = gfx::presentation::now_nanos();
          presentSurface=g_surface;
          g_surface.GetCurrentTexture(&surfaceTexture);
          surfaceWaitNanos = gfx::presentation::now_nanos() - acquireStart;
          acquireNanos=surfaceWaitNanos;
          surfaceStatus = surfaceTexture.status;

          if (surfaceStatus == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal) {
            currentTexture = std::move(surfaceTexture.texture);
            currentView = currentTexture.CreateView();
          }
          }
        }
      }

      const bool canPresent = currentTexture && currentView;

      if (canPresent) {
        wgpu::BindGroup presentBindGroup;

        if (rmlBindGroup && !rmlOverlay) {
          presentBindGroup = rmlBindGroup;
        } else {
          const bool exactPixels = integerPresentation &&
              gfx::pixel_perfect::integer_scale(presentSource.size.width, static_cast<uint32_t>(viewport.width)) &&
              gfx::pixel_perfect::integer_scale(presentSource.size.height, static_cast<uint32_t>(viewport.height)) &&
              static_cast<uint32_t>(viewport.width) / presentSource.size.width ==
                  static_cast<uint32_t>(viewport.height) / presentSource.size.height;
          const bool sceneEffects = aurora_enhanced_bloom_enabled();
          const bool replayHud = (!exactPixels || sceneEffects) && displayCopy && gfx::presentation_hud_ready(pixelPerfectGui);
          auto scene = replayHud ? gfx::prepare_presentation_hud_scene(encoder, pixelPerfectGui) : presentSource;
          if (replayHud && s_enhancedBloom.load(std::memory_order_relaxed)) {
            const webgpu::Viewport sceneViewport{0.f, 0.f, static_cast<float>(scene.size.width),
                                                static_cast<float>(scene.size.height), 0.f, 1.f};
            scene = webgpu::resample_present_source(encoder, sceneViewport, scene, SAMPLER_BILINEAR, true);
          }
          const webgpu::TextureWithSampler* gameImage = nullptr;
          bool fsrOutput = false;
          auto fsr = dlss_dispatch(encoder, scene, viewport, replayHud, pixelPerfectGui);
          if (!fsr.bindGroup) {
            fsr = webgpu::upscale::fsr1_dispatch(encoder, scene, viewport);
            fsrOutput = fsr.image != nullptr;
          }
          if (fsr.bindGroup) {
            presentBindGroup = std::move(fsr.bindGroup);
            gameImage = fsr.image;
          } else {
            presentBindGroup = upscale_presentation(encoder, scene, viewport, metalfxEnabled);
            if (presentBindGroup) {
              gameImage = g_metalfxPendingImage;
            }
          }
          if (replayHud) {
            const auto& target = fsrOutput ? *gameImage
                : webgpu::resample_present_source(encoder, viewport, gameImage ? *gameImage : scene, sampler);
            gfx::render_presentation_hud(encoder, pixelPerfectGui, target);
            gameImage = &target;
            if (!fsrOutput) {
              presentBindGroup = webgpu::create_copy_bind_group(target);
            }
          } else if (!presentBindGroup) {
            const bool oneToOne = viewport.width == static_cast<float>(scene.size.width) &&
                                  viewport.height == static_cast<float>(scene.size.height);
            gameImage = oneToOne ? &scene : &webgpu::resample_present_source(encoder, viewport, scene, sampler);
            presentBindGroup = webgpu::create_copy_bind_group(*gameImage);
          }
          if (gameImage != nullptr) {
            webgpu::upscale::capture_encode(encoder, *gameImage);
          }
        }

        {
          const std::array attachments{
              wgpu::RenderPassColorAttachment{
                  .view = currentView,
                  .loadOp = wgpu::LoadOp::Clear,
                  .storeOp = wgpu::StoreOp::Store,
              },
          };

          const wgpu::RenderPassDescriptor renderPassDescriptor{
              .label = "EFB copy render pass",
              .colorAttachmentCount = attachments.size(),
              .colorAttachments = attachments.data(),
              .timestampWrites = webgpu::gpu_prof::pass_writes("Present blit"),
          };

          const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
          // Copy EFB -> XFB (swapchain)
          if (brightness < 1.0f) {
            pass.SetPipeline(webgpu::g_CopyDimmedPipeline);
            const wgpu::Color blend{brightness, brightness, brightness, 1.0};
            pass.SetBlendConstant(&blend);
          } else {
            pass.SetPipeline(webgpu::g_CopyPipeline);
          }

          pass.SetBindGroup(0, presentBindGroup, 0, nullptr);
          set_present_viewport(pass, viewport, webgpu::g_graphicsConfig.surfaceConfiguration.width,
                               webgpu::g_graphicsConfig.surfaceConfiguration.height);

          if (!black) {
            pass.Draw(3);
          }

          if (rmlBindGroup && rmlOverlay) {
            pass.SetPipeline(webgpu::g_CopyPremultipliedAlphaPipeline);
            pass.SetBindGroup(0, rmlBindGroup, 0, nullptr);
            pass.Draw(3);
          }

          pass.End();
        }

        {
          const std::array attachments{
              wgpu::RenderPassColorAttachment{
                  .view = currentView,
                  .loadOp = wgpu::LoadOp::Load,
                  .storeOp = wgpu::StoreOp::Store,
              },
          };

          const wgpu::RenderPassDescriptor renderPassDescriptor{
              .label = "ImGui render pass",
              .colorAttachmentCount = attachments.size(),
              .colorAttachments = attachments.data(),
              .timestampWrites = webgpu::gpu_prof::pass_writes("ImGui"),
          };

          const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
          pass.SetViewport(0.f, 0.f, static_cast<float>(webgpu::g_graphicsConfig.surfaceConfiguration.width),
                           static_cast<float>(webgpu::g_graphicsConfig.surfaceConfiguration.height), 0.f, 1.f);
          imgui::render(pass, imguiDrawData);
          pass.End();
        }
      } else if (show) {
        Log.info("Skipping present; window not presentable");
      }

      webgpu::dlss::capture_fg(encoder, currentTexture, window::frame_generation_window());
      if (finalPresent) {
        webgpu::gpu_prof::frame_end(encoder);
      }
      const wgpu::CommandBufferDescriptor cmdBufDescriptor{.label = "Redraw command buffer"};
      const auto buffer = encoder.Finish(&cmdBufDescriptor);
      {
        ZoneScopedN("Queue Submit");
        const uint64_t start=gfx::presentation::now_nanos();
        g_queue.Submit(1, &buffer);
        submitNanos=gfx::presentation::now_nanos()-start;
      }
      reflex::mark(reflexFrame, extra ? M::ExtraSubmitEnd : M::SubmitEnd);
      webgpu::upscale::capture_after_submit();

      if (g_metalfxPendingOutput) {
        if (!g_metalfxPendingOutput->end_output()) metalfx_failed(g_metalfxPendingOutput->error());
        g_metalfxPendingOutput = nullptr;
      }
      webgpu::dlss::after_submit();
      webgpu::dlss::present_fg(blockingPresent);
      if (finalPresent) {
        webgpu::gpu_prof::after_submit();
      }
      if (nativePresenter) {
        gfx::after_present();
        gfx::presentation::record_present(false);
      } else if (canPresent && g_surface) {
        ZoneScopedN("Present");
        wgpu::ConvertibleStatus status = wgpu::Status::Error;
        {
          window::SurfaceLock surfaceLock;

          if (window::is_presentable()) {
            const uint64_t presentStart = gfx::presentation::now_nanos();
            reflex::mark(reflexFrame, extra ? M::ExtraPresentStart : M::PresentStart);
            status = presentSurface.Present();
            reflex::mark(reflexFrame, extra ? M::ExtraPresentEnd : M::PresentEnd);
            presentNanos=gfx::presentation::now_nanos()-presentStart;
            surfaceWaitNanos += presentNanos;
          }
        }
        if (blockingPresent) {
          gfx::presentation::report_display_present_wait(surfaceWaitNanos);
        }

        if (status) {
          gfx::after_present();
          gfx::presentation::record_present(false);
        } else {
          Log.warn("Surface present failed");
          webgpu::release_surface();
        }
      } else if (show && g_surface) {
        switch (surfaceStatus) {
        case wgpu::SurfaceGetCurrentTextureStatus::Timeout:
          Log.warn("Surface texture acquisition timed out");
          break;
        case wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal:
        case wgpu::SurfaceGetCurrentTextureStatus::Outdated:
          Log.info("Surface texture is {}, reconfiguring swapchain", magic_enum::enum_name(surfaceStatus));
          window::push_custom_event(window::CustomEvent::RefreshSurface);
          break;
        case wgpu::SurfaceGetCurrentTextureStatus::Lost:
          Log.warn("Surface texture is {}, releasing surface", magic_enum::enum_name(surfaceStatus));
          webgpu::release_surface();
          break;
        case wgpu::SurfaceGetCurrentTextureStatus::Error:
          Log.warn("Surface texture is {}, releasing surface", magic_enum::enum_name(surfaceStatus));
          webgpu::release_surface();
          break;
        default:
          if (!window::is_presentable()) {
            webgpu::release_surface();
          } else {
            Log.error("Failed to get surface texture: {}", magic_enum::enum_name(surfaceStatus));
          }

          break;
        }
      }
      if(acquiredAsync) gfx::presentation::finish_native_surface(replay.nativeUncapped);
      static const bool profile = std::getenv("SIN_RENDER_PROF") != nullptr;
      if (profile && canPresent) {
        static uint64_t count=0, acquire=0, submit=0, display=0, total=0, maxPacketAge=0;
        acquire+=acquireNanos; submit+=submitNanos; display+=presentNanos;
        const auto finishedAt = gfx::presentation::now_nanos();
        total+=finishedAt-phaseStart;
        maxPacketAge=std::max(maxPacketAge, finishedAt-sealedAt);
        if (++count==120) {
          std::fprintf(stderr,"[present-phase] acquire=%.3fms submit=%.3fms present=%.3fms total=%.3fms max_packet_age=%.3fms\n",
                       acquire/120000000.0,submit/120000000.0,display/120000000.0,total/120000000.0,maxPacketAge/1000000.0);
          count=acquire=submit=display=total=maxPacketAge=0;
        }
      }
    };

    // Interpolated slots replay the frame with blended transforms and present between frames. The
    // present points lie on a grid of period `step` guest frames (60 / target Hz); the plan gives each
    // slot's blend weight and slot k goes out at anchor + interval * (weight - step). The native frame
    // (weight 1) goes last when it falls on the grid, otherwise it is submitted without presenting.
    // A display no faster than the native frame rate cannot show them, so present native frames right away.
    const auto& plan = replay.plan;
    const uint32_t slots =
        gfx::presentation::display_shows_interpolation(schedule.intervalNanos) ? std::min(replay.slots, plan.slots) : 0;
    const bool nativeShown = replay.scenePresentOnly || (!replay.nativeUncapped && (plan.nativeShown || slots == 0));
    const auto deadline = [&](float weight) -> uint64_t {
      if (slots == 0 || schedule.baseNanos == 0 || schedule.intervalNanos == 0) {
        return 0;
      }
      const double offset = (static_cast<double>(weight) - static_cast<double>(plan.step)) *
                            static_cast<double>(schedule.intervalNanos);
      return static_cast<uint64_t>(static_cast<int64_t>(schedule.baseNanos) + static_cast<int64_t>(offset));
    };
    const uint64_t stepNanos = static_cast<uint64_t>(static_cast<double>(plan.step) * static_cast<double>(schedule.intervalNanos));
    const auto admit = [&](uint64_t presentAt) {
      return !blockingPresent ||
             gfx::presentation::admit_display_present(std::max(presentAt, gfx::presentation::now_nanos()));
    };
    bool replayUnavailable = false;
    // Interpolated slots are optional; the guest frame rate is not. A slot is dropped when a newer
    // guest frame is already sealed (or the guest is blocked on this worker), or when its measured
    // cost would push the native frame past its own present time. Without this an expensive slot
    // (e.g. one DLSS evaluation per present) backs up the worker and slows the game itself.
    static std::atomic_uint64_t s_guardStale{0}, s_guardBudget{0}, s_guardCancelled{0};
    const uint64_t nativeDue = deadline(1.f);
    const auto newerFrame = []() noexcept { return gfx::render_worker::newer_frame_pending(); };
    for (uint32_t slot = 0; slot < slots; ++slot) {
      const uint64_t due = deadline(plan.weight[slot]);
      // A slot that would only show after the next grid point is due adds latency without motion.
      if (due != 0 && stepNanos != 0 && gfx::presentation::now_nanos() >= due + stepNanos) {
        continue;
      }
      if (newerFrame()) {
        s_guardStale.fetch_add(slots - slot, std::memory_order_relaxed);
        break;
      }
      if (const uint64_t cost = gx::present_cost_nanos(); cost != 0 && nativeDue != 0) {
        constexpr uint64_t Tolerance = 1'000'000;
        if (std::max(gfx::presentation::now_nanos(), due) + cost > nativeDue + Tolerance) {
          s_guardBudget.fetch_add(slots - slot, std::memory_order_relaxed);
          gx::decay_present_cost();
          break;
        }
      }
      if (!admit(due)) {
        continue;
      }
      const uint64_t encodeStart = gfx::presentation::now_nanos();
      const wgpu::CommandEncoderDescriptor slotDescriptor{.label = "Interpolated frame encoder"};
      auto slotEncoder = g_device.CreateCommandEncoder(&slotDescriptor);
      gx::g_velocityWeight = plan.weight[slot];
      if (!replay.encode(slotEncoder, slot)) {
        replayUnavailable = true;
        break;
      }
      const uint64_t encodeNanos = gfx::presentation::now_nanos() - encodeStart;
      if (!replay.nativeUncapped && !gfx::presentation::pace_present_unless(due, newerFrame)) {
        // A newer frame arrived while waiting for this slot's present time. The encoder may hold the
        // frame's staging upload, so submit it without showing, and skip the remaining slots.
        present(slotEncoder, false, false);
        s_guardCancelled.fetch_add(slots - slot, std::memory_order_relaxed);
        break;
      }
      const uint64_t presentStart = gfx::presentation::now_nanos();
      present(slotEncoder, false, true);
      gx::report_present_cost(encodeNanos + (gfx::presentation::now_nanos() - presentStart));
    }
    {
      static const bool perfLog = [] {
        const char* v = std::getenv("SIN_PERF_LOG");
        return v != nullptr && *v != '\0' && *v != '0';
      }();
      static uint64_t lastLog = 0;
      if (const uint64_t now = gfx::presentation::now_nanos(); perfLog && now - lastLog >= 1'000'000'000ull) {
        lastLog = now;
        std::fprintf(stderr, "[interp-guard] slots dropped: newer_frame=%llu budget=%llu cancelled_wait=%llu cost=%.2fms\n",
                     static_cast<unsigned long long>(s_guardStale.exchange(0)),
                     static_cast<unsigned long long>(s_guardBudget.exchange(0)),
                     static_cast<unsigned long long>(s_guardCancelled.exchange(0)),
                     static_cast<double>(gx::present_cost_nanos()) / 1.0e6);
      }
    }
    if (replay.restoreBeforeNative) replay.restoreBeforeNative();
    // The native frame was encoded with its own jitter before the slots changed the current one.
    webgpu::dlss::restore_native_jitter();
    if (nativeShown || replayUnavailable) {
      const uint64_t due = replayUnavailable ? 0 : deadline(1.f);
      if (!replay.nativeUncapped) gfx::presentation::pace_present(due);
      if (!replay.scenePresentOnly) gx::g_velocityWeight = 1.f;
      present(encoder, !replay.scenePresentOnly, admit(due));
    } else {
      present(encoder, true, false);
    }

    if (replay.scenePresentOnly) return;
    for (auto& callback : afterSubmitCallbacks) {
      if (callback) {
        callback();
      }
    }

    gfx::depth_peek::after_submit();

    TracyPlotConfig("aurora: lastVertSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastUniformSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastIndexSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastStorageSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastTextureUploadSize", tracy::PlotFormatType::Memory, false, true, 0);

    const auto& stats = gfx::detail::resources().stats;
    TracyPlot("aurora: queuedPipelines", static_cast<int64_t>(stats.queuedPipelines));
    TracyPlot("aurora: createdPipelines", static_cast<int64_t>(stats.createdPipelines));
    TracyPlot("aurora: drawCallCount", static_cast<int64_t>(stats.drawCallCount));
    TracyPlot("aurora: mergedDrawCallCount", static_cast<int64_t>(stats.mergedDrawCallCount));
    TracyPlot("aurora: lastVertSize", static_cast<int64_t>(stats.lastVertSize));
    TracyPlot("aurora: lastUniformSize", static_cast<int64_t>(stats.lastUniformSize));
    TracyPlot("aurora: lastIndexSize", static_cast<int64_t>(stats.lastIndexSize));
    TracyPlot("aurora: lastStorageSize", static_cast<int64_t>(stats.lastStorageSize));
    TracyPlot("aurora: lastTextureUploadSize", static_cast<int64_t>(stats.lastTextureUploadSize));
  });

#endif
}

void end_frame() noexcept {
#ifdef AURORA_ENABLE_GX
  end_frame_gx(imgui::freeze(), reflex::end());
#endif
}
} // namespace
} // namespace aurora

struct AuroraImguiFrame {
  aurora::imgui::DrawData drawData;
  uint64_t reflexFrame;
};

// C API bindings
AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config) {
  return aurora::initialize(argc, argv, *config);
}

void aurora_shutdown() { aurora::shutdown(); }

const AuroraEvent* aurora_update() { return aurora::update(); }

bool aurora_begin_frame() { return aurora::begin_frame(); }

void aurora_end_frame() { aurora::end_frame(); }

bool aurora_begin_frame_window() { return aurora::begin_frame_window(); }

bool aurora_begin_frame_gx() { return aurora::begin_frame_gx(); }

AuroraImguiFrame* aurora_end_frame_window() {
#ifdef AURORA_ENABLE_GX
  return new AuroraImguiFrame{aurora::imgui::freeze(), aurora::reflex::end()};
#else
  return nullptr;
#endif
}

void aurora_release_imgui_frame(AuroraImguiFrame* frame) { delete frame; }

void aurora_end_frame_gx(AuroraImguiFrame* frame) {
  std::unique_ptr<AuroraImguiFrame> owned{frame};
  aurora::end_frame_gx(owned ? std::move(owned->drawData) : aurora::imgui::DrawData{}, owned ? owned->reflexFrame : 0);
}

SDL_Window* aurora_get_window() { return aurora::window::get_sdl_window(); }

bool aurora_get_present_viewport(float* left, float* top, float* width, float* height) {
#ifdef AURORA_ENABLE_GX
  const std::lock_guard lock{aurora::g_presentVpMutex};
  if (const auto& viewport = aurora::g_presentVp) {
    *left = (*viewport)[0];
    *top = (*viewport)[1];
    *width = (*viewport)[2];
    *height = (*viewport)[3];
    return true;
  }
#endif
  (void)left; (void)top; (void)width; (void)height;
  return false;
}

AuroraWindowSize aurora_get_window_size() {
  return aurora::window::get_sdl_window() != nullptr ? aurora::window::get_window_size() : AuroraWindowSize{};
}

AuroraBackend aurora_get_backend() { return aurora::g_config.desiredBackend; }

const AuroraBackend* aurora_get_available_backends(size_t* count) {
  if (count != nullptr) {
    *count = aurora::PreferredBackendOrder.size();
  }

  return aurora::PreferredBackendOrder.data();
}

void aurora_set_log_level(AuroraLogLevel level) { aurora::g_config.logLevel = level; }

void aurora_set_pause_on_focus_lost(bool value) { aurora::g_config.pauseOnFocusLost = value; }

void aurora_set_display_mode(AuroraDisplayMode mode) { aurora::window::set_display_mode(mode); }

AuroraDisplayMode aurora_get_display_mode() { return aurora::window::get_display_mode(); }

double aurora_get_display_refresh_rate() {
  const uint64_t period = aurora::window::refresh_period_nanos();
  return period != 0 ? 1e9 / static_cast<double>(period) : 0.0;
}

void aurora_set_metalfx_spatial(bool enabled) {
#ifdef AURORA_ENABLE_GX
  aurora::g_metalfxRequested.store(enabled);
#endif
}
bool aurora_get_metalfx_spatial() {
#ifdef AURORA_ENABLE_GX
  return aurora::g_metalfxRequested.load();
#else
  return false;
#endif
}
bool aurora_is_metalfx_spatial_supported() {
#ifdef AURORA_ENABLE_GX
  return aurora::g_metalfxSupported.load();
#else
  return false;
#endif
}
AuroraMetalFXStatus aurora_get_metalfx_status() {
#ifdef AURORA_ENABLE_GX
  return aurora::g_metalfxStatus.load();
#else
  return AURORA_METALFX_UNSUPPORTED;
#endif
}

void aurora_set_upscaler(AuroraUpscaler mode, float scaleFactor, float sharpness) {
#ifdef AURORA_ENABLE_GX
  aurora::webgpu::upscale::Settings settings;
  settings.mode = mode;
  settings.scaleFactor = scaleFactor;
  settings.sharpness = sharpness;
  aurora::webgpu::upscale::set_settings(settings);
  aurora::sync_upscale_window();
#else
  (void)mode;
  (void)scaleFactor;
  (void)sharpness;
#endif
}

void aurora_get_upscale_info(AuroraUpscaleInfo* info) {
  if (info == nullptr) {
    return;
  }
#ifdef AURORA_ENABLE_GX
  *info = aurora::webgpu::upscale::info();
#else
  *info = {};
#endif
}

void aurora_get_dlss_status(AuroraDlssStatus* status) {
  if (status == nullptr) {
    return;
  }
  *status = {};
#ifdef AURORA_ENABLE_GX
  const auto s = aurora::webgpu::dlss::status();
  switch (s.state) {
  case aurora::webgpu::dlss::State::Probing: status->state = AURORA_DLSS_CHECKING; break;
  case aurora::webgpu::dlss::State::Ready: status->state = AURORA_DLSS_AVAILABLE; break;
  case aurora::webgpu::dlss::State::Unavailable: status->state = AURORA_DLSS_UNAVAILABLE; break;
  case aurora::webgpu::dlss::State::Failed: status->state = AURORA_DLSS_FAILED; break;
  default: status->state = AURORA_DLSS_NOT_STARTED; break;
  }
  std::snprintf(status->reason, sizeof status->reason, "%s", s.reason.c_str());
  std::snprintf(status->version, sizeof status->version, "%s", s.version.c_str());
#else
  status->state = AURORA_DLSS_UNAVAILABLE;
  std::snprintf(status->reason, sizeof status->reason, "%s", "graphics are disabled in this build");
#endif
}

bool aurora_reflex_active() {
#ifdef AURORA_ENABLE_GX
  return aurora::reflex::active();
#else
  return false;
#endif
}
static std::atomic<bool> s_frameGenerationRequested{};
bool aurora_frame_generation_requested(void) { return s_frameGenerationRequested.load(); }
bool aurora_frame_generation_active(void) {
#ifdef AURORA_ENABLE_GX
  return aurora::webgpu::dlss::fg_active();
#else
  return false;
#endif
}
void aurora_set_frame_generation(bool enabled) {
  s_frameGenerationRequested.store(enabled);
#ifdef AURORA_ENABLE_GX
  aurora::webgpu::dlss::set_fg(enabled);
#endif
}
void aurora_set_reflex(bool enabled) {
#ifdef AURORA_ENABLE_GX
  aurora::reflex::request(enabled);
#endif
}

void aurora_set_dlss_motion_vectors(bool enabled) {
#ifdef AURORA_ENABLE_GX
  aurora::webgpu::dlss::set_motion_vectors_enabled(enabled);
#else
  (void)enabled;
#endif
}

void aurora_capture_present_image(const char* pngPath) {
#ifdef AURORA_ENABLE_GX
  aurora::webgpu::upscale::request_capture(pngPath != nullptr ? pngPath : "");
#else
  (void)pngPath;
#endif
}

void aurora_set_background_input(bool value) {
  aurora::g_config.allowJoystickBackgroundEvents = value;
  aurora::window::set_background_input(value);
}

void aurora_set_resampler(AuroraSampler sampler) {
#ifdef AURORA_ENABLE_GX
  aurora::webgpu::set_resampler(sampler);
#else
  (void)sampler;
#endif
}

void aurora_set_timescale(float scale) { aurora::time::set_scale(scale); }

float aurora_get_timescale() { return aurora::time::scale(); }
