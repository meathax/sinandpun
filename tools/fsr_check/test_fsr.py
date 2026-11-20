"""Runs the EASU/RCAS WGSL extracted from aurora-main/lib/webgpu/upscale.cpp on the GPU (wgpu-py) and checks it
against an independent numpy port of ffx_fsr1.h (taps named by position, not by gather order)."""
import os, re, struct, sys
import numpy as np
import wgpu
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
os.makedirs(os.path.join(ROOT, "build", "tmp_fsr"), exist_ok=True)
SRC = open(os.path.join(ROOT, "aurora-main", "lib", "webgpu", "upscale.cpp"), encoding="utf8").read()
EASU = re.search(r'easuShader = R"\((.*?)\)"sv;', SRC, re.S).group(1)
RCAS = re.search(r'rcasShader = R"\((.*?)\)"sv;', SRC, re.S).group(1)

def make_scene(w, h):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    img = np.zeros((h, w, 3), np.float32)
    img[..., 0] = 0.15 + 0.5 * x / w
    img[..., 1] = 0.2 + 0.5 * y / h
    img[..., 2] = 0.6 - 0.3 * x / w
    # diagonal edges and thin lines
    d = (x * 0.7 + y) % 90
    img[d < 3] = (0.95, 0.95, 0.9)
    # circle
    c = (x - w * 0.3) ** 2 + (y - h * 0.5) ** 2 < (h * 0.22) ** 2
    img[c] = (0.9, 0.2, 0.15)
    # black and white blocks (exercises the RCAS 0*inf guards and the clamping)
    img[10:60, w - 150:w - 100] = 0.0
    img[10:60, w - 90:w - 40] = 1.0
    # text-like vertical stripes
    s = ((x // 2) % 2 == 0) & (y > h * 0.8) & (y < h * 0.9) & (x > w * 0.5)
    img[s] = (0.05, 0.05, 0.05)
    return np.clip(img, 0, 1)

def box_down(img, f):
    h, w, _ = img.shape
    return img.reshape(h // f, f, w // f, f, 3).mean(axis=(1, 3))

def to8(img):
    return np.round(np.clip(img, 0, 1) * 255).astype(np.uint8)

def bits(a):
    return np.asarray(a, np.float32).view(np.uint32)

def unbits(u):
    return np.asarray(u, np.uint32).view(np.float32)

def rcp_lo(a):
    return unbits((np.uint32(0x7ef07ebb) - bits(a)).astype(np.uint32))

def rsq_lo(a):
    return unbits((np.uint32(0x5f347d74) - (bits(a) >> np.uint32(1))).astype(np.uint32))

def rcp_med(a):
    b = unbits((np.uint32(0x7ef19fff) - bits(a)).astype(np.uint32))
    return b * (-b * a + np.float32(2.0))

def easu_cpu(src, ow, oh):
    ih, iw, _ = src.shape
    f32 = np.float32
    ys, xs = np.mgrid[0:oh, 0:ow].astype(f32)
    ppx = xs * f32(iw / ow) + f32(0.5 * iw / ow - 0.5)
    ppy = ys * f32(ih / oh) + f32(0.5 * ih / oh - 0.5)
    fx = np.floor(ppx); fy = np.floor(ppy)
    px = ppx - fx; py = ppy - fy
    fxi = fx.astype(np.int32); fyi = fy.astype(np.int32)

    def tap(dx, dy):
        xi = np.clip(fxi + dx, 0, iw - 1); yi = np.clip(fyi + dy, 0, ih - 1)
        return src[yi, xi]
    T = {n: tap(*o) for n, o in dict(b=(0, -1), c=(1, -1), e=(-1, 0), f=(0, 0), g=(1, 0), h=(2, 0),
                                     i=(-1, 1), j=(0, 1), k=(1, 1), l=(2, 1), n=(0, 2), o=(1, 2)).items()}
    L = {k: v[..., 2] * f32(0.5) + (v[..., 0] * f32(0.5) + v[..., 1]) for k, v in T.items()}
    dirx = np.zeros_like(px); diry = np.zeros_like(px); ln = np.zeros_like(px)

    def setf(w, lA, lB, lC, lD, lE):
        nonlocal dirx, diry, ln
        dc = lD - lC; cb = lC - lB
        lenX = rcp_lo(np.maximum(np.abs(dc), np.abs(cb)))
        dirX = lD - lB
        dirx = dirx + dirX * w
        lenX = np.clip(np.abs(dirX) * lenX, 0, 1); lenX = lenX * lenX
        ln = ln + lenX * w
        ec = lE - lC; ca = lC - lA
        lenY = rcp_lo(np.maximum(np.abs(ec), np.abs(ca)))
        dirY = lE - lA
        diry = diry + dirY * w
        lenY = np.clip(np.abs(dirY) * lenY, 0, 1); lenY = lenY * lenY
        ln = ln + lenY * w
    setf((1 - px) * (1 - py), L['b'], L['e'], L['f'], L['g'], L['j'])
    setf(px * (1 - py), L['c'], L['f'], L['g'], L['h'], L['k'])
    setf((1 - px) * py, L['f'], L['i'], L['j'], L['k'], L['n'])
    setf(px * py, L['g'], L['j'], L['k'], L['l'], L['o'])
    dirR = dirx * dirx + diry * diry
    zro = dirR < f32(1.0 / 32768.0)
    dirR = rsq_lo(dirR)
    dirR = np.where(zro, f32(1.0), dirR)
    dirx = np.where(zro, f32(1.0), dirx)
    dirx = dirx * dirR; diry = diry * dirR
    ln = ln * f32(0.5); ln = ln * ln
    stretch = (dirx * dirx + diry * diry) * rcp_lo(np.maximum(np.abs(dirx), np.abs(diry)))
    len2x = f32(1.0) + (stretch - f32(1.0)) * ln
    len2y = f32(1.0) - f32(0.5) * ln
    lob = f32(0.5) + f32((1.0 / 4.0 - 0.04) - 0.5) * ln
    clp = rcp_lo(lob)
    mins = np.minimum(np.minimum(T['f'], np.minimum(T['g'], T['j'])), T['k'])
    maxs = np.maximum(np.maximum(T['f'], np.maximum(T['g'], T['j'])), T['k'])
    aC = np.zeros_like(src[:1, :1].repeat(oh, 0).repeat(ow, 1)); aW = np.zeros_like(px)
    offs = dict(b=(0, -1), c=(1, -1), i=(-1, 1), j=(0, 1), f=(0, 0), e=(-1, 0), k=(1, 1), l=(2, 1), h=(2, 0),
                g=(1, 0), o=(1, 2), n=(0, 2))
    for name, (ox, oy) in offs.items():
        offx = f32(ox) - px; offy = f32(oy) - py
        vx = offx * dirx + offy * diry
        vy = offx * (-diry) + offy * dirx
        vx = vx * len2x; vy = vy * len2y
        d2 = np.minimum(vx * vx + vy * vy, clp)
        wB = f32(0.4) * d2 - f32(1.0); wA = lob * d2 - f32(1.0)
        wB = wB * wB; wA = wA * wA
        wB = f32(1.5625) * wB - f32(0.5625)
        w = wB * wA
        aC = aC + T[name] * w[..., None]
        aW = aW + w
    pix = np.minimum(maxs, np.maximum(mins, aC * (f32(1.0) / aW)[..., None]))
    return pix.astype(np.float32)

def rcas_cpu(img, sharp_lin):
    f32 = np.float32
    p = np.pad(img, ((1, 1), (1, 1), (0, 0)), mode='edge')
    e = p[1:-1, 1:-1]; b = p[:-2, 1:-1]; d = p[1:-1, :-2]; f = p[1:-1, 2:]; h = p[2:, 1:-1]
    mn4 = np.minimum(np.minimum(b, np.minimum(d, f)), h)
    mx4 = np.maximum(np.maximum(b, np.maximum(d, f)), h)
    with np.errstate(all='ignore'):
        hitMin = np.minimum(mn4, e) * (f32(1.0) / np.maximum(f32(4.0) * mx4, f32(1e-20)))
        hitMax = (f32(1.0) - np.maximum(mx4, e)) * (f32(1.0) / np.minimum(f32(4.0) * mn4 - f32(4.0), f32(-1e-20)))
    lobeV = np.maximum(-hitMin, hitMax)
    lobe = np.maximum(f32(-(0.25 - 1.0 / 16.0)), np.minimum(lobeV.max(axis=2), f32(0.0))) * f32(sharp_lin)
    rcpL = rcp_med(f32(4.0) * lobe + f32(1.0))
    l = lobe[..., None]
    return ((l * b + l * d + l * h + l * f + e) * rcpL[..., None]).astype(np.float32)

# ---------------- GPU ----------------
adapter = wgpu.gpu.request_adapter_sync(power_preference="high-performance")
print("adapter:", adapter.info.get("device"), adapter.info.get("backend_type"))
device = adapter.request_device_sync()

def easu_constants(iw, ih, ow, oh):
    c = np.zeros(16, np.float32)
    c[0:4] = [iw / ow, ih / oh, 0.5 * iw / ow - 0.5, 0.5 * ih / oh - 0.5]
    c[4:8] = [1 / iw, 1 / ih, 1 / iw, -1 / ih]
    c[8:12] = [-1 / iw, 2 / ih, 1 / iw, 2 / ih]
    c[12:16] = [0, 4 / ih, 0, 0]
    return c

def run_easu(src8, ow, oh, out_format):
    ih, iw, _ = src8.shape
    rgba = np.dstack([src8, np.full((ih, iw), 255, np.uint8)])
    tex = device.create_texture(size=(iw, ih, 1), format="rgba8unorm",
                                usage=wgpu.TextureUsage.TEXTURE_BINDING | wgpu.TextureUsage.COPY_DST)
    device.queue.write_texture({"texture": tex}, rgba.tobytes(), {"bytes_per_row": iw * 4, "rows_per_image": ih},
                               (iw, ih, 1))
    samp = device.create_sampler(address_mode_u="clamp-to-edge", address_mode_v="clamp-to-edge",
                                 mag_filter="nearest", min_filter="nearest")
    ub = device.create_buffer_with_data(data=easu_constants(iw, ih, ow, oh).tobytes(),
                                        usage=wgpu.BufferUsage.UNIFORM)
    bgl = device.create_bind_group_layout(entries=[
        {"binding": 0, "visibility": wgpu.ShaderStage.FRAGMENT, "buffer": {"type": "uniform"}},
        {"binding": 1, "visibility": wgpu.ShaderStage.FRAGMENT, "sampler": {"type": "non-filtering"}},
        {"binding": 2, "visibility": wgpu.ShaderStage.FRAGMENT,
         "texture": {"sample_type": "float", "view_dimension": "2d"}}])
    bg = device.create_bind_group(layout=bgl, entries=[
        {"binding": 0, "resource": {"buffer": ub, "offset": 0, "size": 64}},
        {"binding": 1, "resource": samp},
        {"binding": 2, "resource": tex.create_view()}])
    mod = device.create_shader_module(code=EASU)
    pl = device.create_render_pipeline(
        layout=device.create_pipeline_layout(bind_group_layouts=[bgl]),
        vertex={"module": mod, "entry_point": "vs_main"},
        fragment={"module": mod, "entry_point": "fs_main", "targets": [{"format": out_format}]},
        primitive={"topology": "triangle-list"})
    return render(pl, bg, ow, oh, out_format)

def run_rcas(img_f16, sharp_lin, out_format="rgba8unorm"):
    oh, ow, _ = img_f16.shape
    rgba = np.dstack([img_f16, np.ones((oh, ow), np.float32)]).astype(np.float16)
    tex = device.create_texture(size=(ow, oh, 1), format="rgba16float",
                                usage=wgpu.TextureUsage.TEXTURE_BINDING | wgpu.TextureUsage.COPY_DST)
    device.queue.write_texture({"texture": tex}, rgba.tobytes(), {"bytes_per_row": ow * 8, "rows_per_image": oh},
                               (ow, oh, 1))
    ub = device.create_buffer_with_data(data=np.array([sharp_lin, 0, 0, 0], np.float32).tobytes(),
                                        usage=wgpu.BufferUsage.UNIFORM)
    bgl = device.create_bind_group_layout(entries=[
        {"binding": 0, "visibility": wgpu.ShaderStage.FRAGMENT, "buffer": {"type": "uniform"}},
        {"binding": 1, "visibility": wgpu.ShaderStage.FRAGMENT,
         "texture": {"sample_type": "float", "view_dimension": "2d"}}])
    bg = device.create_bind_group(layout=bgl, entries=[
        {"binding": 0, "resource": {"buffer": ub, "offset": 0, "size": 16}},
        {"binding": 1, "resource": tex.create_view()}])
    mod = device.create_shader_module(code=RCAS)
    pl = device.create_render_pipeline(
        layout=device.create_pipeline_layout(bind_group_layouts=[bgl]),
        vertex={"module": mod, "entry_point": "vs_main"},
        fragment={"module": mod, "entry_point": "fs_main", "targets": [{"format": out_format}]},
        primitive={"topology": "triangle-list"})
    return render(pl, bg, ow, oh, out_format)

def render(pl, bg, w, h, fmt):
    bpp = 8 if fmt == "rgba16float" else 4
    target = device.create_texture(size=(w, h, 1), format=fmt,
                                   usage=wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.COPY_SRC)
    row = (w * bpp + 255) // 256 * 256
    buf = device.create_buffer(size=row * h, usage=wgpu.BufferUsage.COPY_DST | wgpu.BufferUsage.MAP_READ)
    enc = device.create_command_encoder()
    p = enc.begin_render_pass(color_attachments=[{"view": target.create_view(), "load_op": "clear",
                                                  "store_op": "store", "clear_value": (0, 0, 0, 1)}])
    p.set_pipeline(pl); p.set_bind_group(0, bg); p.set_viewport(0, 0, w, h, 0, 1); p.draw(3); p.end()
    enc.copy_texture_to_buffer({"texture": target}, {"buffer": buf, "bytes_per_row": row, "rows_per_image": h},
                               (w, h, 1))
    device.queue.submit([enc.finish()])
    buf.map_sync(wgpu.MapMode.READ)
    raw = np.frombuffer(buf.read_mapped(), np.uint8).reshape(h, row)[:, : w * bpp].copy()
    buf.unmap()
    if fmt == "rgba16float":
        return raw.view(np.float16).reshape(h, w, 4)[..., :3].astype(np.float32)
    return raw.reshape(h, w, 4)[..., :3].astype(np.float32) / 255.0

def psnr(a, b):
    mse = float(np.mean((a - b) ** 2))
    return 99.0 if mse == 0 else 10 * np.log10(1.0 / mse)

def bilinear(src, ow, oh):
    return np.asarray(Image.fromarray(to8(src)).resize((ow, oh), Image.BILINEAR), np.float32) / 255.0

ok = True
gt = make_scene(960, 540)
for factor, name in [(2, "Performance 2.0x"), (1.5, "Quality 1.5x"), (3, "UltraPerf 3.0x")]:
    iw, ih = int(round(960 / factor)), int(round(540 / factor))
    # ground truth downsample: render the scene at the reduced size directly via area resize of the hi-res image
    src = np.asarray(Image.fromarray(to8(gt)).resize((iw, ih), Image.BOX), np.float32) / 255.0
    src8 = to8(src)
    srcq = src8.astype(np.float32) / 255.0
    gpu = run_easu(src8, 960, 540, "rgba16float")
    cpu = easu_cpu(srcq, 960, 540)
    err = np.abs(gpu - cpu)
    # approximate rcp/rsq are bit tricks: compare with a tolerance far under one 8-bit step on >99.9% of pixels
    frac_bad = float(np.mean(err.max(axis=2) > 1.0 / 255.0))
    print(f"{name}: EASU gpu-vs-cpu max {err.max():.5f} mean {err.mean():.6f} frac>1/255 {frac_bad:.5f}")
    if err.max() > 0.02 or frac_bad > 0.001:
        ok = False
    bl = bilinear(srcq, 960, 540)
    print(f"   PSNR vs ground truth: bilinear {psnr(bl, gt):.2f} dB   EASU {psnr(gpu, gt):.2f} dB")
    if factor == 2:
        sharp = 2 ** (-2 * (1 - 0.8))
        g_rcas = run_rcas(gpu, sharp)
        c_rcas = rcas_cpu(np.clip(gpu.astype(np.float16).astype(np.float32), 0, 1), sharp)
        e2 = np.abs(g_rcas - np.round(np.clip(c_rcas, 0, 1) * 255) / 255)
        print(f"   RCAS(80%={sharp:.3f}) gpu-vs-cpu max {e2.max():.5f} frac>1/255 {float(np.mean(e2.max(axis=2) > 1/255+1e-6)):.5f}"
              f"  PSNR vs gt {psnr(g_rcas, gt):.2f} dB; nan={bool(np.isnan(g_rcas).any())}")
        if e2.max() > 3.0 / 255 or np.isnan(g_rcas).any():
            ok = False
        Image.fromarray(to8(np.hstack([bl, gpu.clip(0, 1), g_rcas]))).save(os.path.join(ROOT, "build", "tmp_fsr", "compare_2x.png"))
# unsharpened path straight to 8-bit surface format
g8 = run_easu(to8(np.asarray(Image.fromarray(to8(gt)).resize((480, 270), Image.BOX), np.float32) / 255.0), 960, 540,
              "rgba8unorm")
print("EASU->rgba8unorm ok", g8.shape, float(g8.mean()))
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)



