# Snipline

A standalone screenshot, screen-recording, QR and OCR tool for **Hyprland and
niri**, built with C++20 and Qt Quick.

Snipline provides a compact selection toolbar, explicit capture confirmation,
mixed-scale cross-monitor screenshots and a small background service.
The initial release is **v0.1.0**. Licensed under the [MIT License](LICENSE).

## Features

- **Screenshots:** region, monitor and isolated-window targets, with live or
  frozen selection. Move and resize the selection before confirming.
- **Multiple monitors:** cross-monitor screenshot regions, mixed scaling,
  rotated outputs and negative monitor positions. Export uses the highest
  selected monitor density; gaps remain transparent in supported formats.
- **Export and clipboard:** PNG, JPEG and WebP, unique filenames, atomic saves,
  configurable output directories and image copying.
- **Recording:** monitor, single-output region and isolated-window recording;
  countdown, pause/resume, finish and discard; MP4, WebM and silent GIF output.
- **Audio:** select microphone, output or supported application audio from the
  recording toolbar. Application capture never silently substitutes system audio.
- **Encoding:** software codecs and optional hardware encoders, with preflight
  checks and software fallback in automatic mode.
- **Local recognition:** QR/barcode decoding through ZXing-C++ and OCR through
  Tesseract. Successful results are copied without opening a results window.
  QR links open only when explicitly requested.
- **External editors:** independent screenshot/video commands, with filename
  substitution or file contents on stdin. No implicit shell.
- **Uploads:** native libcurl transport, Catbox, Litterbox retention profiles,
  pomf.lain.la and configured custom `.sxcu` definitions.
- **Google Lens:** an explicit image-upload action that opens Lens after a
  successful upload.
- **Desktop integration:** notification actions, configurable shortcuts, JSON
  themes, single-instance CLI control and an optional tray.
- **Recent files:** bounded, memory-only metadata; forgetting entries never
  deletes your captures.

Ordinary captures and configuration changes never upload anything. Upload/Lens
actions run without additional confirmation dialogs. Isolated-window capture
never falls back to a desktop crop.

## Install

### Arch Linux / AUR

Install the **`snipline`** AUR package with your preferred helper:

```sh
paru -S snipline
```

Or:

```sh
yay -S snipline
```

To build the AUR package manually, first install Arch's `base-devel` and `git`,
then review the PKGBUILD before building:

```sh
git clone https://aur.archlinux.org/snipline.git
cd snipline
makepkg -si
```

The package builds a release version with `BUILD_TESTING=OFF`; it does not build,
run or install tests. Optional feature dependencies are listed below.

### Dependencies

Snipline requires a Wayland session with a session D-Bus and compatible native
capture protocols. Use the `hyprctl` or `niri` executable supplied by your compositor.

| Component | Arch packages / requirement |
|---|---|
| C++ runtime | `gcc-libs`, `glibc` |
| Qt UI and Wayland support | `qt6-base` ≥ 6.5, `qt6-declarative`, `qt6-wayland`, `qt6-svg` |
| Monitor-attached overlays | `layer-shell-qt`, `wayland` |
| Native capture | `grim` with the protocols needed by your compositor |
| Recording | `gstreamer` ≥ 1.20, `gst-plugins-base-libs`, `gst-plugins-base`, `gst-plugins-good`, `gst-plugins-ugly`, `gst-libav` |
| Audio client library | `libpulse`; audio capture also needs a running PulseAudio-compatible server, such as PipeWire-Pulse |
| QR/barcode decoding | `zxing-cpp` ≥ 2.0 |
| Upload transport | `curl` / libcurl; **grabit is not required** |

Additional **build dependencies**:

- A C++20 compiler.
- CMake ≥ 3.24, Ninja and pkg-config (`cmake`, `ninja`, `pkgconf`).
- `wayland-protocols` ≥ 1.41 and `hyprland-protocols` ≥ 0.8.
- Wayland's `wayland-scanner` and the development headers for the libraries above.

Optional dependencies:

