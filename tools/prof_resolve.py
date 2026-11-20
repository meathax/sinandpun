"""Resolve prof_samples.txt (SIN_PROF_SAMPLER=1) RVAs to symbols with llvm-nm.
usage: python tools/prof_resolve.py [prof_samples.txt] [exe] [top=40]
Prints per thread the top symbols by self samples."""
import bisect, subprocess, sys, re, collections
NM = "C:/Users/meath/.local/share/retcomm/toolchains/cmake-clang-v1/latest/bin/llvm-nm.exe"
samples = sys.argv[1] if len(sys.argv) > 1 else "build/native/prof_samples.txt"
exe = sys.argv[2] if len(sys.argv) > 2 else "build/native/WiiCompiled.exe"
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
out = subprocess.run([NM, "-n", "--defined-only", exe], capture_output=True, text=True).stdout
# image base: take from the lowest text symbol vs. known RVA is unknown, so ask objdump-free: use llvm-readobj
ro = subprocess.run([NM.replace("llvm-nm", "llvm-readobj"), "--file-headers", exe], capture_output=True, text=True).stdout
base = int(re.search(r"ImageBase:\s*(0x[0-9A-Fa-f]+)", ro).group(1), 16)
addrs, names = [], []
for line in out.splitlines():
    p = line.split(None, 2)
    if len(p) == 3 and p[1] in "tTwW":
        addrs.append(int(p[0], 16) - base); names.append(p[2])
cur = None; hist = collections.defaultdict(collections.Counter); tot = {}
for line in open(samples):
    p = line.split()
    if p[0] == "tid":
        cur = p[1]; tot[cur] = int(p[3])
    elif p[0] == "rounds" or cur is None:
        continue
    else:
        rva = int(p[1], 16)
        i = bisect.bisect_right(addrs, rva) - 1
        hist[cur]["~EXT(wait/os)" if rva >= 0x0FFFFFF0 else (names[i] if i >= 0 else "?")] += int(p[0])
for tid, c in sorted(hist.items(), key=lambda kv: -tot[kv[0]]):
    print(f"== tid {tid} running samples {tot[tid]}")
    for n, v in c.most_common(top):
        print(f"{100.0*v/tot[tid]:6.2f}% {v:7d} {n}")
