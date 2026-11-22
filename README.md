# Snipline

A standalone C++20 / Qt Quick screenshot and recording selector for Wayland, being built from
[`plan.md`](plan.md). **Early prototype, not a completed Milestone 1 release.**

## Implemented

- Per-output LayerShellQt overlays with a multi-tier QML toolbar.
- Frozen and live region, monitor and isolated-window selection; selecting a
  target does **not** save it.
- Drag to create, move or resize a region; Enter/Capture confirms, Escape cancels.
- Compositor-specific output discovery for Hyprland and niri.
- An asynchronous `grim` capture backend with timeouts, cancellation, image-size
  checks and layout revalidation. Frozen backgrounds are acquired before
  overlays appear; live pixels are acquired only after confirmation.
- A compositor-independent window list and isolated frozen-window buffers. Hyprland uses
  native toplevel-export with retained wlr handles; the standard ext protocol
  path (including compatible niri versions) uses stable identifiers and `grim -T`.
- Live overlays are transparent to the desktop. Confirmation hides all selector
  surfaces/popups, waits for a `wl_display.sync` acknowledgement on **Qt's own
  connection**, then revalidates the layout and acquires fresh pixels. No delay
  or second-connection roundtrip is used as a substitute for that barrier.
- Mixed-density composition at the highest selected scale, including negative
  output positions and transparent desktop gaps.
- Atomic PNG/JPEG/WebP export, unique filenames and owner-only file permissions.
- Clipboard copying, basic desktop notifications and single-instance D-Bus control.
- Validated, data-only JSON theme tokens.
- Validated JSON settings, configurable selector shortcuts and screenshot-image
  notification hints.
- `doctor` diagnostics that do not request screenshots or recording streams.
- Native monitor/single-output region and isolated-window recording with
  software MP4/WebM encoding, silent GIF export, countdown, pause/resume,
  elapsed status, stop/discard and separate system/microphone toggles.

- Local asynchronous QR/barcode recognition (ZXing-C++) and OCR (Tesseract CLI),
  from confirmed selections, saved captures or existing raster images.

**Not yet implemented:** portal fallback, cross-output recording, Satty,
uploads, history, tray or autostart. Native capture depends on the installed
`grim` and compositor supporting a compatible protocol. Hyprland screenshots
have been manually tested by the user; **niri end-to-end support is not
established** by the IPC parser. See the [compatibility matrix](docs/compatibility.md).

## Configuration

The compact SVG toolbar selects Capture, Record, QR or OCR.
The options icon expands flat action-specific controls
above it: target, format, freeze mode, recording audio, FPS and countdown.
The draggable toolbar retains its position and expanded state when switching
tools. Short/narrow outputs use scrolling options and a compact action strip.

The Capture arrow offers **Save** (file only), **Copy** (clipboard only),
**Open folder**, and **Open picture** (save first, then open). The normal Capture
button follows the configured clipboard setting. Annotate is still unavailable.
Google Lens has a magnifying-glass toolbar placeholder, and both result menus
include a disabled Upload entry. Neither performs network requests yet.
The arrow is disabled whenever Capture/Record is disabled.

Recording displays its countdown centered in the selected region, with the
outside area dimmed. Invoking `capture` or `record` again during recording opens
a matching draggable icon toolbar with Pause/Resume, Stop, Discard and Hide.
Opening/hiding controls does **not** pause recording; visible controls may appear
in the recording. The region stays clear and its surroundings dimmed.
Escape hides these controls; Discard is explicit.

The recording arrow chooses saving, opening the folder/video, or editing **after
the recording is successfully saved**. The selected action is applied by
**Finish recording**; there is no separate result-action button.
Configure the video editor command in Session settings or `recording.editor`
in JSON (for example, `"editor": "kdenlive"`). Quoted arguments are supported;
Snipline appends the saved video path as one argument, without invoking a shell.
Discard/failure never launches an application.

