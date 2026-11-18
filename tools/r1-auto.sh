#!/bin/bash
# r1: one automated run of the home screen + Camera scenario, and its numbers.
# usage: tools/r1-auto.sh NAME ["extra boot-args"] [SECONDS]      e.g. tools/r1-auto.sh A ""  /  tools/r1-auto.sh C "fbscale=1"
# The scenario is userland/rc.jobs' rwtouch lines, which run only with autotest=1 (added here), so the normal build is the
# test build: build it as usual first. Leaves logs/NAME.log, logs/NAME-shotN.png (fbshot pictures) and prints one row per
# phase: medians of the 10 s lines between the "auto: X" markers. Only the log's last ~190 KB survive (pstore), so the
# phases are ordered so the ones measured are at the end; "n" says how many 10 s lines a median had.
#   summary of an existing log only: tools/r1-auto.sh NAME --summary
cd "$(dirname "$0")/.." || exit 1
NAME=$1
BASE="connsys_soc=6765 connsys_xo_cw00=0x4e1d connsys_step=17 wifi=1 mtkaudio=2 r1cam=4 pvr=6 pvr_gl=1 fbaspect=1 glwait=3 glthreads=1 pvr_stats=1 cam_fps=6"
# small, rare pictures (22 KB each): one of 120 px every 10 s would be 8 KB/s of log, more than the whole run's tail
SHOTS=${SHOTS-"fbshot=60 fbshot_every=60 fbshot_w=60"}   # SHOTS= for none (more of the log survives)
if [ "$2" != --summary ]; then
	# NOWIFI=1: no Wi-Fi at all (the cellular gate wants it off): CONNSYS is not brought up, wifi=1 is left out
	[ -n "$NOWIFI" ] && BASE=${BASE/connsys_soc=6765 connsys_xo_cw00=0x4e1d connsys_step=17 wifi=1 /}
	# the camera reads the FIRST cam_fps=: one in the extra args replaces the base's
	case "$2" in *cam_fps=*) BASE=${BASE/ cam_fps=6/};; esac
	case "$2" in *soak=1*|*idle=1*|*settings=1*|*bltest=1*|*" flip=1"*|*nosw=1*|*sw1=1*|*swwait=1*|*swhome=1*|*swcam=1*|*swloop=1*|*swprof=1*|*passbench=*) SCEN=;; *) SCEN=autotest=1;; esac   # soak=1: the soak scenario (rc.jobs) instead of the autotest one
	tools/r1-run.sh "$NAME" "${3:-300}" "$BASE $SHOTS $SCEN $2" > "logs/$NAME.out" 2>&1
	head -3 "logs/$NAME.out"
	adb -s "${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the adb serial}" get-state 2>&1 | grep -q device || { echo "r1-auto: THE PHONE IS NOT BACK IN ANDROID: stop"; exit 1; }
fi
rm -f logs/"$NAME"-shot*.png
for n in $(grep -ao 'fbshot [0-9]*: end' "logs/$NAME.log" | cut -d' ' -f2 | tr -d :); do
	python3 tools/fbshot.py "logs/$NAME.log" "logs/$NAME-shot$n.png" "$n" >/dev/null 2>&1
done
python3 - "logs/$NAME.log" <<'EOF'
import re, sys, statistics as st
lines = open(sys.argv[1], 'rb').read().decode('latin-1').split('\n')
t, rows, marks = None, [], {}
for l in lines:
    m = re.match(r'\[ *(\d+\.\d+)\]: (.*)', l)
    if m:
        t, l = float(m.group(1)), m.group(2)
    if t is None:
        continue
    rows.append((t, l))
    m = re.match(r'!auto: (\S+)', l)
    if m and m.group(1) not in marks:
        marks[m.group(1)] = t
