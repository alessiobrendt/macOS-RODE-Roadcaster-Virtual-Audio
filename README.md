<p align="center">
  <img src="gui/RodeVADTester/Resources/AppIconSource/icon-source.png" width="160" alt="VAD app icon">
</p>

# RodeCasterVirtualAudio

<p align="center">
  <a href="https://www.paypal.com/paypalme/alessiobrendt">
    <img src="https://img.shields.io/badge/Donate-PayPal-00457C?style=for-the-badge&logo=paypal&logoColor=white" alt="Donate via PayPal">
  </a>
</p>

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
SwiftUI app, **VAD** (short for "Virtual Audio Driver" -- the app started
life as "RodeVADTester", a single test-tone panel, and has since grown
into the full control surface for this project, so it was renamed to
reflect that; see "Renaming RodeVADTester -> VAD" below for exactly what
did and didn't change). Source lives under `gui/RodeVADTester/` (the
source directory itself was deliberately *not* renamed -- see that same
section for why) and builds to `build/VAD.app`. It has 6 tabs --
**Dashboard**, **Channel Test**, **Levels**, **Channel Mapping**,
**Daemon**, and **Restart** -- all sharing one live app state
(`AppShellView` owns `DeviceStore`, `DaemonController`, `ChannelMapStore`,
`LevelsPoller`, and `RestartController` as `@StateObject`s, injected into
every tab via `.environmentObject`). It never reimplements audio playback
or CoreAudio device enumeration in Swift: every "Play" button still shells
out to the already-verified `testtone` binary, and the Dashboard/Channel
Test tabs still read device info via `testtone --list-machine`.

Build it (implies `make testtone daemon` first, since the GUI shells out to
both binaries, and now embeds copies of both inside itself too -- see
"Self-contained bundle" below):

```
make gui
```

This runs `swift build` (Swift Package Manager, no Xcode.app or `#Preview`
required -- only the Command Line Tools' Swift toolchain) and
hand-assembles the result into `build/VAD.app`, then runs the same style
of verification as the driver bundle: `plutil -lint` on its `Info.plist`,
a check that the embedded `testtone`/`rodevad-router` binaries are present
and executable, ad-hoc `codesign`, and an `otool -L` check that the
executable actually links `SwiftUI`/`AppKit`.

Launch it:

```
open build/VAD.app
```

or run the executable directly to see console output/errors:
`./build/VAD.app/Contents/MacOS/VAD`.

#### Self-contained bundle

`build/VAD.app` is self-contained: `make gui`'s bundle-assembly step
copies `testtone` and `rodevad-router` INSIDE it
(`Contents/MacOS/testtone`, `Contents/MacOS/rodevad-router`), in addition
to leaving the original loose `build/testtone` and `build/rodevad-router`
binaries in place (still useful for the CLI-only dev workflow, e.g.
`./build/testtone --list` directly from a terminal). This is what makes
`VAD.app` relocatable to `/Applications` -- see "Installer (.pkg)" below --
without needing its helper binaries to sit next to it as loose sibling
files. `ProjectLayout.swift`'s binary resolution reflects this priority
order: (1) embedded inside the running app bundle's own
`Contents/MacOS/`, (2) the sibling-of-bundle `build/<name>` location (the
original dev-checkout layout), (3) the dev-mode CWD-walk fallback already
built into `projectRoot()`. Only case 1 is new; cases 2 and 3 are exactly
the pre-existing behavior, so this is fully backward compatible with the
original dev workflow (nothing regresses for `swift run`/using `build/`
directly during development).

`ProjectLayout.swift`'s runtime-data directories (`state/`, `config/`,
`logs/`) also gained a similar preference: they resolve to `~/Library/
Application Support/RodeCasterVirtualAudio/<name>` if that directory
already exists (meaning a proper `.pkg` install has set up a daemon
writing there), falling back to the project-relative `<projectRoot>/<name>`
directory otherwise -- a pure existence check, not a hardcoded "am I
installed" flag, so the same `VAD.app` binary works correctly whether
it's the original dev-checkout copy or a `/Applications`-installed one.

**Dashboard** -- at-a-glance status, no polling of its own: HAL driver
installed (`HALDriverCheck`, a one-line `FileManager` existence check
against `/Library/Audio/Plug-Ins/HAL/RodeCasterVirtualAudio.driver` --
strictly read-only, no install/uninstall action anywhere in this app),
RodeCaster Multitrack device connected (`DeviceStore.multitrackDevice`,
matched by UID substring `"RODECaster Pro II"`, the same convention the
daemon itself uses), all 5 `RVAD *` devices visible
(`DeviceStore.rvadDeviceCount`), and daemon status (from `DaemonController`,
which is already polling while this tab is open). Also has a **"Launch
VAD at Login"** toggle (`LoginItemController`, wrapping `SMAppService.mainApp`,
macOS 13+, no new dependency) that reflects and controls whether the app
itself opens automatically at login -- always shows the *actual* current
registration status rather than tracking local UI state, so it stays
correct even if you change this via System Settings > General > Login
Items directly instead. This is unrelated to the router daemon's own
LaunchAgent, which starts on its own regardless of this toggle. Most
reliable when `VAD.app` lives in a stable location like `/Applications`
(see "Installer (.pkg)" below) -- toggling while running from `build/`
during development still works, but a login item registered against a dev
`build/` path can end up stale after a rebuild moves/replaces that binary.

