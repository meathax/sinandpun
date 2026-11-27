#include "gpu.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <aurora/aurora.h>
#include <aurora/webgpu.hpp>
#include <aurora/gfx.h>
#include <aurora/render_size_limits.hpp>
#include <magic_enum.hpp>
#include <webgpu/webgpu_cpp.h>

#include "../gx/gx.hpp"
#include "../gfx/frame.hpp"
#include "../gfx/presentation.hpp"
#include "../gfx/recording.hpp"
#include "../gfx/render_worker.hpp"
#include "../internal.hpp"
#include "../window.hpp"
#include "dlss.hpp"
#include "gpu_prof.hpp"

#ifdef WEBGPU_DAWN
#include "../dawn/TracyPlatform.hpp"
#ifndef __MINGW32__
#include <dawn/native/DawnNative.h>
#endif
#endif

namespace aurora::webgpu {
static Module Log("aurora::gpu");

wgpu::Device g_device;
wgpu::Queue g_queue;
wgpu::Surface g_surface;
wgpu::BackendType g_backendType;
GraphicsConfig g_graphicsConfig;
TextureWithSampler g_frameBuffer;
TextureWithSampler g_frameBufferResolved;
TextureWithSampler g_depthBuffer;
TextureWithSampler g_normalBuffer;

// EFB -> XFB copy pipeline
static wgpu::BindGroupLayout g_CopyBindGroupLayout;
wgpu::RenderPipeline g_CopyPipeline, g_GiRestorePipeline;
wgpu::RenderPipeline g_CopyDimmedPipeline;
wgpu::RenderPipeline g_CopyPremultipliedAlphaPipeline;
wgpu::BindGroup g_CopyBindGroup;
static std::atomic<AuroraSampler> g_Resampler{SAMPLER_BILINEAR};
static wgpu::BindGroupLayout g_ResampleBindGroupLayout;
static wgpu::RenderPipeline g_ResamplePipeline, g_BouncePipeline;
static wgpu::Buffer g_ResampleUniformBuffer;
static TextureWithSampler g_occlusionFrameBuffer;
static std::array<TextureWithSampler, 6> g_giTargets;
static std::array<wgpu::Buffer, 6> g_giUniforms;
static wgpu::Buffer g_giNodes, g_giTriangles;
static wgpu::Buffer g_OcclusionUniformBuffer;
// Replayed passes (interpolation slots and uncapped idle re-renders) are encoded
// while the next native frame's GI queue writes may be queued but unsubmitted.
// They use their own uniforms so those writes cannot cross frames.
static std::array<wgpu::Buffer, 6> g_giReplayUniforms;
static wgpu::Buffer g_OcclusionReplayUniformBuffer;
static TextureWithSampler g_bloomFrameBuffer;
static wgpu::Buffer g_BloomUniformBuffer;
static TextureWithSampler g_resampledFrameBuffer;

static wgpu::Adapter g_adapter;
wgpu::Instance g_instance;
wgpu::AdapterInfo g_adapterInfo;
static wgpu::SurfaceCapabilities g_surfaceCapabilities;
bool g_hasCoreFeatures = false;
bool g_dualSourceBlendingSupported = false;
bool g_bcTexturesSupported = false;
bool g_astcTexturesSupported = false;
bool g_textureComponentSwizzleSupported = false;
static std::atomic_bool g_initialized = false;
static std::atomic_bool g_vsyncEnabled = true;
// "Uncapped" frame rate: present through a non-blocking mode even when VSync is configured.
static std::atomic_bool g_presentUncapped = false;

namespace {

AuroraLogLevel wgpu_log_level(wgpu::LoggingType type) {
  switch (type) {
  case wgpu::LoggingType::Verbose:
    return LOG_DEBUG;
  case wgpu::LoggingType::Info:
    return LOG_INFO;
  case wgpu::LoggingType::Warning:
    return LOG_WARNING;
  case wgpu::LoggingType::Error:
    return LOG_ERROR;
  default:
    return LOG_FATAL;
  }
}

void wgpu_log(wgpu::LoggingType type, wgpu::StringView message) {
  Log.report(wgpu_log_level(type), "WebGPU message: {}", message);
}

struct ResampleUniformBlock {
  uint32_t samplerMode = 0;
  float frameWidth = 0.f;
  float frameHeight = 0.f;
  float bounceGain = 1.25f;
  alignas(16) std::array<float, 4> projection{};
  std::array<float, 2> screenCenter{};
};

constexpr std::string_view resampleShaderSource = R"(
struct Uniforms {
    sampler_mode: u32,
    frame_width: f32,
    frame_height: f32,
    bounce_gain: f32,
    projection: vec4f,
    screen_center: vec2f,
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var s: sampler;
@group(0) @binding(2) var t: texture_2d<f32>;
@group(0) @binding(3) var scene_depth: texture_2d<f32>;
@group(0) @binding(4) var irradiance: texture_2d<f32>;
struct BvhNode { low: vec3f, escape: u32, high: vec3f, leaf: u32 };
struct SceneTriangle { a: vec3f, red: f32, b: vec3f, green: f32, c: vec3f, blue: f32 };
@group(0) @binding(5) var<storage, read> bvh: array<BvhNode>;
@group(0) @binding(6) var<storage, read> triangles: array<SceneTriangle>;

var<private> pos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(-1.0, 1.0),
    vec2(-1.0, -3.0),
    vec2(3.0, 1.0),
);
var<private> uvs: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(0.0, 0.0),
    vec2(0.0, 2.0),
    vec2(2.0, 0.0),
);

@vertex
fn vs_main(@builtin(vertex_index) vtxIdx: u32) -> VertexOutput {
    var out: VertexOutput;
    out.position = vec4<f32>(pos[vtxIdx], 0.0, 1.0);
    out.uv = uvs[vtxIdx];
    return out;
}

// BEGIN AREA SAMPLER REGION

fn sample_by_pixel(pixel: vec2<i32>) -> vec4<f32> {
    let source_dims = textureDimensions(t);
    let max_coord = vec2<i32>(source_dims) - vec2<i32>(1, 1);
    let coord = clamp(pixel, vec2<i32>(0, 0), max_coord);
    return textureLoad(t, coord, 0);
}

fn sample_area(frag_position: vec4<f32>) -> vec4<f32> {
    let source_size = vec2<f32>(textureDimensions(t));
    let target_size = max(vec2<f32>(uniforms.frame_width, uniforms.frame_height), vec2<f32>(1.0, 1.0));

    let source_min = clamp((frag_position.xy - vec2<f32>(0.5, 0.5)) / target_size,
                           vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 1.0)) * source_size;
    let source_max = clamp((frag_position.xy + vec2<f32>(0.5, 0.5)) / target_size,
                           vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 1.0)) * source_size;

    let first_pixel = vec2<i32>(floor(source_min));
    let last_pixel = vec2<i32>(ceil(source_max));
    let max_iterations: i32 = 16;

    var avg_color = vec4<f32>(0.0, 0.0, 0.0, 0.0);
    var total_weight = 0.0;

    for (var iy: i32 = 0; iy < max_iterations; iy = iy + 1) {
        let source_y = first_pixel.y + iy;
        if (source_y < last_pixel.y) {
            let y0 = f32(source_y);
            let weight_y = max(min(source_max.y, y0 + 1.0) - max(source_min.y, y0), 0.0);

            for (var ix: i32 = 0; ix < max_iterations; ix = ix + 1) {
                let source_x = first_pixel.x + ix;
                if (source_x < last_pixel.x) {
                    let x0 = f32(source_x);
                    let weight_x = max(min(source_max.x, x0 + 1.0) - max(source_min.x, x0), 0.0);
                    let weight = weight_x * weight_y;
                    avg_color += weight * sample_by_pixel(vec2<i32>(source_x, source_y));
                    total_weight += weight;
                }
            }
        }
    }

    return avg_color / max(total_weight, 0.000001);
}

// END AREA SAMPLER REGION

// Reconstruct positive eye distance from the reversed GX perspective projection.
fn eye_depth(p: vec2i) -> f32 {
    let raw = textureLoad(scene_depth, p, 0).r;
    return -uniforms.projection.w / max(raw - uniforms.projection.z, 1e-7);
}
fn eye_position(p: vec2i, dim: vec2i) -> vec3f {
    let ndc = (vec2f(p)+0.5)/vec2f(dim)*2.0-1.0-uniforms.screen_center;
    let z = eye_depth(p);
    return z * vec3f(ndc.x/uniforms.projection.x, -ndc.y/uniforms.projection.y, -1.0);
}
fn receiver_normal(p: vec2i, dim: vec2i, center: vec3f) -> vec3f {
    let hi = dim-1;
    let l = center-eye_position(clamp(p-vec2i(1,0),vec2i(0),hi),dim);
    let r = eye_position(clamp(p+vec2i(1,0),vec2i(0),hi),dim)-center;
    let u = center-eye_position(clamp(p-vec2i(0,1),vec2i(0),hi),dim);
    let d = eye_position(clamp(p+vec2i(0,1),vec2i(0),hi),dim)-center;
    let n = cross(select(d,u,abs(u.z)<abs(d.z)), select(r,l,abs(l.z)<abs(r.z)));
    return n * inverseSqrt(max(dot(n,n),1e-18));
}

fn triangle_hit(o: vec3f, direction: vec3f, tri: SceneTriangle) -> f32 {
    let ab = tri.b-tri.a;
    let ac = tri.c-tri.a;
    let p = cross(direction,ac);
    let determinant = dot(ab,p);
    if (abs(determinant)<1e-7) { return -1.0; }
    let fromA = o-tri.a;
    let u = dot(fromA,p)/determinant;
    let q = cross(fromA,ab);
    let v = dot(direction,q)/determinant;
    if (min(u,v)<0.0 || u+v>1.0) { return -1.0; }
    return dot(ac,q)/determinant;
}

