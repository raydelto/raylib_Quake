#!/usr/bin/env python3
"""macOS runtime checks (sibling of verify.py, which stays Linux/Xvfb only).

No Xvfb: the real window is used. Window ids come from tools/macos_windows.swift
(CGWindowList, no permission), screenshots from `screencapture -l <id>` and
synthetic input from tools/macos_input.swift. Every child is a Popen owned by
this script and is reaped before returning; nothing is killed by name.

Permissions the user may have to grant to the terminal/agent running this:
  * Screen Recording  - screencapture of another app's window (a missing grant
                        yields blank/desktop-only images; inspect the PNGs)
  * Accessibility     - key/mouse injection for play/fullscreen/quit/audio
`launch demo timedemo soak` need neither beyond the screenshot; use --headless
with a null-video binary to skip windows entirely.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import wave
import audioop

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / 'tools'
PAK_SHA = '35a9c55e5e5a284a159ad2a62e0e8def23d829561fe2f54eb402dbc0a9a946af'
FATAL = re.compile(r'^Error:|Host_Error|Sys_Error|couldn.t open|couldn.t load|segmentation fault|'
                   r'AddressSanitizer|runtime error:|Assertion failed', re.I | re.M)
METRIC = re.compile(r'(\d+) frames\s+([\d.]+) seconds\s+([\d.]+) fps')
BASELINE = {'demo1': 969, 'demo2': 985, 'demo3': 1090}
# macOS virtual key codes (ANSI)
KEY = {'up': 126, 'down': 125, 'left': 123, 'right': 124, 'comma': 43, 'period': 47, 'ctrl': 59,
       'space': 49, 'enter': 36, 'q': 12, 'f5': 96, 'f6': 97, 'f7': 98, 'f8': 100}
ORIGIN = re.compile(r"^origin\s+'([-\d. ]+)'", re.M)
ANGLES = re.compile(r"^v_angle\s+'([-\d. ]+)'", re.M)
MODES = ['launch', 'demo', 'timedemo', 'soak', 'play', 'audio', 'fullscreen', 'quit', 'frames']


def stop(child):
    if child is not None and child.poll() is None:
        child.terminate()
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait(timeout=5)


def interrupted(signum, frame):
    raise RuntimeError('interrupted by signal %d' % signum)


def build_tool(name, outdir):
    """Compile tools/<name>.swift once into outdir and return the path."""
    out = outdir / name
    if not out.exists():
        src = TOOLS / (name + '.swift')
        subprocess.run(['swiftc', '-O', str(src), '-o', str(out)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)
    return out


class Session:
    def __init__(self, args, output):
        self.args = args
        self.output = output
        self.bin = output / 'bin'
        self.bin.mkdir()
        self.windows = None
        self.inp = None
        self.engine = None
        self.console = None
        self.result = {}

    def tools(self, need_input):
        if not self.args.headless:
            self.windows = build_tool('macos_windows', self.bin)
        if need_input:
            self.inp = build_tool('macos_input', self.bin)
            check = subprocess.run([str(self.inp), 'check'], capture_output=True, text=True)
            if check.returncode != 0:
                raise RuntimeError('Accessibility permission missing: grant it to the terminal/agent in '
                                   'System Settings > Privacy & Security > Accessibility')

    def basedir(self, name):
        base = self.output / (name + '-data')
        (base / 'id1').mkdir(parents=True)
        (base / 'id1' / 'pak0.pak').symlink_to(self.args.pak)
        return base

    def start(self, label, base, extra, sound=False):
        command = [str(self.args.binary), *self.args.args, '-basedir', str(base), '-condebug', '-nocdaudio']
        if not sound:
            command.append('-nosound')
        command += ['+developer', '1', '+echo', 'QA_VERIFY_READY', *extra]
        (self.output / (label + '.command.json')).write_text(json.dumps(command, indent=2) + '\n')
        self.console = base / 'id1' / 'qconsole.log'
        self.console.unlink(missing_ok=True)
        self.out = (self.output / (label + '.stdout.log')).open('wb')
        self.err = (self.output / (label + '.stderr.log')).open('wb')
        self.engine = subprocess.Popen(command, cwd=base, stdout=self.out, stderr=self.err)
        self.result.setdefault('engine_pids', []).append(self.engine.pid)
        self.t0 = time.monotonic()
        return self.engine

    def text(self):
        parts = []
        parts.append(Path(self.out.name).read_text(errors='replace'))
        parts.append(Path(self.err.name).read_text(errors='replace'))
        if self.console.exists():
            parts.append(self.console.read_text(errors='replace'))
        return '\n'.join(parts)

    def check_alive(self, label):
        if self.engine.poll() is not None:
            raise RuntimeError('%s: premature engine exit %s' % (label, self.engine.returncode))
        if FATAL.search(self.text()):
            raise RuntimeError('%s: engine error in logs' % label)

    def wait_ready(self, label):
        while True:
            self.check_alive(label)
            if 'Quake Initialized' in self.text() or 'QA_VERIFY_READY' in self.text():
                return time.monotonic()
            if time.monotonic() - self.t0 > self.args.timeout:
                raise RuntimeError('%s: readiness timeout' % label)
            time.sleep(0.1)

    def window(self, label, wait=True):
        """Largest on-screen window owned by the engine PID."""
        if self.args.headless:
            return None
        deadline = time.monotonic() + (self.args.timeout if wait else 0)
        while True:
            out = subprocess.run([str(self.windows), str(self.engine.pid)], capture_output=True, text=True,
                                 timeout=10).stdout
            wins = [w for w in json.loads(out or '[]') if w['onscreen'] and w['bounds']['Width'] > 100]
            if wins:
                return max(wins, key=lambda w: w['bounds']['Width'] * w['bounds']['Height'])
            if time.monotonic() > deadline:
                raise RuntimeError('%s: no visible window owned by pid %d' % (label, self.engine.pid))
            self.check_alive(label)
            time.sleep(0.2)

    def shot(self, label, name):
        if self.args.headless:
            return None
        win = self.window(label)
        path = self.output / (label + '-' + name + '.png')
        subprocess.run(['screencapture', '-x', '-o', '-l', str(win['id']), str(path)], check=True, timeout=15)
        return path

    def key(self, name, ms=150, mod=None):
        cmd = [str(self.inp), 'key', str(self.engine.pid), str(KEY[name]), str(ms)]
        if mod:
            cmd.append(mod)
        subprocess.run(cmd, check=True, timeout=30)

    def mouse(self, dx, dy, steps):
        subprocess.run([str(self.inp), 'mouse', str(self.engine.pid), str(dx), str(dy), str(steps)],
                       check=True, timeout=30)

    def focus(self):
        subprocess.run([str(self.inp), 'activate', str(self.engine.pid)], check=True, timeout=10)
        time.sleep(0.8)

    def player(self):
        """Press F5 (bound to `edict 1`) and parse the newest origin/v_angle from the console log."""
        before = len(ORIGIN.findall(self.console.read_text(errors='replace'))) if self.console.exists() else 0
        self.key('f5', 150)
        for _ in range(30):
            time.sleep(0.1)
            log = self.console.read_text(errors='replace')
            found = ORIGIN.findall(log)
            ang = ANGLES.findall(log)
            if len(found) > before and len(ang) >= len(found):
                return [float(x) for x in found[-1].split()], [float(x) for x in ang[-1].split()] if ang else None
        raise RuntimeError('no edict output; F5 binding or Accessibility injection failed')

    def finish(self, label):
        if self.console is not None and self.console.exists():
            shutil.copyfile(self.console, self.output / (label + '.console.log'))
        stop(self.engine)
        self.engine = None
        self.out.close()
        self.err.close()


def run_basic(s, args):
    checks = ['demo1', 'demo2', 'demo3'] if args.mode == 'timedemo' else [args.mode]
    s.tools(False)
    for check in checks:
        label = check if args.mode == 'timedemo' else args.mode
        base = s.basedir(label)
        extra = {'timedemo': ['+timedemo', check], 'launch': ['+toggleconsole']}.get(args.mode, [])
        s.start(label, base, extra)
        ready = None
        window = None
        expected = ['demo1', 'demo2', 'demo3', 'demo1']
        while True:
            s.check_alive(label)
            text = s.text()
            now = time.monotonic()
            if ready is None and ('Quake Initialized' in text or 'QA_VERIFY_READY' in text):
                ready = now
            if window is None and not args.headless:
                try:
                    window = s.window(label, wait=False)
                except RuntimeError:
                    window = None
            if args.mode == 'demo':
                demos = re.findall(r'Playing demo from (?:.*[/\\])?(demo[123])\.dem\.',
                                   s.console.read_text(errors='replace') if s.console.exists() else text)
                progress = 0
                for d in demos:
                    if d == expected[progress]:
                        progress += 1
                        if progress == len(expected):
                            break
                complete = progress == len(expected)
                deadline = args.timeout * max(1, progress)
            elif args.mode == 'timedemo':
                m = METRIC.search(text)
                complete = m is not None
                deadline = args.timeout
                if complete:
                    frames, seconds, fps = map(float, m.groups())
                    if min(frames, seconds, fps) <= 0:
                        raise RuntimeError('invalid timedemo metrics')
                    s.result.setdefault('timedemos', {})[check] = {'frames': int(frames), 'seconds': seconds, 'fps': fps,
                                                                    'baseline': BASELINE[check],
                                                                    'match': int(frames) == BASELINE[check]}
            else:
                duration = 60 if args.mode == 'soak' else args.settle
                complete = ready is not None and now - ready >= duration
                deadline = args.timeout if ready is None else now - s.t0 + 1
            if complete and (args.headless or window is not None):
                if args.mode == 'timedemo':
                    print('%s: %s' % (check, METRIC.search(text).group(0)), flush=True)
                if not args.headless:
                    s.shot(label, 'final')
                s.check_alive(label)
                s.result.setdefault('elapsed_seconds', {})[label] = round(now - s.t0, 3)
                print('PASS: %s%s' % (label, ' (headless; screenshot SKIP)' if args.headless else ' (window captured)'),
                      flush=True)
                break
            if now - s.t0 > deadline:
                raise RuntimeError('%s: timeout (readiness/window/completion missing)' % label)
            time.sleep(0.1)
        s.finish(label)
    for check, r in s.result.get('timedemos', {}).items():
        if not r['match']:
            raise RuntimeError('%s: %d frames, expected baseline %d' % (check, r['frames'], r['baseline']))


def door_state(s):
    s.key('f6', 150)
    time.sleep(0.4)
    log = s.console.read_text(errors='replace')
    return re.findall(r"^absmin\s+'([-\d. ]+)'", log, re.M)[-1]


def run_play(s, args):
    """E1M1: walk forward/back/strafe left, turn left/right, mouse look, fire; positions from `edict 1`."""
    s.tools(True)
    base = s.basedir('play')
    (base / 'id1' / 'play.cfg').write_text('bind F5 "edict 1"\nbind F6 "edict 8"\nbind F7 centerview\n+mlook\n')  # +mlook: mouse Y pitches (stock Quake moves)
    s.start('play', base, ['+exec', 'play.cfg', '+map', 'e1m1'])
    s.wait_ready('play')
    time.sleep(3)
    s.focus()
    steps = []
    notes = []

    def step(name, action, expect):
        p0, a0 = s.player()
        action()
        time.sleep(0.4)
        p1, a1 = s.player()
        s.shot('play', name)
        d = [round(b - a, 1) for a, b in zip(p0, p1)]
        da = None if not (a0 and a1) else [round(((b - a + 180) % 360) - 180, 1) for a, b in zip(a0, a1)]
        ok, why = expect(d, da, p0, a0)
        steps.append({'step': name, 'origin_before': p0, 'origin_after': p1, 'delta': d, 'angle_delta': da,
                      'ok': ok, 'why': why})
        print('%s: %s delta=%s dangle=%s' % ('PASS' if ok else 'FAIL', name, d, da), flush=True)

    def moved(d):
        return math.hypot(d[0], d[1])

    def local(d, a0):
        yaw = math.radians(a0[1])
        fwd = d[0] * math.cos(yaw) + d[1] * math.sin(yaw)
        side = d[0] * math.sin(yaw) - d[1] * math.cos(yaw)  # +side = right of view
        return fwd, side

    step('forward', lambda: s.key('up', 700),
         lambda d, da, p, a: (local(d, a)[0] > 20, 'forward component %.1f' % local(d, a)[0]))
    step('back', lambda: s.key('down', 700),
         lambda d, da, p, a: (local(d, a)[0] < -20, 'forward component %.1f' % local(d, a)[0]))
    step('strafe_left', lambda: s.key('comma', 700),
         lambda d, da, p, a: (local(d, a)[1] < -20, 'side component %.1f (negative = left)' % local(d, a)[1]))
    step('strafe_right', lambda: s.key('period', 700),
         lambda d, da, p, a: (local(d, a)[1] > 20, 'side component %.1f' % local(d, a)[1]))
    step('turn_left_key', lambda: s.key('left', 500),
         lambda d, da, p, a: (da is not None and da[1] > 5, 'yaw delta %s (positive = left)' % (da and da[1])))
    step('turn_right_key', lambda: s.key('right', 500),
         lambda d, da, p, a: (da is not None and da[1] < -5, 'yaw delta %s' % (da and da[1])))
    step('mouse_look_x', lambda: s.mouse(-40, 0, 12),
         lambda d, da, p, a: (da is not None and abs(da[1]) > 2, 'yaw delta %s' % (da and da[1])))
    step('mouse_look_y', lambda: s.mouse(0, 30, 12),
         lambda d, da, p, a: (da is not None and abs(da[0]) > 2, 'pitch delta %s' % (da and da[0])))
    s.key('f7', 150)  # bound to centerview
    time.sleep(0.5)

    def ammo():
        s.key('f5', 150)
        time.sleep(0.5)
        return float(re.findall(r'^ammo_shells\s+([-\d.]+)', s.console.read_text(errors='replace'), re.M)[-1])

    before = ammo()
    s.key('ctrl', 300, 'ctrl')  # Ctrl is a modifier on macOS: delivered as flagsChanged
    time.sleep(1.2)
    s.shot('play', 'fire')
    after = ammo()
    steps.append({'step': 'fire', 'ok': after < before, 'why': 'ammo_shells %s -> %s' % (before, after)})
    print('%s: fire ammo_shells %s -> %s' % ('PASS' if after < before else 'FAIL', before, after), flush=True)
    # Door: steer greedily toward the nearest touch door (edict 8, E1M1 double door near (232, 576)); the
    # door has opened when its absmin changes. Straight-line steering can be blocked by geometry; then the
    # result is INCONCLUSIVE (not FAIL) and the closest approach is recorded.
    door0 = door_state(s)
    best = 1e9
    opened = False
    history = []
    for i in range(60):
        p, a = s.player()
        dx, dy = 232 - p[0], 576 - p[1]
        dist = math.hypot(dx, dy)
        best = min(best, dist)
        if door_state(s) != door0:
            opened = True
            break
        err = ((math.degrees(math.atan2(dy, dx)) - a[1] + 180) % 360) - 180
        if abs(err) > 8:
            s.key('left' if err > 0 else 'right', int(min(abs(err), 90) / 130 * 1000))
        history.append(dist)
        if len(history) > 3 and history[-4] - dist < 20:  # stuck on geometry: sidestep
            s.key('comma' if i % 2 else 'period', 600)
        s.key('up', 500)
    s.shot('play', 'door')
    steps.append({'step': 'door', 'ok': True, 'result': 'OPENED' if opened else 'INCONCLUSIVE',
                  'closest_approach': round(best, 1)})
    print('%s: door %s (closest approach %.1f units)' % ('PASS' if opened else 'INCONCLUSIVE', 'opened' if opened else 'not reached', best), flush=True)
    s.check_alive('play')
    s.result['steps'] = steps
    s.finish('play')
    bad = [x['step'] for x in steps if not x['ok']]
    if bad:
        raise RuntimeError('play steps failed: %s' % bad)


def run_audio(s, args):
    s.tools(True)
    rates = [int(x) for x in args.rates.split(',')]
    res = {}
    for rate in rates:
        label = 'audio%d' % rate
        base = s.basedir(label)
        wav = s.output / (label + '.wav')
        s.start(label, base, ['+map', 'e1m1', '-sndspeed', str(rate), '-sndcapture', str(wav)], sound=True)
        s.wait_ready(label)
        time.sleep(3)
        s.focus()
        s.key('ctrl', 300, 'ctrl')
        s.key('up', 1500)
        s.key('ctrl', 300, 'ctrl')
        time.sleep(2)
        s.check_alive(label)
        s.key('q', 200, 'cmd')  # clean quit so the capture header is finalised
        try:
            s.engine.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
        s.finish(label)
        raw = wav.read_bytes()
        hdr_rate = int.from_bytes(raw[24:28], 'little')
        pcm = raw[44:]  # analyse the payload regardless of the header's data size
        frames = len(pcm) // 4
        rms = audioop.rms(pcm[:frames * 4], 2) if frames else 0
        peak = audioop.max(pcm[:frames * 4], 2) if frames else 0
        rate_out = hdr_rate
        init = re.search(r'raylib audio driver initialized \((\d+) Hz', Path(base / 'id1/qconsole.log').read_text(errors='replace'))
        res[rate] = {'wav_rate': rate_out, 'seconds': round(frames / max(rate_out, 1), 2), 'rms': rms, 'peak': peak,
                     'driver_hz': int(init.group(1)) if init else None, 'non_silent': rms > 5}
        print('%s: %s' % ('PASS' if res[rate]['non_silent'] and rate_out == rate else 'FAIL', label), res[rate], flush=True)
    s.result['audio'] = res
    s.result['listened'] = 'no: capture analysis only; nobody physically listened'
    if not all(r['non_silent'] and r['wav_rate'] == k for k, r in res.items()):
        raise RuntimeError('audio capture silent or wrong rate')


def run_fullscreen(s, args):
    s.tools(True)
    base = s.basedir('fullscreen')
    s.start('fullscreen', base, ['+map', 'e1m1'])
    s.wait_ready('fullscreen')
    time.sleep(3)
    s.focus()
    w0 = s.window('fullscreen')['bounds']
    s.shot('fullscreen', 'windowed')
    s.key('enter', 200, 'alt')
    time.sleep(2.5)
    w1 = s.window('fullscreen')['bounds']
    s.shot('fullscreen', 'full')
    s.check_alive('fullscreen')
    s.key('enter', 200, 'alt')
    time.sleep(2.5)
    w2 = s.window('fullscreen')['bounds']
    s.shot('fullscreen', 'back')
    s.check_alive('fullscreen')
    s.result['bounds'] = {'windowed': w0, 'fullscreen': w1, 'restored': w2}
    ok = w1['Width'] > w0['Width'] and w2['Width'] == w0['Width'] and w2['Height'] == w0['Height']
    s.finish('fullscreen')
    print('%s: fullscreen toggle %s -> %s -> %s' % ('PASS' if ok else 'FAIL', w0, w1, w2), flush=True)
    if not ok:
        raise RuntimeError('fullscreen bounds did not change and restore')


def run_quit(s, args):
    s.tools(True)
    base = s.basedir('quit')
    s.start('quit', base, ['+map', 'e1m1'])
    s.wait_ready('quit')
    time.sleep(3)
    s.focus()
    s.key('q', 200, 'cmd')
    try:
        code = s.engine.wait(timeout=10)
    except subprocess.TimeoutExpired:
        raise RuntimeError('Cmd+Q did not terminate the engine within 10 s')
    s.result['exit_code'] = code
    print('%s: Cmd+Q exit code %s' % ('PASS' if code == 0 else 'FAIL', code), flush=True)
    s.finish('quit')
    if code != 0:
        raise RuntimeError('Cmd+Q exit code %s' % code)


def run_frames(s, args):
    """Deterministic frame hashes: fixed host_framerate demo playback + `screenshot` (PCX of the 8-bit
    frame buffer, works with the null driver). Compare hashes across arm64 / x86_64 / Linux builds."""
    s.tools(False)
    points = [int(x) for x in args.frames.split(',')]
    hashes = {}
    for demo in ('demo1', 'demo2', 'demo3'):
        base = s.basedir(demo)
        lines = ['host_framerate 0.02', 'con_notifytime 0', 'playdemo %s' % demo, 'wait']
        done = 0
        for p in points:
            lines += ['wait'] * (p - done) + ['screenshot']
            done = p
        (base / 'id1' / 'frames.cfg').write_text('\n'.join(lines) + '\n')
        s.start(demo + '-frames', base, ['+exec', 'frames.cfg'])
        deadline = time.monotonic() + args.timeout
        while len(list((base / 'id1').glob('quake*.pcx'))) < len(points):
            s.check_alive(demo)
            if time.monotonic() > deadline:
                raise RuntimeError('%s: screenshots not produced' % demo)
            time.sleep(0.2)
        time.sleep(0.5)
        s.finish(demo + '-frames')
        for i, f in enumerate(sorted((base / 'id1').glob('quake*.pcx'))):
            h = hashlib.sha256(f.read_bytes()).hexdigest()
            hashes['%s@%d' % (demo, points[i])] = h
            shutil.copyfile(f, s.output / ('%s-%d.pcx' % (demo, points[i])))
    s.result['frame_hashes'] = hashes
    print(json.dumps(hashes, indent=1), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('mode', choices=MODES)
    parser.add_argument('--headless', action='store_true', help='null-video binary: skip window/screenshot checks')
    parser.add_argument('--data', type=Path, default=ROOT / 'data')
    parser.add_argument('--output', type=Path, help='new directory for evidence')
    parser.add_argument('--timeout', type=float, default=300)
    parser.add_argument('--settle', type=float, default=3)
    parser.add_argument('--allow-ubsan', action='store_true', help='sanitizer builds: UBSan reports are collected, not fatal')
    parser.add_argument('--rates', default='11025,22050,44100', help='audio mode sample rates')
    parser.add_argument('--frames', default='100,300,600,900', help='frames mode: frame numbers to hash')
    parser.add_argument('binary')
    parser.add_argument('args', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if platform.system() != 'Darwin':
        parser.error('macOS only; use tools/verify.py on Linux')
    if not math.isfinite(args.timeout) or args.timeout <= 0 or args.settle < 0:
        parser.error('timeout must be positive and settle nonnegative')
    global FATAL
    if args.allow_ubsan:
        FATAL = re.compile(FATAL.pattern.replace('|runtime error:', ''), FATAL.flags)
    args.binary = Path(args.binary).resolve()
    if not args.binary.is_file() or not os.access(args.binary, os.X_OK):
        parser.error('not executable: %s' % args.binary)
    args.pak = args.data.resolve() / 'id1/pak0.pak'
    if not args.pak.is_file() or hashlib.sha256(args.pak.read_bytes()).hexdigest() != PAK_SHA:
        parser.error('missing or invalid shareware pak0.pak; run tools/fetch_shareware.sh')
    if args.output:
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=False)
    else:
        (ROOT / 'verification-results').mkdir(exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix='mac-' + args.mode + '-', dir=ROOT / 'verification-results'))
    print('Evidence: %s' % output, flush=True)
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, interrupted)
    s = Session(args, output)
    s.result.update(mode=args.mode, headless=args.headless, status='FAIL', machine=platform.machine(),
                    macos=platform.mac_ver()[0], binary_arch=subprocess.run(['file', '-b', str(args.binary)],
                    capture_output=True, text=True).stdout.strip())
    try:
        {'launch': run_basic, 'demo': run_basic, 'timedemo': run_basic, 'soak': run_basic,
         'play': run_play, 'audio': run_audio, 'fullscreen': run_fullscreen, 'quit': run_quit,
         'frames': run_frames}[args.mode](s, args)
        s.result['status'] = 'PASS'
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError) as error:
        s.result['error'] = str(error)
        print('FAIL: %s' % error, file=sys.stderr, flush=True)
    finally:
        stop(s.engine)
        (output / 'result.json').write_text(json.dumps(s.result, indent=2) + '\n')
    return 0 if s.result['status'] == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
