#!/usr/bin/env python3
"""Turn translated stage-module functions into build shards + switchable code sets.

Input : build/generated-stgNN/functions/func_*.cpp and base_translation_output.json
        (written by `translate-recursive --project projects/r2ve01/stgNN.yml`)
Output: generated/module_shards/
          stgNN/shard_K.cpp          function bodies, symbols renamed func_X -> stgNN_func_X
          stgNN/registration.cpp     sorted BulkTranslatedFunctionRecord table + ModuleCodeSet
          module_table.cpp           g_moduleCodeSets[] / g_moduleCodeSetCount
          module_shards.cmake        MKW_MODULE_SHARDS source list (included by PublicProducts.cmake)

Only functions inside the module window are emitted; DOL functions reached during discovery are
already in the base build. Module code calls everything through InvokeDirectCpu<addr>, so the
renamed symbols are referenced only from the code-set table.

Usage: python tools/module_shards.py [--shards 24] [stg00 stg01 ...]
"""
import argparse
import json
import os
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WINDOW_BASE = 0x97DC0000
WINDOW_SIZE = 0x00200000
SYM = re.compile(r"\bfunc_([0-9A-Fa-f]{8})")


def add_title_hooks(name, address, text):
    # These special player constructors replace the default character's base
    # HP. Duel mode inherits current HP, so its transfer must stay unscaled.
    player_life_calls = {("stg03", 0x97E0FE9C): 0x97E97090,
                         ("stg03", 0x97E16E48): 0x97E97090,
                         ("stg07", 0x97E32D3C): 0x97E6BBD0}
    life_call = player_life_calls.get((name, address))
    if life_call is not None:
        operand = f"InvokeDirectCpu<0x{life_call:08X}u>(ctx);"
        if text.count(operand) != 1:
            raise ValueError(f"Player base life initialization changed at {name}:{address:08X}")
        text = '#include "sin_coop.h"\n' + text.replace(operand,
            "ctx->fpr[1].d = SinCoopBaseLife(ctx->fpr[1].d);\n    " + operand)
    if name == "stg00" and address == 0x97DD81AC:
        # The 256 contact records at +576 end exactly at the count at +15936.
        # Drop excess contacts before the narrow phase and truncate a manifold
        # which only partially fits. The local manifold holds 64 Vec3s.
        start = "r6 = (r1 + 8);"
        count = "r8 = MemoryInline::FlatRead32((r1 + 788));"
        if text.count(start) != 1 or text.count(count) != 1:
            raise ValueError("Stage 0 rigid-box contact layout changed")
        return text.replace(start, "if (MemoryInline::FlatRead32(r29 + 15936) >= 256u) "
                            "{ r3 = 0; goto loc_97DD82AC; }\n    " + start).replace(
            count, count + "\n    if (static_cast<int32_t>(r8) < 0) r8 = 0;"
            "\n    if (r8 > 64u) r8 = 64u;"
            "\n    const uint32_t room = 256u - MemoryInline::FlatRead32(r29 + 15936);"
            "\n    if (r8 > room) r8 = room;")
    if name == "stg00" and address == 0x97DD88E0:  # RigidBoxManager::CalculateHitPoint
        # GetNumHitPoints can select all eight box vertices. The unsorted
        # path copies both lists into four-Vec3 buffers in the retail frame.
        # Give each list eight slots and keep saved registers at their
        # original offsets relative to the caller's stack pointer.
        offsets = {-320: -416, 216: 264, 220: 268, 224: 272, 228: 276,
                   304: 400, 312: 408, 320: 416, 324: 420}
        stack = re.compile(r"\(r1 \+ (-?\d+)\)")
        used = {int(match[1]) for match in stack.finditer(text)}
        if used.intersection(offsets) != offsets.keys() or any(
                offset >= 216 and offset not in offsets for offset in used):
            raise ValueError("Stage 0 rigid-box stack layout changed")
        return stack.sub(lambda match: f"(r1 + {offsets[int(match[1])]})"
                         if int(match[1]) in offsets else match[0], text)
    if name == "stg03" and address == 0x97E11DDC:  # BikePlayer::cbMdMove, receiver in r28
        operand = "f3.d = MemoryInline::FlatReadFloat32((r3 + 1936));\n    r30 = (r3 + 1924);"
        if text.count(operand) != 1:
            raise ValueError("Stage 3 bike movement input changed")
        return '#include "sin_coop.h"\n' + text.replace(operand,
            "r30 = SinCoopPad(ctx->gpr[13], r28, r3 + 1924);\n    f3.d = MemoryInline::FlatReadFloat32(r30 + 12);")
    # Stage 7 constructs only the selected transformation. Finish the other
    # character through the retail allocator after its factory has returned.
    if name == "stg07" and address == 0x97E32D3C:
        operand = "r4 = MemoryInline::FlatRead8((r3 + 9));"
        if text.count(operand) != 1:
            raise ValueError("Stage 7 transformation identity changed")
        return '#include "sin_coop.h"\n' + text.replace(operand,
            "r4 = SinCoopFinaleCharacter(r30, MemoryInline::FlatRead8(r3 + 9));")
    if name != "stg07" or address != 0x97E377E8:
        return text
    text = text.replace("    goto loc_97E377E8;", "    SinCoopSeedTestCharacter(ctx);\n    goto loc_97E377E8;")
    epilogue = "    ctx->cr = cr;\n    return;"
    if text.count(epilogue) != 1:
        raise ValueError("Stage 7 player factory epilogue changed")
    return '#include "sin_coop.h"\n' + text.replace(
        epilogue, "    ctx->cr = cr;\n    SinCoopFinishStageSeven(ctx);\n    return;")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("modules", nargs="*")
    ap.add_argument("--shards", type=int, default=24)
    ap.add_argument("--generated-root", default="build", help="directory containing generated-stgNN outputs")
    ap.add_argument("--module-root", default="build/modules", help="directory containing linked stgNN.json files")
    ap.add_argument("--out", default=None, help="output directory (default generated/module_shards)")
    a = ap.parse_args()
    modules = a.modules or [f"stg0{i}" for i in range(8)]
    out_root = Path(a.out) if a.out else ROOT / "generated" / "module_shards"
    if not out_root.is_absolute(): out_root = ROOT / out_root
    out_root.mkdir(parents=True, exist_ok=True)
    all_sources = []
    for name in modules:
        generated_root = Path(a.generated_root)
        if not generated_root.is_absolute(): generated_root = ROOT / generated_root
        module_root = Path(a.module_root)
        if not module_root.is_absolute(): module_root = ROOT / module_root
        src_dir = generated_root / f"generated-{name}" / "functions"
        meta = json.load(open(generated_root / f"generated-{name}" / "base_translation_output.json"))
        info = {}
        for fn in meta["functions"]:
            addr = fn["entryPoint"]
            if WINDOW_BASE <= addr < WINDOW_BASE + WINDOW_SIZE and fn.get("relativePath", "").startswith("func_"):
                b = fn["build"]
                info[addr] = (fn["relativePath"], bool(b["preservesNonvolatileFprs"]), int(b["nonvolatileFprWriteMask"]))
        addrs = sorted(info)
        mod_dir = out_root / name
        mod_dir.mkdir(parents=True, exist_ok=True)
        for old in mod_dir.glob("*.cpp"):
            old.unlink()
        # balance shards by source size (greedy, largest first)
        sizes = {ad: os.path.getsize(src_dir / info[ad][0]) for ad in addrs}
        buckets = [[] for _ in range(a.shards)]
        loads = [0] * a.shards
        for ad in sorted(addrs, key=lambda x: -sizes[x]):
            k = loads.index(min(loads))
            buckets[k].append(ad)
            loads[k] += sizes[ad]
        for k, members in enumerate(buckets):
            if not members:
                continue
            parts = [f"// Generated by tools/module_shards.py - do not edit ({name}, shard {k}).\n",
                     '#include "guest_abi/guest_abi.h"\n']
            for ad in sorted(members):
                text = add_title_hooks(name, ad, (src_dir / info[ad][0]).read_text())
                text = SYM.sub(lambda m: f"{name}_func_{m.group(1).upper()}", text)
                parts.append(f'#line 1 "{info[ad][0]}"\n')
                parts.append(text if text.endswith("\n") else text + "\n")
            p = mod_dir / f"shard_{k:02d}.cpp"
            p.write_text("".join(parts))
            all_sources.append(p)
        reg = [f"// Generated by tools/module_shards.py - do not edit ({name} code set).\n",
               '#include "guest_abi/guest_abi.h"\n#include "guest_abi/module_codeset.h"\n', 'extern "C" {\n']
        for ad in addrs:
            reg.append(f"void {name}_func_{ad:08X}(CpuContext*);\n")
        reg.append("}\n")
        reg.append(f"static const BulkTranslatedFunctionRecord k_{name}_records[] = {{\n")
        for ad in addrs:
            _, preserves, mask = info[ad]
            reg.append(f"    {{0x{ad:08X}u, nullptr, &{name}_func_{ad:08X}, FunctionKind::BaseTranslated, "
                       f"{'true' if preserves else 'false'}, 0x{mask:08X}u, 0u, 0ull, true}},\n")
        reg.append("};\n")
        reg.append(f'extern "C" const ModuleCodeSet g_moduleCodeSet_{name} = {{"{name}", 0x{WINDOW_BASE:08X}u, '
                   f"0x{WINDOW_SIZE:08X}u, k_{name}_records, sizeof(k_{name}_records) / sizeof(k_{name}_records[0]), "
                   f"0x{json.load(open(module_root / (name + '.json')))['prolog']:08X}u, "
                   f"0x{json.load(open(module_root / (name + '.json')))['epilog']:08X}u, "
                   f"0x{json.load(open(module_root / (name + '.json')))['unresolved_fn']:08X}u}};\n")
        p = mod_dir / "registration.cpp"
        p.write_text("".join(reg))
        all_sources.append(p)
        print(f"{name}: {len(addrs)} functions in {sum(1 for b in buckets if b)} shards")
    table = ['// Generated by tools/module_shards.py - do not edit.\n', '#include "guest_abi/module_codeset.h"\n']
    for name in modules:
        table.append(f'extern "C" const ModuleCodeSet g_moduleCodeSet_{name};\n')
    table.append('extern "C" const ModuleCodeSet* const g_moduleCodeSets[] = {\n')
    for name in modules:
        table.append(f"    &g_moduleCodeSet_{name},\n")
    table.append("};\n")
    table.append(f'extern "C" const unsigned long long g_moduleCodeSetCount = {len(modules)};\n')
    tp = out_root / "module_table.cpp"
    tp.write_text("".join(table))
    all_sources.append(tp)
    cm = ["# Generated by tools/module_shards.py - do not edit.\n", "set(MKW_MODULE_SHARDS\n"]
    cm += [f'  "{p.as_posix()}"\n' for p in all_sources]
    cm.append(")\n")
    (out_root / "module_shards.cmake").write_text("".join(cm))
    print(f"wrote {len(all_sources)} sources under {out_root}")


if __name__ == "__main__":
    main()