fn trace_bounce(origin: vec3f, direction: vec3f, reach: f32, dim: vec2i) -> vec4f {
    let inverse = 1.0/select(direction,vec3f(1e-7),abs(direction)<vec3f(1e-7));
    var best = reach;
    var hit = 0xffffffffu;
    var nodeId = 0u;
    let end = bvh[0].escape;
    for (var visited=0u; nodeId<end && visited<96u; visited++) {
        let node = bvh[nodeId];
        let lower = (node.low-origin)*inverse;
        let upper = (node.high-origin)*inverse;
        let start = min(lower,upper);
        let stop = max(lower,upper);
        if (max(0.0,max(start.x,max(start.y,start.z))) > min(best,min(stop.x,min(stop.y,stop.z)))) {
            nodeId = node.escape;
        } else {
            for (var j=0u; j<(node.leaf & 7u); j++) {
                let id = (node.leaf >> 3u)+j;
                let t = triangle_hit(origin,direction,triangles[id]);
                if (t > 0.0001 && t < best) {
                    best = t;
                    hit = id;
                }
            }
            nodeId += 1u;
        }
    }
    if (hit == 0xffffffffu) { return vec4f(0); }
    let tri = triangles[hit];
    let worldHit = origin+direction*best;
    // Off-screen or occluded surfaces retain their own material radiance.
    let normal = normalize(cross(tri.b-tri.a,tri.c-tri.a));
    let facing = abs(dot(normal,direction));
    var light = vec3f(tri.red,tri.green,tri.blue) * (0.28+0.20*abs(normal.y));
    let z = -worldHit.z;
    let ndc = vec2f(worldHit.x,-worldHit.y)*uniforms.projection.xy/max(z,1e-6)+uniforms.screen_center;
    if (z>0.0 && all(abs(ndc)<vec2f(1))) {
        let pixel = clamp(vec2i((ndc*0.5+0.5)*vec2f(dim)),vec2i(0),dim-1);
        if (abs(eye_depth(pixel)-z) < 0.025*z) {
            let sample = textureLoad(t,pixel,0).rgb;
            // Bright, rapidly animated GX effects can overlap an opaque hit.
            // Limit their diffuse energy so a single tap cannot paint a flash.
            light = min(sample, vec3f(0.68));
        }
    }
    let weight = facing * (1.0-smoothstep(0.04,1.0,best/reach));
    return vec4f(light*light*weight,weight);
}

fn gather_rays(p: vec2i, dim: vec2i) -> vec4f {
    let center = eye_position(p,dim);
    let normal = receiver_normal(p,dim,center);
    if (dot(normal,normal)<0.5) { return vec4f(0); }
    let tangent = normalize(cross(select(vec3f(0,1,0),vec3f(1,0,0),abs(normal.y)>0.90),normal));
    let bitangent = cross(normal,tangent);
    // A low-discrepancy rotation spreads directions within the filter footprint.
    let cell = vec2f(p/4);
    let angle = 6.2831853*fract(dot(cell,vec2f(0.75487767,0.56984029)));
    let lateral = cos(angle)*tangent+sin(angle)*bitangent;
    let across = cross(normal,lateral);
    let origin = center+normal*max(0.018,-center.z*0.0016);
    let reach = clamp(-center.z*0.78,5.0,5000.0);
    // Keep both elevation strata at every receiver. Neighboring cells trace
    // opposing azimuths so the depth-aware spatial filter combines both sides.
    let side = select(1.0,-1.0,((p.x/4+p.y/4)&1)!=0);
    var light = trace_bounce(origin,side*0.8660254*lateral+0.5*normal,reach,dim);
    light += trace_bounce(origin,-side*0.5*across+0.8660254*normal,reach,dim);
    return vec4f(sqrt(clamp(light.rgb*0.32,vec3f(0),vec3f(1))),light.a*0.5);
}

// Each screen-space slice integrates only newly visible solid angle as it moves
// outward. Samples behind an already encountered horizon contribute no light.
fn gather_bounce(p: vec2i, dim: vec2i) -> vec4f {
    let center = eye_position(p,dim);
    let normal = receiver_normal(p,dim,center);
    let dirs = array<vec2f,8>(vec2f(1,0),vec2f(0.707,0.707),vec2f(0,1),vec2f(-0.707,0.707),
                               vec2f(-1,0),vec2f(-0.707,-0.707),vec2f(0,-1),vec2f(0.707,-0.707));
    // Only local geometry contributes to screen-space GI. Long screen-space
    // offsets copy foreground silhouettes onto unrelated receivers.
    // The 44 px cap was tuned at 1080 rows; scale it with height above that so
    // the search covers the same screen fraction at higher internal resolutions.
    let radius = clamp(0.045*f32(dim.y),8.0,44.0*max(1.0,f32(dim.y)/1080.0));
    var light = vec3f(0);
    var blocked = 0.0;
    for (var i=0; i<8; i++) {
        var horizon = 0.04;
        for (var j=1; j<=5; j++) {
            let q = p+vec2i(round(dirs[i]*max(2.0,radius*f32(j*j)/25.0)));
            if (any(q<vec2i(0)) || any(q>=dim)) { break; }
            let delta = eye_position(q,dim)-center;
            let dist = length(delta);
            let relativeDistance = dist/max(0.001,-center.z);
            let reach = 1.0-smoothstep(0.025,0.14,relativeDistance);
            let elevation = dot(normal,delta)/max(dist,1e-6);
            let visible = max(0.0,elevation-horizon)*reach;
            horizon = max(horizon,mix(horizon,elevation,reach));
            // Prefilter the emitter so a character or muzzle flash cannot
            // appear as a translated image in the indirect lighting.
            let uv = (vec2f(q)+0.5)/vec2f(dim);
            let radiance = min(textureSampleLevel(irradiance,s,uv,0.0).rgb,vec3f(0.63));
            light += visible*radiance*radiance;
        }
        blocked += max(0.0,horizon-0.04);
    }
    // Store square-root encoded diffuse irradiance for precision in an 8-bit target.
    return vec4f(sqrt(clamp(light/8.0,vec3f(0),vec3f(1))), blocked/8.0);
}
@fragment
fn fs_bounce(in: VertexOutput) -> @location(0) vec4<f32> {
    if (uniforms.sampler_mode == 8u) {
        let dim = vec2i(textureDimensions(scene_depth));
        let p = min(vec2i(in.position.xy)*4+2,dim-1);
        if (textureLoad(scene_depth,p,0).r <= 1e-7) { return vec4f(0); }
        return gather_rays(p,dim);
    }
    if (uniforms.sampler_mode == 10u) {
        let dim = vec2i(textureDimensions(scene_depth));
        let reduced = vec2i(textureDimensions(irradiance));
        let pixel = min(vec2i(in.position.xy)*4+2,dim-1);
        let raw = textureLoad(scene_depth,pixel,0).r;
        if (raw <= 1e-7) { return vec4f(0); }
        let depth = eye_depth(pixel);
        var sum = vec4f(0);
        var total = 0.0;
        // Tap spacing of 2 cells was tuned at 1080 rows (270 quarter rows). Widen it
        // with resolution so the denoise footprint stays a fixed fraction of the screen.
        let tap_stride = max(2, i32(round(f32(reduced.y)/135.0)));
        for (var y=-4;y<=4;y++) { for (var x=-4;x<=4;x++) {
            let offset = vec2i(x,y);
            let q = clamp(vec2i(in.position.xy)+tap_stride*offset,vec2i(0),reduced-1);
            let neighbor = min(q*4+2,dim-1);
            if (textureLoad(scene_depth,neighbor,0).r <= 1e-7) { continue; }
            let error = abs(eye_depth(neighbor)-depth)/max(depth,1e-6);
            let w = exp2(-0.14*f32(x*x+y*y)) / (1.0+1700.0*error*error);
            let tap = textureLoad(irradiance,q,0);
            sum += w*vec4f(tap.rgb*tap.rgb,tap.a);
            total += w;
        }}
        let blurred = sum/max(total,0.00001);
        return vec4f(sqrt(blurred.rgb),blurred.a);
    }
    if (uniforms.sampler_mode == 7u) {
        // Filter a broad emitter footprint so recognizable silhouettes cannot be
        // projected across the frame as indirect light. Alpha stores log eye depth.
        let dim = vec2i(textureDimensions(scene_depth));
        let footprint = vec2f(dim)/vec2f(uniforms.frame_width,uniforms.frame_height);
        let start = floor(in.position.xy)*footprint;
        var average = vec4f(0);
        for (var y=0;y<4;y++) { for (var x=0;x<4;x++) {
            let p = clamp(vec2i(start+(vec2f(vec2i(x,y))+0.5)*footprint/4.0),vec2i(0),dim-1);
            let radiance = textureLoad(t,p,0).rgb;
            let logZ = clamp((log2(max(eye_depth(p),0.004))+8.0)/32.0,0.0,1.0);
            average += vec4f(radiance*radiance,logZ);
        }}
        return vec4f(sqrt(average.rgb/16.0),average.a/16.0);
    }
    if (uniforms.sampler_mode == 5u) {
        let dim = vec2i(textureDimensions(scene_depth));
        let p = min(vec2i(in.position.xy)*2+1,dim-1);
        if (textureLoad(scene_depth,p,0).r <= 1e-7) { return vec4f(0.0); }
        return gather_bounce(p,dim);
    }
    if (uniforms.sampler_mode == 6u || uniforms.sampler_mode == 9u) {
        let p = vec2i(in.position.xy);
        let base = sample_by_pixel(p);
        let dim = vec2i(textureDimensions(scene_depth));
        if (textureLoad(scene_depth,p,0).r <= 1e-7) { return base; }
        let rt = uniforms.sampler_mode == 9u;
        let stride = select(2,4,rt);
        let grid = (vec2f(p)-f32(stride/2))/f32(stride);
        let nearest = vec2i(floor(grid+0.5));
        let z = eye_depth(p);
        var sum = vec3f(0);
        var total = 0.0;
        var blocked = 0.0;
        // Quadratic B-spline weights give adjacent grid cells matching slopes.
        for (var y=-1;y<=1;y++) { for (var x=-1;x<=1;x++) {
            let cell = nearest+vec2i(x,y);
            let q = clamp(cell,vec2i(0),vec2i(textureDimensions(irradiance))-1);
            let neighbor = min(q*stride+stride/2,dim-1);
            if (textureLoad(scene_depth,neighbor,0).r <= 1e-7) { continue; }
            let error = abs(eye_depth(neighbor)-z)/max(z,1e-6);
            let offset = abs(vec2f(cell)-grid);
            let tail = 1.5-offset;
            let spatial = select(0.5*tail*tail,0.75-offset*offset,offset<=vec2f(0.5));
            let windowWeight = spatial.x*spatial.y;
            let w = windowWeight/(1.0+40000.0*error*error);
            let tap = textureLoad(irradiance,q,0);
            sum += w*tap.rgb*tap.rgb;
            blocked += w*tap.a;
            total += w;
        }}
        let indirect = sum/max(total,0.002);
        // Both GI paths shade the receiver with a bounded relative gain. This
        // keeps isolated hits from washing dark metal white in strong-bounce mode.
        let gain = uniforms.bounce_gain * indirect / (vec3f(0.20)+indirect);
        let response = 0.45 * gain / (vec3f(1.0)+gain);
        return vec4f(base.rgb + base.rgb*(1.0-base.rgb)*response,base.a);
    }
    return vec4f(0);
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    if (uniforms.sampler_mode == 4u) {
        let p = vec2i(in.position.xy);
        let base = sample_by_pixel(p);
        let z = textureLoad(scene_depth, p, 0).r;
        let limit = vec2i(textureDimensions(scene_depth)) - 1;
        let r = max(1, i32(round(uniforms.frame_height / 180.0)));
        let dirs = array<vec2i, 4>(vec2i(r,0),vec2i(0,r),vec2i(r,r),vec2i(r,-r));
        var occlusion = 0.0;
        for (var i = 0; i < 4; i++) {
            let za = textureLoad(scene_depth, clamp(p + dirs[i], vec2i(0), limit), 0).r;
            let zb = textureLoad(scene_depth, clamp(p - dirs[i], vec2i(0), limit), 0).r;
            // Opposed taps cancel a planar depth slope. Large jumps indicate disjoint silhouettes.
            let delta = (vec2f(za, zb) - z) / max(z, 0.0001);
            let proximity = 1.0 - smoothstep(0.08, 0.25, max(abs(delta.x), abs(delta.y)));
            occlusion += smoothstep(0.002, 0.04, delta.x + delta.y) * proximity;
        }
        return vec4f(base.rgb * (1.0 - 0.065 * occlusion), base.a);
    }
    if (uniforms.sampler_mode == 3u) {
        let center = vec2i(in.position.xy);
        let base = sample_by_pixel(center);
        let radius = max(1, i32(round(uniforms.frame_height / 360.0)));
        var light = vec3f(0.0);
        for (var y = -1; y <= 1; y++) {
            for (var x = -1; x <= 1; x++) {
                let tap = sample_by_pixel(center + vec2i(x, y) * radius).rgb;
                let peak = max(tap.r, max(tap.g, tap.b));
                light += tap * smoothstep(0.72, 1.0, peak) * f32((2 - abs(x)) * (2 - abs(y)));
            }
        }
        return vec4f(base.rgb + (1.0 - base.rgb) * light * (0.25 / 16.0), base.a);
    }
    if (uniforms.sampler_mode == 2u) {
        let output = vec2<u32>(vec2f(uniforms.frame_width, uniforms.frame_height));
        let replication = output / textureDimensions(t);
        return vec4(sample_by_pixel(vec2<i32>(vec2<u32>(in.position.xy) / replication)).rgb, 1.0);
    }
    var color = textureSampleLevel(t, s, in.uv, 0.0);
    if (uniforms.sampler_mode == 1u) {
        color = sample_area(in.position);
    }
    return vec4(color.rgb, 1.0);
}
)"sv;

