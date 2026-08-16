# RodeCasterVirtualAudio

A self-built macOS CoreAudio virtual audio driver, written from scratch, to
replace RØDE's official virtual audio driver for the RodeCaster Pro 2 after
it stopped working.

## Why this exists

The RodeCaster Pro 2's official RØDE Central / virtual-audio software driver
on this Mac stopped functioning correctly. Rather than depend on RØDE to fix
their driver, this project implements an independent, original CoreAudio HAL
plug-in that provides the same basic capability RØDE's driver provided: a
virtual audio device that macOS apps can select as an input or output, so
audio can be routed between apps and (eventually) mixed with the RodeCaster
hardware.

This is **not** a kernel extension. On modern macOS, CoreAudio HAL plug-ins
have been a userspace mechanism for years: a `.driver` bundle implementing
`AudioServerPlugInDriverInterface` (declared in
`<CoreAudio/AudioServerPlugIn.h>`), loaded by `coreaudiod` from
`/Library/Audio/Plug-Ins/HAL/`. This is the same general mechanism used by
well-known open-source loopback drivers (e.g. BlackHole). The code here is an
original implementation of that standard, publicly documented interface --
nothing is copied from any third-party driver.

## Architecture

- **`src/RodeCasterVirtualAudio.c`** -- the entire driver. A single static
  object graph:
  - PlugIn object (id 1)
    - Box object (id 2) -- describes the virtual "device box"
      - Device object (id 3) -- `"RodeCaster Virtual Audio"`
        - Input stream (id 4)
        - Output stream (id 5)
  - Implements the full `AudioServerPlugInDriverInterface`: `QueryInterface`
    / `AddRef` / `Release`, `Initialize`, `CreateDevice` / `DestroyDevice`
    (no-ops -- this driver exposes one static device, it does not support
    dynamic device creation), all property entry points (`HasProperty`,
    `IsPropertySettable`, `GetPropertyDataSize`, `GetPropertyData`,
    `SetPropertyData`), and the full IO cycle (`StartIO`, `StopIO`,
    `GetZeroTimeStamp`, `WillDoIOOperation`, `BeginIOOperation`,
    `DoIOOperation`, `EndIOOperation`).
  - **The "virtual cable" behavior**: a shared ring buffer
    (`gRingBuffer`, 65536 frames, power-of-two sized so index wrapping is a
    bitmask instead of a modulo). `DoIOOperation` writes output-stream audio
    into the ring buffer at the position given by the IO cycle's output
    sample time, and reads input-stream audio back out at the position
    given by the input sample time. Whatever an app plays to
    "RodeCaster Virtual Audio" as output can be captured back from it as
    input by any other app (or itself), just like a physical loopback
    cable.
  - **Channel count** is controlled by a single constant,
    `kNumber_Channels`, currently `2` (stereo). It is written so that
    bumping it to, say, `8` (to carry every RodeCaster Pro 2 fader/channel
    individually) only requires changing that constant -- the channel-layout
    and ASBD-building helpers (`FillStereoASBD`, `FillChannelLayout`)
    already loop over `kNumber_Channels` rather than hardcoding 2.
  - Supports both 44.1kHz and 48kHz nominal sample rates
    (`kAudioDevicePropertyNominalSampleRate` is settable; the available-rate
    list advertises both). Latency and safety offset are both reported as 0
    (there's no real hardware, so no inherent extra delay).
  - Sample format: 32-bit float, linear PCM, interleaved.

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
  and exercises `Initialize`, several property getters, and a full
  `StartIO` -> `DoIOOperation` write -> `DoIOOperation` read ->
  `StopIO` round trip through the ring buffer, to prove the loopback logic
  actually works end to end. It does not install anything or touch
  `coreaudiod`. Build/run it with:

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
system_profiler SPAudioDataType | grep -A 8 "RodeCaster Virtual Audio"
```

or open **Audio MIDI Setup.app** (Applications > Utilities) and look for
"RodeCaster Virtual Audio" in the device list on the left. It should show
2 input channels and 2 output channels at 44.1kHz/48kHz.

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
in `--list` or by a case-insensitive substring of its name:

```
./build/testtone --device "RodeCaster Virtual Audio" --duration 3
```

**Play a test tone to one specific channel only** (all other channels stay
silent), so you can verify, one at a time, that each virtual
output/fader channel is actually routed correctly:

```
./build/testtone --device "RodeCaster Virtual Audio" --channel 1 --duration 3
./build/testtone --device "RodeCaster Virtual Audio" --channel 2 --duration 3
```

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
  device picker, defaulting to "RodeCaster Virtual Audio" if it's present
  (otherwise the first device in the list).
- Once a device is selected, shows one row per output channel (channel
  count comes straight from that device -- 2 for our virtual driver by
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

## Routing the RodeCaster Pro 2 through this driver (conceptual)

This driver is a pure macOS-side virtual cable -- it has no knowledge of the
RodeCaster Pro 2 hardware itself. To actually get RodeCaster audio flowing
through it, you still combine it with the RodeCaster's own USB audio
interface (which shows up in macOS as its own multi-channel device,
independent of this project) and RØDE's routing apps:

1. In **RodeCaster Central** / **FineTune** (RØDE's hardware control app --
   this still works even if their *virtual audio driver* is what's broken),
   pick which hardware channels/faders you want to send out over USB.
2. In **macOS Sound settings** (or Audio MIDI Setup, or the target app's own
   audio-device picker), select **"RodeCaster Virtual Audio"** as the
   input device for whatever app needs to *receive* that routed audio
   (e.g. a recording/streaming app), and/or as the output device for an app
   whose audio you want to feed back into the RodeCaster's mix.
3. Use `testtone` (above) against "RodeCaster Virtual Audio" to confirm the
   macOS side is actually passing audio before troubleshooting the
   RodeCaster-side routing in RodeCaster Central.

Increasing `kNumber_Channels` in `src/RodeCasterVirtualAudio.c` (e.g. to 8)
and rebuilding gives you one virtual channel per RodeCaster fader instead of
a single stereo pair, if you want full multi-channel routing rather than a
stereo mix-down.

## Known issues

- **No per-app channel/volume controls exposed.** The device intentionally
  does not implement `AudioObjectPropertyControlList` entries (volume/mute
  controls) -- `kAudioObjectPropertyControlList` returns an empty list. This
  keeps the property-handling code simpler and avoids a class of subtle
  bugs in control-element property forwarding; it does not affect basic
  loopback functionality, but you won't get a macOS volume slider for this
  device in Sound settings.
- **`CreateDevice`/`DestroyDevice` are unsupported** on purpose -- this
  driver exposes exactly one static device rather than the dynamic
  multi-device workflow some drivers use (e.g. "New Aggregate Device"-style
  tools). If you need multiple independent virtual devices, duplicate the
  object-ID scheme and bundle identifier under a new name rather than trying
  to make this one driver spawn more devices at runtime.
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

No other blockers were hit: the driver builds cleanly with `-Wall -Wextra`
and zero warnings, and the functional test harness's write-then-read round
trip through the ring buffer confirms the loopback logic itself is correct
end to end, independent of `coreaudiod`.

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
| `test_harness` dlopen + `Initialize` + property round trip | All checks pass |
| `test_harness` `StartIO`/`DoIOOperation` write->read ring-buffer round trip | Samples match exactly (loopback proven) |
| `testtone --list` / `--list-machine` | Correctly enumerates all CoreAudio output devices |
| Installed live via `install.sh` | Confirmed: `system_profiler SPAudioDataType` shows "RodeCaster Virtual Audio", 2 in/2 out @ 48kHz |
| `testtone --device "RodeCaster Virtual Audio" --channel 1` | Confirmed: played successfully against the live installed driver |
| `make gui` (SwiftUI app build) | Pass, 0 warnings |
| GUI `plutil -lint` / `codesign -dv` / `otool -L` (SwiftUI linkage) | All pass |

One real bug was caught and fixed during this process: the CFPlugIn
factory function initially returned the address of the driver-reference
variable (`&gDriverRef`) instead of the driver reference itself
(`gDriverRef`, which is already the correctly-typed
`AudioServerPlugInDriverInterface**`). That extra level of indirection
would have made `coreaudiod` dereference garbage and fail to load the
driver. The `test_harness` tool caught this immediately (`HasProperty`
returned false / calls behaved incorrectly) before ever touching the real
system -- which is exactly why that harness exists.

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
│   ├── RodeCasterVirtualAudio.c   # the driver
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
└── build/                         # created by `make`/`make gui`; gitignored-worthy output
    ├── RodeCasterVirtualAudio.driver/
    │   └── Contents/
    │       ├── Info.plist
    │       ├── MacOS/RodeCasterVirtualAudio
    │       └── version.plist
    ├── testtone
    ├── RodeVADTester.app/
    │   └── Contents/
    │       ├── Info.plist
    │       └── MacOS/RodeVADTester
    └── test_harness (if built manually)
```

## Next manual steps (you run these yourself)

1. `cd ~/Developer/RodeCasterVirtualAudio && make` -- already done and
   verified as part of building this project, but re-run any time you
   change the source.
2. `./install.sh` -- installs to `/Library/Audio/Plug-Ins/HAL/` with `sudo`
   and restarts `coreaudiod`. **This was intentionally not run for you** --
   it touches system-wide state and restarts the audio server, which needs
   your explicit go-ahead.
3. Verify: `system_profiler SPAudioDataType | grep -A 8 "RodeCaster Virtual Audio"`
   or check Audio MIDI Setup.app.
4. `./build/testtone --list`, then `./build/testtone --device "RodeCaster
   Virtual Audio" --channel 1` (and `--channel 2`) to confirm each channel
   actually carries audio -- or `make gui && open build/RodeVADTester.app`
   for the same thing with buttons instead of flags.
5. Pick "RodeCaster Virtual Audio" as the input/output device in whatever
   app needs it, and configure RodeCaster Central / FineTune routing as
   described above.
6. **Rollback if anything goes wrong:** `./uninstall.sh` removes the driver
   and restarts `coreaudiod` again, returning the system to its prior
   state. If some app is misbehaving because it had "RodeCaster Virtual
   Audio" selected when it was removed, just re-pick a real device in that
   app's own audio settings.