| Feature | Dependency |
|---|---|
| OCR | `tesseract` and language models, for example `tesseract-data-eng` |
| Screenshot annotation | An editor such as `satty`, configured explicitly |
| Video editing | A configured video editor |
| GIF export | `ffmpeg` |
| Additional/hardware encoders | Appropriate GStreamer plugins, including `gst-plugins-bad` where applicable, GPU drivers and supported hardware |
| Tray icon | A desktop StatusNotifierItem/system-tray host |
| Notification buttons | A notification service with action support |
| Lens / opening links | A configured default browser / desktop URL handler |

Only installed and usable encoders are available. Run `snipline doctor` to inspect
dependencies, advertised capture capabilities and codecs without capturing pixels.

### Build v0.1.0 from source

On Arch, install the required build and runtime dependencies:

```sh
sudo pacman -S --needed base-devel cmake ninja pkgconf \
  qt6-base qt6-declarative qt6-wayland qt6-svg layer-shell-qt \
  wayland wayland-protocols hyprland-protocols grim curl \
  gstreamer gst-plugins-base-libs gst-plugins-base gst-plugins-good \
  gst-plugins-ugly gst-libav libpulse zxing-cpp
```

Download the **`snipline-0.1.0.tar.gz`** source asset from the **v0.1.0** release,
then extract and build it:

```sh
tar -xf snipline-0.1.0.tar.gz
cd snipline-0.1.0

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DBUILD_TESTING=OFF
cmake --build build --parallel
sudo cmake --install build
```

The install includes `snipline`, its encoder-probe and recognition helpers,
the desktop launcher, example configuration, default theme and optional
desktop-integration examples. Startup and compositor bindings are **not enabled
or modified automatically**.

You can run `./build/snipline` before installing. If another version is already
resident, run `snipline quit` first so commands reach the newly built binary.

## Usage

```sh
# Screenshots: select first, then confirm
snipline capture region
snipline capture monitor --live
snipline capture window --frozen

# Recording
snipline record region
snipline record pause
snipline record resume
snipline record stop
snipline record status
snipline record discard

# Local recognition
snipline qr region
snipline ocr monitor --frozen
snipline ocr --file "/path/to/image.png"

# Explicit uploads and Lens
snipline upload --file "/path/to/file.png" --service pomf
snipline lens region --live
snipline lens --file "/path/to/image.png"

# Background service and optional tray
snipline daemon
snipline daemon --tray

# Recent metadata and lifecycle
snipline recent
snipline forget
snipline cancel
snipline quit

# Diagnostics and command help
snipline doctor
snipline help
snipline record --help
```

Live selection is the default. **Enter or Space** confirms; **Escape** cancels
selection. The arrow menu offers save/copy/open/editor/upload actions as
applicable. Opening, editing, uploading and Lens do not also produce a redundant
saved-file notification.

Recording pause/resume commands both toggle the current pause state. Discard
deletes the unfinished recording; finish publishes the saved recording.
`snipline cancel` also cancels uploads and can discard an active recording.

The process stays resident for clipboard ownership and notification actions.
Further invocations forward commands to it. Restart it to change startup options,
configuration or tray mode. Closing/hiding recording controls does not pause
recording; visible controls may appear in the recorded content.

### Default shortcuts

| Action | Shortcut |
|---|---|
| Capture / Record / QR / OCR / Lens | Ctrl+1 / Ctrl+2 / Ctrl+3 / Ctrl+4 / Ctrl+5 |
| Region / Monitor / Window | Shift+1 / Shift+2 / Shift+3 |
| Toggle live/frozen selection | Ctrl+L |
| Confirm / cancel selection | Enter or Space / Escape |
| Save / copy | Ctrl+S / Ctrl+C |
| Open / open folder | Ctrl+O / Ctrl+Shift+O |
| Annotate or edit / upload | Ctrl+E / Ctrl+U |

Actions are available only where applicable. Shortcuts can be overridden in JSON.
See [desktop binding examples](packaging/bindings.md) for optional compositor-level
bindings and login startup.

## Configuration

The optional configuration file is:

```text
~/.config/snipline/config.json
```

`$XDG_CONFIG_HOME` is respected. Use `--config /path/to/config.json` for an
alternative file. Defaults work without creating a configuration file.

Start with [`config.example.json`](config.example.json), also installed under
`<prefix>/share/snipline/`. For an AUR installation this is
`/usr/share/snipline/config.example.json`.