wgpu::TextureFormat to_linear(wgpu::TextureFormat format) {
  if (format == wgpu::TextureFormat::RGBA8UnormSrgb) {
    return wgpu::TextureFormat::RGBA8Unorm;
  }
  if (format == wgpu::TextureFormat::BGRA8UnormSrgb) {
    return wgpu::TextureFormat::BGRA8Unorm;
  }
  return format;
}

wgpu::TextureFormat best_surface_format() {
  if (g_surfaceCapabilities.formatCount == 0) {
    return wgpu::TextureFormat::Undefined;
  }
  for (size_t i = 0; i < g_surfaceCapabilities.formatCount; ++i) {
    const auto format = to_linear(g_surfaceCapabilities.formats[i]);
    if (format == wgpu::TextureFormat::RGBA8Unorm || format == wgpu::TextureFormat::BGRA8Unorm) {
      return format;
    }
  }
  return g_surfaceCapabilities.formats[0];
}

uint32_t sampler_mode(AuroraSampler sampler) noexcept {
  switch (sampler) {
  case SAMPLER_AREA:
    return 1;
  case SAMPLER_BILINEAR:
  default:
    return 0;
  }
}

uint32_t viewport_extent(float value) noexcept {
  return std::max(1u, static_cast<uint32_t>(std::lround(std::max(value, 1.f))));
}

} // namespace

bool vsync_enabled() noexcept {
  // VRR presents through the tearing-allowed mode and bypasses VSync entirely.
  return g_vsyncEnabled.load(std::memory_order_acquire) && !g_presentUncapped.load(std::memory_order_acquire) &&
         !gfx::presentation::vrr_wanted();
}

void set_present_uncapped(bool uncapped) noexcept {
  if (g_presentUncapped.exchange(uncapped, std::memory_order_acq_rel) != uncapped) {
    // The present mode depends on it; rebuild the swapchain at the next frame boundary.
    window::push_custom_event(window::CustomEvent::RefreshSurface);
  }
  // The frame-rate mode also feeds VRR's Auto rule and the limiter target.
  gfx::presentation::vrr_update();
}

const char* present_mode_name() noexcept {
  switch (g_graphicsConfig.surfaceConfiguration.presentMode) {
  case wgpu::PresentMode::Fifo:
    return "Fifo";
  case wgpu::PresentMode::FifoRelaxed:
    return "FifoRelaxed";
  case wgpu::PresentMode::Immediate:
    return "Immediate";
  case wgpu::PresentMode::Mailbox:
    return "Mailbox";
  default:
    return "Unknown";
  }
}

wgpu::PresentMode select_present_mode(const wgpu::SurfaceCapabilities& capabilities) noexcept {
  const auto supports = [&capabilities](const wgpu::PresentMode candidate) {
    for (size_t i = 0; i < capabilities.presentModeCount; ++i) {
      if (capabilities.presentModes[i] == candidate) {
        return true;
      }
    }
    return false;
  };
  // VRR (G-SYNC / FreeSync) needs presents that are not tied to a vblank. On D3D12 Immediate is the
  // only mode that creates the swapchain with ALLOW_TEARING and presents with DXGI_PRESENT_ALLOW_TEARING
  // (Mailbox is Present(0, 0) on a 3-buffer flip swapchain: no tearing, so the display stays fixed rate).
  const bool immediateSupported = supports(wgpu::PresentMode::Immediate);
  gfx::presentation::vrr_set_surface_caps(immediateSupported, g_backendType == wgpu::BackendType::D3D12);
  if (immediateSupported && gfx::presentation::vrr_wanted()) {
    gfx::presentation::vrr_note_applied(true);
    return wgpu::PresentMode::Immediate;
  }
  gfx::presentation::vrr_note_applied(false);
  if (immediateSupported && g_presentUncapped.load(std::memory_order_acquire))
    return wgpu::PresentMode::Immediate;
  if (vsync_enabled()) {
    return wgpu::PresentMode::Fifo;
  }
  // Dawn only disables CAMetalLayer displaySyncEnabled for Immediate on Metal
  if (g_backendType != wgpu::BackendType::Metal && supports(wgpu::PresentMode::Mailbox)) {
    return wgpu::PresentMode::Mailbox;
  }
  if (supports(wgpu::PresentMode::Immediate)) {
    return wgpu::PresentMode::Immediate;
  }
  return wgpu::PresentMode::Fifo;
}