**Channel Test** -- the original per-channel test-tone panel (renamed from
`ContentView`/"RodeCaster Virtual Audio — Channel Tester" to
`ChannelTesterView`/"Channel Test"), plus an **Auto Test** mode: plays
every channel of the selected device in sequence (channel 1, then 2, ...)
with a 0.4s gap between channels (`autoTestGapSeconds`), reusing the exact
same `playingChannels`/`channelStatus` state the manual per-row Play
buttons use -- a row shows "Playing…" identically whether started
manually or by Auto Test, no separate parallel UI state. The button toggles
to **Stop** while running; stopping is cooperative (checked *between*
channels via `Task.isCancelled`, never an abrupt kill mid-tone -- the
in-flight `testtone` invocation for the current channel always finishes
naturally). Manual Play buttons and the device picker are disabled while
Auto Test runs; changing the selected device mid-sequence (or navigating
away from the tab) stops it cleanly rather than continuing into a stale
channel count.

**Levels** -- live per-channel meters for all 5 devices (L/R, RMS fill +
peak marker, color ramps green→yellow→red), reading
`state/rodevad-router.levels` roughly 8x/second via `LevelsPoller`, which
is deliberately *not* shared app-wide: it only polls while this tab is
visible (`start()`/`stop()` in `onAppear`/`onDisappear`), not for the
app's whole lifetime. Shows an explicit "no live data" banner instead of
frozen/stale-looking bars when the daemon isn't running or hasn't updated
recently (timestamp older than 1s, or the file is missing/unparseable).
This is for "is there signal, roughly how hot" sanity-checking, not
broadcast-grade metering.

