# Runtime verification

Run from the repository root on Linux x86_64. The harness accepts an executable
path and preserves each trailing argument as a separate argument; it does not
build the engine. Use a direct executable, not a daemonizing launcher. All child
work runs in the foreground and is reaped before the harness returns.

```sh
tools/fetch_shareware.sh
tools/verify.sh launch /absolute/path/to/quake -width 640 -height 480
tools/verify.sh demo /absolute/path/to/quake
tools/verify.sh timedemo /absolute/path/to/quake
tools/verify.sh soak /absolute/path/to/quake
# Null-video build: explicitly acknowledge that no screenshot is possible.
tools/verify.sh launch --headless /absolute/path/to/headless-quake
```

Harness options precede the binary. `--data DIR` selects a directory containing
`id1/pak0.pak`; `--output DIR` must name a new directory; `--timeout SECONDS`
defaults to 300 per demo/timedemo or startup; `--settle SECONDS` defaults to 3
for launch. Soak always observes **60 seconds after initialization**. Outputs
default to unique directories in gitignored `verification-results/`.

## Data provenance

The fetcher downloads only [Quake 1.06 shareware](https://www.gamers.org/pub/idgames/idstuff/quake/quake106.zip).
It checks the archive before extracting `resource.1`, then reads only
`ID1/PAK0.PAK` from its self-extracting LZH with 7-Zip. It never executes DOS
programs or extracts registered content. Hashes are pinned against
[Debian game-data-packager's Quake manifest](https://alioth-lists-archive.debian.net/pipermail/pkg-games-commits/2015-January/023332.html)
and were reproduced by the fetch dry run:

| File | SHA256 |
|---|---|
| `quake106.zip` | `ec6c9d34b1ae0252ac0066045b6611a7919c2a0d78a3a66d9387a8f597553239` |
| Shareware `id1/pak0.pak` (18,689,235 bytes) | `35a9c55e5e5a284a159ad2a62e0e8def23d829561fe2f54eb402dbc0a9a946af` |

An existing valid PAK is reused; a mismatched one fails without overwriting it.
Downloads have connection/transfer timeouts. `data/`, results, and all `*.pak`
and `*.PAK` files are ignored. Never commit or attach PAKs or registered data.

## Check contract

Each invocation allocates its own Xvfb display with `-displayfd`, disables TCP,
and starts the binary with `-nosound -nocdaudio`, SDL dummy audio, OpenAL null,
and a private ALSA null configuration. These are non-audio tests; they do not
verify audible output or mixer pacing. The harness terminates only its own
engine and Xvfb `Popen` children, with a bounded TERM/KILL fallback, including
on interruption. It never kills by process name or signals Multica.

Each run creates a private basedir and symlinks the checksum-verified shareware
PAK, avoiding personal configs. It preserves the stock `quake.rc` attract loop:
PAK files take precedence over loose files in the same directory, so writing a
loose `quake.rc` cannot override it. Injecting a second `+startdemos` while
demo1 is playing disables the loop (`demonum = -1`). Demo and soak therefore
inject no playback command. Engine args are forwarded first; the
harness owns `-basedir`, `-condebug`, audio flags and test commands. Mods and
registered data are outside this harness's scope.

| Mode | Required evidence |
|---|---|
| `launch` | `Quake Initialized` or harness `QA_VERIFY_READY` echo marker, live process for the settling interval, visible window owned by that PID, window-only PNG showing the console (`+toggleconsole`). Inspect the PNG to confirm recognizable output. |
| `demo` | Stock `quake.rc` playback; ordered log transitions demo1 → demo2 → demo3 → demo1 show a complete loop; live process and window-only PNG (explicit skip with `--headless`). |
| `timedemo` | Separate processes for demo1, demo2, demo3; positive frames/seconds/fps parsed from Quake's report and printed, live process, window-only PNG per demo. |
| `soak` | Initialization followed by 60 s of process liveness, stderr collection, and a window-only PNG. It preserves the shareware demo loop. |

All modes fail on premature exit, timeout, missing window, matched engine fatal
errors, or capture failure. `--headless` skips the window/PNG requirement and
prints that skip explicitly; it still requires initialization for launch/soak
and demo/metric evidence for demo/timedemo. A live but hung process can pass soak;
review demo output and screenshots separately. Demo transitions are observable
completion evidence, not a proof of gameplay synchronization or correctness.
The readiness echo is executed through `stuffcmds` and captured by `-condebug`,
so buffered platform stdout cannot hide startup readiness.

No synthetic keys are used: test commands come through Quake's command line.
If adding input checks, hold every synthetic key down at least 100 ms before
key-up. Do not use xdotool's default short taps.

Evidence includes stdout, stderr, Xvfb logs, command argv JSON, console logs,
PNGs and `result.json` (PASS/FAIL, child PIDs, metrics/error). Exit 0 means the
selected automated contract passed; nonzero means failure. Attach logs and PNGs
to the issue; omit the private `data/` symlink tree to avoid distributing assets.

## Tools on raydelto-PC

Observed 2026-10-14 UTC with `dpkg-query -W` and version commands:

| Tool | Version | Use |
|---|---|---|
| Xvfb / xvfb-run | `2:21.1.12-1ubuntu1.8` | Virtual display (harness owns Xvfb directly) |
| xdotool | `1:3.20160805.1-5build1` | PID-owned window discovery |
| ImageMagick convert/import | `8:6.9.12.98+dfsg1-5.2build2` | Window-only PNG capture via import |
| scrot | `1.10-1build2` | Available alternate manual capture tool |
| ffmpeg | `7:6.1.1-3ubuntu5` | Available for later recordings; not required by this harness |
| Python | `3.12.3` | Standard-library process supervisor |
| GCC | `13.3.0` | Stand-in fixture only; no engine build performed |
| 7-Zip | `23.01` | LZH extraction |

Also required: bash, curl, unzip, sha256sum. Fixture validation requires X11
development headers/library. If missing, ask the owner to install:

```sh
sudo apt-get update && sudo apt-get install -y xvfb xdotool imagemagick scrot ffmpeg curl unzip p7zip-full python3 libx11-dev gcc
```

## QA tooling dry run and engine baselines

`tools/tests/standin.c` creates a clearly labelled X11 window and scripted
Quake-shaped log messages. It validates harness lifecycle/capture and failure
paths, **not** the renderer, demos or engine performance. Build it outside the
tracked source tree:

```sh
mkdir -p verification-results
gcc -Wall -Wextra -Werror tools/tests/standin.c -lX11 -o verification-results/standin
tools/verify.sh launch --timeout 10 verification-results/standin
tools/verify.sh demo --timeout 10 verification-results/standin
tools/verify.sh timedemo --timeout 10 verification-results/standin
tools/verify.sh soak --timeout 10 verification-results/standin
tools/verify.sh launch --timeout 2 verification-results/standin --fixture-exit
tools/verify.sh launch --timeout 2 verification-results/standin --fixture-no-ready
tools/verify.sh launch --timeout 2 verification-results/standin --fixture-no-window
tools/verify.sh launch --timeout 2 verification-results/standin --fixture-error
```

The first four should return 0; the last four must return nonzero. The fixture
simulates the stock attract loop and rejects an injected `+startdemos`, guarding
against the duplicate-command regression. Fixture
timedemos emit **100 frames / 1.0 seconds / 100.0 fps**, intentionally synthetic.
Check that the recorded child PIDs are absent after every run.

| Engine baseline | demo1 | demo2 | demo3 |
|---|---|---|---|
| Frames | 969 | 985 | 1090 |
| Reported seconds | 0.3 | 0.3 | 0.4 |
| Reported FPS | 2997.7 | 3133.6 | 2808.2 |

Measured 2026-10-14 UTC against PR #2 head
`7599117637ec023643695122ef1512fb2744c668`, built in a separate checkout with:

```sh
cmake -S . -B build -G Ninja && cmake --build build
tools/verify.sh timedemo --headless /absolute/path/to/pr2/build/quake
tools/verify.sh demo --headless /absolute/path/to/pr2/build/quake
```

Configuration: GCC 13.3.0, CMake 4.3.5, Ninja 1.11.1, default RelWithDebInfo,
Linux x86_64 on raydelto-PC (Intel Core i5-11400H), `QUAKE_RAYLIB=OFF`, null
video rendering into a 320×200 buffer, shareware 1.06, audio muted by the harness.
The timedemo measurements ran alongside one demo-loop check. Seconds are the
engine's rounded output; do not recompute FPS from those rounded values.
Frame counts are useful regression baselines; FPS is machine/load-dependent.
Compare equivalent configurations; no DOOM gametic count applies.

The null-driver engine supports real launch/demo/timedemo/soak checks but has no
game window. Headless runs explicitly skip screenshots. Visible raylib output,
input, audible output and gameplay synchronization remain unverified.

### raylib build (QUAK-5)

Since QUAK-5, `build/quake` is the raylib build and runs the window checks
(no `--headless`); `build/quake-null` is the null-driver build for headless
runs. With the raylib build at 640×480 under the harness's Xvfb (Mesa
llvmpipe, so no real vsync), all four modes pass with window captures and the
timedemo frame counts are unchanged: demo1 969, demo2 985, demo3 1090
(about 520–590 FPS there, against about 2800–3000 for `quake-null`, which
neither converts nor presents frames). FPS under Xvfb says nothing about a
real display. Audible output is still null until QUAK-6.

## macOS harness (`tools/verify_macos.py`)

`verify.py` stays Linux/Xvfb only. On macOS use the sibling script; it runs the real
window (no Xvfb), uses `screencapture -l <windowid>` for window-only PNGs, and builds two
tiny Swift helpers on first use into the evidence directory (`macos_windows.swift`: window
ids by PID via CGWindowList; `macos_input.swift`: key/mouse injection). Needs only Xcode
command line tools and `tools/fetch_shareware.sh` data.

```sh
tools/verify_macos.sh launch|demo|timedemo|soak  build/quake [engine args]
tools/verify_macos.sh play        build/quake    # E1M1 move/strafe/turn/mouse-look/fire/door
tools/verify_macos.sh audio       build/quake    # --rates 11025,22050,44100, WAV capture analysis
tools/verify_macos.sh fullscreen  build/quake    # Alt+Enter and back
tools/verify_macos.sh quit        build/quake    # Cmd+Q must exit 0
tools/verify_macos.sh frames      build/quake -width 320 -height 200   # fixed-frame PCX hashes
tools/verify_macos.sh timedemo --headless build/quake-null
python3 tools/compare_frames.py EVIDENCE_A EVIDENCE_B   # per-frame differing pixels
```

Permissions to grant to the terminal/agent (System Settings > Privacy & Security): **Accessibility**
for `play audio fullscreen quit` (synthetic input; checked up front with a clear error), and
**Screen Recording** for readable window screenshots (without it `screencapture` returns blank or
wallpaper-only images: inspect the PNGs). `launch demo timedemo soak frames --headless` need neither.

Notes: Ctrl/Alt/Cmd are sent as real modifier (flagsChanged) events, since raylib polls modifier
state; `play` reads the player with an `edict 1` binding (F5) from `qconsole.log`, so movement is
asserted on origin/angles, not just pixels; `audio` analyses the driver's own `-sndcapture` WAV
(proves the mixer produced non-silent PCM at that rate, not that anyone heard it); `frames` uses
`host_framerate 0.02` + `screenshot` so the 8-bit frame is comparable across builds/arches (run-to-run
noise on one build is a few hundred pixels of particles; compare the same driver only). Run a
Rosetta build with `-noudp` if `gethostbyname` of the machine name fails there (see QUAK-21).