TextureWithSampler create_render_texture(uint32_t width, uint32_t height, bool multisampled) {
  const wgpu::Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  const auto format = g_graphicsConfig.surfaceConfiguration.format;
  uint32_t sampleCount = 1;
  if (multisampled) {
    sampleCount = g_graphicsConfig.msaaSamples;
  }
  if (width == 0 || height == 0) {
    Log.fatal("Invalid render texture size! {}x{}, multisampled {}, format {}", width, height,
              static_cast<uint32_t>(format), multisampled);
  }
  const wgpu::TextureDescriptor textureDescriptor{
      .label = "Render texture",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc |
               wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = format,
      .mipLevelCount = 1,
      .sampleCount = sampleCount,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);

  constexpr wgpu::TextureViewDescriptor viewDescriptor{
      .label = "Render texture view",
      .dimension = wgpu::TextureViewDimension::e2D,
  };
  auto view = texture.CreateView(&viewDescriptor);

  constexpr wgpu::SamplerDescriptor samplerDescriptor{
      .label = "Render sampler",
      .addressModeU = wgpu::AddressMode::ClampToEdge,
      .addressModeV = wgpu::AddressMode::ClampToEdge,
      .addressModeW = wgpu::AddressMode::ClampToEdge,
      .magFilter = wgpu::FilterMode::Linear,
      .minFilter = wgpu::FilterMode::Linear,
      .mipmapFilter = wgpu::MipmapFilterMode::Linear,
      .lodMinClamp = 0.f,
      .lodMaxClamp = 1000.f,
      .maxAnisotropy = 1,
  };
  auto sampler = g_device.CreateSampler(&samplerDescriptor);

  return {
      .texture = std::move(texture),
      .view = std::move(view),
      .size = size,
      .format = format,
      .sampler = std::move(sampler),
  };
}

const TextureWithSampler& present_source() noexcept {
  return g_graphicsConfig.msaaSamples > 1 ? g_frameBufferResolved : g_frameBuffer;
}

void set_resampler(AuroraSampler sampler) noexcept {
  switch (sampler) {
  case SAMPLER_AREA:
  case SAMPLER_INTEGER:
  case SAMPLER_BILINEAR:
    g_Resampler = sampler;
    return;
  default:
    g_Resampler = SAMPLER_BILINEAR;
    return;
  }
}

AuroraSampler get_resampler() noexcept { return g_Resampler; }

Viewport calculate_integer_present_viewport(uint32_t surfaceWidth, uint32_t surfaceHeight,
                                            uint32_t sourceWidth, uint32_t sourceHeight,
                                            const Viewport& fitted) noexcept {
  if (sourceWidth == 0 || sourceHeight == 0) {
    return fitted;
  }
  const uint32_t scale = std::min(surfaceWidth / sourceWidth, surfaceHeight / sourceHeight);
  if (scale == 0) {
    return fitted;
  }
  const uint32_t width = sourceWidth * scale;
  const uint32_t height = sourceHeight * scale;
  // The XFB can be anamorphic. Only offer exact replication when it preserves the intended aspect.
  if (std::abs(static_cast<double>(width) * fitted.height - static_cast<double>(height) * fitted.width) >
      std::max(fitted.width, fitted.height)) {
    return fitted;
  }
  return {static_cast<float>((surfaceWidth - width) / 2), static_cast<float>((surfaceHeight - height) / 2),
          static_cast<float>(width), static_cast<float>(height), 0.f, 1.f};
}

Viewport calculate_present_viewport(uint32_t surface_width, uint32_t surface_height, uint32_t content_width,
                                    uint32_t content_height) noexcept {
  if (content_width == 0 || content_height == 0) {
    return {};
  }
  return calculate_present_viewport_for_aspect(surface_width, surface_height,
                                               static_cast<float>(content_width) / static_cast<float>(content_height));
}

Viewport calculate_present_viewport_for_aspect(uint32_t surface_width, uint32_t surface_height,
                                               float content_aspect) noexcept {
  if (surface_width == 0 || surface_height == 0 || !(content_aspect > 0.f)) {
    return {};
  }

  uint32_t viewport_width = surface_width;
  uint32_t viewport_height = std::min<uint32_t>(
      surface_height, std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(static_cast<double>(viewport_width) /
                                                                               static_cast<double>(content_aspect)))));
  if (viewport_height == surface_height) {
    viewport_width = std::min<uint32_t>(
        surface_width, std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(static_cast<double>(viewport_height) *
                                                                                static_cast<double>(content_aspect)))));
  }

  return {
      .left = static_cast<float>((surface_width - viewport_width) / 2),
      .top = static_cast<float>((surface_height - viewport_height) / 2),
      .width = static_cast<float>(viewport_width),
      .height = static_cast<float>(viewport_height),
      .znear = 0.f,
      .zfar = 1.f,
  };
}

static TextureWithSampler create_depth_texture(uint32_t width, uint32_t height) {
  const wgpu::Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  const auto format = g_graphicsConfig.depthFormat;
  const wgpu::TextureDescriptor textureDescriptor{
      .label = "Depth texture",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = format,
      .mipLevelCount = 1,
      .sampleCount = g_graphicsConfig.msaaSamples,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);

  const wgpu::TextureViewDescriptor viewDescriptor{
      .label = "Depth texture view",
      .dimension = wgpu::TextureViewDimension::e2D,
  };
  auto view = texture.CreateView(&viewDescriptor);

  const wgpu::SamplerDescriptor samplerDescriptor{
      .label = "Depth sampler",
      .addressModeU = wgpu::AddressMode::ClampToEdge,
      .addressModeV = wgpu::AddressMode::ClampToEdge,
      .addressModeW = wgpu::AddressMode::ClampToEdge,
      .magFilter = wgpu::FilterMode::Linear,
      .minFilter = wgpu::FilterMode::Linear,
      .mipmapFilter = wgpu::MipmapFilterMode::Linear,
      .lodMinClamp = 0.f,
      .lodMaxClamp = 1000.f,
      .maxAnisotropy = 1,
  };
  auto sampler = g_device.CreateSampler(&samplerDescriptor);

  return {
      .texture = std::move(texture),
      .view = std::move(view),
      .size = size,
      .format = format,
      .sampler = std::move(sampler),
  };
}

static TextureWithSampler create_normal_texture(uint32_t width, uint32_t height) {
  const wgpu::Extent3D size{width, height, 1};
  const wgpu::TextureDescriptor desc{
      .label = "Scene normals",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc,
      .size = size,
      .format = NormalBufferFormat,
  };
  auto texture = g_device.CreateTexture(&desc);
  auto view = texture.CreateView();
  return {.texture = std::move(texture), .view = std::move(view), .size = size, .format = NormalBufferFormat};
}

bool enable_normal_buffer() {
  if (g_graphicsConfig.normalBuffer) {
    return true;
  }
  if (!g_hasCoreFeatures || g_graphicsConfig.msaaSamples != 1 || !g_device || g_frameBuffer.size.width == 0 ||
      g_frameBuffer.size.height == 0) {
    return false;
  }
  g_normalBuffer = create_normal_texture(g_frameBuffer.size.width, g_frameBuffer.size.height);
  g_graphicsConfig.normalBuffer = true;
  return true;
}

void create_copy_pipeline() {
  wgpu::ShaderSourceWGSL sourceDescriptor{};
  sourceDescriptor.code = R"""(
@group(0) @binding(0)
var efb_sampler: sampler;
@group(0) @binding(1)
var efb_texture: texture_2d<f32>;

struct VertexOutput {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

var<private> pos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(-1.0, 1.0),
    vec2(-1.0, -3.0),
    vec2(3.0, 1.0),
);
var<private> uvs: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(0.0, 0.0),
    vec2(0.0, 2.0),
    vec2(2.0, 0.0),
);

@vertex
fn vs_main(@builtin(vertex_index) vtxIdx: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4<f32>(pos[vtxIdx], 0.0, 1.0);
    out.uv = uvs[vtxIdx];
    return out;
}

@fragment
fn fs_opaque(in: VertexOutput) -> @location(0) vec4<f32> {
    let color = textureSample(efb_texture, efb_sampler, in.uv);
    return vec4(color.rgb, 1.0);
}

@fragment
fn fs_premultiplied_alpha(in: VertexOutput) -> @location(0) vec4<f32> {
    return textureSample(efb_texture, efb_sampler, in.uv);
}
)""";
  const wgpu::ShaderModuleDescriptor moduleDescriptor{
      .nextInChain = &sourceDescriptor,
      .label = "XFB Copy Module",
  };
  auto module = g_device.CreateShaderModule(&moduleDescriptor);
  const std::array bindGroupLayoutEntries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Fragment,
          .sampler =
              wgpu::SamplerBindingLayout{
                  .type = wgpu::SamplerBindingType::Filtering,
              },
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture =
              wgpu::TextureBindingLayout{
                  .sampleType = wgpu::TextureSampleType::Float,
                  .viewDimension = wgpu::TextureViewDimension::e2D,
              },
      },
  };
  const wgpu::BindGroupLayoutDescriptor bindGroupLayoutDescriptor{
      .entryCount = bindGroupLayoutEntries.size(),
      .entries = bindGroupLayoutEntries.data(),
  };
  g_CopyBindGroupLayout = g_device.CreateBindGroupLayout(&bindGroupLayoutDescriptor);
  const wgpu::PipelineLayoutDescriptor layoutDescriptor{
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &g_CopyBindGroupLayout,
  };
  auto pipelineLayout = g_device.CreatePipelineLayout(&layoutDescriptor);

  const auto make_copy_pipeline = [&](const char* label, const char* fragmentEntryPoint,
                                      const wgpu::BlendState* blend, unsigned samples = 1) {
    const std::array colorTargets{wgpu::ColorTargetState{
        .format = g_graphicsConfig.surfaceConfiguration.format,
        .blend = blend,
        .writeMask = wgpu::ColorWriteMask::All,
    }};
    const wgpu::FragmentState fragmentState{
        .module = module,
        .entryPoint = fragmentEntryPoint,
        .targetCount = colorTargets.size(),
        .targets = colorTargets.data(),
    };
    const wgpu::RenderPipelineDescriptor pipelineDescriptor{
        .label = label,
        .layout = pipelineLayout,
        .vertex =
            wgpu::VertexState{
                .module = module,
                .entryPoint = "vs_main",
            },
        .primitive =
            wgpu::PrimitiveState{
                .topology = wgpu::PrimitiveTopology::TriangleList,
            },
        .multisample =
            wgpu::MultisampleState{
                .count = samples,
                .mask = UINT32_MAX,
            },
        .fragment = &fragmentState,
    };
    return g_device.CreateRenderPipeline(&pipelineDescriptor);
  };
  g_CopyPipeline = make_copy_pipeline("XFB Copy Pipeline", "fs_opaque", nullptr);
  g_GiRestorePipeline = make_copy_pipeline("GI restore", "fs_premultiplied_alpha", nullptr, g_graphicsConfig.msaaSamples);
  const wgpu::BlendState dimmingBlend{
	.color = {
		.operation = wgpu::BlendOperation::Add,
		.srcFactor = wgpu::BlendFactor::Constant,
		.dstFactor = wgpu::BlendFactor::Zero,
	},
	.alpha = {
		.operation = wgpu::BlendOperation::Add,
		.srcFactor = wgpu::BlendFactor::One,
		.dstFactor = wgpu::BlendFactor::Zero,
	},
  };
  g_CopyDimmedPipeline = make_copy_pipeline("XFB Dimmed Copy Pipeline", "fs_opaque", &dimmingBlend);

  const wgpu::BlendState premultipliedAlphaBlend{
      .color =
          {
              .operation = wgpu::BlendOperation::Add,
              .srcFactor = wgpu::BlendFactor::One,
              .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha,
          },
      .alpha =
          {
              .operation = wgpu::BlendOperation::Add,
              .srcFactor = wgpu::BlendFactor::One,
              .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha,
          },
  };
  g_CopyPremultipliedAlphaPipeline =
      make_copy_pipeline("XFB Premultiplied Alpha Copy Pipeline", "fs_premultiplied_alpha", &premultipliedAlphaBlend);
}

