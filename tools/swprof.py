#!/usr/bin/env python3
"""
swprof.py NAME [NAME...]: the numbers of docs/swipe-profile.md from logs/NAME.log (runs with userland/rc.jobs.swprof).
  passbench runs (pass_bench): a table of the cases, with the kernel's jobs and syncobj requests per pass
  swprof runs (swprof=1, bb_prof_all=1): the swipe phase's frames, passes, requests per frame, the swprof counters,
  and the render thread's top frames. Kernel "pvr: stats:" blocks have no stamp: they are taken by their place
  between the stamped lines; only the log before "kept for the host by the kernel" is read (after it is a copy).
"""
import re, sys

import bisect, os, subprocess
LIBS = {  # image name -> an unstripped copy with the same addresses
    'libgallium-26.3.0.dylib': 'xnu-deps/build/pvr-mesa/build-ios-zink/src/gallium/targets/dri/libgallium-26.3.0.dylib',
    'libvulkan_powervr_mesa.dylib': 'xnu-deps/build/pvr-mesa/build-ios/src/imagination/vulkan/libvulkan_powervr_mesa.dylib',
    'libredwood-gl.dylib': 'userland/obj/libredwood-gl.dylib',
    'libredwood.dylib': 'userland/obj/libredwood.dylib',
}
_syms = {}
def symbolise(image, off):
    path = LIBS.get(image)
    if path is None or not os.path.exists(path):
        return f'{image}+{off:#x}'
    if image not in _syms:
        out = subprocess.run(['nm', '-n', path], capture_output=True, text=True).stdout
        rows = [(int(a, 16), n) for a, t, n in (l.split(None, 2) for l in out.splitlines() if len(l.split(None, 2)) == 3) if t in 'tTsS' for _ in [0]]
        _syms[image] = ([r[0] for r in rows], [r[1] for r in rows])
    addrs, names = _syms[image]
    i = bisect.bisect_right(addrs, off) - 1
    return f'{image}`{names[i]}' if i >= 0 else f'{image}+{off:#x}'

def blocks_of(lines):
    out, blk = [], None
    for i, l in enumerate(lines):
        if re.search(r'pvr: stats: \d+ clients', l):
            blk = {'at': i, 'req': {}, 'us': {}}
            out.append(blk)
        elif blk is not None:
            m = re.search(r'jobs so far (\d+) in', l)
            if m: blk['in'] = int(m.group(1))
            m = re.search(r'stats: (geometry|fragment): submit to kick.*?(\d+) in all', l)
            if m: blk[m.group(1)] = int(m.group(2))
            if 'stats: requests' in l:
                for n, c, us in re.findall(r'([A-Z_]+) (\d+): (\d+)/', l):
                    blk['req'][n] = blk['req'].get(n, 0) + int(c)
                    blk['us'][n] = blk['us'].get(n, 0) + int(us)
    return out

def load(name):
    t = open(f'logs/{name}.log', 'rb').read().decode('latin-1')
    t = t.split('kept for the host by the kernel')[0]
    return t.split('\n')

def stamped(lines, pat):
    seen, out = set(), []
    for i, l in enumerate(lines):
        m = re.match(r'\[\s*([\d.]+)\]: !?(' + pat + r'.*)', l)
        if m and (m.group(1), m.group(2)) not in seen:
            seen.add((m.group(1), m.group(2)))
            out.append((i, float(m.group(1)), m.group(2)))
    return out

