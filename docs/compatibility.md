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

Automated development checks acquired no real desktop pixels or streams.
The user has since confirmed that screenshots work on Hyprland. This is manual
validation of their setup; the compositor version, output layout and individual
edge cases were not recorded. Recording and niri remain unvalidated.
No compositor settings, bindings or startup files were changed.

The niri logical-output parser follows the upstream
[`niri-ipc` output schema](https://github.com/YaLTeR/niri/blob/main/niri-ipc/src/lib.rs)
and is exercised with synthetic fixtures. This is not an end-to-end niri test.

## Runtime matrix

| Operation | Hyprland | niri |
|---|---|---|
| Protocol advertisement | Detected in development session | Not checked |
| Screenshot capture (overall) | User-tested successfully; version/layout not recorded | Not manually verified |
| Native output-discovery parser | Fixture-tested | Fixture-tested |
| Layer-shell positioning/focus | Implemented, not manually verified | Implemented, not manually verified |
| Frozen region/monitor capture via grim | Implemented, not manually verified | Adapter present; compatible protocol required, unverified |
| Mixed-scale/rotated export | Synthetic tests only | Synthetic tests only |
| Cross-monitor drag/grab | Not manually verified | Not manually verified |
| Clipboard persistence/pasting | Local API path only | Not manually verified |
| Screenshot image notifications | Saved-file URI sent as icon and `image-path` hint; message unit-tested, desktop rendering unverified | Same |
| Live selection/synchronization | Implemented; hide/barrier/acquire ordering fixture-tested, not manually verified | Same |
| Window enumeration/isolation | wlr list + native Hyprland toplevel-export v2; private fake-compositor tested | ext list + stable identifier + grim -T; fake-compositor/command tested, requires advertised ext source/copy protocols |
| Click-to-select windows | Hyprland mapping + stack/layer-aware IPC hit test; fixture-tested | Native `niri msg --json pick-window`; stable-ID handoff fixture-tested |
| Portal capture/PipeWire streams | Not implemented | Not implemented |
| Native recording | Native repeated-capture path with software/optional hardware encoding; synthetic lifecycle/encoding tests, live desktop test pending | Same, requires compatible native capture protocols; live test pending |
| Recording audio | PulseAudio/PipeWire-Pulse sources; synthetic mixer tests only | Same |
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
- Frozen window confirmation exports the retained isolated frame without
  recapturing. No separate image preview is displayed.
- Selection highlighting is tested offscreen: selected window/monitor areas
  stay undimmed, other areas are dimmed with validated per-mode theme tokens.
  Repeated clicks and retargeting during frozen acquisition require no re-arm
  action and do not save or accept stale pixels from the previous target.
- Hover uses the cached hit-test metadata and a frame-rate-limited UI update,
  without changing the clicked target or acquiring any pixels. Offscreen tests
  check simultaneous selected/hover cutouts (including overlap), pointer leave,
  and stationary-pointer updates after geometry changes.
- Click picking is selection only, with a separate confirmation. Overlap tests
  assert that stack order, not focus history, decides the topmost same-layer
  hit. Floating, pinned and fullscreen precedence, inactive workspaces, hidden
  windows, absent mappings and missing topmost handles are covered.
- Hyprland's mapping protocol is exercised against the private compositor,
  including full 64-bit addresses. niri's native picker handoff preserves
  integer IDs above JavaScript's exact-double range; cancellation is not success.

These tests do **not** prove that a real compositor implements covered-window
isolation correctly or excludes the unmounted overlays from its capture frames.

## Recording evidence

- Countdown and reopened-control UI tests verify region-centered positioning,
  persistent outside-region dimming, no automatic pause, explicit pause/resume,
  hide/reopen, stop and discard. Result-action tests verify file-only save,
  clipboard-only copy, open-after-save, and no launch after discard.
- Application PCM acquisition now uses a dedicated threaded PulseAudio loop,
  float samples and sample-count timestamps; device sources retain their own
  sample timestamps. Tests check decoded continuous tone windows under a
  650 ms Qt-thread stall, with AAC/Opus and pause/resume.
- Per-app capture now negotiates the native output rate before starting the
  encoder, then normalizes each input to stereo 48 kHz float PCM before mixing.
  Known-PCM `paplay` fixtures exercise 44.1/48 kHz playback/output combinations;
  this avoids confounding GStreamer playback-clock reslaving with capture errors.
  These are synthetic/private-server tests, not general real-device certification.
- Paused apps have a clocked silence bed and can resume into the same audio
  track. No other device's audio replaces them. Tests decode initial silence,
  resumed playback and a closed quiet source. Finalization rejects empty encoded
  tracks: a paused-source regression previously produced an invalid empty AAC
  track and is now covered explicitly.
- An explicitly user-authorized five-second MPD app-only probe produced a
  decodable stereo 48 kHz AAC MP4 with synthetic black video. This is real-source
  evidence, not listening validation or real-compositor video validation.

- Real GStreamer pipelines encode synthetic RGBA frames to H.264 MP4 and VP8
  WebM; FFmpeg palette conversion exports a GIF. `ffprobe` verifies decoding,
  stream counts and fixed canvas dimensions. Resized inputs fit the original
  canvas. No real desktop or microphone was acquired for these checks.
- Two injected `audiotestsrc` sources exercise the same mixer/encoder topology
  as system audio and microphone, including pause/resume, duration excluding
  pauses and EOS while paused. This does not establish real-device sync or
  PulseAudio source selection.
- Decoded color/letterbox assertions check that GIFs contain later frames, not
  merely a valid header. Tests inject audio-source errors and use a confined
  file-size limit to exercise encoder write-failure cleanup without filling a
  disk. Filename collision tests preserve preexisting output.
- Synthetic controller tests cover explicit confirmation, countdown cancel,
  hide/display-barrier/acquire order, continuous isolated identity, no desktop
  acquisitions for windows, stop on target closure/lock, discard, late replies
  and encoder failures.
- Toolbar tests exercise action switching through the bottom tier, per-action
  format/audio controls, settings validation, popup Escape handling and narrow
  output wrapping. Switching back to a frozen screenshot waits for the display
  barrier, refreshes pixels and restores the region without confirming capture.
- The user's compact flat QML layout and SVG resources are integrated. Timing
  switches retain QQuickViews, the QML engine/root, expanded options, toolbar
  position and selection. Fresh freeze commits transparent buffers while keeping
  surfaces mapped, then synchronizes and updates the same image provider. Tests
  assert no window visibility changes/recreation, fresh pixel revisions, and
  restoration on failure/cancel. Actual compositor animation behavior still
  requires manual validation; fresh clean acquisition needs brief transparency.
- `pactl` JSON source/sink-input discovery is asynchronous and metadata-only.
  Tests cover string/numeric indices, friendly labels, PID matching and ambiguous
  shared-process windows. Application recording uses `pa_stream_set_monitor_stream`,
  verifies stream identity and never substitutes an output mix.
- A private PipeWire/PulseAudio server with a policy-only WirePlumber instance
  exercises real per-stream monitoring: the chosen generated tone survives and
  the other application's tone is excluded from the decoded recording. Private
  runtime/config/state and disabled hardware profiles isolate the test from the
  user's audio server/devices.
- The implementation uses bounded native acquisitions and encoder queues,
  software/optional hardware encoding and explicit single-output region limits.
  Hardware candidates are preflighted with synthetic frames at the actual
  recording dimensions in a separate helper process before output/audio starts.
  A probe crash cannot kill the resident. Automatic mode falls back
  to software; explicit hardware modes fail rather than change backends.
  Cross-output recording remains disabled.
- Software H.264, H.265, VP8, VP9 and AV1 are exercised through real encoding,
  finalization and FFmpeg decoding, including MP4/WebM compatibility. Hardware
  tests exercise an available backend or explicitly reject an unusable device;
  unavailable NVENC/QSV plugins are skipped, not claimed as validated.
  Cancellation during preflight and unsupported-dimension automatic fallback
  are covered, as is simulated helper-crash fallback. Small hardware HEVC regions
  are rejected (software is used in auto mode) to avoid a demonstrated VA plugin
  assertion below 128×128. Native compositor/hardware combinations still need manual checks.
- On the available Intel VA-API device, H.264 and H.265 synthetic recording,
  pause/resume, finalization and decoding succeeded using `vah264enc` and
  `vah265enc`. This validates those encoder paths, not compositor throughput.
- Selected audio-device/app loss now finalizes existing output instead of
  discarding it. Tests cover running/paused device errors, initial device/setup
  failure cleanup, app closure, output moves, identity rejection and private
  Pulse server disconnection. No replacement device/output is selected.
  Metadata is periodically revalidated as well as subscribed: a move event can
  precede the server's updated routing information.
- Real manual checks are still required for overlap/workspace isolation,
  fractional scales/rotation, throughput, audio source removal, lock signals,
  disk-full behavior and audiovisual synchronization on each compositor.

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
   clipboard. In frozen window mode, confirm that the pixels are from target
   selection time even though only an in-place highlight is displayed.
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

Third-party overview transformations are not integrated. The local
`hyprland-scroll-overview` patches were investigated read-only; no plugin or
configuration changes were made. Overview picking remains unsupported rather
than being certified by normal-desktop tests.

PipeWire, lock handling and notification actions need separate validation when
those features exist. Real live synchronization and isolated-window checks
above remain outstanding on both compositors.