void create_resample_pipeline() {
  wgpu::ShaderSourceWGSL sourceDescriptor{};
  sourceDescriptor.code = resampleShaderSource;
  const wgpu::ShaderModuleDescriptor moduleDescriptor{
      .nextInChain = &sourceDescriptor,
      .label = "Present Resample Module",
  };
  auto module = g_device.CreateShaderModule(&moduleDescriptor);
  const std::array colorTargets{wgpu::ColorTargetState{
      .format = g_graphicsConfig.surfaceConfiguration.format,
      .writeMask = wgpu::ColorWriteMask::All,
  }};
  const wgpu::FragmentState fragmentState{
      .module = module,
      .entryPoint = "fs_main",
      .targetCount = colorTargets.size(),
      .targets = colorTargets.data(),
  };
  const std::array bindGroupLayoutEntries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Fragment,
          .buffer =
              wgpu::BufferBindingLayout{
                  .type = wgpu::BufferBindingType::Uniform,
              },
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .sampler =
              wgpu::SamplerBindingLayout{
                  .type = wgpu::SamplerBindingType::Filtering,
              },
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 2,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture =
              wgpu::TextureBindingLayout{
                  .sampleType = wgpu::TextureSampleType::Float,
                  .viewDimension = wgpu::TextureViewDimension::e2D,
              },
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 3,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = wgpu::TextureBindingLayout{
              .sampleType = wgpu::TextureSampleType::UnfilterableFloat,
              .viewDimension = wgpu::TextureViewDimension::e2D,
          },
      },
  };
  auto resampleEntries = std::vector<wgpu::BindGroupLayoutEntry>(bindGroupLayoutEntries.begin(), bindGroupLayoutEntries.end());
  auto indirect = resampleEntries.back();
  indirect.binding = 4;
  indirect.texture.sampleType = wgpu::TextureSampleType::Float;
  resampleEntries.push_back(indirect);
  for (unsigned binding : {5u, 6u})
    resampleEntries.push_back({.binding=binding, .visibility=wgpu::ShaderStage::Fragment,
                              .buffer={.type=wgpu::BufferBindingType::ReadOnlyStorage}});
  const wgpu::BindGroupLayoutDescriptor bindGroupLayoutDescriptor{
      .entryCount = resampleEntries.size(), .entries = resampleEntries.data(),
  };
  g_ResampleBindGroupLayout = g_device.CreateBindGroupLayout(&bindGroupLayoutDescriptor);
  const wgpu::PipelineLayoutDescriptor layoutDescriptor{
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &g_ResampleBindGroupLayout,
  };
  auto pipelineLayout = g_device.CreatePipelineLayout(&layoutDescriptor);
  const wgpu::RenderPipelineDescriptor pipelineDescriptor{
      .label = "Present Resample Pipeline",
      .layout = pipelineLayout,
      .vertex =
          wgpu::VertexState{
              .module = module,
              .entryPoint = "vs_main",
          },
      .primitive =
          wgpu::PrimitiveState{
              .topology = wgpu::PrimitiveTopology::TriangleList,
          },
      .multisample =
          wgpu::MultisampleState{
              .count = 1,
              .mask = UINT32_MAX,
          },
      .fragment = &fragmentState,
  };
  g_ResamplePipeline = g_device.CreateRenderPipeline(&pipelineDescriptor);
  auto bounceFragment = fragmentState;
  bounceFragment.entryPoint = "fs_bounce";
  auto bounceDescriptor = pipelineDescriptor;
  bounceDescriptor.label = "Screen-space diffuse bounce";
  bounceDescriptor.fragment = &bounceFragment;
  g_BouncePipeline = g_device.CreateRenderPipeline(&bounceDescriptor);

  const wgpu::BufferDescriptor uniformBufferDescriptor{
      .label = "Present Resample Uniform Buffer",
      .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::Uniform,
      .size = AURORA_ALIGN(sizeof(ResampleUniformBlock), 16),
  };
  g_ResampleUniformBuffer = g_device.CreateBuffer(&uniformBufferDescriptor);
  g_BloomUniformBuffer = g_device.CreateBuffer(&uniformBufferDescriptor);
  g_OcclusionUniformBuffer = g_device.CreateBuffer(&uniformBufferDescriptor);
  for (auto& buffer : g_giUniforms) buffer = g_device.CreateBuffer(&uniformBufferDescriptor);
  for (auto& buffer : g_giReplayUniforms) buffer = g_device.CreateBuffer(&uniformBufferDescriptor);
  g_OcclusionReplayUniformBuffer = g_device.CreateBuffer(&uniformBufferDescriptor);
  const auto storage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::Storage;
  const wgpu::BufferDescriptor nodeBuffer{.usage=storage, .size=sizeof(rtgi::Node)*rtgi::TriangleLimit};
  g_giNodes = g_device.CreateBuffer(&nodeBuffer);
  const wgpu::BufferDescriptor triangleBuffer{.usage=storage, .size=sizeof(rtgi::Triangle)*rtgi::TriangleLimit};
  g_giTriangles = g_device.CreateBuffer(&triangleBuffer);
}

wgpu::BindGroup create_copy_bind_group(const TextureWithSampler& source) {
  const std::array bindGroupEntries{
      wgpu::BindGroupEntry{
          .binding = 0,
          .sampler = source.sampler,
      },
      wgpu::BindGroupEntry{
          .binding = 1,
          .textureView = source.view,
      },
  };
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .layout = g_CopyBindGroupLayout,
      .entryCount = bindGroupEntries.size(),
      .entries = bindGroupEntries.data(),
  };
  return g_device.CreateBindGroup(&bindGroupDescriptor);
}

bool gi_irradiance_ready(bool rayTraced) noexcept {
  return rayTraced ? static_cast<bool>(g_giTargets[5].view) : static_cast<bool>(g_giTargets[0].view);
}