def passbench(name):
    lines = load(name)
    cases = [(i, t, [x.strip() for x in s.split('|')]) for i, t, s in stamped(lines, r'pass_bench: case: ') if len(s.split('|')) >= 6 and s.split('|')[1].strip().isdigit()]
    if not cases:
        return
    blocks = blocks_of(lines)
    print(f'### {name}: one render pass, 375x500 RGBA8 (CPU ms: app thread / rest of process = Zink driver thread + Vulkan driver + kernel calls)\n')
    print('| case | app CPU | driver CPU | issue wall | all-done wall | geom jobs | frag jobs | SYNCOBJ_TRANSFER | SYNCOBJ_RESET | SYNCOBJ_WAIT | us in syncobj calls |')
    print('|---|---|---|---|---|---|---|---|---|---|---|')
    used = set()
    for i, t, f in cases:
        n = int(f[1])
        label = f[0].split(':', 2)[-1].strip() if 'case:' in f[0] else f[0]
        # the first unused block after the case that holds about its passes (blocks are 10 s; the case is idle 11 s after)
        b = next((b for b in blocks if b['at'] > i - 0 and b['at'] not in used and 0.9 * n <= b['req'].get('SUBMIT_JOBS', 0) + 0 <= 1.2 * n), None)
        if b is None:
            print(f'| {label} | {f[2]} | {f[3]} | {f[4]} | {f[5]} | - | - | - | - | - | - |')
            continue
        used.add(b['at'])
        nxt = next((x for x in blocks if x['at'] > b['at']), {})
        for k in ('geometry', 'fragment'):      # (a block's job counts can sit in the one after it)
            b[k] = max(b.get(k, 0), nxt.get(k, 0) if nxt.get(k, 0) <= 1.1 * 2 * n else 0)
        g = lambda k: b['req'].get(k, 0)
        us = sum(b['us'].get(k, 0) for k in ('SYNCOBJ_TRANSFER', 'SYNCOBJ_RESET', 'SYNCOBJ_WAIT'))
        print(f"| {label} | {f[2]} | {f[3]} | {f[4]} | {f[5]} | {b.get('geometry', 0) / n:.1f} | {b.get('fragment', 0) / n:.1f} | {g('SYNCOBJ_TRANSFER') / n:.1f} | "
              f"{g('SYNCOBJ_RESET') / n:.1f} | {g('SYNCOBJ_WAIT') / n:.1f} | {us / n:.0f} |")
    print('\n(jobs and requests per pass are for the whole ten-second block holding the case: the idle system adds a little)\n')