**Settings…** edits the save directory, filename template, notifications and
screenshot clipboard copying for the resident session. The audio selector is a
searchable multi-select dropdown populated automatically from `pactl` devices
and application playback streams, with human-readable device/app/media names.
Changes are validated and locked after confirmation.
These controls do **not** overwrite the config file; edit JSON to persist them.

Screenshots use `screenshots.directory` (default `~/Pictures/Snipline`);
videos/GIFs use `recording.directory` (default `~/Videos/Snipline`, honoring the
platform's Videos location). Relative config paths resolve beside the config
file, and `~` is supported. The session Save directory field edits only the
currently selected tool's destination. `--directory` explicitly overrides both
destinations for that resident instance.

```json
{
  "screenshots": {
    "directory": "~/Pictures/Snipline",
    "target": "region",
    "live": true,
    "format": "png"
  },
  "recording": { "directory": "~/Videos/Snipline" }
}
```

Older top-level `directory`, `format`, `target` and `live`, and the
`directory.screenshots`/`directory.recordings` layout remain readable.
Do not specify both an old and new key for the same setting; this is rejected
instead of silently choosing one. `recording.directory` is now canonical.

Switching Capture ↔ Record does not save or start recording. Region/monitor
selection is retained. Switching to live is immediate; switching back to frozen
refreshes pixels **in place**, without closing/reopening windows or rebuilding
QML. A clean fresh snapshot still requires briefly rendering transparent UI
buffers, waiting for their swaps and the Wayland display barrier, then restoring
the same surfaces. Toolbar position, popups/options state and selection survive.
Isolated window pixels are reacquired without hiding the desktop selector.
The cursor option is currently a clearly labeled UI-only placeholder; inactive
advanced actions are not presented as implemented capture operations.

Copy [`config.example.json`](config.example.json) to
`~/.config/snipline/config.json` (or `$XDG_CONFIG_HOME/snipline/config.json`).
Missing files use defaults; malformed files, unknown keys and invalid values
fail startup rather than being partially applied. No config is created or
overwritten automatically. Use `--config /path/to/config.json` for another file.

### QR/barcodes and OCR

Select QR or OCR in the toolbar, select a target and explicitly confirm.
Recognition does not save a screenshot or open anything automatically.
No results window opens after recognition.
The first HTTP(S) link is copied automatically (or the first payload when no
web link is found). A desktop notification offers **Open**; only an explicit
click opens it. Multiple codes are decoded, but only the first web link (or first
payload) is copied. Notification buttons require a desktop notification server
with action support.
Empty/failed/cancelled scans leave the clipboard unchanged.
Binary barcode payloads are copied as Base64.

OCR uses Tesseract asynchronously. Select installed languages in **OCR languages**
in the toolbar options (settings icon) before confirming the capture; multiple
languages can be checked. The searchable dropdown uses the audio selector's
spacing and window-local placement logic. Cursor capture is always off for
QR/OCR and no cursor setting is shown. Successful nonempty OCR results are copied
automatically and announced with a notification. Paste into an editor to edit them.
Recognition notifications
honor `notifications` and `notificationTimeout`; they do not expose the payload
or recognized text in the notification body.
Set preferred languages in JSON, for example
`"ocr": {"languages": ["eng", "pol"]}`; session changes do not rewrite config.
Missing Tesseract/language data produces an error, not a different recognition backend.
Tesseract's orientation/script detection can identify scripts, not reliably
distinguish exact languages (for example, English versus Polish). Select multiple
installed languages to recognize mixed-language text; automatic language
identification is not implemented.

Saved captures and other existing images can be recognized through the CLI:

```sh
snipline qr                       # select a new region
snipline qr region --live         # acquire cursor-free pixels after confirmation
snipline ocr --frozen             # select over a frozen, cursor-free snapshot
snipline qr --file "/path/code.png"
snipline ocr --file "/path/capture.png"
```

QR/OCR also accept `monitor` and `window` targets. `--live` and `--frozen`
override `screenshots.live` for that request, including when forwarding to a running
instance. They conflict with each other and cannot be used with `--file`.
Existing image contents are recognized as-is; a cursor already baked into a file
cannot be removed. Use `snipline <command> [target-or-control] --help` for
command-specific positional arguments, options, defaults and restrictions.

Image decoding/ZXing run in the installed `snipline-recognize` helper;
OCR receives PNG via stdin and returns text via stdout, without a shell or
temporary screenshot files. `snipline cancel` stops the active process and clears
its input/results; locking the session does the same. Nothing is uploaded.
Limits: 16 megapixels, 16,384 pixels per dimension, 64 MiB input files and
1 MiB text/barcode results. Image preparation/QR have a 15-second deadline;
Tesseract language discovery has five seconds and OCR has 60 seconds.
Install Tesseract plus the language data you want; OCR accuracy depends on image
quality and the selected models. Raster formats depend on installed Qt image plugins.

Screenshot settings live under `screenshots`: `directory`, `format` (`png`,
`jpeg`, `webp`), `target` and `live` (live selection by default).
Shared settings include `filenameTemplate`, `clipboard`, `notifications`, `notificationTimeout` (milliseconds;
`0` requests no expiry), optional `theme` (a JSON theme file), and `keybindings`.
Paths accept `~/`; relative config paths resolve beside the config file.
CLI `--directory`, `--format`, `--theme`, explicit target and `--live`/`--frozen`
override the corresponding defaults. Restart the resident process after editing
settings; save/clipboard/notification/theme/keybinding settings belong to that
resident, not subsequent capture clients.

Default selector shortcuts (all configurable under `keybindings`):

| Action | Key |
| --- | --- |
| Confirm | Return, Enter, Space |
| Cancel | Escape |
| Toggle live/frozen | Ctrl+L |
| Toggle cursor preference (UI-only) | Ctrl+Shift+C |
| Save (file only) | Ctrl+S |
| Copy screenshot (clipboard only) | Ctrl+C |
| Save and open picture/video | Ctrl+O |
| Save and open folder | Ctrl+Shift+O |
| Save and edit recording | Ctrl+E |
| Capture / Record / QR / OCR | Ctrl+1 / Ctrl+2 / Ctrl+3 / Ctrl+4 |
| Region / Monitor / Window | Shift+1 / Shift+2 / Shift+3 |

Keys are local to the selector, not global desktop shortcuts. Tool/target
switches do not confirm or start recording. Live/frozen toggling is disabled
while recording. Empty arrays disable an action; duplicate shortcuts are rejected.
The cursor shortcut is disabled for QR/OCR. Clipboard copying is controlled only
in main session settings, not duplicated in screenshot-specific options.
Result-action shortcuts match the arrow-menu actions. For recording selection,
they start recording with that post-save action; while recording controls are
visible, they finish recording with that action. Copy is capture-only and Edit
is recording-only. Shortcuts are disabled in settings/audio/language popups and
when confirmation/finalization is unavailable. Menus show the first assigned key.
When migrating an older keymap, move the old Ctrl+number target bindings to
Shift+number (or reassign the tool shortcuts) to avoid conflicts.

Shortcuts use portable Qt sequences, e.g. `"confirm": ["Ctrl+Return"]`.
An empty array disables an action's shortcuts. These are **in-selector**
shortcuts, not global hotkeys; global launch bindings remain in Hyprland/niri.
Duplicate shortcuts and multi-step sequences are rejected.

Saved screenshot notifications supply the image file URI as both the icon and
the standard `image-path` hint. Actual thumbnail size/display depends on the
notification daemon. Disable notifications with `"notifications": false`.

### Screenshot filenames

Set `filenameTemplate` to a filename **without its extension**, for example:

```json
"filenameTemplate": "Screenshot_{datetime:yyyyMMdd_HHmmss_zzz}"
```

`{datetime:FORMAT}` passes `FORMAT` directly to
[`QDateTime::toString()`](https://doc.qt.io/qt-6/qdatetime.html#toString),
using local save time. All Qt custom date/time format tokens are available:
day/month names, two/four-digit years, 12/24-hour clocks, AM/PM, milliseconds
and timezone forms (`t`, `tt`, `ttt`, `tttt`). Multiple datetime placeholders
use the same instant. Examples:

```json
"filenameTemplate": "Screenshot_{datetime:yyyy-MM-dd_HH-mm-ss.zzz}"
```

```json
"filenameTemplate": "{datetime:dddd, d MMMM yyyy 'at' hh-mm-ss AP}_{uuid}"
```

```json
"filenameTemplate": "Shot_{datetime:yyyy-MM-dd'T'HH:mm:ss.zzzttt}"
```

Qt single-quoted literals and doubled apostrophes work inside the format,
including quoted braces, e.g. `{datetime:'{shot}'_yyyy}`. Outside placeholders,
`{{` and `}}` produce literal braces. Unknown Qt format letters retain Qt's own
behavior; Snipline does not reinterpret or restrict the format tokens.

The shorthand placeholders remain supported: `{date}` → `2026-11-20`,
`{time}` → `14-30-05`, `{timestamp}` → `2026-11-20_14-30-05-123`.
`{uuid}` produces a random 32-character identifier. The default is
`Snipline_{datetime:yyyy-MM-dd_HH-mm-ss-zzz}_{uuid}`. The chosen image format
adds `.png`, `.jpeg` or `.webp` automatically. Existing names are never
overwritten: collisions add `_1`, `_2`, etc. Path separators, control characters,
unknown placeholders and excessively long names are rejected, including when
produced by a datetime format (use `-` rather than `/` for dates). Use
`directory` to choose the destination folder. Restart Snipline after changing
the template.

## Recording

Restart an older resident instance with `./build/snipline quit`, then:

```sh
./build/snipline record region
./build/snipline record monitor --video-format webm --system-audio
./build/snipline record window --video-format mp4 --microphone
./build/snipline record region --video-format gif --no-system-audio --no-microphone
```

Choose a target, adjust format/audio toggles and press **Record/Enter**.
Selection alone does not acquire recording pixels or audio. The cancellable
countdown defaults to three seconds. After countdown, all selector surfaces and
popups are hidden and the Wayland display barrier completes before acquisition.
Use a second terminal or compositor keybindings for controls:

```sh
./build/snipline record status    # JSON state, active elapsedMs, countdown, saved path/error
./build/snipline record pause     # Toggle running ↔ paused; run again to resume
./build/snipline record resume    # Alias for the same toggle
./build/snipline record stop      # Drain encoders, finalize and save
./build/snipline record discard   # Cancel and remove temporary/reserved output
```

`cancel` also discards; `quit`, Ctrl+C and SIGTERM finalize a running recording
before exiting. No HUD is shown during acquisition, since exclusion of a HUD
from every capture path cannot be guaranteed. Recordings use the configured
`directory` and `filenameTemplate`, with the recording format's extension.
The clipboard is not replaced by videos. A notification reports saved output
or failure. Saving drains EOS, then atomically publishes the completed file;
encoding uses private temporary files and an owner-only filename reservation.

Help is organized by subcommand: `snipline --help`, `snipline help record`,
`snipline record --help`, and `snipline record pause --help`. Both `record pause`
and `record resume` toggle the current state; neither starts a new recording.
Recording controls are nested under `record`, not top-level commands.

The `recording` config object controls:

```json
{
  "format": "mp4",
  "codec": "auto",
  "encoder": "auto",
  "fps": 30,
  "countdown": 3,
  "maxDuration": 3600,
  "editor": ""
}
```

`fps` is 1–120; countdown is 0–30 seconds; `maxDuration` is 1–86400 seconds
and includes pauses for safety. Elapsed time excludes acknowledged pauses.
Audio uses PulseAudio or PipeWire-Pulse. Audio is not configured in JSON;
choose audio sources in the recording UI. Remove any old `systemAudio`,
`microphone`, `systemAudioDevice`, `microphoneDevice` or `audio` recording keys
when migrating; they are rejected rather than silently ignored.
Desktop-output and microphone defaults are off.
The UI dropdown allows up to 16 individual devices or applications,
mixed into one audio track. Rows labeled **All apps** deliberately capture a
complete output mix; application rows use PulseAudio's per-sink-input monitor
API instead. No modules are loaded and no streams are moved on the user's server.
Application acquisition runs on a dedicated PulseAudio thread, using float PCM
and sample-count timestamps, with bounded non-leaky audio buffering. Each input
is converted to stereo float PCM at 48 kHz **before** mixing. A single device
input bypasses the mixer; application inputs retain a scheduling allowance.
`record status` includes per-app PCM byte and hole counters for diagnostics.

Application playback/output sample rates can differ: capture follows the output's
native rate and GStreamer resamples each branch before mixing. Decoded-tone tests
cover 44.1/48 kHz playback and output combinations with AAC/Opus. The fixture feeds
known PCM through independent `paplay` clients, avoiding live test-generator
clock adjustment being confused with recorder corruption.

Paused/idle applications contribute **silence**, not another device's audio.
A clocked zero-valued input keeps their audio track valid, and resuming playback
reanchors sample timestamps after the gap. Recording starts after app-audio
identity/setup is verified. Empty encoded tracks are rejected rather than
published as corrupt MP4 files. Real-device/compositor A/V sync still needs
manual validation; no full-output fallback is ever added.

In window mode, application audio is auto-selected using compositor PID/app
identity and, where needed, exact stream/window-title metadata. Manual extra
selections are retained on retargeting, and manual unchecks survive metadata
refreshes. Shared-process/browser windows without an unambiguous match require
manual selection; Snipline never substitutes the full desktop mix. Selecting an
app captures the identified playback stream, not necessarily every sound from
every process in a browser/app group. Streams that do not yet exist cannot be
selected. Reused indices are verified against the original client/process/app
identity before opening the stream.

`--system-audio` explicitly adds the configured full mix even for window mode;
otherwise window auto-selection does not enable a full mix on its own.
Audio selection/discovery is metadata-only until recording confirmation.
Missing `pactl`, unavailable sources and backend errors are reported rather
than silently selecting another device. GIF requires no selected audio sources.

### Recording limits and validation

#### Codecs and hardware encoding

Select the container, codec and encoder separately in recording options:

| Container | Video codecs | Audio |
|---|---|---|
| MP4 | H.264, H.265/HEVC, AV1 | AAC |
| WebM | VP8, VP9, AV1 | Opus |
| GIF | VP8 intermediate, then palette conversion | None |

`recording.codec` defaults to `auto`: H.264 for MP4, VP8 for WebM/GIF.
`recording.encoder` defaults to `auto`. Options:

- `software`: x264, x265, libvpx VP8/VP9, or libaom AV1.
- `auto`: try installed VA-API, NVIDIA NVENC and Intel QSV encoders, then software.
- `vaapi`, `nvenc`, `qsv`: require that hardware backend; never silently switch
  to software. Codec support depends on the GPU, driver and GStreamer plugins.

Hardware candidates first encode a synthetic frame at the **actual recording
dimensions**. Probing runs asynchronously in `snipline-encoder-probe`, bounded
to five seconds per candidate, so a probe crash does not kill the resident.
Automatic mode falls back if a candidate's probe fails, before starting the
recording encoder/audio or reserving an output file. Once a candidate is accepted,
later pipeline failures are reported and cleaned up, not silently retried or
switched midway through a recording. Hardware encoding requires at least 32×32,
and hardware HEVC requires at least 128×128;
smaller regions use software in automatic mode or fail clearly in explicit
hardware mode. GIF remains a software conversion.

```sh
snipline record monitor --video-format mp4 --codec h264 --encoder auto
snipline record window --video-format mp4 --codec h265 --encoder vaapi
snipline record region --video-format webm --codec vp9 --encoder software
```

Set the same defaults through `recording.codec` and `recording.encoder` in JSON.
Use `snipline doctor` to see installed elements and `snipline record status` to
see the selected codec/encoder during recording. Installed plugins alone do
not prove that a device can encode. This still uses CPU image capture/copying
and uploads frames to the GPU; it is **not** a zero-copy high-FPS capture path.

- Software and optional hardware encoding support the combinations above.
  Missing plugins fail explicitly.
  GIF uses a silent VP8 intermediate and two FFmpeg passes for palette
  generation/conversion, avoiding whole-video frame buffering.
- Monitor and fixed-region recordings currently require **one output**.
  A cross-output region is rejected rather than treated as synchronized video.
- Window recording repeatedly uses the retained isolated capture identity,
  never a desktop crop. Resizes fit the initial, even-sized canvas with black
  letterboxing, preserving aspect ratio.
- Capture and encoding run asynchronously with one native acquisition in
  flight and bounded latest-frame/encoder queues. Slow acquisition repeats the
  newest frame with real running-time timestamps; requested FPS is not a
  guarantee of native acquisition throughput. The CPU/SHM path is not intended
  as a zero-copy high-FPS/game recorder.
- Canvas/source frames are limited to 8192 pixels per side and 16 megapixels.
  Source closure, layout changes and capture revocation stop/finalize an
  already-started recording without substituting targets. Losing a selected
  audio device/app or audio server also finalizes usable output, including while
  paused; output moves/identity changes are revalidated. A warning remains
  in recording status. Audio initialization/identity failures and
  encoder/write failures discard incomplete output; neither substitutes devices.
- logind `Lock`/`LockedHint` and ScreenSaver activation stop recording.
  Shells/lockers that emit neither cannot be detected reliably: manually stop
  before locking if logind monitoring is unavailable. A warning is logged.
- Backend preparation, pause and finalization have timeouts. GIF conversion
  must finish within the 60-second finalization budget; use short clips.
  Forced termination (SIGKILL/crash) cannot finalize output and may leave
  hidden temporary files or an empty filename reservation; these are not
  reported as successful captures.

Automated tests encode/decode synthetic MP4/WebM/GIF, mix two synthetic audio
sources, check pause/resume/EOS and exercise native target/barrier lifecycle.
An optional private PipeWire/Pulse server test runs two generated tones and
verifies that selecting one app excludes the other, including pause/resume.
It uses a policy-only WirePlumber profile, private sockets/state and no hardware
monitors; it does not record the user's desktop audio or microphone.
The tests inspect continuous decoded windows (not just tone presence), include a
650 ms blocked UI thread, AAC/Opus, pause/resume, mixed-rate resampling, identity rejection,
application closure, output moves and private-server disconnects. Injected
device errors exercise finalization while running and paused; startup audio
failures never publish an empty success. Paused-app silence/resumption and empty
encoded-track rejection have dedicated regression tests.
For an explicitly requested manual app-only check, the test build also provides:

```sh
build/snipline_audio_probe --app "Music Player Daemon" --output /tmp/opencode/mpd-audio --seconds 5
```

This records only the uniquely named application, with synthetic black video.
It is not run by CTest. A successful probe verifies capture/decoding, not
subjective sound quality.
**Live Hyprland/niri recording and real audio-device validation are still
pending**; successful Hyprland screenshot testing is not evidence of recording
performance, audiovisual sync or lock behavior on a specific desktop.

## Build

On Arch Linux, the initial dependencies are:

```sh
sudo pacman -S --needed base-devel cmake ninja pkgconf qt6-base qt6-declarative qt6-wayland qt6-svg layer-shell-qt wayland wayland-protocols hyprland-protocols grim gstreamer gst-plugins-base gst-plugins-good gst-plugins-ugly gst-libav libpulse ffmpeg zxing-cpp tesseract tesseract-data-eng
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

JPEG/WebP availability depends on Qt's installed image plugins; `doctor` lists
the encoders. GStreamer development libraries are required at build time;
runtime plugins are checked for the chosen recording format. FFmpeg is required
for GIF export; `ffprobe` is used by the recording tests. PulseAudio or
PipeWire-Pulse is required only when recording audio.
The source dropdown needs `pactl`; application isolation links against libpulse.
The optional `isolated_audio` test additionally needs PipeWire, pipewire-pulse
and WirePlumber's policy profile, plus `paplay` for independent test tones.

## Run

```sh
./build/snipline doctor
QT_QPA_PLATFORM=wayland ./build/snipline capture region
./build/snipline capture monitor
./build/snipline capture window
./build/snipline capture region --live
./build/snipline capture monitor --live
./build/snipline capture window --live
./build/snipline cancel
./build/snipline quit
```

With default settings and no arguments, Snipline opens a live region selector. Capture requires a
Wayland session, a supported compositor and a session D-Bus.

Frozen region/monitor capture snapshots the enabled outputs **into memory**, then opens the
selector. No capture file is created or clipboard replaced until confirmation.
Cancel discards those frames. Outputs are acquired sequentially, so animated
content on different monitors is not a synchronized snapshot.

Window mode supports clicking the actual desktop window as well as the list.
On Hyprland, click a window behind the selector; overlaps select the topmost
candidate using the compositor's bottom-to-top client stack and floating,
pinned, fullscreen and special-workspace layers—not focus-history order.
This requires Hyprland's toplevel-mapping protocol to associate the clicked
window's full 64-bit address with a retained capture handle.

The selected window is left undimmed **in place**, rather than displayed as a
separate preview. Click another window to retarget directly—even while the
previous frozen frame is being acquired. There is no reselect/re-arm button.
Hovering a window also leaves it undimmed without changing the selected capture
target. Moving over the toolbar or leaving the selector clears the hover
highlight; the clicked selection remains highlighted. Hover uses cached
metadata, not a compositor subprocess per pointer event. On niri, ambiguous
overlapping hover targets are left dimmed rather than guessing their order.
Monitor selection follows the pointer, leaving the selected monitor undimmed
and slightly dimming the other outputs. Capture still requires confirmation.

On niri, click the desktop area to enter niri's native picker, then click the target.
Snipline hides its overlays first; niri resolves overlapping windows itself.
Escape cancels niri's picker. The returned numeric window ID is matched to its
foreign-toplevel identifier, never its title.

Choosing a window does **not** confirm capture. The toolbar returns so you can
press Capture/Enter or Cancel. Desktop clicks remain active after selection;
the list remains available for covered windows and older compositor
versions. A missing/unavailable topmost window is not replaced by a lower hit.

Frozen mode captures the isolated window into memory when you select it;
confirmation saves those pixels, not a new frame. No separate image preview is
shown. Window picking needs no desktop snapshot. Live mode captures the selected window only after
confirmation. A closed/unavailable window fails the job: **a screen crop or a
different window is never substituted**. Window titles are displayed as plain
text and are not used to identify the target.

Use the toolbar's Frozen/Live control to restart the selector in the other mode.
Region, monitor and window target types can also be changed in the toolbar.
If the compositor lacks the required window protocols, the picker explains
what is missing; portal fallback is not implemented yet.

Confirmed captures default to `Pictures/Snipline` (using Qt's Pictures location).
The process stays alive after capture/cancellation to retain Wayland clipboard
ownership and accept subsequent commands. `daemon` starts it without capturing.
It does not register itself for login startup.

Settings apply to the resident instance:

```sh
./build/snipline quit
QT_QPA_PLATFORM=wayland ./build/snipline daemon \
  --directory "$HOME/Pictures/Screenshots" \
  --format png \
  --theme themes/default.json
```

Then invoke `capture region`, `capture monitor` or `capture window`, optionally
with `--live`, in another terminal or from a
binding. Changing settings while an instance runs is rejected rather than
silently ignored. Theme changes currently require restarting the instance.
Invalid themes retain the default appearance.

Selection dimming has separate theme tokens (opacity range `0`–`1`):

```json
{
  "windowDimColor": "#000000",
  "windowDimOpacity": 0.25,
  "monitorDimColor": "#000000",
  "monitorDimOpacity": 0.25
}
```

Region selection continues to use `dimColor` and `dimOpacity`. Highlight
rectangles are display-only metadata; they are never used to crop isolated
window captures. Desktop window positions are refreshed while selecting.

## Development

| Location | Responsibility |
|---|---|
| `src/core/` | Logical geometry, composition, selection, theme validation, export, output parsing |
| `src/capture.*` | Capture interface and asynchronous native subprocess adapter |
| `src/windows.*` | Native foreign-toplevel enumeration, isolated Hyprland SHM capture, standard ext adapter |
| `src/windowpicker.*`, `src/core/windowtargets.*` | Asynchronous click picking, stacking-aware hit testing and stable identity matching |
| `src/surfacebarrier.*` | Cancellable acknowledgement that the compositor processed UI hiding |
| `src/controller.*` | Selection lifecycle, overlays, confirmation, save/copy/notify |
| `qml/Overlay.qml` | Compositor-independent toolbar and selection rendering |
| `src/doctor.*` | Bounded protocol/dependency/encoder diagnostics |
| `tests/` | Synthetic image, geometry, process, QML and private-bus tests |

Tests use synthetic pixels and controlled fake executables, **not desktop
screenshots**. A private fake Wayland compositor exercises native toplevel
enumeration, exact-handle capture, closed windows and failures. QML rendering
runs offscreen; single-instance tests use a private
D-Bus when `dbus-run-session` is available. Export tests use temporary directories.

Known early limitations:

- Real mixed-scale/rotated overlays and cross-monitor pointer grabs need manual
  checks. Geometry unit tests do not establish compositor correctness.
- Transformed window geometry inside third-party Hyprland overviews is not
  supported. In particular, scrolloverview hit testing is unchanged; normal
  desktop coordinates must not be assumed to match its scaled workspace cards.
- Native IPC schema changes fail closed. Screen-layout changes require restarting
  the selection; no attempt is made to reuse possibly stale frames.
- Export/encoding is currently synchronous; very large captures can block the UI.
- Notifications have no actions yet. Clipboard acceptance is checked locally,
  but successful pasting into other clients still needs compositor testing.
- No persistent preferences, theme hot reload, Arch package recipe or chosen
  project license yet. The desktop launcher is installed only by an explicit
  `cmake --install build`; no compositor config is rewritten.

Next: perform the Milestone 0 manual matrix, close any geometry/backend gaps,
then implement portal fallback for configurations the native adapters cannot
capture. Live UI exclusion and covered-window isolation still require real
compositor checks before claiming Milestone 0 is complete.

## Qt warning messages

`qt.qpa.wayland.textinput: ... Trying to disable ... but ... is focused` is a
Qt Wayland text-input/focus bookkeeping warning that can occur when overlays or
popups lose focus. It is generally non-fatal and does not itself indicate bad
capture pixels. If Escape, keyboard navigation or text input stops working,
report that behavior with Qt/compositor versions; do not just hide the warning.

`qt.qpa.services: Failed to register with host portal ... App info not found
for 'snipline'` means the portal cannot find `snipline.desktop` in the XDG
application directories. Running a build-tree binary without installing its
desktop entry commonly produces this. Native capture does not depend on this
registration, but portal integration may be affected. Install explicitly:

```sh
cmake --install build --prefix "$HOME/.local"
```

Ensure `~/.local/bin` is in `PATH`, then quit and restart Snipline. This installs
the binary and matching desktop entry; it does not change compositor bindings or
enable autostart. `doctor` reports whether Qt can find the desktop entry. No
desktop entry is installed automatically when running the build-tree binary.