const TextureWithSampler& resample_present_source(const wgpu::CommandEncoder& encoder, const Viewport& viewport,
                                                  const TextureWithSampler& source, AuroraSampler sampler, bool bloom, const wgpu::TextureView& depth, BouncePass bounce, std::array<float, 4> projection,
                                                  std::span<const rtgi::Triangle> triangles, std::span<const rtgi::Node> nodes,
                                                  std::array<float, 2> screenCenter, bool independentGeometry,
                                                  bool replay) {
  const auto giPass = static_cast<unsigned>(bounce);
  AURORA_ASSERT(!replay || (bounce != BouncePass::RayGather && bounce != BouncePass::RayFilter &&
                            bounce != BouncePass::Gather && bounce != BouncePass::Radiance),
                "Replays composite existing irradiance and must not rewrite GI inputs");
  auto& target = giPass ? g_giTargets[giPass-1] : depth ? g_occlusionFrameBuffer : bloom ? g_bloomFrameBuffer : g_resampledFrameBuffer;
  auto& buffer = giPass ? (replay ? g_giReplayUniforms : g_giUniforms)[giPass-1]
               : depth ? (replay ? g_OcclusionReplayUniformBuffer : g_OcclusionUniformBuffer)
               : bloom ? g_BloomUniformBuffer : g_ResampleUniformBuffer;
  const uint32_t width = viewport_extent(viewport.width);
  const uint32_t height = viewport_extent(viewport.height);
  if (!target.view || target.size.width != width ||
      target.size.height != height || target.format != g_graphicsConfig.surfaceConfiguration.format) {
    target = create_render_texture(width, height, false);
  }

  const bool integer = sampler == SAMPLER_INTEGER &&
      gfx::pixel_perfect::integer_scale(source.size.width, width) &&
      gfx::pixel_perfect::integer_scale(source.size.height, height) &&
      width / source.size.width == height / source.size.height;
  const ResampleUniformBlock uniform{
      .samplerMode = giPass ? 4u+giPass : depth ? 4u : bloom ? 3u : integer ? 2u : sampler_mode(sampler),
      .frameWidth = static_cast<float>(width),
      .frameHeight = static_cast<float>(height),
      .bounceGain = (bounce == BouncePass::Composite || bounce == BouncePass::RayComposite) && aurora_strong_bounce_enabled() ? 3.2f : 1.25f,
      .projection = projection,
      .screenCenter = screenCenter,
  };
  AURORA_ASSERT(gfx::render_worker::is_worker_thread(), "Present resample queue write must run on the render worker");
  g_queue.WriteBuffer(buffer, 0, &uniform, sizeof(uniform));
  auto nodeBuffer = g_giNodes;
  auto triangleBuffer = g_giTriangles;
  if (bounce == BouncePass::RayGather) {
    if (independentGeometry) {
      const auto usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::Storage;
      const wgpu::BufferDescriptor nodeDesc{.usage=usage, .size=nodes.size_bytes()};
      const wgpu::BufferDescriptor triangleDesc{.usage=usage, .size=triangles.size_bytes()};
      nodeBuffer = g_device.CreateBuffer(&nodeDesc);
      triangleBuffer = g_device.CreateBuffer(&triangleDesc);
    }
    g_queue.WriteBuffer(nodeBuffer, 0, nodes.data(), nodes.size_bytes());
    g_queue.WriteBuffer(triangleBuffer, 0, triangles.data(), triangles.size_bytes());
  }

  const std::array bindGroupEntries{
      wgpu::BindGroupEntry{
          .binding = 0,
          .buffer = buffer,
          .size = AURORA_ALIGN(sizeof(ResampleUniformBlock), 16),
      },
      wgpu::BindGroupEntry{
          .binding = 1,
          .sampler = source.sampler,
      },
      wgpu::BindGroupEntry{
          .binding = 2,
          .textureView = source.view,
      },
      wgpu::BindGroupEntry{.binding = 3, .textureView = depth ? depth : source.view},
      wgpu::BindGroupEntry{.binding = 4, .textureView = bounce == BouncePass::RayComposite ? g_giTargets[5].view :
                         bounce == BouncePass::RayFilter ? g_giTargets[3].view :
                         giPass == 2 ? g_giTargets[0].view : giPass == 1 ? g_giTargets[2].view : source.view},
      wgpu::BindGroupEntry{.binding=5, .buffer=nodeBuffer},
      wgpu::BindGroupEntry{.binding=6, .buffer=triangleBuffer},
  };
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .layout = g_ResampleBindGroupLayout,
      .entryCount = bindGroupEntries.size(),
      .entries = bindGroupEntries.data(),
  };
  const auto bindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);

  const std::array attachments{
      wgpu::RenderPassColorAttachment{
          .view = target.view,
          .loadOp = wgpu::LoadOp::Clear,
          .storeOp = wgpu::StoreOp::Store,
      },
  };
  const wgpu::RenderPassDescriptor renderPassDescriptor{
      .label = "Present resample render pass",
      .colorAttachmentCount = attachments.size(),
      .colorAttachments = attachments.data(),
      .timestampWrites = gpu_prof::pass_writes(bounce == BouncePass::RayFilter ? "RTGI filter" : bounce == BouncePass::RayGather ? "RTGI trace" : bounce == BouncePass::RayComposite ? "RTGI composite" : giPass == 3 ? "GI radiance" : giPass == 1 ? "GI gather" : giPass == 2 ? "GI composite" : depth ? "Ambient occlusion" : bloom ? "Enhanced bloom" : "Present resample"),
  };
  const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
  pass.SetPipeline(giPass ? g_BouncePipeline : g_ResamplePipeline);
  pass.SetBindGroup(0, bindGroup, 0, nullptr);
  pass.SetViewport(0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f);
  pass.Draw(3);
  pass.End();

  return target;
}

static wgpu::BackendType to_wgpu_backend(AuroraBackend backend) {
  switch (backend) {
  case BACKEND_WEBGPU:
    return wgpu::BackendType::WebGPU;
  case BACKEND_D3D11:
    return wgpu::BackendType::D3D11;
  case BACKEND_D3D12:
    return wgpu::BackendType::D3D12;
  case BACKEND_METAL:
    return wgpu::BackendType::Metal;
  case BACKEND_VULKAN:
    return wgpu::BackendType::Vulkan;
  case BACKEND_OPENGL:
    return wgpu::BackendType::OpenGL;
  case BACKEND_OPENGLES:
    return wgpu::BackendType::OpenGLES;
  default:
    return wgpu::BackendType::Null;
  }
}

static uint32_t g_maxTextureDimension2D = 0;

static bool release_surface_locked() noexcept {
  if (!g_surface) {
    return false;
  }
  g_surface.Unconfigure();
  g_surface = {};
  return true;
}

// A retired swapchain is freed only after a later submission completes and the device ticks.
static void wait_surface_retired() noexcept {
  if (!g_instance || !g_device || !g_queue) {
    return;
  }
  const auto commands = g_device.CreateCommandEncoder().Finish();
  g_queue.Submit(1, &commands);
  const auto future =
      g_queue.OnSubmittedWorkDone(wgpu::CallbackMode::WaitAnyOnly, [](wgpu::QueueWorkDoneStatus, wgpu::StringView) {});
  if (const auto status = g_instance.WaitAny(future, 1000000000); status != wgpu::WaitStatus::Success) {
    Log.warn("Timed out waiting for queue idle on surface release: {}", magic_enum::enum_name(status));
  }
  g_device.Tick();
}

static bool create_surface() {
  SDL_Window* window = window::get_sdl_window();
  if (window == nullptr) {
    Log.error("Failed to create surface: no window");
    return false;
  }
  bool released = false;
  {
    window::SurfaceLock surfaceLock;
    released = release_surface_locked();
  }
  if (released) {
    wait_surface_retired();
  }
  window::SurfaceLock surfaceLock;
  g_surface = create_window_surface(g_instance, window, "Surface");
  if (!g_surface) {
    Log.error("Failed to create surface");
    return false;
  }
  return true;
}

