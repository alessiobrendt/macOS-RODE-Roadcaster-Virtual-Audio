# RodeCasterVirtualAudio

A self-built macOS CoreAudio virtual audio driver plus a routing daemon,
written from scratch, to fully replace RØDE's official virtual audio driver
for the RodeCaster Pro 2 after it stopped working -- with the goal of
matching RØDE's own feature set: routing macOS app audio through the
RodeCaster Pro 2's physical hardware faders.

## Why this exists

The RodeCaster Pro 2's official RØDE Central / virtual-audio software driver
on this Mac stopped functioning correctly -- it plays no audio at all. Rather
than depend on RØDE to fix their driver, this project implements an
independent, original CoreAudio HAL plug-in that provides the same
capability RØDE's driver provided: 5 named virtual audio devices macOS apps
can select as input/output, plus a background daemon that copies each one
into the RodeCaster Pro 2's real hardware channels -- so audio really does
end up routed through the physical faders, not just looped back to itself.

**Key finding that makes this possible without reverse-engineering anything:**
`system_profiler SPAudioDataType` shows a device "RODECaster Pro II Main
Multitrack" with 10 channels, whose CoreAudio UID is prefixed
`AppleUSBAudioEngine:RØDE:RODECaster Pro II:...`. The `AppleUSBAudioEngine`
prefix means this 10-channel interface is exposed by **Apple's own built-in,
standard USB Audio Class 2.0 driver**, not a proprietary RØDE kernel driver.
There is no proprietary USB protocol here to reverse-engineer -- it's already
an ordinary, standard multi-channel CoreAudio playback device that any
userspace app (including ours) can open and write to via the normal
`AudioDeviceIOProc` mechanism. RØDE's official driver, when working, exposes
5 separate named virtual stereo devices ("System", "Game", "Music",
"Virtual A", "Virtual B") -- 5 x 2 = 10, lining up exactly with the
Multitrack device's 10 channels. RØDE's official setup is almost certainly:
N virtual loopback devices (like this project's) + a background daemon that
copies each one's captured audio into a fixed channel pair of that same
10-channel device. This project replicates that same two-part pattern.

This is **not** a kernel extension. On modern macOS, CoreAudio HAL plug-ins
have been a userspace mechanism for years: a `.driver` bundle implementing
`AudioServerPlugInDriverInterface` (declared in
`<CoreAudio/AudioServerPlugIn.h>`), loaded by `coreaudiod` from
`/Library/Audio/Plug-Ins/HAL/`. This is the same general mechanism used by
well-known open-source loopback drivers (e.g. BlackHole). The code here is an
original implementation of that standard, publicly documented interface --
nothing is copied from any third-party driver.

## Two-part architecture

1. **The HAL driver** (`src/RodeCasterVirtualAudio.c`) exposes 5 independent
   virtual stereo devices apps can select as input/output -- see "5-device
   architecture" below.
2. **The routing daemon** (`daemon/rodevad-router.c`), a completely separate
   ordinary background process (not part of the HAL plug-in), taps each of
   those 5 devices and copies its audio into one channel pair of the real
   RodeCaster Pro 2 "Main Multitrack" hardware device -- see "Routing
   daemon" below.

Both are needed for the full RØDE-equivalent behavior: the driver alone only
gets you loopback-to-self virtual devices (still useful on its own -- e.g.
for routing between two apps); the daemon is what actually gets that audio
onto the physical RodeCaster hardware.

## Architecture: the HAL driver

- **`src/RodeCasterVirtualAudio.c`** -- the entire driver. One plug-in
  object owns one box object, associated with **5** independent virtual
  stereo device objects (not 1 -- see "5-device architecture" below for the
  full naming/UID/object-ID scheme), each with its own input stream, output
  stream, and ring buffer:
  - PlugIn object (id 1)
    - Box object (id 2) -- describes the virtual "device box"
    - 5 Device objects (ids 10-14) -- `"RVAD System"`, `"RVAD Game"`,
      `"RVAD Music"`, `"RVAD Virtual A"`, `"RVAD Virtual B"`
      - each owns 1 input stream (ids 20-24) and 1 output stream (ids 30-34)
  - Implements the full `AudioServerPlugInDriverInterface`: `QueryInterface`
    / `AddRef` / `Release`, `Initialize`, `CreateDevice` / `DestroyDevice`
    (no-ops -- this driver exposes 5 static devices, it does not support
    dynamic device creation), all property entry points (`HasProperty`,
    `IsPropertySettable`, `GetPropertyDataSize`, `GetPropertyData`,
    `SetPropertyData`), and the full IO cycle (`StartIO`, `StopIO`,
    `GetZeroTimeStamp`, `WillDoIOOperation`, `BeginIOOperation`,
    `DoIOOperation`, `EndIOOperation`).
  - **The "virtual cable" behavior**: each device has its own ring buffer
    (`gDeviceState[i].mRingBuffer`, 65536 frames per device, power-of-two
    sized so index wrapping is a bitmask instead of a modulo).
    `DoIOOperation` writes a device's output-stream audio into that same
    device's ring buffer at the position given by the IO cycle's output
    sample time, and reads that device's input-stream audio back out at the
    position given by the input sample time. Whatever an app plays to e.g.
    "RVAD System" as output can be captured back from "RVAD System" (and
    only "RVAD System" -- the 5 devices are fully isolated from each other,
    verified by the test harness's cross-talk check) as input, just like a
    physical loopback cable per device.
  - **Channel count per device** is controlled by a single constant,
    `kNumber_Channels`, currently `2` (stereo, matching RØDE's own layout).
    **Device count** is controlled by `kNumber_VirtualDevices`, currently
    `5`. Both are written so changing either only requires updating that one
    constant plus the `kVirtualDevices[]` name/UID table -- the
    channel-layout and ASBD-building helpers (`FillStereoASBD`,
    `FillChannelLayout`) already loop over `kNumber_Channels` rather than
    hardcoding 2, and all object-ID/property logic loops over
    `kNumber_VirtualDevices` via `DeviceIndexForID()` /
    `InputStreamIndexForID()` / `OutputStreamIndexForID()` rather than
    hardcoding 5. (Bumping either also requires updating the router
    daemon's channel-mapping table to match -- see "Routing daemon".)
  - Supports both 44.1kHz and 48kHz nominal sample rates
    (`kAudioDevicePropertyNominalSampleRate` is settable per device, but the
    rate itself is shared across all 5 -- see the code comment on
    `gDevice_SampleRate` for why). Latency and safety offset are both
    reported as 0 (there's no real hardware, so no inherent extra delay).
  - Sample format: 32-bit float, linear PCM, interleaved.

## 5-device architecture

RØDE's official (broken) driver exposes 5 named virtual stereo devices:
"System", "Game", "Music", "Virtual A", "Virtual B". This driver mirrors
that same 5-device shape (needed so the router daemon's 5x stereo -> 10ch
mapping lines up with the real Multitrack hardware), but with **deliberately
different names and UIDs** so they can never be visually confused with
RØDE's own devices if both happen to be installed side by side:

| RØDE's device (broken) | This driver's device | UID |
|---|---|---|
| System | **RVAD System** | `com.abrendt.rodecastervad.system` |
| Game | **RVAD Game** | `com.abrendt.rodecastervad.game` |
| Music | **RVAD Music** | `com.abrendt.rodecastervad.music` |
| Virtual A | **RVAD Virtual A** | `com.abrendt.rodecastervad.virtuala` |
| Virtual B | **RVAD Virtual B** | `com.abrendt.rodecastervad.virtualb` |

**Naming rationale:** the `RVAD` prefix (RodeCaster Virtual Audio Device)
makes every one of our 5 devices immediately, visually distinguishable from
RØDE's identically-purposed devices in any device picker (macOS Sound
settings, Audio MIDI Setup, a DAW's audio preferences, etc.) -- you should
never be unsure which "System" device you're looking at. UIDs are scoped
under this project's own bundle ID (`com.abrendt.rodecastervad.*`) rather
than reusing anything resembling RØDE's `RodeVirtualAudioDevice_UID*`
pattern, so there is zero possibility of UID collision even if both drivers
are simultaneously installed. Names are also **deliberately plain ASCII**:
an earlier bug in `tools/testtone.c`'s fixed-width device listing was caused
by a *different* vendor's device name containing a multi-byte UTF-8
character (RØDE's own "RODECaster" contains "Ø", U+00D8, which is 2 bytes in
UTF-8 but 1 display character -- it broke C's byte-counted `%-Ns` field-width
assumption). Keeping our own 5 names ASCII-only sidesteps that whole class of
bug rather than relying on every downstream consumer (testtone, the GUI,
the router daemon's log messages) handling it correctly; `testtone.c` and
the GUI needed no code changes for the 5-device expansion as a result --
they already enumerate the system's device list generically and don't know
or care about this driver's internal object-ID scheme.

**Channel mapping** (which 2-channel slice of the 10-channel Multitrack
device each virtual device's audio is copied into by the router daemon --
see `daemon/rodevad-router.c`'s `kChannelMap[]`):

| Virtual device | Multitrack channels (1-based) |
|---|---|
| RVAD System | 1-2 |
| RVAD Game | 3-4 |
| RVAD Music | 5-6 |
| RVAD Virtual A | 7-8 |
| RVAD Virtual B | 9-10 |

**This mapping is our own guess, not confirmed against RØDE's real driver**
-- see "Known limitations" below.

## Other components

- **`Resources/Info.plist`** -- CFPlugIn bundle metadata: `CFBundlePackageType`
  = `BNDL`, bundle identifier `com.abrendt.rodecastervad`, executable name
  `RodeCasterVirtualAudio` (matches the Makefile's build output),
  `CFPlugInFactories` mapping a factory UUID to the entry-point function
  `RodeCasterVirtualAudio_Factory`, and `CFPlugInTypes` mapping Apple's
  well-known `kAudioServerPlugInTypeUUID`
  (`443ABAB8-E7B3-491A-B985-BEB9187030DB`) to that same factory UUID.

- **`Makefile`** -- compiles `src/RodeCasterVirtualAudio.c` for `arm64`,
  links `CoreFoundation`, `CoreAudio`, and `AudioToolbox`, and assembles
  `build/RodeCasterVirtualAudio.driver` (`Contents/Info.plist`,
  `Contents/MacOS/RodeCasterVirtualAudio`, `Contents/version.plist`). `make`
  (or `make all`) also runs the full verification suite (see below) and
  builds the `testtone` CLI tool.

- **`tools/testtone.c`** -- a standalone command-line utility (see
  "Testing individual outputs" below) for listing CoreAudio output devices
  and playing a test sine tone to a specific device and channel, so you can
  verify each virtual channel actually carries audio before wiring the
  driver into RodeCaster Central / FineTune.

- **`gui/RodeVADTester/`** -- a small native SwiftUI app, built as a Swift
  Package (no Xcode.app required, just the Command Line Tools' `swift`
  toolchain), that wraps `testtone` in a window: a device picker and one
  "Play" button per channel, for testing without a terminal. See "Testing
  individual outputs > GUI" below. It shells out to the already-verified
  `testtone` binary rather than reimplementing playback in Swift.

- **`src/test_harness.c`** -- a small extra verification tool (not part of
  the shipped driver) that loads the built `.driver` bundle the same way
  `coreaudiod` does (via `CFBundle`/`dlopen`), calls the factory function,
  and exercises `Initialize` plus, discovered dynamically via
  `kAudioPlugInPropertyDeviceList` (not hardcoded object IDs), all 5
  devices: confirms there are exactly 5, confirms all 5 names and UIDs are
  pairwise distinct, and runs a full `StartIO` -> `DoIOOperation` write ->
  `DoIOOperation` read -> `StopIO` round trip through *each* device's own
  ring buffer with a distinct per-device data pattern, then cross-checks
  that no device's captured input ever matches another device's written
  output (i.e. proves the 5 virtual "cables" are fully isolated from each
  other, not just that loopback works). It does not install anything or
  touch `coreaudiod`. Build/run it with:

  ```
  clang -arch arm64 -isysroot "$(xcrun --sdk macosx --show-sdk-path)" \
      -o build/test_harness src/test_harness.c \
      -framework CoreFoundation -framework CoreAudio -framework AudioToolbox
  ./build/test_harness "$(pwd)/build/RodeCasterVirtualAudio.driver"
  ```

## How to build

```
cd ~/Developer/RodeCasterVirtualAudio
make
```

`make` builds `build/RodeCasterVirtualAudio.driver`, `build/testtone`, and
then runs:

1. `plutil -lint` on the bundle's `Info.plist`.
2. Ad-hoc code signing: `codesign --force --deep --sign - build/RodeCasterVirtualAudio.driver`
3. `codesign -dv` to print the resulting signature info.
4. `nm -gU` on the built executable to confirm
   `RodeCasterVirtualAudio_Factory` is an exported (global, undefined-import-free)
   symbol, i.e. discoverable by `CFBundleGetFunctionPointerForName`.

All four checks currently pass cleanly (see "Verification results" below).

`make clean` removes the `build/` directory.

## How to install / uninstall

**This project never runs `sudo`, `killall coreaudiod`, or `install.sh`
automatically.** You install it yourself, deliberately:

```
cd ~/Developer/RodeCasterVirtualAudio
make              # build + verify
./install.sh      # prompts for confirmation, then uses sudo internally
```

`install.sh`:
1. Builds the driver if it hasn't been built yet.
2. Asks you to confirm.
3. Copies `build/RodeCasterVirtualAudio.driver` to
   `/Library/Audio/Plug-Ins/HAL/` with `sudo`.
4. Fixes ownership (`root:wheel`) and permissions (`755`) on the installed
   copy.
5. Runs `sudo killall coreaudiod` so the audio server restarts and reloads
   its HAL plug-ins, picking up the new device.

To remove it:

```
./uninstall.sh
```

which deletes the installed bundle and restarts `coreaudiod` again.

### If macOS refuses to load the unsigned driver

Ad-hoc code signing (`codesign --sign -`, what this project uses) is
normally sufficient for `coreaudiod` to load a HAL plug-in locally on your
own Mac. However:

- **Check System Settings > Privacy & Security** after installing. Newer
  macOS versions can show a one-time blocked-extension prompt the first
  time an unnotarized audio driver extension tries to load; there is
  usually an "Allow" control there. After allowing it, run
  `sudo killall coreaudiod` again.
- Check the system log if the device never appears:
  ```
  log show --predicate 'process == "coreaudiod"' --last 2m
  ```
- Ad-hoc signing is **not** a substitute for a real **Developer ID
  signature + notarization**. It's fine for this single Mac, but if you
  reinstall macOS, move to another Mac, or macOS tightens Gatekeeper
  enforcement for audio driver extensions in a future update, you may need
  a real Developer ID certificate (`codesign --sign "Developer ID
  Application: ..."` + `xcrun notarytool submit ...`) to keep it loading
  without friction. This is flagged as a known possible sticking point,
  not something this project currently does.

## How to verify it's live

After running `install.sh`:

```
system_profiler SPAudioDataType | grep -A 8 "RVAD"
```

or open **Audio MIDI Setup.app** (Applications > Utilities) and look for all
5 of "RVAD System", "RVAD Game", "RVAD Music", "RVAD Virtual A", and
"RVAD Virtual B" in the device list on the left. Each should show 1 input
channel pair and 1 output channel pair at 44.1kHz/48kHz.

**Note:** if you had an earlier single-device version of this driver
installed (from before the 5-device expansion), re-run `./install.sh` to
overwrite it -- the old single `"RodeCaster Virtual Audio"` device name is
gone, replaced by the 5 `RVAD *` devices above.

## Testing individual outputs

Once the driver is installed (or even before, against your built-in
speakers or any other device), use the bundled `testtone` CLI to confirm
individual channels/devices actually carry audio before wiring things up in
RodeCaster Central / FineTune / macOS Sound settings.

Build (included in `make all`, or standalone):

```
make testtone
```

**List every CoreAudio output device** (index, name, channel count, UID):

```
./build/testtone --list
```

**Play a test tone to a specific device**, selecting it by the index shown
in `--list`, by a case-insensitive substring of its name, or by its exact
UID:

```
./build/testtone --device "RVAD System" --duration 3
```

**Play a test tone to one specific channel only** (all other channels stay
silent), so you can verify, one at a time, that each virtual
output/fader channel is actually routed correctly:

```
./build/testtone --device "RVAD System" --channel 1 --duration 3
./build/testtone --device "RVAD System" --channel 2 --duration 3
```

Repeat against `"RVAD Game"`, `"RVAD Music"`, `"RVAD Virtual A"`, and
`"RVAD Virtual B"` to check all 5 virtual devices individually.

Other flags: `--freq HZ` (default 440), `--duration SECS` (default 3).
Run `./build/testtone --help` for the full usage summary.

Internally, `testtone` enumerates devices with
`AudioObjectGetPropertyData`/`kAudioHardwarePropertyDevices` and
`kAudioDevicePropertyStreamConfiguration` (exactly the CoreAudio APIs the
driver itself is queried through), and plays audio via an `AudioQueue`
routed to the chosen device with `kAudioQueueProperty_CurrentDevice`, filling
only the requested channel's samples in the interleaved buffer and leaving
every other channel at 0.

`testtone` also has a `--list-machine` mode that prints the same device
listing as tab-separated fields (`index\tchannels\tname\tuid`, no header) for
other programs to parse -- this is what the GUI below uses instead of
scraping the human-readable `--list` table.

### GUI

For clicking buttons instead of typing commands, there's a small native
SwiftUI app, **RodeCaster VAD Tester**, under `gui/RodeVADTester/`. It's a
thin GUI wrapper around the same `testtone` binary described above -- it
does not reimplement audio playback -- so it shares exactly the same
tested, verified playback path; it just gives you a device picker and one
"Play" button per channel instead of a terminal.

Build it (implies `make testtone` first, since the GUI shells out to it):

```
make gui
```

This runs `swift build` (Swift Package Manager, no Xcode.app required --
only the Command Line Tools' Swift toolchain) and hand-assembles the result
into `build/RodeVADTester.app`, then runs the same style of verification as
the driver bundle: `plutil -lint` on its `Info.plist`, ad-hoc `codesign`,
and an `otool -L` check that the executable actually links
`SwiftUI`/`AppKit`.

Launch it:

```
open build/RodeVADTester.app
```

or run the executable directly to see console output/errors:

```
./build/RodeVADTester.app/Contents/MacOS/RodeVADTester
```

What it does:
- On launch (and via the **Refresh** button), runs
  `./build/testtone --list-machine` next to the app bundle and populates a
  device picker, defaulting to "RVAD System" if it's present (otherwise the
  first device in the list). All 5 `RVAD *` devices show up once the driver
  is installed.
- Once a device is selected, shows one row per output channel (channel
  count comes straight from that device -- 2 per `RVAD *` device by
  default, but it works for any device with any channel count, e.g. the
  RodeCaster Pro 2's own 10-channel "Main Multitrack" USB stream).
- Each row's **Play** button runs
  `./build/testtone --device <uid> --channel <n> --duration <n>` as a
  subprocess (selecting the device by its stable UID, not by name/index,
  to avoid any ambiguity), shows "Playing channel N..." while it runs, and
  disables that row's button until it finishes (so you can't overlap two
  calls to the same channel). A stepper lets you adjust the tone length
  (0.5-5.0s, default 1.5s) before pressing Play.
- The app is not installed anywhere system-wide -- it stays in `build/`
  alongside `testtone` and the driver bundle.

## Routing daemon

`daemon/rodevad-router.c` is what actually gets app audio onto the physical
RodeCaster Pro 2, by bridging the 5 `RVAD *` virtual devices into the real
"RODECaster Pro II Main Multitrack" hardware device. It is a **completely
separate, ordinary command-line process** -- not part of the HAL plug-in,
not loaded by `coreaudiod`. This is the piece that plays the same role as
RØDE's own background daemon in their (broken) official driver.

### What it does

1. **Waits** for the 5 `RVAD *` virtual devices (from this project's driver)
   and the RodeCaster's "Main Multitrack" device (found by matching a UID
   substring `"RODECaster Pro II"` plus an exact 10-channel output count,
   not by exact device name, so small name changes across
   firmware/driver updates don't break discovery) to all be present,
   retrying roughly once a second for up to ~5 minutes per launch. Safe to
   start before the RodeCaster is even plugged in -- see `--selftest`
   below for how this is tested without needing hardware.
2. **Validates format compatibility** before moving a single sample: all 5
   virtual devices and the Multitrack device must report the same sample
   rate, and Linear PCM Float32. If anything doesn't match, it prints a
   specific error and refuses to start, rather than silently writing
   garbled/misrouted audio to real hardware. (It does not currently
   resample via `AudioConverter` if rates differ -- see "Known
   limitations".)
3. **Taps each virtual device's output** via its own `AudioDeviceIOProc`
   (registered with `AudioDeviceCreateIOProcID`, the same standard
   mechanism any CoreAudio client uses) and pushes the captured audio into
   a small per-device lock-free ring buffer (plain atomics, no mutex, no
   allocation -- safe to touch from a real-time audio thread).
4. **A single IOProc on the Multitrack hardware device** pops from all 5
   ring buffers every hardware IO cycle and writes each one into its
   assigned 2-channel slice of the Multitrack device's interleaved
   10-channel output, per the channel mapping table in "5-device
   architecture" above (`kChannelMap[]` in the source -- a simple constant
   table, deliberately easy to edit if the mapping needs to change).
   Handles both the "one big 10-channel interleaved buffer" and "10
   separate mono buffers" `AudioBufferList` layouts a real device might
   present; if neither pattern matches, it logs a specific error once (not
   spammed every callback) instead of corrupting memory or guessing.
5. **Cleans up properly on exit**: SIGINT/SIGTERM (Ctrl-C, or
   `launchctl unload`) stops and destroys all 6 IOProcs before exiting, so
   it never leaves the RodeCaster or the virtual devices in a stuck state.

### Build it

```
make daemon
```

Builds `build/rodevad-router`, ad-hoc code-signs it (`codesign -dv` to
confirm), and runs its **offline self-test** (`--selftest`) automatically.

### Offline self-test

```
./build/rodevad-router --selftest
```

Touches **zero real audio devices** -- it only exercises the ring-buffer
push/pop math and the channel-mixing/`AudioBufferList`-layout math in
isolation, using fabricated in-memory buffers. It checks: an exact
push/pop round trip, correct silence-on-underrun behavior, correct
partial-availability behavior, all 5 devices mapping into the correct
(non-overlapping) channel pairs of a single interleaved 10-channel buffer
simultaneously, the alternate non-interleaved (10 mono buffers) layout
mapping correctly, and that an out-of-range/unrecognized layout is safely
rejected rather than corrupting memory. This is the "bounded, silent unit
test of the channel-copy/mixing math" verification step for this daemon --
it was also run under AddressSanitizer/UBSan during development (not part
of the normal build) with zero issues found.

### Install as a per-user LaunchAgent (manual -- not run automatically)

`daemon/com.abrendt.rodevad.router.plist` is a **template**;
`daemon/install-daemon.sh` fills in this project's actual paths and copies
the result to `~/Library/LaunchAgents/`, then runs `launchctl load`. Like
the HAL driver's `install.sh`, **this project never runs this script or
`launchctl load` automatically** -- you run it yourself, deliberately, once
you're ready to test with real hardware:

```
cd ~/Developer/RodeCasterVirtualAudio
make daemon                    # build + ad-hoc sign + offline self-test
./daemon/install-daemon.sh      # prompts nothing destructive, but reads carefully first
```

Unlike `install.sh` for the driver, **this never needs `sudo`** --
LaunchAgents live under your own `~/Library/LaunchAgents/` and run in your
own login session.

To stop and remove it:

```
./daemon/uninstall-daemon.sh
```

which runs `launchctl unload` (stopping it immediately -- the daemon's
SIGTERM handler cleans up its IOProcs) and deletes the installed plist.

### Manual live-testing safety notes (read before running this against real hardware)

This is the one part of this whole project that moves real audio through
real hardware in real time, and it has **not** been tested against the
live "RODECaster Pro II Main Multitrack" device in this session by design
-- see "Known limitations" and the task history for why. Before you (with
the coordinator) test it live:

- **Turn your system volume down first**, and turn down whatever's
  monitoring the RodeCaster's outputs (headphones, speakers). The first
  live run is exactly when a channel-mapping mistake, feedback loop, or
  format-conversion bug would be loudest and most surprising.
- **Watch/listen for pops, glitches, clicking, or feedback** in the first
  few seconds after `install-daemon.sh` loads it. If anything sounds wrong,
  run `./daemon/uninstall-daemon.sh` immediately -- it stops audio flow
  right away.
- **Check the logs before assuming silence means it's broken (or that it
  means it's working)** -- `tail -f logs/rodevad-router.out.log` and
  `logs/rodevad-router.err.log`. If the RodeCaster wasn't detected yet,
  the daemon just sits in its wait loop logging a status line every ~10s;
  that's normal, not a hang.
- **The channel mapping is a guess** (see "Known limitations") -- if audio
  comes out of the wrong RodeCaster channels/faders, that's the first thing
  to check and adjust in `kChannelMap[]`, not necessarily a deeper bug.
- Confirm `launchctl list | grep com.abrendt.rodevad.router` shows it
  loaded, and that `ps aux | grep rodevad-router` shows it running, as
  independent confirmation alongside the logs.

### Multi-channel routing beyond stereo

Increasing `kNumber_Channels` in `src/RodeCasterVirtualAudio.c` (e.g. to 8)
and rebuilding gives you one virtual channel per RodeCaster fader instead of
a single stereo pair, if you want full multi-channel routing rather than a
stereo mix-down -- but the router daemon's `kChannelMap[]` and channel-count
assumptions would need updating to match (it currently assumes
`kNumberOfVirtualDevices x kChannelsPerVirtualDevice == 10`).

## Known issues

- **No per-app channel/volume controls exposed.** The device intentionally
  does not implement `AudioObjectPropertyControlList` entries (volume/mute
  controls) -- `kAudioObjectPropertyControlList` returns an empty list. This
  keeps the property-handling code simpler and avoids a class of subtle
  bugs in control-element property forwarding; it does not affect basic
  loopback functionality, but you won't get a macOS volume slider for this
  device in Sound settings.
- **`CreateDevice`/`DestroyDevice` are unsupported** on purpose -- this
  driver exposes exactly 5 static devices rather than the dynamic
  multi-device workflow some drivers use (e.g. "New Aggregate Device"-style
  tools). If you need a 6th (or Nth) independent virtual device, add an
  entry to `kVirtualDevices[]` and bump `kNumber_VirtualDevices` rather than
  trying to make this driver spawn devices at runtime -- and update the
  router daemon's `kChannelMap[]` and channel-count assumptions to match.
- **Ad-hoc signing only.** See "If macOS refuses to load the unsigned
  driver" above -- flagged here again because it's the most likely
  practical blocker if this driver stops loading after a macOS update or on
  a different Mac.
- **Gatekeeper / System Extension prompts are OS-version-dependent** and
  cannot be fully predicted or automated from this project; the exact
  wording and location of the "allow this extension" control has moved
  around between recent macOS versions.
- **The GUI app must stay next to `testtone`.** `RodeVADTester.app` locates
  the `testtone` binary as a sibling of the `.app` bundle (i.e. both must
  live directly inside `build/`, which is exactly what `make gui` produces).
  If you move the `.app` out of `build/` on its own, it will fail to find
  `testtone` and show an error banner rather than silently doing nothing.
  No app icon is bundled (falls back to the generic app icon), and it is
  not code-signed with a Developer ID -- same ad-hoc-signing caveat as the
  driver itself, and it is never installed outside `build/`.
- **No SwiftUI `#Preview` support in this build.** Xcode's `#Preview` macro
  requires a plugin (`PreviewsMacros`) that only ships with Xcode.app, not
  the bare Command Line Tools, so it was removed from `ContentView.swift`.
  This has no effect on the built app; it only means there's no
  canvas-style live preview while editing.

No other driver blockers were hit: it builds cleanly with `-Wall -Wextra`
and zero warnings, and the functional test harness's write-then-read round
trips through each of the 5 ring buffers (plus the cross-talk check)
confirm the loopback logic is correct end to end for all 5 devices,
independent of `coreaudiod`.

## Known limitations (routing daemon)

These are specific to `daemon/rodevad-router.c` and are called out
separately because, unlike everything else in this project, **none of this
has been exercised against the real RodeCaster Pro 2 hardware yet** -- see
"Routing daemon > Manual live-testing safety notes" above for why, and what
running it live for the first time should look like.

- **The 10-channel mapping is our own guess, not confirmed to match what
  RØDE's software did.** We know the Multitrack device is 10 channels and
  RØDE's driver exposes 5 stereo devices (5 x 2 = 10), which is strong
  circumstantial evidence for "5 sequential stereo pairs," but we have not
  captured RØDE's actual working driver's channel assignments to confirm
  System really is channels 1-2, Game really is 3-4, etc. **This may need
  to change once tested live** -- if audio comes out of unexpected
  RodeCaster channels/faders, edit `kChannelMap[]` in
  `daemon/rodevad-router.c` (it's a single small constant table
  specifically so this is easy) and rebuild with `make daemon`.
- **No sample-rate conversion.** If the virtual devices and the Multitrack
  device ever end up at different nominal sample rates, the daemon refuses
  to start (loud, specific error) rather than resample or produce garbled
  audio. Both default to 48kHz and share the same code path in the driver,
  so a mismatch should be uncommon, but adding real `AudioConverter`-based
  resampling would be the fix if this is ever hit in practice.
- **`AudioBufferList` layout on the real Multitrack device is unconfirmed.**
  `WriteStereoIntoBufferList` handles the two most common CoreAudio device
  buffer layouts (one big interleaved buffer, or one mono buffer per
  channel) and safely no-ops with a logged error on anything else -- but
  which layout the RodeCaster's USB Audio Class 2.0 interface actually uses
  has not been observed live. If routing silently doesn't work for one or
  more devices once tested, check `logs/rodevad-router.err.log` first for
  this specific error before assuming a deeper bug.
- **Ring buffer backpressure/clock-drift behavior is untested under real
  load.** The virtual devices' software clock and the Multitrack's real
  hardware clock are two independent clocks; `RingBufferPop`'s "skip ahead
  if the producer has lapped us" logic is a reasonable, standard mitigation
  for gradual drift, but its actual audible behavior (occasional sample
  skips vs. smooth) under sustained real-world use has not been observed.
- **No feedback-loop protection.** If you route a virtual device's audio
  back into an app that's also feeding that same virtual device (directly
  or via the RodeCaster's own mix-back), you can create an audio feedback
  loop. This is a routing/configuration hazard inherent to any loopback
  setup (not unique to this daemon), but worth remembering when picking
  which apps use which `RVAD *` device live.

## Verification results

All of these were run locally as part of building this project (see
"How to build"):

| Check | Result |
|---|---|
| `make` (compile, `arch arm64`, `-Wall -Wextra`) | Pass, 0 warnings |
| `plutil -lint Contents/Info.plist` | `OK` |
| `codesign --force --deep --sign -` (ad-hoc) | Succeeds |
| `codesign -dv` | Valid ad-hoc signature, correct bundle identifier |
| `nm -gU` factory symbol export check | `RodeCasterVirtualAudio_Factory` exported |
| `test_harness` dlopen + `Initialize`, 5 devices discovered dynamically | All checks pass |
| `test_harness`: all 5 device names/UIDs pairwise distinct | Confirmed |
| `test_harness`: `StartIO`/`DoIOOperation` write->read round trip, all 5 devices | Samples match exactly on every device (loopback proven) |
| `test_harness`: cross-talk check across all 5 devices | No device's input ever matches another device's output (isolation proven) |
| `testtone --list` / `--list-machine` | Correctly enumerates all CoreAudio output devices |
| `make gui` (SwiftUI app build) | Pass, 0 warnings |
| GUI `plutil -lint` / `codesign -dv` / `otool -L` (SwiftUI linkage) | All pass |
| `make daemon` (compile, `-Wall -Wextra`) | Pass, 0 warnings |
| `codesign --sign -` / `codesign -dv` on `rodevad-router` | Succeeds |
| `rodevad-router --selftest` (ring buffer + channel-mixing math, no device IO) | All checks pass |
| `rodevad-router --selftest` under AddressSanitizer + UBSan | Zero issues found |
| `daemon/install-daemon.sh` plist template, sed-resolved and `plutil -lint`'d in isolation | Structurally valid |

**Not yet done, and intentionally out of scope for this round:** installing
the expanded 5-device driver live via `./install.sh` and re-confirming with
`system_profiler`/`testtone` against real `coreaudiod` (an earlier,
single-device version of this driver *was* confirmed live in an earlier
round of this project -- `./install.sh` needs to be re-run to pick up the
5-device version), and any live run of `rodevad-router` against the real
RodeCaster hardware. Both require the manual/live steps in "Next manual
steps" below.

One real bug was caught and fixed during the original single-device build:
the CFPlugIn factory function initially returned the address of the
driver-reference variable (`&gDriverRef`) instead of the driver reference
itself (`gDriverRef`, which is already the correctly-typed
`AudioServerPlugInDriverInterface**`). That extra level of indirection
would have made `coreaudiod` dereference garbage and fail to load the
driver. The `test_harness` tool caught this immediately before ever
touching the real system -- which is exactly why that harness (now
expanded to cover all 5 devices) exists. During the daemon build, a similar
class of bug was caught by code review before it was ever compiled: one of
the `--selftest` cases initially allocated an `AudioBufferList` with an
under-sized, hand-rolled formula (`sizeof(UInt32) + N*sizeof(AudioBuffer)`,
which ignores whatever struct padding the compiler inserts before the
`mBuffers` array) instead of the correct `sizeof(AudioBufferList) +
(N-1)*sizeof(AudioBuffer)` idiom; it was fixed before the first build, and
`--selftest` was then also run under AddressSanitizer + UBSan as extra
insurance against exactly this class of mistake, with zero issues found.

## File layout

```
RodeCasterVirtualAudio/
├── Makefile
├── README.md
├── install.sh
├── uninstall.sh
├── Resources/
│   ├── Info.plist
│   └── version.plist
├── src/
│   ├── RodeCasterVirtualAudio.c   # the driver: 5 virtual devices, one plug-in bundle
│   └── test_harness.c             # local dlopen-based functional test (not shipped)
├── tools/
│   └── testtone.c                 # CLI: list devices / play test tone to device+channel
├── gui/
│   └── RodeVADTester/              # SwiftUI channel-tester GUI (Swift Package)
│       ├── Package.swift
│       ├── Info.plist              # Info.plist for the hand-assembled .app bundle
│       └── Sources/RodeVADTester/
│           ├── RodeVADTesterApp.swift
│           ├── ContentView.swift
│           ├── DeviceStore.swift
│           ├── AudioDevice.swift
│           ├── TestToneLocator.swift
│           └── TestToneRunner.swift
├── daemon/
│   ├── rodevad-router.c                     # the routing daemon (standalone binary)
│   ├── com.abrendt.rodevad.router.plist     # LaunchAgent template (placeholders filled at install time)
│   ├── install-daemon.sh                    # per-user install (sed-resolves plist, launchctl load) -- not run automatically
│   └── uninstall-daemon.sh                  # launchctl unload + remove -- not run automatically
├── logs/                           # created by install-daemon.sh; rodevad-router's stdout/stderr
└── build/                         # created by `make`/`make gui`/`make daemon`; gitignored-worthy output
    ├── RodeCasterVirtualAudio.driver/
    │   └── Contents/
    │       ├── Info.plist
    │       ├── MacOS/RodeCasterVirtualAudio
    │       └── version.plist
    ├── testtone
    ├── rodevad-router
    ├── RodeVADTester.app/
    │   └── Contents/
    │       ├── Info.plist
    │       └── MacOS/RodeVADTester
    └── test_harness (if built manually)
```

## Next manual steps (you run these yourself)

**Steps 1-5 are the driver alone** (safe, no real hardware audio moves yet).
**Steps 6+ involve the routing daemon and real hardware, and need the
coordinator and user present together watching/listening** -- not run
autonomously, per this project's verification constraints.

1. `cd ~/Developer/RodeCasterVirtualAudio && make` -- already done and
   verified (5-device driver) as part of building this project, but
   re-run any time you change the source.
2. `./install.sh` -- installs to `/Library/Audio/Plug-Ins/HAL/` with `sudo`
   and restarts `coreaudiod`. **This was intentionally not run for you.**
   **Important:** if an older single-device version of this driver was
   installed in an earlier round, this overwrites it with the new
   5-device version -- the old `"RodeCaster Virtual Audio"` device name
   will disappear, replaced by the 5 `RVAD *` devices.
3. Verify: `system_profiler SPAudioDataType | grep -A 8 "RVAD"` or check
   Audio MIDI Setup.app -- confirm all 5 `RVAD *` devices appear.
4. `./build/testtone --list`, then `./build/testtone --device "RVAD
   System" --channel 1` (and `--channel 2`, and the other 4 devices) to
   confirm each channel actually carries audio -- or `make gui && open
   build/RodeVADTester.app` for the same thing with buttons instead of
   flags.
5. At this point the driver alone is fully working (5 independent virtual
   loopback devices) -- useful on its own for routing between apps, even
   before the next steps.
6. **Connect the RodeCaster Pro 2 over USB** if it isn't already, and
   confirm `system_profiler SPAudioDataType` shows "RODECaster Pro II Main
   Multitrack" with 10 channels and a UID starting `AppleUSBAudioEngine:`.
7. `make daemon` -- builds, ad-hoc signs, and runs the offline
   `--selftest` for `rodevad-router` (safe, no real device IO -- already
   done and passing as part of this build).
8. **With the coordinator and user both present, volume turned down**:
   `./daemon/install-daemon.sh` -- loads the router as a per-user
   LaunchAgent (no sudo). Watch/listen for pops, glitches, or feedback
   immediately. See "Routing daemon > Manual live-testing safety notes"
   above for the full checklist before doing this.
9. Check `tail -f logs/rodevad-router.out.log` and
   `logs/rodevad-router.err.log` to confirm it found all 6 devices,
   passed its format check, and is running -- not just that it's silent.
10. Test actual routing: play audio to one `RVAD *` device (e.g. via
    `testtone` or any app) and confirm it comes out of the expected
    RodeCaster channel pair. **If it comes out of the wrong channels**,
    that's expected to potentially need adjustment -- see "Known
    limitations" for how to edit `kChannelMap[]` and retry.
11. **Rollback if anything goes wrong:**
    - Daemon/routing issues: `./daemon/uninstall-daemon.sh` stops audio
      flow immediately (no sudo, no `coreaudiod` restart needed).
    - Driver issues: `./uninstall.sh` removes the driver and restarts
      `coreaudiod`, returning the system to its prior state. If some app
      is misbehaving because it had an `RVAD *` device selected when it
      was removed, just re-pick a real device in that app's own audio
      settings.