Minimal example:

```json
{
  "clipboard": true,
  "notifications": true,
  "notificationTimeout": 5000,
  "screenshots": {
    "directory": "~/Pictures/Snipline",
    "live": true,
    "format": "png",
    "editor": ""
  },
  "recording": {
    "directory": "~/Videos/Snipline",
    "format": "mp4",
    "codec": "auto",
    "encoder": "auto",
    "fps": 30
  },
  "uploaders": {
    "default": "catbox",
    "timeout": 180,
    "custom": []
  },
  "lens": {
    "destination": "litterbox-1h"
  }
}
```

Audio sources are selected in the recording toolbar, not JSON. There is no
session-settings panel; regular capture/recording options remain in the toolbar.
Data-only JSON themes can be selected with `--theme /path/to/theme.json`; the
default tokens are in [`themes/default.json`](themes/default.json).

### Editors

Set `screenshots.editor` and/or `recording.editor`, for example:

```json
{
  "screenshots": { "editor": "satty --filename -" },
  "recording": { "editor": "losslesscut {}" }
}
```

`{}` is replaced with the saved filename after argument parsing, preserving
spaces in filenames. Without `{}`, the file's **contents** are connected to stdin;
no filename is appended. No shell is invoked. An unconfigured editor is disabled
in-app and omitted from notification actions.

Editors control their own output paths. Snipline does not infer a successful edit
from an editor closing, and an editor given a filename can modify that original.

### Uploads and Lens

Built-in destination IDs:

- `catbox`
- `litterbox-1h`, `litterbox-12h`, `litterbox-24h`, `litterbox-72h`
- `pomf` — pomf.lain.la

Add trusted local `.sxcu` paths to `uploaders.custom`, then select them using
`sxcu:Name`, where `Name` is the definition's name. Relative paths resolve against
the configuration directory; `~/` is supported.

The supported subset is POST/PUT multipart requests with a fixed HTTPS endpoint,
`FileFormName`, literal string `Arguments`, string `Headers`, and a URL template
of `{response}` or a JSON path such as `{json:files[0].url}`. HTTP is allowed only
for loopback endpoints. Definitions are limited to 64 KiB; treat authentication
headers as credentials. Redirects and automatic host fallback are not used.

Uploads retain the original file. Success copies the URL and offers **Open only**
in the completion notification. Ordinary uploads do not launch a browser.
Failures include curl/HTTP details and expire after the configured notification
timeout, or five seconds if that setting is zero.

Lens uploads an image to its configured host, then opens Google Lens with the
public image URL. This sends data to both services; Google's retention is
independent of the intermediate host. Lens does not replace the clipboard.
Cancellation cannot remove data already accepted remotely.

## v0.1.0 limitations

- Native capture requires compatible compositor protocols; **portal fallback is
  not implemented**. Unsupported isolated capture fails without substituting
  another source.
- Cross-monitor screenshots are supported; **cross-monitor recording is not**.
- Real Hyprland/niri mixed-DPI, window-isolation and recording certification
  remains incomplete. Automated/offscreen tests are not proof of every desktop
  combination.
- Cursor preference controls are currently UI-only. Cursor inclusion is not
  implemented; QR/OCR have no cursor option.
- Public upload availability is not guaranteed. Catbox and Litterbox have
  returned empty responses or HTTP 403/412, including `Invalid uploader`.
  Lens cannot proceed if its intermediate upload fails.
- Recent metadata is limited to 32 deduplicated successful saves and is
  memory-only. It is hidden while locked and forgotten on restart.

See the [compatibility notes](docs/compatibility.md) and
[manual acceptance checklist](docs/manual-testing.md).

## Development and testing

To build and run automated tests separately from the release build:

```sh
cmake -S . -B build-tests -G Ninja -DBUILD_TESTING=ON
cmake --build build-tests --parallel
ctest --test-dir build-tests --output-on-failure
```

Some integration tests additionally use Python, FFmpeg, D-Bus utilities and a
private PulseAudio server. Public-host upload tests require explicit opt-in;
normal regression tests do not upload desktop captures.

For release archives, checksummed PKGBUILDs and GitHub workflows, see
[`packaging/README.md`](packaging/README.md).