bool initialize(AuroraBackend auroraBackend, bool allowCpu) {
  if (!g_instance) {
    Log.info("Creating WebGPU instance");
    const std::array requiredInstanceFeatures{
        wgpu::InstanceFeatureName::TimedWaitAny,
    };
    wgpu::InstanceDescriptor instanceDescriptor{
        .requiredFeatureCount = requiredInstanceFeatures.size(),
        .requiredFeatures = requiredInstanceFeatures.data(),
    };
#ifdef WEBGPU_DAWN
    constexpr std::array instanceToggles{
        "allow_unsafe_apis",
    };
    wgpu::DawnTogglesDescriptor instanceTogglesDescriptor{wgpu::DawnTogglesDescriptor::Init{
        .enabledToggleCount = instanceToggles.size(),
        .enabledToggles = instanceToggles.data(),
    }};
#ifdef __MINGW32__
    // DawnNative.h's C++ descriptor has an MSVC ABI that llvm-mingw cannot link against.
    instanceDescriptor.nextInChain = &instanceTogglesDescriptor;
#else
    dawn::native::DawnInstanceDescriptor dawnInstanceDescriptor{};
    dawnInstanceDescriptor.nextInChain = &instanceTogglesDescriptor;
#ifdef AURORA_GFX_DEBUG_GROUPS
    dawnInstanceDescriptor.backendValidationLevel = auroraBackend == BACKEND_VULKAN
                                                        ? dawn::native::BackendValidationLevel::Partial
                                                        : dawn::native::BackendValidationLevel::Disabled;
#else
    dawnInstanceDescriptor.backendValidationLevel = dawn::native::BackendValidationLevel::Disabled;
#endif
    dawnInstanceDescriptor.SetLoggingCallback(wgpu_log);
#ifdef TRACY_ENABLE
    dawnInstanceDescriptor.platform = tracy_dawn_platform();
#endif
    instanceDescriptor.nextInChain = &dawnInstanceDescriptor;
#endif
#endif
    g_instance = wgpu::CreateInstance(&instanceDescriptor);
    if (!g_instance) {
      Log.error("Failed to create WebGPU instance");
      return false;
    }
  }
  const wgpu::BackendType backend = to_wgpu_backend(auroraBackend);
  Log.info("Attempting to initialize {}", magic_enum::enum_name(backend));
#if 0
  // D3D12's debug layer is very slow
  g_dawnInstance->EnableBackendValidation(backend != WGPUBackendType::D3D12);
#endif

  if (!create_surface()) {
    return false;
  }
  {
    const wgpu::RequestAdapterOptions options{
        .featureLevel = wgpu::FeatureLevel::Compatibility,
        .powerPreference = wgpu::PowerPreference::HighPerformance,
        .backendType = backend,
        .compatibleSurface = g_surface,
    };
    Log.info("Requesting adapter\n  Feature level: {}\n  Power preference: {}\n  Backend: {}\n  Compatible surface: {}",
             magic_enum::enum_name(options.featureLevel), magic_enum::enum_name(options.powerPreference),
             magic_enum::enum_name(options.backendType), static_cast<bool>(options.compatibleSurface));
    bool requestAdapterCallbackCompleted = false;
    wgpu::RequestAdapterStatus requestAdapterStatus = wgpu::RequestAdapterStatus::CallbackCancelled;
    std::string requestAdapterMessage;
    const auto future = g_instance.RequestAdapter(
        &options, wgpu::CallbackMode::WaitAnyOnly,
        [&](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView message) {
          requestAdapterCallbackCompleted = true;
          requestAdapterStatus = status;
          requestAdapterMessage = std::string{std::string_view{message}};
          if (status == wgpu::RequestAdapterStatus::Success) {
            g_adapter = std::move(adapter);
          } else {
            Log.warn("Adapter request failed: {}: {}", magic_enum::enum_name(status), message);
          }
        });
    const auto status = g_instance.WaitAny(future, 5000000000);
    if (status != wgpu::WaitStatus::Success) {
      if (requestAdapterCallbackCompleted) {
        Log.error("Failed to create adapter: wait status {}, request status {}, message: {}",
                  magic_enum::enum_name(status), magic_enum::enum_name(requestAdapterStatus), requestAdapterMessage);
      } else {
        Log.error("Failed to create adapter: wait status {}, request callback did not complete",
                  magic_enum::enum_name(status));
      }
      return false;
    }
    if (!g_adapter) {
      if (requestAdapterCallbackCompleted) {
        Log.error("Failed to create adapter: request status {}, message: {}",
                  magic_enum::enum_name(requestAdapterStatus), requestAdapterMessage);
      } else {
        Log.error("Failed to create adapter: request callback did not complete");
      }
      return false;
    }
  }
  g_adapter.GetInfo(&g_adapterInfo);
  auto adapterName = g_adapterInfo.device;
  if (adapterName.IsUndefined()) {
    adapterName = wgpu::StringView("Unknown");
  }
  if (!allowCpu && g_adapterInfo.adapterType == wgpu::AdapterType::CPU && backend != wgpu::BackendType::Null) {
    Log.warn("Ignoring CPU adapter: {}", adapterName);
    g_adapterInfo = {};
    g_adapter = {};
    return false;
  }
  auto description = g_adapterInfo.description;
  if (description.IsUndefined()) {
    description = wgpu::StringView("Unknown");
  }
  g_backendType = g_adapterInfo.backendType;
  const auto backendName = magic_enum::enum_name(g_backendType);
  Log.info("Graphics adapter information\n  API: {}\n  Device: {} ({})\n  Driver: {}", backendName, adapterName,
           magic_enum::enum_name(g_adapterInfo.adapterType), description);

  {
    wgpu::Limits supportedLimits{};
    g_adapter.GetLimits(&supportedLimits);
    wgpu::CompatibilityModeLimits compatibilityModeLimits{wgpu::CompatibilityModeLimits::Init{
        .maxStorageBuffersInVertexStage = 2,
        .maxStorageBuffersInFragmentStage = 2,
    }};
    g_maxTextureDimension2D = supportedLimits.maxTextureDimension2D;
    const wgpu::Limits requiredLimits{
        .nextInChain = &compatibilityModeLimits,
        // Use "best" supported limits
        .maxTextureDimension1D = supportedLimits.maxTextureDimension1D == 0 ? WGPU_LIMIT_U32_UNDEFINED
                                                                            : supportedLimits.maxTextureDimension1D,
        .maxTextureDimension2D = supportedLimits.maxTextureDimension2D == 0 ? WGPU_LIMIT_U32_UNDEFINED
                                                                            : supportedLimits.maxTextureDimension2D,
        .maxTextureDimension3D = supportedLimits.maxTextureDimension3D == 0 ? WGPU_LIMIT_U32_UNDEFINED
                                                                            : supportedLimits.maxTextureDimension3D,
        .maxTextureArrayLayers = supportedLimits.maxTextureArrayLayers == 0 ? WGPU_LIMIT_U32_UNDEFINED
                                                                            : supportedLimits.maxTextureArrayLayers,
        .maxStorageBuffersPerShaderStage = 2,
        .minUniformBufferOffsetAlignment =
            supportedLimits.minUniformBufferOffsetAlignment < 64 ? 64 : supportedLimits.minUniformBufferOffsetAlignment,
        .minStorageBufferOffsetAlignment =
            supportedLimits.minStorageBufferOffsetAlignment < 16 ? 16 : supportedLimits.minStorageBufferOffsetAlignment,
        .maxImmediateSize = sizeof(gx::DrawImmediateData),
    };
    Log.info(
        "Using limits:"
        "\n  maxTextureDimension1D: {}"
        "\n  maxTextureDimension2D: {}"
        "\n  maxTextureDimension3D: {}"
        "\n  maxTextureArrayLayers: {}"
        "\n  maxStorageBuffersPerShaderStage: {}"
        "\n  minUniformBufferOffsetAlignment: {}"
        "\n  minStorageBufferOffsetAlignment: {}"
        "\n  maxImmediateSize: {}",
        requiredLimits.maxTextureDimension1D, requiredLimits.maxTextureDimension2D,
        requiredLimits.maxTextureDimension3D, requiredLimits.maxTextureArrayLayers,
        requiredLimits.maxStorageBuffersPerShaderStage, requiredLimits.minUniformBufferOffsetAlignment,
        requiredLimits.minStorageBufferOffsetAlignment, requiredLimits.maxImmediateSize);
    std::vector<wgpu::FeatureName> requiredFeatures;
    // Optional native sharing for MetalFX. Devices without either feature keep
    // the normal renderer; the upscaler checks the enabled pair at runtime.
    if (backend == wgpu::BackendType::Metal &&
        g_adapter.HasFeature(wgpu::FeatureName::SharedTextureMemoryIOSurface) &&
        g_adapter.HasFeature(wgpu::FeatureName::SharedFenceMTLSharedEvent)) {
      requiredFeatures.push_back(wgpu::FeatureName::SharedTextureMemoryIOSurface);
      requiredFeatures.push_back(wgpu::FeatureName::SharedFenceMTLSharedEvent);
    }

    // Optional D3D12 resource sharing for NVIDIA DLSS (docs/DLSS-NOTES.md): DLSS runs on a second D3D12 device and its
    // textures/fences are shared with Dawn. Devices without the pair keep the normal renderer.
    if (backend == wgpu::BackendType::D3D12 && g_adapter.HasFeature(wgpu::FeatureName::SharedTextureMemoryDXGISharedHandle) &&
        g_adapter.HasFeature(wgpu::FeatureName::SharedFenceDXGISharedHandle)) {
      requiredFeatures.push_back(wgpu::FeatureName::SharedTextureMemoryDXGISharedHandle);
      requiredFeatures.push_back(wgpu::FeatureName::SharedFenceDXGISharedHandle);
    }

    g_hasCoreFeatures = false;
    g_bcTexturesSupported = false;
    g_astcTexturesSupported = false;
    g_textureComponentSwizzleSupported = false;
    g_dualSourceBlendingSupported = false;
    wgpu::SupportedFeatures supportedFeatures;
    g_adapter.GetFeatures(&supportedFeatures);
    for (size_t i = 0; i < supportedFeatures.featureCount; ++i) {
      const auto feature = supportedFeatures.features[i];
      if (feature == wgpu::FeatureName::CoreFeaturesAndLimits || feature == wgpu::FeatureName::TextureCompressionBC ||
          feature == wgpu::FeatureName::TextureCompressionASTC ||
          feature == wgpu::FeatureName::TextureComponentSwizzle || feature == wgpu::FeatureName::DualSourceBlending) {
        if (feature == wgpu::FeatureName::CoreFeaturesAndLimits) {
          g_hasCoreFeatures = true;
        } else if (feature == wgpu::FeatureName::TextureCompressionBC) {
          g_bcTexturesSupported = true;
        } else if (feature == wgpu::FeatureName::TextureCompressionASTC) {
          g_astcTexturesSupported = true;
        } else if (feature == wgpu::FeatureName::TextureComponentSwizzle) {
          g_textureComponentSwizzleSupported = true;
        } else if (feature == wgpu::FeatureName::DualSourceBlending) {
          g_dualSourceBlendingSupported = true;
        }
        requiredFeatures.push_back(feature);
      }
      if (feature == wgpu::FeatureName::TimestampQuery) {
#ifndef TRACY_ENABLE
        // SIN_GPU_PROF=1 logs per-pass GPU times (gpu_prof.cpp) without Tracy.
        if (const char* prof = std::getenv("SIN_GPU_PROF"); prof != nullptr && *prof != '\0' && *prof != '0')
#endif
        {
          requiredFeatures.push_back(feature);
        }
      }
    }
    std::string featureList;
    for (auto featureName : requiredFeatures) {
      featureList += "\n  ";
      featureList += magic_enum::enum_name(featureName);
    }
    Log.info("Enabling features: {}", featureList);
#ifdef WEBGPU_DAWN
    wgpu::DawnCacheDeviceDescriptor cacheDescriptor({.nextInChain = nullptr});
    cacheDescriptor.SetDawnLoadCacheDataCallback(
        [](std::span<const std::byte> key, std::span<std::byte> value) noexcept -> size_t {
          return load_from_cache(key.data(), key.size(), value.data(), value.size(), nullptr);
        });
    cacheDescriptor.SetDawnStoreCacheDataCallback(
        [](std::span<const std::byte> key, std::span<const std::byte> value) noexcept {
          store_to_cache(key.data(), key.size(), value.data(), value.size(), nullptr);
        });

    constexpr std::array enableToggles{
#if _WIN32
        "use_dxc",
#ifndef NDEBUG
        "emit_hlsl_debug_symbols",
#endif
#endif
#ifdef NDEBUG
        "skip_validation",
        "disable_robustness",
#endif
#ifndef ANDROID
        "use_user_defined_labels_in_backend",
#endif
        "disable_symbol_renaming",
        "enable_immediate_error_handling",
        "gl_allow_context_on_multi_threads",
    };
    constexpr std::array disableToggles{
        "timestamp_quantization",
        // Adreno strikes again!
        // https://github.com/TwilitRealm/dusklight/issues/2563
        "use_spirv_reconvergence_mode",
    };
    wgpu::DawnTogglesDescriptor togglesDescriptor(wgpu::DawnTogglesDescriptor::Init{
        .nextInChain = &cacheDescriptor,
        .enabledToggleCount = enableToggles.size(),
        .enabledToggles = enableToggles.data(),
        .disabledToggleCount = disableToggles.size(),
        .disabledToggles = disableToggles.data(),
    });
#endif
    wgpu::DeviceDescriptor deviceDescriptor({
#ifdef WEBGPU_DAWN
        .nextInChain = &togglesDescriptor,
#endif
        .requiredFeatureCount = requiredFeatures.size(),
        .requiredFeatures = requiredFeatures.data(),
        .requiredLimits = &requiredLimits,
    });
    deviceDescriptor.SetUncapturedErrorCallback(
        [](const wgpu::Device& device, wgpu::ErrorType type, wgpu::StringView message) {
          if (g_initialized) {
            FATAL("WebGPU error {}: {}", underlying(type), message);
          } else {
            Log.warn("WebGPU error {}: {}", underlying(type), message);
          }
        });
    deviceDescriptor.SetDeviceLostCallback(
        wgpu::CallbackMode::AllowSpontaneous,
        [](const wgpu::Device& device, wgpu::DeviceLostReason reason, wgpu::StringView message) {
          if (g_initialized) {
            FATAL("Device lost: {}", message);
          } else {
            Log.warn("Device lost: {}", message);
          }
        });
    const auto future =
        g_adapter.RequestDevice(&deviceDescriptor, wgpu::CallbackMode::WaitAnyOnly,
                                [](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView message) {
                                  if (status == wgpu::RequestDeviceStatus::Success) {
                                    g_device = std::move(device);
                                  } else {
                                    Log.warn("Device request failed: {}", message);
                                  }
                                });
    const auto status = g_instance.WaitAny(future, 5000000000);
    if (status != wgpu::WaitStatus::Success) {
      Log.error("Failed to create device: {}", magic_enum::enum_name(status));
      return false;
    }
    if (!g_device) {
      return false;
    }
    g_device.SetLoggingCallback(wgpu_log);
  }
  g_queue = g_device.GetQueue();
  dlss::start(g_device, g_adapterInfo, g_backendType,
              g_device.HasFeature(wgpu::FeatureName::SharedTextureMemoryDXGISharedHandle) &&
                  g_device.HasFeature(wgpu::FeatureName::SharedFenceDXGISharedHandle),
              {});

  const wgpu::Status status = g_surface.GetCapabilities(g_adapter, &g_surfaceCapabilities);
  if (status != wgpu::Status::Success) {
    Log.error("Failed to get surface capabilities: {}", magic_enum::enum_name(status));
    return false;
  }
  if (g_surfaceCapabilities.formatCount == 0) {
    Log.error("Surface has no formats");
    return false;
  }
  if (g_surfaceCapabilities.presentModeCount == 0) {
    Log.error("Surface has no present modes");
    return false;
  }
  auto surfaceFormat = best_surface_format();
  g_vsyncEnabled.store(g_config.vsync, std::memory_order_release);
  auto presentMode = select_present_mode(g_surfaceCapabilities);
  Log.info("Using surface format {}, present mode {}", magic_enum::enum_name(surfaceFormat),
           magic_enum::enum_name(presentMode));
  const auto size = window::get_window_size();
  g_graphicsConfig = GraphicsConfig{
      .surfaceConfiguration =
          wgpu::SurfaceConfiguration{
              .format = surfaceFormat,
              .usage = wgpu::TextureUsage::RenderAttachment | (g_surfaceCapabilities.usages & wgpu::TextureUsage::CopySrc),
              .width = size.native_fb_width,
              .height = size.native_fb_height,
              .presentMode = presentMode,
          },
      .depthFormat = wgpu::TextureFormat::Depth32Float,
      .msaaSamples = g_config.msaa,
      .textureAnisotropy = g_config.maxTextureAnisotropy,
  };
  create_copy_pipeline();
  create_resample_pipeline();
  gpu_prof::initialize();
  resize_swapchain(size.fb_width, size.fb_height, size.native_fb_width, size.native_fb_height, true);
  g_initialized = true;
  return true;
}

