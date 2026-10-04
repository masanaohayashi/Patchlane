# Patchlane

Audio routing for macOS on Apple Silicon. Route audio from apps and devices to up to four stereo outputs, with input gain, channel selection, and live level meters.

![Patchlane in dedicated driver mode](docs/images/patchlane.png)

## Features

- Four stereo inputs routed independently to Main, Aux 1, Aux 2, and Aux 3.
- Per-output stereo dipole with a one-click bypass, 31-band GEQ, and named presets.
- Built-in Patchlane 2ch and Patchlane 8ch virtual audio devices.
- Dedicated driver mode for a direct shared-memory audio path to output devices.
- Adjustable sample rate, buffer size, and additional buffering.
- Output sliders control the selected device's volume. Devices without volume control show a disabled slider.
- Automatic reconnection after interruptions or sustained underruns.
- English and Japanese localization, with light and dark appearance following macOS.
- Automatic settings persistence and restoration of dedicated driver mode at launch.

## Requirements

- macOS 13 or later
- A Mac with Apple Silicon

## Installation

Download a distribution ZIP from [Releases](https://github.com/masanaohayashi/Patchlane/releases), when available, and run `Patchlane-Installer.pkg`. The installer includes the app and both virtual audio devices. Audio may stop briefly during installation.

To uninstall, run the included `Patchlane Uninstaller.app`. Saved settings and other vendors' audio drivers are preserved.

## Getting started

1. Choose an input device and its left/right channels.
2. Use **SEND TO** to select the output buses for that input.
3. Select an output device and left/right channels for each bus you want to use.
4. Adjust the input gain and output device volume as needed.

The mixer starts automatically while the app is open.

### Route audio from another app

Set the source app's output—or the macOS sound output—to **Patchlane 2ch** or **Patchlane 8ch**, then select the same device as an input in Patchlane. The two virtual devices are independent; the source and input selections must match.

Use **Patchlane 2ch** for stereo workflows, including QuickTime screen recording with a stereo audio source.

### Dedicated driver mode

Enable **Dedicated driver** to route Input 1 from a Patchlane virtual device directly to the selected outputs. Inputs 2–4 are disabled in this mode. Input gain, routing, channel selection, and meters remain available in the same interface.

Turning this mode on stops the normal mixer before connecting. Turning it off automatically resumes the normal mixer. Changing connection settings reconnects the audio path with the new settings.

### Stereo dipole and GEQ

Each output card has a **Dipole** switch, a preset picker, and a settings button. Effects default to **off**, so existing routes remain unchanged. Both normal mixer and dedicated driver modes support the effect.

In the settings window, enter the actual speaker centre spacing, listening distance, head width and boost limit. The generated filter preserves the electrical Mid/Side magnitude below 100Hz and transitions to regularized cancellation between 100 and 300Hz. It is an approximate head model, not a measured room or individual HRTF calibration.

The linked-stereo 31-band GEQ covers 20Hz–20kHz with ±12dB per band and an input trim. The signal order is **existing correction → GEQ → dipole**. The common scalar correction commutes with the linked-stereo GEQ, allowing correction and the symmetric dipole matrix to share one FIR processor. GEQ remains a separate IIR stage to preserve exact low-band gain. Changes apply during playback after a brief synthesis debounce and fade; toggling off skips FFT and GEQ processing after the release fade.

Selecting a preset fills the preset name field. Enter a name and choose **Save**; an existing name requires overwrite confirmation (including case-insensitive matches). Each physical output device remembers its own enabled state, preset and parameters by its stable Core Audio UID. Switching devices restores that device’s last settings before applying the effect; previously unconfigured devices start with Dipole off and default parameters. The same device selected in another Main/Aux output uses the same remembered state. Saving an existing name updates the preset, all linked outputs and remembered assignments for disconnected devices. Editing a parameter makes that output Custom without changing other outputs. **Delete selected preset** removes the named entry and clears its output and remembered device links, retaining those outputs' last settings as Custom. Presets, links and enabled state persist in settings.json and are included in the existing settings format; files from before dipole support load with dipole disabled. Earlier dipole settings migrate to the currently selected device; device associations that were never stored cannot be reconstructed.

The effect applies no automatic attenuation or limiter. Boosted signals can clip at the existing output clamp; input trim and GEQ change gain only when explicitly adjusted.

The C++ processor uses two Mid/Side paths, 8192 FIR taps, 256-frame partitions, Accelerate FFT, positive-frequency-only convolution, and ARM NEON complex MAC/stereo biquads. Zero-gain GEQ bands are omitted. Preparation and memory reclamation happen on serialized control queues; the callback neither allocates nor takes locks. The included model is computed locally, with no Wareing IR redistribution.

The Mid and Side filters use standard homomorphic minimum-phase reconstruction: log magnitude → inverse FFT (real cepstrum) → causal cepstral folding → FFT → complex exponential → inverse FFT. A 32768-point synthesis grid and one-sided tail taper produce 8192 taps with no added FIR design delay. This preserves the two modal magnitude targets but changes their relative phase; it is an experimental approximation and can change crosstalk cancellation. It does not reproduce a verified Hamada-specific implementation.

The effect adds **256 samples** of fixed buffering while enabled (about **5.8ms at 44.1kHz**, **5.3ms at 48kHz**), separately from routing/device latency. Minimum-phase filters still have frequency-dependent group delay; these numbers are the FFT block buffering, not a constant total delay at all frequencies. Disabled outputs add no effect delay. Outputs with different effect states are therefore not time-aligned.

### Buffering and latency

Both modes retain one base buffer. **Extra buffers: 0** means one buffer in total; **Extra buffers: 1** means two, and so on.

The displayed latency is an estimate of internal buffering:

```text
(buffer size × (extra buffers + 1)) / sample rate
```

For example, 32 samples at 44.1 kHz with no extra buffers corresponds to about 0.73 ms. This is not measured end-to-end latency: device latency and callback timing also contribute. Increase buffering if your setup produces dropouts.

Settings are saved automatically in `~/Library/Application Support/Patchlane/`.

## Build from source

Requires Xcode Command Line Tools with Swift 5.9 or later. There are no external Swift package dependencies.

```sh
git clone https://github.com/masanaohayashi/Patchlane.git
cd Patchlane
bash scripts/build-app.sh
```

The Release app is written to `build/Patchlane.app`. Local builds use ad-hoc signing by default. For dedicated driver connections, place the app at `/Applications/Patchlane.app` and install the dedicated driver components.

To build the installer and uninstaller:

```sh
bash scripts/package-driver.sh
```

This produces `build/Patchlane-Installer.pkg` and `build/Patchlane Uninstaller.app`.

## Tests

```sh
swift test -j 1
bash scripts/test-driver.sh
bash scripts/test-realtime.sh
python3 scripts/test-uninstaller.py
```

## License

[MIT](LICENSE) © 2026 Masanao Takeuchi.
