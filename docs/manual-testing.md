# Manual acceptance checklist

Status: automated tests are passing, but **not every milestone is complete or
certified**. Use this checklist on Hyprland and niri separately. Record the
compositor version, Snipline version, output layout/scales/rotations, GPU and
notification/tray host with each result.

## Known gaps — do not mistake these for completed features

- Portal screenshot/recording fallback is not implemented.
- Cross-monitor **screenshots** are supported; cross-monitor **recording** is not.
- Cursor toggles are UI-only; a cursor-rendering capture backend is not implemented.
  QR/OCR must still exclude cursor pixels.
- Generic editor handoff does not track arbitrary edited outputs or preserve an
  original against modifications made by the editor itself.
- Catbox/Litterbox currently reject some requests (including HTTP 403/412 and
  empty responses). Lens cannot open a search until its configured host returns
  a valid public image URL. No silent fallback to another host is used.
- GitHub releases require repository/remote setup; the root PKGBUILD's
  repository/checksum placeholders must be populated before building a release.
- Real mixed-DPI pointer grabs, window isolation, recording/audio, tray-host
  rendering and notification behavior still require the tests below.

## Preparation

- [ ] Build the latest source and quit any older resident with `snipline quit`
      before starting the new binary. Confirm `snipline --version`.
- [ ] Save `snipline doctor` output. This command must not capture pixels.
- [ ] Use non-sensitive test content. Use a generated image, not a desktop
      screenshot, when testing public uploads or Lens.
- [ ] Use a temporary capture directory and test configuration. Do not alter your
      production compositor configuration just to run these checks.

## 1. Screenshots — highest priority

Repeat the following in **live and frozen** modes using
`snipline capture region --live` / `--frozen`.

- [ ] Capture a region entirely inside each monitor. Each overlay must use that
      monitor's coordinates; frozen backgrounds must show the correct monitor.
- [ ] Drag from monitor A to B, then B to A. The selected rectangle must remain
      continuous and match the exported crop, with no jumps or clipping at the seam.
- [ ] Repeat with equal scales, mixed scales (for example 100%/150% or 125%/200%),
      an output to the left/above the origin, and a rotated monitor, where available.
- [ ] Select across all available monitors and across a desktop gap. PNG output
      must preserve monitor positions and leave the gap transparent.
- [ ] Check exported dimensions: logical selection width/height multiplied by the
      **highest intersected output scale**, rounded up. An unselected higher-DPI
      monitor must not change the export density.
- [ ] Inspect text/grid content around seams: no missing strips, double rotation,
      unintended overlaps or distorted crops. Lower-density content may be upscaled.
- [ ] Move and resize a cross-monitor selection in both directions.
- [ ] Test `snipline capture monitor` on each output. Selecting a monitor must
      not save immediately; confirm explicitly.
- [ ] In live mode, move a test window after opening the selector: the result
      must contain its new position and no toolbar, handles or popup pixels.
- [ ] In frozen mode, the result must retain the original frozen pixels.
- [ ] Cancel with Escape: no saved file or clipboard replacement.
- [ ] If you deliberately change the output layout during selection, the job
      must fail clearly rather than save using stale coordinates. Only do this
      manually when convenient; no desktop configuration changes are required.
- [ ] Repeat exports in PNG, JPEG and WebP. Check filenames, unique collision
      handling, chosen directory and clipboard pasting into another application.

## 2. Isolated windows and tool switching

- [ ] Capture a window partly/fully covered by another window. Only the selected
      window's content may appear, never the covering window or desktop.
- [ ] Test two windows with identical titles; the selected identity must remain stable.
- [ ] Resize, move or change workspace while selecting. Close the selected window:
      the job must fail, never substitute a monitor crop or another window.
- [ ] Switch Capture/Record/QR/OCR/Lens and Region/Monitor/Window in place.
      Verify the compact layout, selection and toolbar position remain sensible.
- [ ] Confirm Ctrl+1–5, Shift+1–3, Ctrl+L, Space/Enter and configured overrides.
      Result shortcuts must not fire while editing a popup/text input.

## 3. Recording and audio

- [ ] Record a monitor, a single-output region, and an isolated window.
- [ ] Verify countdown, pause/resume, elapsed time, finish and discard.
      Discard must publish no recording.
- [ ] Hide and reopen controls. This must not pause recording or lose the target.
      Visible controls may be recorded; do not assume universal HUD exclusion.