def swprof(name):
    lines = load(name)
    sw = stamped(lines, r'redwood-gl: swprof: ')
    if not sw:
        return
    marks = {m: t for i, t, m in stamped(lines, r'auto: (?:swprof|end)')}
    start = marks.get('auto: swprof'); end = marks.get('auto: end')
    print(f'### {name}: swipe phase (auto: swprof at {start}, end {end})\n')
    rows = []
    for i, t, s in sw:
        m = re.search(r'(\d+) frames, (\d+) display, (\d+) glFlush, (\d+) glFinish, (\d+) draws, (\d+) clears, (\d+) fetch draws, (\d+) fetch programs made, fetch (\w+), ([\d.]+) ms a frame, ([\d.]+) in glFlush', s)
        if m:
            rows.append((t, i) + tuple(float(x) if '.' in x else (int(x) if x.isdigit() else x) for x in m.groups()))
    inside = [r for r in rows if start is not None and start <= r[0] <= (end or 1e9) + 12]
    print('| log s | frames | display | glFlush | glFinish | draws | clears | fetch draws | ms/frame | ms in glFlush/frame |')
    print('|---|---|---|---|---|---|---|---|---|---|')
    for r in inside:
        print(f'| {r[0]:.0f} | {r[2]} | {r[3]} | {r[4]} | {r[5]} | {r[6]} | {r[7]} | {r[8]} | {r[11]} | {r[12]} |')
    blocks = [b for b in blocks_of(lines)]
    # kernel blocks between the first and last swprof line of the phase, by place in the file
    if inside:
        lo, hi = inside[0][1], inside[-1][1]
        sel = [b for b in blocks if lo <= b['at'] <= hi]
        frames = sum(r[2] for r in inside[1:])      # (the first line's window began before the swipes)
        nb = max(len(sel) - 1, 1)
        print(f'\nkernel blocks in the phase: {len(sel)}; frames in the phase windows (without the first): {frames}')
        if len(sel) > 1:
            tot = lambda k: sum(b['req'].get(k, 0) for b in sel[1:])
            fr = (sel[-1].get('fragment', 0) - sel[0].get('fragment', 0))
            ge = (sel[-1].get('geometry', 0) - sel[0].get('geometry', 0))
            print(f'geometry jobs {ge}, fragment jobs {fr} between the first and last block')
            if frames:
                print(f'per frame: geometry jobs {ge / frames:.1f}, fragment jobs {fr / frames:.1f}, SUBMIT_JOBS {tot("SUBMIT_JOBS") / frames:.1f}, '
                      f'SYNCOBJ_TRANSFER {tot("SYNCOBJ_TRANSFER") / frames:.1f}, SYNCOBJ_RESET {tot("SYNCOBJ_RESET") / frames:.1f}, SYNCOBJ_WAIT {tot("SYNCOBJ_WAIT") / frames:.1f}')
                us = sum(sum(b['us'].get(k, 0) for k in ('SYNCOBJ_TRANSFER', 'SYNCOBJ_RESET', 'SYNCOBJ_WAIT')) for b in sel[1:])
                sub = sum(b['us'].get('SUBMIT_JOBS', 0) for b in sel[1:])
                print(f'kernel time in syncobj requests {us / frames:.0f} us/frame, in SUBMIT_JOBS {sub / frames:.0f} us/frame')
    # the profile
    for i, t, s in stamped(lines, r'bbprof (?:state|:|per second)'):
        print(f'\n    {s}')
    print('\n| rank | thread | samples | of which running | image`symbol (innermost frame) |\n|---|---|---|---|---|')
    for k, (i, t, s) in enumerate([x for x in stamped(lines, r'bbprof leaf ')][:40]):
        m = re.match(r'bbprof leaf (\d+) of (\d+) \(run (\d+)\) tid (\w+) (.*)', s)
        if m: print(f'| {k + 1} | {m.group(4)} | {m.group(1)} | {m.group(3)} | {m.group(5)} |')
    print('\n| rank | thread | samples (anywhere in the stack) | of which running | image`symbol |\n|---|---|---|---|---|')
    for k, (i, t, s) in enumerate([x for x in stamped(lines, r'bbprof incl ')][:70]):
        m = re.match(r'bbprof incl (\d+) of (\d+) \(run (\d+)\) tid (\w+) (.*)', s)
        if m: print(f'| {k + 1} | {m.group(4)} | {m.group(1)} | {m.group(3)} | {m.group(5)} |')
    # the GL stack's frames by exact address, named here
    for tid in sorted({m.group(1) for i, t, x in stamped(lines, r'bbprof pc ') for m in [re.match(r'bbprof pc \d+ \(run \d+\) tid (\w+)', x)] if m}):
        agg = {}
        for i, t, x in stamped(lines, r'bbprof pc '):
            m = re.match(r'bbprof pc (\d+) \(run (\d+)\) tid (\w+) (\S+)\+0x([0-9a-f]+)', x)
            if m and m.group(3) == tid:
                f = symbolise(m.group(4), int(m.group(5), 16))
                a = agg.setdefault(f, [0, 0]); a[0] += int(m.group(1)); a[1] += int(m.group(2))
        print(f'\nthread {tid}: GL stack frames by function, samples with a call from it on the stack (exact return addresses summed per function):\n')
        print('| rank | samples | of which running | function |\n|---|---|---|---|')
        for k, (f, (a, r)) in enumerate(sorted(agg.items(), key=lambda kv: -kv[1][0])[:40]):
            print(f'| {k + 1} | {a} | {r} | {f} |')
    print()

for n in sys.argv[1:]:
    passbench(n)
    swprof(n)
