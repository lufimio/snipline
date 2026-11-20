# Snipline

A standalone C++20 / Qt Quick screenshot selector for Wayland, being built from
[`plan.md`](plan.md). **Early prototype, not a completed Milestone 1 release.**

## Implemented

- Per-output LayerShellQt overlays with a QML toolbar.
- Frozen and live region, monitor and isolated-window selection; selecting a
  target does **not** save it.
- Drag to create, move or resize a region; Enter/Capture confirms, Escape cancels.
- Compositor-specific output discovery for Hyprland and niri.
- An asynchronous `grim` capture backend with timeouts, cancellation, image-size
  checks and layout revalidation. Frozen backgrounds are acquired before
  overlays appear; live pixels are acquired only after confirmation.
- A compositor-independent window list and frozen-window preview. Hyprland uses
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
- `doctor` diagnostics that do not request screenshots or recording streams.

**Not yet implemented:** portal fallback,
recording, OCR/QR, Satty, uploads, history, tray or autostart. Native capture
depends on the installed `grim` and compositor supporting a compatible protocol.
In particular, **niri end-to-end support is not established** by the IPC parser.
See the [compatibility matrix](docs/compatibility.md).

## Build

On Arch Linux, the initial dependencies are:

```sh
sudo pacman -S --needed base-devel cmake ninja pkgconf qt6-base qt6-declarative qt6-wayland layer-shell-qt wayland wayland-protocols hyprland-protocols grim
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

JPEG/WebP availability depends on Qt's installed image plugins; `doctor` lists
the encoders. Recording and integration tools are optional diagnostics only.

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

With no arguments, Snipline opens a frozen region selector. Capture requires a
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

On niri, press **Pick window**, then click the target in niri's native picker.
Snipline hides its overlays first; niri resolves overlapping windows itself.
Escape cancels niri's picker. The returned numeric window ID is matched to its
foreign-toplevel identifier, never its title.

Choosing a window does **not** confirm capture. The toolbar returns so you can
press Capture/Enter or Cancel. Use **Pick another window** to re-arm desktop
selection; the list remains available for covered windows and older compositor
versions. A missing/unavailable topmost window is not replaced by a lower hit.

Frozen mode captures and
previews the isolated window when you select it; confirmation saves that
preview, not a new frame. Window picking needs no desktop snapshot. Live mode captures the selected window only after
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