- [ ] Play MP4, WebM and GIF outputs. Check dimensions, duration, image quality
      and the selected available codec/encoder. GIF is silent.
- [ ] Resize a recorded window: the fixed canvas must fit it without distortion.
- [ ] Enable microphone, system audio and both; verify selected sources and
      audiovisual synchronization. Audio selection stays in the recording UI.
- [ ] Test application-only audio with another app playing: the other app must
      not leak into the recording. Missing/failed app capture must never silently
      become whole-output audio.
- [ ] If safe, remove a selected audio source or close the recorded window.
      Expect clear warning/finalization or failure, not source substitution.
- [ ] With a disposable recording, lock the session. It must stop/finalize
      appropriately; old notification actions must not execute behind the lock.
- [ ] Test explicit unsupported hardware selection: fail clearly. Automatic
      encoder selection may use the documented software fallback.

Do not test disk-full behavior by filling your real home filesystem. Use an
isolated filesystem if doing additional failure testing.

## 4. QR and OCR

- [ ] Recognize generated QR/barcodes and clear text, from selections and local images.
- [ ] No results window opens. Successful text/payload is copied and notified.
- [ ] No code/empty text/error/cancellation preserves the previous clipboard.
- [ ] QR links open only through an explicit Open action.
- [ ] Position the cursor over the source: cursor pixels must not appear in
      recognition input. There must be no QR/OCR cursor option.
- [ ] Check installed-language selection and live/frozen CLI flags.

## 5. Save, editor and notification actions

- [ ] Plain Save/Capture produces the configured notification and applicable
      Open/Open Folder/Edit/Upload actions. No Lens action is shown.
- [ ] Direct Upload, Lens, Edit, Open and Open Folder do **not** additionally show
      a saved-file notification.
- [ ] Screenshot and recording editors use their independent configured commands.
      Test a filename containing spaces with `{}`, and file-content stdin without it.
- [ ] Without an editor, Annotate/Edit is disabled in-app and omitted from notifications.
- [ ] Open/Open Folder use the correct original file/location even after another capture.
- [ ] Test notifications with your actual host (including DMS/Noctalia if used):
      buttons, image hints, dismissal and expiration.

## 6. Uploads and Lens

Public-host acceptance is currently an unresolved dependency, not a passing test.

- [ ] Use a generated PNG to test each host and any trusted custom `.sxcu`.
      Save stdout logs on failure, but redact credentials and private URLs.
- [ ] Changing destinations/loading configuration must never start an upload.
- [ ] Selection labels contain only the uploader name and retention where applicable.
- [ ] On success, the URL is copied and the completion notification offers **Open only**.
      Ordinary uploads must not automatically open a browser.
- [ ] Lens automatically opens the correctly encoded Google Lens URL after a
      successful upload; it must not replace the clipboard.
- [ ] Confirm there are no additional upload/Lens confirmation dialogs.
- [ ] Test cancellation and network failure. Originals remain; no other host is tried.
      Cancellation cannot erase data already accepted by a remote host.
- [ ] Failure notifications expire, including with `notificationTimeout: 0`.
- [ ] Cancelling an upload during recording must not discard the recording.

## 7. Background service, recent files and packaging

- [ ] With no resident, invoke capture. Invoke a second command while resident:
      it must use the same instance, not create another selector/service.
- [ ] Test `snipline daemon --tray` with your tray host. Try capture, recording
      controls, recent-file Open and Cancel upload. No automatic capture on startup.
- [ ] `snipline recent` contains at most 32 deduplicated successful saves and is
      hidden while locked. Restarting forgets it: metadata is memory-only.
- [ ] `snipline forget` / Forget recent files must not delete saved files.
- [ ] Test Quit/SIGTERM with a disposable recording: finalize safely and exit.
- [ ] If opting into startup, use **one** documented mechanism, then log in again.
      The package must not enable startup or edit bindings by itself.
- [ ] After configuring the real repository, test release generation, source
      checksums, `.SRCINFO`, package installation and launching installed helpers.
      The package must not build, run or install tests.

## Automated regression command

```sh
cmake --build build -j 4
ctest --test-dir build --output-on-failure
python -m unittest discover -s packaging -p 'test_*.py'
```

The public-upload tests are opt-in and are not included as a passing service
availability claim. Automated/offscreen tests do not replace real compositor checks.