void shutdown() {
  g_initialized = false;
  gfx::gpu_synchronize();
  dlss::shutdown();
  gpu_prof::shutdown();
  g_CopyBindGroupLayout = {};
  g_CopyPipeline = {};
  g_GiRestorePipeline = {};
  g_CopyDimmedPipeline = {};
  g_CopyPremultipliedAlphaPipeline = {};
  g_CopyBindGroup = {};
  g_ResampleBindGroupLayout = {};
  g_ResamplePipeline = {};
  g_BouncePipeline = {};
  g_ResampleUniformBuffer = {};
  g_giTargets = {};
  g_giUniforms = {};
  g_giReplayUniforms = {};
  g_OcclusionReplayUniformBuffer = {};
  g_giNodes = {};
  g_giTriangles = {};
  g_occlusionFrameBuffer = {};
  g_OcclusionUniformBuffer = {};
  g_bloomFrameBuffer = {};
  g_BloomUniformBuffer = {};
  g_resampledFrameBuffer = {};
  g_frameBuffer = {};
  g_frameBufferResolved = {};
  g_depthBuffer = {};
  g_normalBuffer = {};
  g_graphicsConfig.normalBuffer = false;
  g_queue = {};
  g_surface = {};
  g_device = {};
  g_adapter = {};
  g_instance = {};
  cache_shutdown();
}

void release_surface() noexcept {
  const gfx::presentation::SurfaceAcquirePause pause;
  gfx::gpu_synchronize();
  bool released = false;
  {
    window::SurfaceLock surfaceLock;
    released = release_surface_locked();
  }
  if (released) {
    wait_surface_retired();
  }
}

static void resize_swapchain_internal(uint32_t width, uint32_t height, uint32_t nativeWidth, uint32_t nativeHeight,
                                      bool force) {
  if (!g_surface || !g_device || width == 0 || height == 0 || nativeHeight == 0 || nativeWidth == 0) {
    return;
  }
  const auto budgeted = render_size_limits::fit_framebuffer_to_budget(width, height, g_maxTextureDimension2D);
  if (budgeted.width != width || budgeted.height != height) {
    Log.warn("Render target {}x{} exceeds the framebuffer budget; clamping to {}x{}", width, height, budgeted.width,
             budgeted.height);
    width = budgeted.width;
    height = budgeted.height;
  }
  const bool sizeChanged = g_graphicsConfig.surfaceConfiguration.width != nativeWidth ||
                           g_graphicsConfig.surfaceConfiguration.height != nativeHeight ||
                           g_frameBuffer.size.width != width || g_frameBuffer.size.height != height;
  if (!force && !sizeChanged) {
    return;
  }
  g_graphicsConfig.surfaceConfiguration.width = nativeWidth;
  g_graphicsConfig.surfaceConfiguration.height = nativeHeight;
  if (const auto presentMode = select_present_mode(g_surfaceCapabilities);
      presentMode != g_graphicsConfig.surfaceConfiguration.presentMode) {
    Log.info("Using present mode {}", magic_enum::enum_name(presentMode));
    g_graphicsConfig.surfaceConfiguration.presentMode = presentMode;
  }
  auto surfaceConfiguration = g_graphicsConfig.surfaceConfiguration;
  surfaceConfiguration.device = g_device;
  {
    window::SurfaceLock surfaceLock;
    g_surface.Configure(&surfaceConfiguration);
  }
  g_frameBuffer = create_render_texture(width, height, true);
  g_frameBufferResolved = create_render_texture(width, height, false);
  g_depthBuffer = create_depth_texture(width, height);
  if (g_graphicsConfig.normalBuffer) {
    g_normalBuffer = create_normal_texture(width, height);
  }
  g_CopyBindGroup = create_copy_bind_group(present_source());
}

bool refresh_surface(bool recreate) {
  const gfx::presentation::SurfaceAcquirePause pause;
  gfx::gpu_synchronize();
  if (!g_instance || !g_device) {
    return false;
  }
  if (!window::is_presentable()) {
    bool released = false;
    {
      window::SurfaceLock surfaceLock;
      released = release_surface_locked();
    }
    if (released) {
      wait_surface_retired();
    }
    return false;
  }
  if ((!g_surface || recreate) && !create_surface()) {
    return false;
  }
  uint32_t width = g_graphicsConfig.surfaceConfiguration.width;
  uint32_t height = g_graphicsConfig.surfaceConfiguration.height;
  uint32_t nativeWidth = width;
  uint32_t nativeHeight = height;
  if (window::get_sdl_window() != nullptr) {
    const auto size = window::get_window_size();
    width = size.fb_width;
    height = size.fb_height;
    nativeWidth = size.native_fb_width;
    nativeHeight = size.native_fb_height;
  }
  if (width != 0 && height != 0) {
    resize_swapchain_internal(width, height, nativeWidth, nativeHeight, true);
  }
  return true;
}

void resize_swapchain(uint32_t width, uint32_t height, uint32_t nativeWidth, uint32_t nativeHeight, bool force) {
  const gfx::presentation::SurfaceAcquirePause pause;
  gfx::gpu_synchronize();
  resize_swapchain_internal(width, height, nativeWidth, nativeHeight, force);
}
} // namespace aurora::webgpu

void aurora_enable_vsync(const bool enabled) {
  aurora::webgpu::g_vsyncEnabled.store(enabled, std::memory_order_release);
  aurora::window::push_custom_event(aurora::window::CustomEvent::RefreshSurface);
}
