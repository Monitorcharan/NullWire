# NullWire Aero UI

The Windows sender and Android receiver use a matching white/mint and dark/mint appearance. The UI uses the existing engine controls and keeps the streaming, discovery, protocol, microphone and native DSP implementations unchanged.

## Screens and controls

- Windows: Overview, Sound, Devices and Settings; persistent Light/Dark choice; audio output menu and manual phone IP; connect/disconnect; Auto-connect and FastPath; four DSP sliders and the original four presets; stereo levels, seven-band spectrum and waveform; optional visualizer display; keyboard Tab/Shift+Tab, Enter/Space and slider arrow/Home/End controls.
- Android: Stream, Sound, Devices and Settings; persistent Light/Dark choice applied without activity recreation; the existing listening/microphone actions, pairing identity and Wi-Fi toggle; bass/treble sliders and shortcuts that reuse their listeners; system media-volume panel; RMS level history from the existing meter. Theme changes preserve the same views, slider positions and listening state.
- The Android receiver exposes bass and treble only. Presence and spatial controls remain on Windows; the UI does not invent native support for them. The mobile profiles are documented shortcuts for those two sliders, not copies of the Windows four-parameter presets.
- The old Windows latency/jitter/buffer readouts and Android latency/loss strings were not measurements. The new presentation uses packet counts, audio format and live level data, and explains that the sender does not measure those other metrics. The telemetry/engine implementation itself is untouched.

## Integrating concurrent work

This branch starts at `f9c7295bb2c674b7286ce4f86f0023eeb98aaad3`. Apply it as a UI-only change over concurrent engine work; do not replace `main.cpp` or `MainActivity.kt` wholesale with older versions.

`sender/AeroUi.h` owns rendering, navigation, appearance preferences and keyboard routing. `sender/main.cpp` includes it in place of the old rendering functions and forwards UI events. The existing `AudioEngine` definition and telemetry synchronization function are unchanged.

`AeroAppearance.kt` owns Android theme/navigation/preset UI, and `AeroLevelView.kt` displays the existing RMS level history. `MainActivity.kt` retains the original engine and lifecycle calls; its changes bind these UI helpers, save the selected page, apply window insets and use theme-aware presentation colors. Existing XML control IDs are retained.

No release binaries, signing files, website, installers, native receiver code or production build configuration are part of this change.

## UI verification

The usual native Windows and Android build commands still apply. For a Windows UI smoke test, compile `sender/tests/aero_ui_smoke.cpp` as a console program with the same libraries used by `sender/build.bat`:

```powershell
g++ -std=c++20 -O1 sender/tests/aero_ui_smoke.cpp -lgdiplus -lavrt -lwinmm -lksuser -lpropsys -lole32 -lws2_32 -lgdi32 -lcomctl32 -lshell32 -ldwmapi -lmsimg32 -static -o ui-smoke.exe
./ui-smoke.exe
```

An optional directory argument saves all eight rendered Windows page/theme combinations. The harness checks control bounds, visualizer navigation state and keyboard IP editing without starting discovery or audio capture.

Android UI tests run with an opt-in Gradle init script, keeping production dependencies unchanged:

```powershell
gradle -p receiver -I ui-tests.gradle :app:testDebugUnitTest :app:assembleDebug
```

The Robolectric fixture inflates the actual layout in a UI-only activity. It checks both themes, all four pages, retained meter/slider values and preset callbacks without loading the native audio engine. Hardware playback and Android device rendering still require a device check.

The current baseline has three Android lint errors outside this UI change: two `MissingPermission` findings at microphone `AudioRecord` construction and one `NotificationPermission` finding at notification posting. They are intentionally left for the separate engine/behavior work.