first = rows[0][0] if rows else 0
print(f"{sys.argv[1]}: log tail from {first:.0f} s; markers: " + ' '.join(f'{k}@{v:.0f}' for k, v in marks.items()))
phases = [('camera', 'camera', 20, 'home'), ('home', 'home', 5, 'end')]   # (row, from marker, + seconds, to marker)
med = lambda v: f'{st.median(v):.1f}' if v else '-'
print(f"{'phase':7}{'n':>3} {'swap/s':>7} {'GLfr/10s':>8} {'ms/fr':>6} {'flush':>6} {'fragus':>7} {'idle%':>6} {'kern%':>6} {'ISPms':>6} {'msd%':>5} {'pic/10s':>7} {'fr/pic':>6} {'busy%':>6}  top programs (% of a core)")
for name, a, skip, b in phases:
    if a not in marks and 'shot1' in marks and a == 'camera':
        marks[a] = marks['shot1'] - 45            # (its marker fell out of the log's kept tail)
    if a not in marks:
        continue
    t0, t1 = max(marks[a] + skip, first), marks.get(b, 1e9)   # skip: the step's own start-up
    w = [l for tt, l in rows if t0 <= tt < t1]
    num = lambda pat, g=1: [float(m.group(g)) for l in w for m in [re.search(pat, l)] if m]
    swaps = [float(m.group(1)) * 1000 / float(m.group(2)) for l in w for m in [re.match(r'MiniIOMobileFramebuffer: (\d+) frames in (\d+) ms', l)] if m]
    glf = [float(m.group(1)) * 10 / float(m.group(2)) for l in w for m in [re.match(r'!redwood-gl: pvr: (\d+) frames in (\d+) s', l)] if m]
    glms = num(r'^!redwood-gl: pvr: \d+ frames in \d+ s, ([\d.]+) ms each')
    flush = num(r'^!redwood-gl: pvr: \d+ frames in \d+ s.* ([\d.]+) in glFlush')
    frag = num(r'pvr: stats: fragment: kick to done: \d+ us least, (\d+) middle')
    idle = num(r'^!redwood-gl: threads: \d+ cores (\d+)% idle')
    kern = num(r'^!redwood-gl: threads: \d+ cores \d+% idle (\d+)% kernel')
    isp = num(r'^!redwood: camera: 10 s: PrimaryScaler: ISP ([\d.]+) ms')
    msd = num(r"^!redwood: camera: 10 s: .*processor time (\d+)% of one core")
    pics = num(r'pictures given: PrimaryScaler (\d+)')
    ratio = lambda a, b: f'{st.median(a) / st.median(b):.2f}' if a and b and st.median(b) else '-'
    busy = f'{st.median(glf) * st.median(glms) / 100:.0f}' if glf and glms else '-'   # GL fr/10 s x ms/fr / 10000 ms = %
    progs = {}
    pl = [l for l in w if l.startswith('!redwood-gl: programs')]
    for l in pl:
        for p, v in re.findall(r'(\S+)\[\d+\] (\d+)', l.split(':', 2)[-1]):
            progs[p] = progs.get(p, 0) + int(v) / len(pl)
    top = ' '.join(f'{p} {v:.0f}' for p, v in sorted(progs.items(), key=lambda kv: -kv[1])[:4])
    print(f"{name:7}{len(glf):>3} {med(swaps):>7} {med(glf):>8} {med(glms):>6} {med(flush):>6} {med(frag):>7} {med(idle):>6} {med(kern):>6} {med(isp):>6} {med(msd):>5} {med(pics):>7} {ratio(glf, pics):>6} {busy:>6}  {top}")
# swipe: home-screen page swipes (touch_lat=1). Whole-text findall: lines are interleaved and tl lines are kept twice (set() dedupes).
text = '\n'.join(lines)
P = sorted(set((int(b), int(a)) for a, b in re.findall(r'tl: p (\d) \d+ \d+ \d+ at (\d+)', text)))
S = sorted(set(int(b) for a, b in re.findall(r'tl: s (\d+) at (\d+)', text)))
G = sorted(set((int(a), int(b)) for a, b, c in re.findall(r'tl: g \d+ began (\d+) ended (\d+) (\d+)', text)), key=lambda x: x[1])
gest, down = [], None
for tm, ty in P:
    if ty == 1:
        down = tm
    elif ty == 3 and down is not None:
        if tm - down >= 100000:                      # (shorter = a tap, not a swipe)
            gest.append((down, tm + 800000))
        down = None
rows_s = []
for a, b in gest:
    sw = [x for x in S if a <= x <= b]
    if len(sw) < 3:
        continue
    iv = [(y - x) / 1000 for x, y in zip(sw, sw[1:])]
    ch = [g for g in G if a <= g[1] <= b]
    rows_s.append(((len(sw) - 1) / ((sw[-1] - sw[0]) / 1e6), st.median(iv), sorted(iv)[min(len(iv) - 1, int(len(iv) * .95))],
                   len(ch) / len(sw), sum(g[1] - g[0] for g in ch) / 1000 / len(sw)))
if rows_s:
    m = [st.median(c) for c in zip(*rows_s)]
    print(f"swipe  {len(rows_s)} gestures: {m[0]:.1f} fps, interval median {m[1]:.1f} p95 {m[2]:.1f} ms, {m[3]:.1f} chunks/swap, {m[4]:.1f} ms render thread/swap")
else:
    print("swipe  -  (needs touch_lat=1 and a swipe with swaps in the log)")
seen = set()
for tt, l in rows:
    if re.search(r'still: tap \d+.*WRITTEN in|camera app: tap \d+: (THE PHOTO|the library REFUSED)', l) and l[:150] not in seen:
        seen.add(l[:150])
        print(f'  {tt:7.1f} {l[:150]}')
for tt, l in rows:
    if re.search(r'Corpse allowed|panic\(|SIGTRAP|CRASH signal|camera launched', l):
        print(f'  {tt:7.1f} {l[:160]}')
EOF
ls logs/"$NAME"-shot*.png 2>/dev/null | tr '\n' ' '; echo
