# Compatibility investigation

Milestone 0 is **in progress**, not passed. An advertised protocol is not proof
of correct screenshots, layer positioning, isolation, or clipboard persistence.

## Evidence collected

Initial development environment: Linux, Qt 6.12.0, wayland-client 1.26.0,
Hyprland session. `snipline doctor` connected to Wayland and reported:

- `zwlr_layer_shell_v1` version 5.
- `zwlr_screencopy_manager_v1` version 3.
- `ext_image_copy_capture_manager_v1` version 1.
- Both wlr foreign-toplevel and ext foreign-toplevel-list protocols.
- Updated window diagnostics also detect Hyprland toplevel-export version 2
  and the ext foreign-toplevel image capture source manager version 1. Both
  native window paths are advertised; installed grim exposes `-T`.
- A registered/activatable desktop portal and session D-Bus.
- Installed `grim`, compositor CLIs and PNG/JPEG/WebP encoders.

**No real desktop pixels or streams were acquired.** No compositor version is
marked tested. No compositor settings, bindings or startup files were changed.

The niri logical-output parser follows the upstream
[`niri-ipc` output schema](https://github.com/YaLTeR/niri/blob/main/niri-ipc/src/lib.rs)
and is exercised with synthetic fixtures. This is not an end-to-end niri test.

## Runtime matrix

| Operation | Hyprland | niri |
|---|---|---|
| Protocol advertisement | Detected in development session | Not checked |
| Native output-discovery parser | Fixture-tested | Fixture-tested |
| Layer-shell positioning/focus | Implemented, not manually verified | Implemented, not manually verified |
| Frozen region/monitor capture via grim | Implemented, not manually verified | Adapter present; compatible protocol required, unverified |
| Mixed-scale/rotated export | Synthetic tests only | Synthetic tests only |
| Cross-monitor drag/grab | Not manually verified | Not manually verified |
| Clipboard persistence/pasting | Local API path only | Not manually verified |
| Basic notifications | D-Bus path implemented, not manually verified | Same |
| Live selection/synchronization | Implemented; hide/barrier/acquire ordering fixture-tested, not manually verified | Same |
| Window enumeration/isolation | wlr list + native Hyprland toplevel-export v2; private fake-compositor tested | ext list + stable identifier + grim -T; fake-compositor/command tested, requires advertised ext source/copy protocols |
| Click-to-select windows | Hyprland mapping + stack/layer-aware IPC hit test; fixture-tested | Native `niri msg --json pick-window`; stable-ID handoff fixture-tested |
| Portal capture/PipeWire streams | Not implemented | Not implemented |
| Notification actions under DMS/Noctalia | Not implemented | Not implemented |

If `grim` cannot capture, the job reports an error. It does not fall back to an
unrelated source, change compositor configuration, invoke a shell, or silently
save anything.

## Window and live implementation evidence

- The private test compositor offers **no output-capture protocol**. Two windows
  deliberately share a title; capture of the second retained wlr handle returns
  only its synthetic buffer through the actual Hyprland SHM protocol path.
- Tests cover supported SHM formats, bounds/stride validation, inverted buffers,
  protocol absence, closed windows, native capture failures and stable ext
  identifiers passed to `grim -T` without a shell.
- Controller tests assert that live preparation acquires no pixels, confirmation
  hides overlays before the barrier, and no acquisition occurs until it succeeds.
  Cancellation, a failed barrier, and a changed layout produce no saved file.
- Live overlays are rendered offscreen to verify that the selected area contains
  no frozen image and is transparent. Final exports use the newly acquired frame.
- Frozen window confirmation exports the selected preview without recapturing.
- Click picking is selection only, with a separate confirmation. Overlap tests
  assert that stack order, not focus history, decides the topmost same-layer
  hit. Floating, pinned and fullscreen precedence, inactive workspaces, hidden
  windows, absent mappings and missing topmost handles are covered.
- Hyprland's mapping protocol is exercised against the private compositor,
  including full 64-bit addresses. niri's native picker handoff preserves
  integer IDs above JavaScript's exact-double range; cancellation is not success.

These tests do **not** prove that a real compositor implements covered-window
isolation correctly or excludes the unmounted overlays from its capture frames.

## Manual validation checklist

Run for each compositor/version and record the version, output layout and result
without attaching private screenshots:

1. Run `doctor`; distinguish missing binaries from unavailable protocols.
2. Open `capture region`. Check all overlays align and focus/keyboard shortcuts
   work. Confirm capture UI is absent in the result.
3. Create, move and resize selections; cross an output boundary during a drag.
4. Check negative output positions, fractional/mixed scales, 90°/180°/270°
   rotation and flipped outputs. Compare exported dimensions and pixel landmarks.
5. Switch Region/Monitor/Window; choosing any target must not save or replace
   clipboard. In frozen window mode, compare the preview to the confirmed image.
6. Escape/Cancel must leave the save directory and clipboard untouched. Also use
   `snipline cancel` during preparation.
7. Change layout or hotplug while selecting. Confirmation must fail rather than
   export using stale geometry.
8. Confirm PNG/JPEG/WebP exports, filename uniqueness and read-only-directory
   failures. Paste in another app before and after dismissing the selector.
9. Repeat captures through the resident process; check buffers are released when
   idle and quitting leaves no streams, child processes or overlays.
10. Check notification display under the target desktop shells.
11. Repeat region and monitor checks with `--live`. Change visible content after
    selecting; the final image must contain the new content and no selector UI,
    including ComboBox popups. Test slow rendering and high/variable refresh.
12. Test `capture window` and `capture window --live` with overlapping, fully
    covered, identical-title and resized windows. Confirm that only the chosen
    window's content is exported. Close the target before and during capture:
    the job must fail, never switch to another window or the desktop.
13. Switch Frozen/Live in the toolbar; the restarted selector must not capture
    its predecessor. Test cancellation while the UI-hide barrier is pending.
14. Click overlapping windows, including a raised-but-unfocused floating window,
    fullscreen windows and pinned/special-workspace windows. The topmost eligible
    window must win; choosing it must not save. On niri, check the native picker
    consumes the click without activating application controls and Escape returns
    to Snipline without saving. Test older versions without mapping support.

PipeWire, lock handling and notification actions need separate validation when
those features exist. Real live synchronization and isolated-window checks
above remain outstanding on both compositors.