**Channel Mapping** -- 5 steppers (1-9) for each device's starting
Multitrack channel, with inline red highlighting on any overlapping pair,
plus a **"Reset to Defaults"** button (`ChannelMapStore.resetToDefaults()`)
that snaps all 5 steppers back to the compiled-in defaults
(system=1/game=3/music=5/virtuala=7/virtualb=9). Reset is a **local edit
only**, exactly like changing one Stepper by hand: it does not touch
`config/channel-map.conf` on disk and does not restart the daemon by
itself -- "Apply" is still required afterward to actually save and
restart with the reset values, going through the exact same
`hasUnsavedChanges`/`validationError` flow a manual edit would. Loads
`config/channel-map.conf` if present, else falls back to
`daemon/channel-map.example.conf` as a starting template. Client-side
validation mirrors the daemon's own range/overlap rules purely for instant
UX feedback -- **the daemon re-validates the saved file itself and refuses
to start on bad input; that's the real safety authority, not this UI**.
"Apply (restarts daemon)" is the *only* way a change takes effect (save →
stop → start, in that order, surfacing each step's progress/errors) --
there is currently no live-reload/SIGHUP path, and the button is
disabled whenever there are no unsaved changes or the current values don't
validate.

**Daemon** -- Start/Stop buttons calling `DaemonController.start()`/`stop()`,
which shell out to the exact same `daemon/install-daemon.sh` /
`daemon/uninstall-daemon.sh` scripts described below (both scripts have no
interactive prompts, confirmed safe to drive via a plain `Process` with no
PTY). Status is a **heuristic**, not a state machine: PID present via
`launchctl list <label>` → running; PID absent + no recent `ERROR` in the
logs → probably still waiting for devices; PID absent + a recent `ERROR`
line → that line is surfaced directly. `launchctl`'s text output format is
informal and undocumented, so **the raw log tail is always shown alongside
the derived status, never hidden behind it** -- judge for yourself rather
than trusting the heuristic blindly.

**Restart** -- a heavier hammer than the Daemon tab's own Start/Stop, for
when the RVAD devices or routing get stuck in a way a plain daemon restart
doesn't fix. Restarts macOS's `coreaudiod` itself (which reloads every HAL
audio driver plug-in system-wide, including ours) and then the router
daemon. This is the one action in the whole app that touches audio
outside this project's own devices:

- **Restarting `coreaudiod` briefly interrupts ALL audio on the Mac** --
  every app, not just RodeCaster routing -- and requires administrator
  privileges. It is gated behind an explicit confirmation dialog (never a
  silent one-click action), matching the tone of `install-daemon.sh`'s own
  safety notes.
- Elevation is requested for exactly **one** command (`killall coreaudiod`)
  via the standard macOS-native `osascript ... do shell script ... with
  administrator privileges` pattern (`RestartController`), which shows the
  normal native admin-password dialog -- this app never embeds a raw
  `sudo` call or builds a custom password UI.
- Sequenced and status-visible, not one opaque spinner (`RestartController.Phase`:
  stopping the daemon → restarting `coreaudiod` → waiting for devices to
  reappear (polls `testtone --list-machine` a few times, a second apart --
  best-effort, since the daemon's own ~5-minute internal retry loop is the
  real safety net here) → starting the daemon again), with each step's
  outcome appended to a visible log. If the admin prompt is cancelled or
  `coreaudiod` fails to restart, the sequence stops cleanly there rather
  than blindly continuing.
- **Never reinstalls or recopies the driver bundle** -- that stays
  `install.sh`'s job, out of GUI scope. This only asks `coreaudiod` to
  *reload* whatever is already installed at
  `/Library/Audio/Plug-Ins/HAL/`.

A pink heart **Donate** button lives in the toolbar (opens
`https://www.paypal.com/paypalme/alessiobrendt` via `NSWorkspace`) --
unobtrusive, never gates any functionality.

### Renaming RodeVADTester -> VAD

The app's user-visible name changed from "RodeVADTester" to **VAD**
(display name "VAD — Virtual Audio Driver") once it grew from a single
test-tone panel into the full control surface described above. What
changed and what deliberately didn't:

- **Changed**: `CFBundleName`/`CFBundleExecutable` ("VAD"),
  `CFBundleDisplayName` ("VAD — Virtual Audio Driver"), the built artifact
  path (`build/RodeVADTester.app` -> `build/VAD.app`), the window title,
  the Dashboard header text, and the Swift Package's product/executable
  target name in `Package.swift` (`RodeVADTester` -> `VAD`, so
  `swift build` now produces `.build/release/VAD`).
- **Deliberately left alone**: the bundle identifier
  (`com.abrendt.rodecastervad.gui` -- an internal string, not user-facing;
  changing it has more ripple effects for no visible benefit), the source
  directory name (`gui/RodeVADTester/` -- renaming it would just churn
  every file's location for a purely cosmetic reason), internal Swift
  type/file names (`RodeVADTesterApp.swift`, `ChannelTesterView.swift`,
  etc. -- not user-facing), and the daemon's own LaunchAgent label
  (`com.abrendt.rodevad.router` -- a separate, unrelated identifier that
  was never in scope for this rename).

#### App icon

The app has a real icon (a mixer-with-faders "VAD" design), not the
generic default. Source of truth is a single 1024x1024 PNG at
`gui/RodeVADTester/Resources/AppIconSource/icon-source.png`; the Makefile's
`gui-icon` target (a prerequisite of `gui`, also runnable standalone) turns
it into a proper `.iconset` using `sips` (Command Line Tools, no
ImageMagick needed) at all 10 standard sizes (16 through 512@2x/1024), then
`iconutil -c icns` collapses that into `gui/RodeVADTester/Resources/AppIcon.icns`.

That generated `.icns` is **committed to the repo** (small, binary, same
pattern as the other tracked `Resources/*.plist` files) rather than
regenerated into `build/` on every run -- a fresh checkout doesn't need
`sips`/`iconutil` to run just to build. It stays reproducible anyway: the
Makefile rule is a normal file-dependency rule keyed on
`icon-source.png`'s mtime, so `make` (or `make gui-icon` directly)
automatically regenerates `AppIcon.icns` if the source PNG is ever
replaced with an updated design, rather than silently building with a
stale icon.

`gui/RodeVADTester/Info.plist` declares `CFBundleIconFile` = `AppIcon`
(the traditional key -- simple, and sufficient for an ad-hoc-signed local
utility; no `.xcassets`/Asset Catalog needed since there's no Xcode
project). The `gui` target's bundle-assembly step copies
`AppIcon.icns` into `Contents/Resources/AppIcon.icns` (creating
`Contents/Resources/` if needed), and `gui-verify` checks it's actually
there and that `file` recognizes it as a `Mac OS X icon` before signing.

See "Known issues" for the Finder/Dock icon-cache-refresh caveat --
seeing the old/generic icon briefly after a rebuild is a macOS caching
quirk, not a sign anything is wrong.

The app is not installed anywhere system-wide -- it stays in `build/`
alongside `testtone` and `rodevad-router`. `ProjectLayout.swift` is now the
single source of truth for every path the GUI touches (project root,
`build/`, `logs/`, `state/`, `config/`, the daemon scripts); other files
that used to each do their own path-walking (`TestToneLocator`) now
delegate to it.

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
   ring buffers every hardware IO cycle, computes each device's level
   (peak with cheap exponential decay + RMS -- see "Live level meters"
   below) right there so it reflects the actual post-mix signal, and
   writes each one into its assigned 2-channel slice of the Multitrack
   device's interleaved 10-channel output, per the current channel mapping
   (`sChannelMap[]` in the source -- mutable at startup, see "Runtime
   channel mapping" below). Handles both the "one big 10-channel
   interleaved buffer" and "10 separate mono buffers" `AudioBufferList`
   layouts a real device might present; if neither pattern matches, it
   logs a specific error once (not spammed every callback) instead of
   corrupting memory or guessing.
5. **Cleans up properly on exit**: SIGINT/SIGTERM (Ctrl-C, or
   `launchctl unload`) stops and destroys all 6 IOProcs, then joins the
   levels-writer thread (see below), before exiting -- so it never leaves
   the RodeCaster, the virtual devices, or that background thread in a
   stuck state.

### Live level meters (state/rodevad-router.levels)

A plain POSIX writer thread (not a real-time audio thread -- an ordinary
`pthread_create`d thread, started only once every IOProc is already
running) rewrites `state/rodevad-router.levels` roughly every 75ms: format
into a stack buffer, write to a `.tmp` file, `rename()` over the real
path. `rename()` is atomic on the same volume, so a reader (the GUI's
`LevelsPoller`) never sees a half-written file. The level *values*
themselves are computed inside the real-time `HardwareIOProc` using only
plain float math -- no allocation, no locks, no syscalls -- and handed off
to the writer thread via a small array of C11 atomics (`_Atomic float`,
`memory_order_relaxed`); the writer thread does the actual (blocking)
file IO, which would never be safe to do on the audio callback thread
itself.

Format (fixed device-line order matching `kVirtualDeviceIdentities`,
values clamped to `[0,1]`, `timestamp` lets a reader detect staleness):

```
version=1
timestamp=1755364821.123
system peakL=0.482 peakR=0.451 rmsL=0.201 rmsR=0.190
game peakL=0.000 peakR=0.000 rmsL=0.000 rmsR=0.000
music peakL=0.912 peakR=0.885 rmsL=0.430 rmsR=0.410
virtuala peakL=0.010 peakR=0.011 rmsL=0.004 rmsR=0.004
virtualb peakL=0.000 peakR=0.000 rmsL=0.000 rmsR=0.000
```

If `state/` doesn't exist or isn't writable, the writer thread silently
skips that round rather than spamming the log every 75ms (`mkdir("state",
0755)` at startup should make this a non-issue in normal operation).

### Runtime channel mapping (config/channel-map.conf)

The channel mapping described in "5-device architecture" above is no
longer purely compile-time: `LoadChannelMapFromConfig` reads
`config/channel-map.conf` once at startup, **before any IOProc is
created**, and overrides the compiled-in defaults (`kDefaultChannelMap[]`)
if the file is present and valid. See `daemon/channel-map.example.conf`
for the documented format and defaults:

```
# device=<1-based starting Multitrack channel of this device's stereo pair>
# Each device occupies channel N and N+1. Valid range 1-9, pairs must not overlap.
system=1
game=3
music=5
virtuala=7
virtualb=9
```

- **Missing file:** not an error -- one informational log line, falls
  back to the compiled-in defaults.
- **Present but invalid** (bad range, missing/duplicate key, or
  overlapping channel pairs): refuses to start, printing the exact
  problem (which line, which key, which values overlap) -- same
  fail-loudly philosophy this file already uses for sample-rate/format
  mismatches. Fix the file (or delete it to fall back to defaults) and
  restart.

`--selftest` (see below) never reads this file -- it always exercises its
own private, `static const` copy of the compiled-in defaults, so the
self-test stays deterministic and filesystem-independent regardless of
what `config/channel-map.conf` currently contains. Applying a new mapping
always requires stopping and restarting the daemon (via the GUI's
"Apply (restarts daemon)" button, or manually) -- there is no
SIGHUP/live-reload support currently.

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

Touches **zero real audio devices** -- it only exercises pure in-memory
buffer/file math, using fabricated buffers and (for the config-parsing
tests) temp files under `/tmp`, never this project's real
`config/channel-map.conf`. It checks: an exact ring-buffer push/pop round
trip, correct silence-on-underrun behavior, correct partial-availability
behavior, all 5 devices mapping into the correct (non-overlapping) channel
pairs of a single interleaved 10-channel buffer simultaneously, the
alternate non-interleaved (10 mono buffers) layout mapping correctly, that
an out-of-range/unrecognized layout is safely rejected rather than
corrupting memory, the level-metering math (silence/peak/RMS/decay/
clamping all correct), and that `LoadChannelMapFromConfig` accepts a
missing file (falls back to defaults) and a valid, deliberately-permuted
config, while rejecting an overlapping config, an out-of-range value, and
a config missing a required key -- each with a nonzero exit were it run
standalone. This is the "bounded, silent unit test of the channel-copy/
mixing math" verification step for this daemon -- it was also run under
AddressSanitizer/UBSan during development (not part of the normal build)
with zero issues found, both before and after the levels/channel-map
additions.

### Install as a per-user LaunchAgent (manual -- not run automatically)

`daemon/com.abrendt.rodevad.router.plist` is a **template**;
`daemon/install-daemon.sh` fills in this project's actual paths --
including `WorkingDirectory`, so the daemon's relative `state/`/`config/`
paths resolve the same way whether it's started manually from the project
root or via `launchd` -- and copies the result to `~/Library/LaunchAgents/`,
then runs `launchctl load`. It also creates `state/` and `config/`
alongside `logs/`. Like the HAL driver's `install.sh`, **this project never
runs this script or `launchctl load` automatically** -- you run it
yourself, deliberately, once you're ready to test with real hardware:

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
real hardware in real time. As of this writing it has not been exercised
against the live "RODECaster Pro II Main Multitrack" device under
sustained real-world use -- see "Known limitations" below for specifics.
Before testing it live:

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

## Installer (.pkg)

A proper, double-clickable macOS installer package --
`build/RodeCasterVirtualAudio-Installer.pkg` -- that installs everything
in one step: the HAL driver, `VAD.app`, and the router daemon's LaunchAgent,
using the native Installer.app wizard UI (Introduction/Install/Summary,
plus a Welcome pane explaining what gets installed and a Conclusion pane
with next steps). Built with `pkgbuild` + `productbuild`, both part of
macOS itself -- no Xcode.app needed.

### Build it

```
make installer
```

This runs `make everything` first (driver + `testtone` + daemon +
self-contained `VAD.app`, fully built and verified per the sections
above), then:

1. **Stages a payload** at `build/pkg-root/`: `VAD.app` directly (installs
   to `/Applications`), plus a hidden `.rodecaster-payload/` staging
   folder containing the driver bundle and the LaunchAgent plist template
   -- these aren't meant to land at that path permanently; the
   postinstall script (below) relocates/consumes them and deletes the
   staging folder.
2. **`pkgbuild --root ... --install-location /Applications --scripts
   installer/scripts`** builds the component package
   (`build/VAD-component.pkg`), embedding `installer/scripts/postinstall`
   to run after the payload is placed.
3. **`productbuild --distribution installer/distribution.xml --resources
   installer/resources`** wraps that component into the final
   distribution package (`build/RodeCasterVirtualAudio-Installer.pkg`),
   adding the Welcome/Conclusion panes.
4. Runs `pkgutil --check-signature` on the result and prints what it
   says (see "Signing" below -- reporting "no signature" here is expected
   and not a build failure).

### What `installer/scripts/postinstall` does (runs as root)

This is the only place in the whole project that performs a full,
non-interactive system install -- `install.sh`/`install-daemon.sh` stay
interactive (they prompt for confirmation) for manual/dev use; a pkg
postinstall script can't prompt, so the Installer.app's own UI (which the
user already clicked through, including the admin password prompt) IS the
confirmation.

1. Moves the driver bundle from its temporary staging location to
   `/Library/Audio/Plug-Ins/HAL/RodeCasterVirtualAudio.driver`, with
   `root:wheel` ownership -- the same end state `install.sh` produces
   manually.
2. Restarts `coreaudiod` so the driver loads immediately (fine to do as
   root in this context -- the Installer.app has already warned about
   this in the Welcome pane).
3. Sets up the per-user LaunchAgent for `rodevad-router`, targeting the
   **console user** (the person actually logged into the GUI session),
   not root: finds them via `stat -f%Su /dev/console`, resolves their
   home directory via `dscl`, creates `~/Library/Application
   Support/RodeCasterVirtualAudio/{state,config,logs}` owned by that user,
   writes the LaunchAgent plist into *their* `~/Library/LaunchAgents/`
   (reusing the exact same template + `sed` substitution pattern
   `install-daemon.sh` uses, just filled in with the installed-app paths),
   and loads it via `launchctl bootstrap gui/<uid>` (the modern
   replacement for `launchctl load` in root-context installer scripts),
   falling back to `sudo -u <user> launchctl load` if that fails.
4. **Fails loudly** (nonzero exit) if the driver copy or LaunchAgent setup
   fails, so the Installer.app shows a failure rather than silently
   completing with a broken setup. If no console user can be determined
   (edge case -- e.g. installing at the login screen before anyone signs
   in), it warns and skips step 3 rather than failing the whole install,
   since the driver + `coreaudiod` restart (steps 1-2) already succeeded
   independently.

### Signing

This package is **ad-hoc signed** (no paid Apple Developer ID Installer
certificate available in this environment) -- same caveat as the driver
and the app bundle elsewhere in this project. `pkgutil --check-signature`
on it reports:

```
Status: no signature
```

which is expected, not an error -- `make installer` deliberately doesn't
treat that as a build failure. In practice this means **double-clicking
the built .pkg will trigger a Gatekeeper "unidentified developer"
warning** before macOS will run it. To proceed anyway: right-click (or
Control-click) the .pkg in Finder and choose **Open**, or go to **System
Settings > Privacy & Security** after the first blocked attempt and look
for an "Open Anyway" button -- the same pattern already documented above
for the HAL driver itself.

### Verification performed (static only -- see below for why)

- `make installer` builds cleanly end to end (driver, daemon, GUI, then
  the two-stage `pkgbuild`/`productbuild` pkg build).
- `pkgutil --check-signature` reports "no signature" as expected.
- `pkgutil --expand-full` on the built `.pkg`, followed by manual
  inspection: `Distribution` XML resolves correctly (title, welcome/
  conclusion panes, `pkg-ref` pointing at the component package),
  `PackageInfo` shows the correct `install-location="/Applications"` and
  identifier, the payload contains `VAD.app` (with `testtone`/
  `rodevad-router` embedded inside it) and the `.rodecaster-payload/`
  staging content, and `installer/scripts/postinstall` is present with a
  600-second timeout configured.
- `bash -n installer/scripts/postinstall` (both the source file and the
  copy inside the expanded `.pkg`) -- valid syntax.

**Treat running the installer as a deliberate, manual step**, not
something to do casually while exploring this repo: installing it makes
real, system-wide changes (installs the driver with elevated privileges,
restarts the live `coreaudiod` -- briefly interrupting all system audio --
and sets up a new LaunchAgent for your user account). See "Next manual
steps" below for the recommended install sequence and safety notes.

## Known issues

- **The Restart tab is the highest-blast-radius action in this app.**
  Unlike everything else, restarting `coreaudiod` affects every audio app
  on the Mac, not just this project, and needs an admin password. It's
  gated behind a confirmation dialog for exactly this reason -- see
  "Restart" under the GUI section above and the live-testing safety notes
  under "Routing daemon" for the same underlying caution applied to a
  bigger blast radius.
- **"Launch VAD at Login" is most reliable once installed to
  `/Applications`.** `SMAppService.mainApp` registers a login item pointed
  at the app's current on-disk path; toggling it on while running
  `build/VAD.app` during development works, but a subsequent `make clean`
  + rebuild replaces that binary, and the previously-registered login item
  can end up stale (pointing at a path that either no longer exists or now
  holds an unrelated fresh build). The `.pkg`-installed
  `/Applications/VAD.app` case doesn't have this problem, since that path
  isn't touched by ordinary `make` runs in this project checkout.
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
- **The GUI app is self-contained as of the bundle-embedding update** --
  `VAD.app` carries its own copies of `testtone` and `rodevad-router`
  inside `Contents/MacOS/`, so it's relocatable (e.g. to `/Applications`
  via the `.pkg` installer) without needing loose sibling binaries next to
  it. When running the unrelocated `build/VAD.app` straight out of
  `build/` during development, `ProjectLayout` still also checks the
  sibling-of-bundle and dev-CWD-walk locations as fallbacks, so the
  original dev workflow is unaffected. It is not code-signed with a
  Developer ID -- same ad-hoc-signing caveat as the driver itself and the
  `.pkg` installer (see "Installer (.pkg)").
- **App icon caching: it may not visually refresh immediately.** The app
  now has a real icon (see "App icon" below), but macOS aggressively caches
  app icons by bundle identifier at the Finder/Dock/LaunchServices level.
  If you rebuild after changing the icon and the old (or generic) icon
  still shows in Finder/Dock, that's a macOS caching quirk, not a build
  problem -- quit and relaunch the app, or as a last resort
  `killall Dock` and/or `killall Finder` (both just restart those
  background processes, no data loss) to force the cache to refresh.
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
| `rodevad-router --selftest`: level-metering math (silence/peak/RMS/decay/clamping) | Correct |
| `rodevad-router --selftest`: `LoadChannelMapFromConfig` accept/reject cases (missing, valid, overlapping, out-of-range, missing key) | All correct, using temp files under `/tmp`, never the real `config/channel-map.conf` |
| Full clean `swift build` (deleted `.build`/`.swiftpm` first) | Pass, 0 warnings |
| GUI `nonisolated` actor-isolation cleanup (Swift 5 language mode; would be hard errors under Swift 6) | Fixed, 0 warnings remain |
| `make gui-icon` (sips + iconutil, 10 sizes) | Generates a valid `.icns`; `make gui-icon` re-run with unchanged source correctly no-ops (`make: Nothing to be done`) |
| `file Contents/Resources/AppIcon.icns` | `Mac OS X icon, ... "ic12" type` |
| `codesign -dv` on GUI app after icon embedding | `Sealed Resources ... files=1` (icon sealed into signature) |
| GUI rebuild with Reset to Defaults (Channel Mapping) | Full clean `swift build`, 0 warnings |
| GUI rebuild with Restart tab (`RestartController`/`RestartView`) | Full clean `swift build`, 0 warnings |
| GUI rebuild with Auto Test mode (`ChannelTesterView`) | Full clean `swift build`, 0 warnings |
| `make gui` after bundle self-containment (embed testtone/rodevad-router) | Pass; `codesign --verify --deep --strict` on the bundle exits 0 |
| Embedded `testtone`/`rodevad-router` individually via `codesign -dv` | Both show `Signature=adhoc` (re-signed correctly by the outer `--deep` bundle sign) |
| `install-daemon.sh` no-args behavior after `--router-bin`/`--working-dir` parameterization | `bash -n` valid; default paths traced through manually -- identical to pre-change behavior |
| GUI rebuild after RodeVADTester -> VAD rename (Package.swift, Info.plist, Makefile) | Full clean `swift build`: `Compiling VAD ...` / `Linking VAD`; 0 warnings |
| `make installer` (pkgbuild + productbuild) | Builds `build/RodeCasterVirtualAudio-Installer.pkg` successfully |
| `pkgutil --check-signature` on the installer .pkg | `Status: no signature` (expected/ad-hoc; documented, not treated as a build failure) |
| `pkgutil --expand-full` + manual inspection (Distribution, PackageInfo, Payload, Scripts) | Structurally correct: correct install-location/identifier, `VAD.app` + staged driver + plist template present |
| `bash -n installer/scripts/postinstall` (source and expanded-pkg copy) | Valid syntax |
| Live daemon process survived `make clean` + full rebuild of `build/` | Confirmed: same PID throughout (no restart), matching Unix unlink-while-open semantics |

Verification for the driver, `testtone`, the daemon, and the GUI has all
been done through compilation, static analysis, code signing, and offline
self-tests -- see the checks above and the per-component sections earlier
in this README. **Not yet exercised as live, real-world runs**: the
router daemon moving audio through the live Multitrack hardware over a
sustained period, the GUI's Channel Mapping "Apply" button, the Restart
tab's `coreaudiod` restart, the Login Item toggle's actual registration
call, and double-clicking/running the built `.pkg` installer. See "Next
manual steps" below for how to exercise these deliberately, and "Known
limitations" above for what specifically remains unconfirmed about live
hardware routing.

**Notes on live testing / rebuilding while a daemon is already running:**
if you rebuild the daemon (`make daemon`/`make gui`) while an earlier
build of it is already running (e.g. loaded as a LaunchAgent), be aware
that both write to the same binary path the running process was launched
from. On macOS/Unix this is safe for the process that's already running
-- it keeps executing the version of the code it originally loaded into
memory; overwriting the file on disk doesn't retroactively change a
process already running from it. **However**, the LaunchAgent plist
(`com.abrendt.rodevad.router.plist`) has `KeepAlive` configured to
relaunch the daemon on any non-zero exit, using whatever binary happens
to be on disk at that moment. That means if the running process crashes,
is manually restarted, or the Mac reboots before you've deliberately
tested a newly-built binary, it will pick up that new code automatically
rather than continuing to run the previously-verified build. If you want
a guaranteed-inert safety margin while testing a change, either stop the
daemon first (`./daemon/uninstall-daemon.sh`), temporarily set
`KeepAlive` to `false` in the plist, or copy a known-good binary aside
before rebuilding.

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
│   └── RodeVADTester/              # SwiftUI control-surface GUI (Swift Package; dir name kept, see "Renaming" above)
│       ├── Package.swift            # product/executable target renamed to "VAD"
│       ├── Info.plist              # Info.plist for the hand-assembled .app bundle (incl. CFBundleIconFile)
│       ├── Resources/
│       │   ├── AppIconSource/icon-source.png  # 1024x1024 source, single source of truth for the icon
│       │   └── AppIcon.icns                    # generated by `make gui-icon`; committed to the repo
│       └── Sources/RodeVADTester/
│           ├── RodeVADTesterApp.swift    # @main, hosts AppShellView
│           ├── AppShellView.swift        # owns shared state, TabView, toolbar/donate button
│           ├── ProjectLayout.swift       # single source of truth for every on-disk path
│           ├── DashboardView.swift       # at-a-glance status tab + Launch at Login toggle
│           ├── LoginItemController.swift # SMAppService.mainApp wrapper ("Launch at Login")
│           ├── ChannelTesterView.swift   # per-channel test-tone tab (was ContentView) + Auto Test
│           ├── MetersView.swift          # live level meters tab
│           ├── LevelMeterView.swift      # dependency-free RMS+peak bar
│           ├── LevelsPoller.swift        # polls state/rodevad-router.levels, tab-scoped
│           ├── ChannelMapEditorView.swift # channel-mapping editor tab + Reset to Defaults
│           ├── ChannelMapStore.swift     # config/channel-map.conf load/save/validate/apply/reset
│           ├── DaemonControlView.swift   # daemon start/stop/status/log tab
│           ├── DaemonController.swift    # shells out to install/uninstall-daemon.sh, launchctl status
│           ├── DaemonLogTail.swift       # cheap seek-from-end log tailing
│           ├── RestartView.swift         # Restart tab (coreaudiod + daemon restart)
│           ├── RestartController.swift   # the restart sequence + osascript admin-privileged step
│           ├── HALDriverCheck.swift      # read-only HAL driver install check
│           ├── DonateButton.swift        # pink heart -> PayPal
│           ├── DeviceStore.swift
│           ├── AudioDevice.swift
│           ├── TestToneLocator.swift
│           └── TestToneRunner.swift
├── daemon/
│   ├── rodevad-router.c                     # the routing daemon (standalone binary)
│   ├── channel-map.example.conf             # documented format/defaults + GUI fallback template
│   ├── com.abrendt.rodevad.router.plist     # LaunchAgent template (placeholders filled at install time)
│   ├── install-daemon.sh                    # per-user install; --router-bin/--working-dir params -- not run automatically
│   └── uninstall-daemon.sh                  # launchctl unload + remove -- not run automatically
├── installer/                       # the .pkg installer (see "Installer (.pkg)")
│   ├── distribution.xml             # productbuild distribution: title, welcome/conclusion, pkg-ref
│   ├── scripts/postinstall          # root-context: driver copy, coreaudiod restart, LaunchAgent setup
│   └── resources/
│       ├── welcome.html
│       └── conclusion.html
├── logs/                           # created by install-daemon.sh (dev mode); rodevad-router's stdout/stderr
├── state/                          # created by rodevad-router itself (dev mode); rodevad-router.levels
├── config/                         # created by install-daemon.sh (dev mode); channel-map.conf (optional override)
└── build/                         # created by `make`/`make gui`/`make daemon`/`make installer`; gitignored-worthy output
    ├── RodeCasterVirtualAudio.driver/
    │   └── Contents/
    │       ├── Info.plist
    │       ├── MacOS/RodeCasterVirtualAudio
    │       └── version.plist
    ├── testtone
    ├── rodevad-router
    ├── VAD.app/
    │   └── Contents/
    │       ├── Info.plist
    │       ├── Resources/AppIcon.icns
    │       └── MacOS/
    │           ├── VAD
    │           ├── testtone            # embedded copy (self-contained bundle)
    │           └── rodevad-router      # embedded copy (self-contained bundle)
    ├── VAD-component.pkg              # intermediate pkgbuild output
    ├── RodeCasterVirtualAudio-Installer.pkg  # final productbuild output -- the double-clickable installer
    └── test_harness (if built manually)
```

## Next manual steps (you run these yourself)

**Current status:** the HAL driver (5 `RVAD *` devices) and
`rodevad-router` can be installed and run live against real RodeCaster
Pro 2 hardware, either from this dev checkout or via the `.pkg` installer.
The GUI (Dashboard with Login Item toggle, Levels, Channel Mapping with
Reset to Defaults, Daemon, and Restart tabs), Auto Test mode on Channel
Test, the app icon, and the `.pkg` installer have all been built,
self-tested, and statically verified -- see "Verification results" above
for exactly what has and hasn't been exercised through sustained live use.
**Perform the steps below deliberately, with volume turned down and
someone present to watch/listen for anything that touches real audio** --
don't run them unattended.

### A. Continue in dev-checkout mode (lower-risk, incremental)

1. Read the "Notes on live testing / rebuilding while a daemon is already
   running" note under "Verification results" above before doing anything
   else -- if `build/rodevad-router` has been rebuilt since the live
   daemon process last started, the running process itself is unaffected
   (it keeps executing the old, proven code from memory), but a crash or
   restart before you're ready to test the new build would auto-load it
   via `KeepAlive`.
2. When ready to test the new build deliberately (not by accident): with
   volume turned down, either let the current live process keep running
   as-is for now, or deliberately restart it via `./daemon/uninstall-daemon.sh`
   followed by `./daemon/install-daemon.sh` once you want to switch over.
   Watch/listen for pops, glitches, or feedback immediately, same as any
   live daemon test -- see "Routing daemon > Manual live-testing safety
   notes" above.
3. Check `tail -f logs/rodevad-router.out.log` and
   `logs/rodevad-router.err.log` -- confirm it found all 6 devices, passed
   its format check, loaded `config/channel-map.conf` (or logged that it's
   using defaults), and started the levels-writer thread.
4. Confirm `state/rodevad-router.levels` is being rewritten roughly every
   75ms (`watch -n 0.2 cat state/rodevad-router.levels` or similar) --
   this is what the GUI's Levels tab reads.
5. `make gui && open build/VAD.app` -- launch the GUI and check:
   - **Dashboard**: all 4 status rows show green/good; try the "Launch
     VAD at Login" toggle (low-risk -- it only registers/unregisters a
     login item, doesn't touch audio) and confirm it reflects reality in
     System Settings > General > Login Items.
   - **Channel Test**: try **Auto Test** -- this is low-risk, same
     playback primitive as the already-tested manual Play button, fine to
     try whenever convenient (unlike the Restart tab below).
   - **Levels**: bars move when audio plays into an `RVAD *` device, and
     show the "no live data" state correctly if you stop the daemon.
   - **Channel Mapping**: try "Reset to Defaults", confirm it's a local
     edit (no daemon restart) until you press Apply; edit a value to
     create a deliberate overlap and confirm the red highlighting works,
     then fix it and try "Apply (restarts daemon)" -- watch/listen through
     this restart too, same caution as any daemon restart.
   - **Daemon**: Start/Stop buttons work, status + raw log tail both
     update.
   - **Restart** (do this deliberately, volume down, not by accident --
     this is the highest-blast-radius action in the app): confirm the
     confirmation dialog appears, that cancelling it does nothing, and
     that confirming shows the native admin-password prompt, restarts
     `coreaudiod` (**all system audio briefly interrupts, not just this
     project**), waits for devices, and restarts the daemon -- watch the
     step-by-step status log in the tab.

### B. Migrate to a proper installed setup (bigger, one-time transition)

`./build/RodeCasterVirtualAudio-Installer.pkg` (built via `make installer`)
is a **meaningful one-time transition**, not just another incremental
tweak: running it moves your setup from *this dev checkout's*
`build/rodevad-router` + project-relative `state/`/`config/`/`logs/`
directories to `/Applications/VAD.app`'s embedded `rodevad-router` +
`~/Library/Application Support/RodeCasterVirtualAudio/`. Both setups can
coexist (they use different binary paths and different LaunchAgent plist
contents, though the same LaunchAgent *label* -- installing via the .pkg
will replace whichever LaunchAgent is currently registered under
`com.abrendt.rodevad.router`), so be aware this is the point where you'd
be swapping the currently-live dev-checkout daemon for the
installed-app one, not layering a second one alongside it.

1. Double-click `build/RodeCasterVirtualAudio-Installer.pkg` in Finder.
   Since it's ad-hoc signed, macOS will likely show an "unidentified
   developer" Gatekeeper warning first -- right-click > Open, or System
   Settings > Privacy & Security > "Open Anyway" (see "Installer (.pkg) >
   Signing" above).
2. Read the Welcome pane, then proceed through Install (enter your admin
   password when prompted -- this is the postinstall script's *one*
   privileged operation, described in detail above).
3. Check the Conclusion pane's next steps, then open **VAD** from
   `/Applications`.
4. On the Dashboard tab, confirm all 4 status rows are green, confirming
   the new installed daemon (now running from
   `/Applications/VAD.app/Contents/MacOS/rodevad-router`, writing to
   `~/Library/Application Support/RodeCasterVirtualAudio/`) is healthy.
5. Optionally enable "Launch VAD at Login" now that the app lives at a
   stable path.

### C. Rollback if anything goes wrong

- Daemon/routing issues (dev-checkout setup): `./daemon/uninstall-daemon.sh`
  stops audio flow immediately (no sudo, no `coreaudiod` restart needed).
- Daemon/routing issues (`.pkg`-installed setup): the same
  `daemon/uninstall-daemon.sh` command works here too if run with
  `--router-bin /Applications/VAD.app/Contents/MacOS/rodevad-router`
  matching what was installed -- or simply `launchctl bootout
  gui/$(id -u) ~/Library/LaunchAgents/com.abrendt.rodevad.router.plist`
  directly.
- Driver issues: `./uninstall.sh` (project root, needs `sudo`) removes
  the driver and restarts `coreaudiod`, returning the system to its
  prior state. If some app is misbehaving because it had an `RVAD *`
  device selected when it was removed, just re-pick a real device in
  that app's own audio settings.
- Channel-mapping issues specifically: edit `config/channel-map.conf`
  (dev-checkout) or `~/Library/Application
  Support/RodeCasterVirtualAudio/config/channel-map.conf`
  (`.pkg`-installed) by hand -- or delete it to fall back to compiled-in
  defaults -- and restart the daemon; no rebuild required, since the
  mapping is loaded at runtime.
- App install issues: just delete `/Applications/VAD.app` -- it's a
  normal, self-contained app bundle, no special uninstaller needed for
  the app itself (only the driver and the daemon's LaunchAgent need the
  scripts above).
