"""Edit UserData/Config.toml sections for the mouse tests (line based, section aware).
usage: mouse_cfg.py CONFIG [section.key=value ...]   (value is written verbatim, quote strings yourself)
"""
import re
import sys

path = sys.argv[1]
edits = {}
for a in sys.argv[2:]:
    k, v = a.split('=', 1)
    sec, key = k.rsplit('.', 1)
    if not re.fullmatch(r'-?[0-9.]+|true|false|0x[0-9a-fA-F]+', v.strip('"')) and not v.startswith('"'):
        v = '"' + v + '"'
    edits[(sec, key)] = v

lines = open(path, encoding='utf-8').read().split('\n')
out = []
sec = ''
done = set()
last_in_sec = {}
for i, ln in enumerate(lines):
    m = re.match(r'^\[(.+)\]\s*$', ln)
    if m:
        sec = m.group(1)
        out.append(ln)
        last_in_sec[sec] = len(out) - 1
        continue
    m = re.match(r'^(\w+)\s*=', ln)
    if m and (sec, m.group(1)) in edits:
        out.append(f'{m.group(1)} = {edits[(sec, m.group(1))]}')
        done.add((sec, m.group(1)))
    else:
        out.append(ln)
    if m:
        last_in_sec[sec] = len(out) - 1
# missing keys: append to the end of their section (or a new section)
for (s, k), v in edits.items():
    if (s, k) in done:
        continue
    if s in last_in_sec:
        out.insert(last_in_sec[s] + 1, f'{k} = {v}')
        for kk in last_in_sec:
            if last_in_sec[kk] > last_in_sec[s]:
                last_in_sec[kk] += 1
        last_in_sec[s] += 1
    else:
        out += ['', f'[{s}]', f'{k} = {v}']
        last_in_sec[s] = len(out) - 1
open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))

