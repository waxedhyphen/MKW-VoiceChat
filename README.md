# MKW VoiceChat

Room-based voice chat for Retro Rewind running on [WiiCompiled](https://github.com/patchzyy/Wiicompiled).

Everyone in the same Retro Rewind online room who also runs MKW VoiceChat is connected automatically. No room codes, no separate app.

> [!IMPORTANT]
> This is an **unofficial, independent** project. It is **not** affiliated with, endorsed by or supported by WiiCompiled, Wheel Wizard or Retro Rewind, and **absolutely not** by Nintendo. Do not report issues with this project to any of them.

## AI usage and project intentions

Large parts of this repository were generated with AI, including the native runtime, the installer/patcher and the CI/release workflows. The commit history was recreated and ported with AI from a private repository and does not reflect the original development history. The project is neither optimized nor technically polished and is not intended for merging into the real WiiCompiled.

## About the installer

[The WiiCompiled README](https://github.com/patchzyy/Wiicompiled/blob/main/README.md) tells users to only use builds from its official Releases page and to avoid third-party installers. This project still provides an installer for convenience. And again: it's **not** an official WiiCompiled build.

If you don't trust it, don't use it, or read the source and build it yourself (see [Building](#building)).

![MKW VoiceChat Demo](https://media1.tenor.com/m/IF58iPssQxsAAAAd/tung-sahur-side-eye-tung-tung-tung-sahur.gif)

## Features

- Automatic voice rooms based on your current Retro Rewind room
- Opus, 48 kHz mono, 20 ms frames, FEC and packet-loss concealment
- Peer-to-peer over WebRTC (libdatachannel, DTLS-encrypted), TURN relay fallback when configured
- RNNoise noise suppression, noise gate, compressor, automatic normalization, mic gain
- Voice activation, push-to-talk, push-to-mute, mute and deafen with keyboard/controller hotkeys
- Per-player volume and mute
- Mute presets: everyone, friends only, everyone but friends, teammates, everyone but teammates, new players
- Speaker overlay with friend/muted/deafened state and a local status indicator
- Microphone loopback test and output test tone
- Update check in-game, separate updater with SHA-256 verification

## Supported versions

Defined in [`version.json`](version.json):

| | |
|---|---|
| MKW VoiceChat | 0.14.3 |
| Protocol | 1 |
| WiiCompiled base | 0.2.32 |

The installer only builds against this exact WiiCompiled version. When a new WiiCompiled version is released, installing is blocked until an MKW VoiceChat update supporting it is available.

Until then, the existing Voicechat build will most likely not be able to connect to WFC, so you'll have to play the official build through Wheel Wizard without voice chat. You don't need to uninstall the Voicechat build or clean up any files. The next MKW VoiceChat release will update everything automatically.

## Installation

Requirements: Windows 10/11 x64, [Wheel Wizard](https://github.com/TeamWheelWizard/WheelWizard) with a clean PAL `RMCP01` image configured, and Retro Rewind installed/launched once. WiiCompiled does not need to be installed, the installer sets it up automatically. An existing WiiCompiled installation is never removed or replaced. MKW VoiceChat is built as a separate copy, so you end up with both.

1. Download `WiiCompiled-VoiceChat-Installer.exe` from [Releases](../../releases/latest).
2. Close the game and run the installer, choose **Install / Update**.
3. Start the game through the **Wiicompiled (Voicechat)** desktop shortcut.

> [!WARNING]
> Wheel Wizard's normal Retro Rewind button launches the official build **without** voice chat.

What the installer does:

- Installs, verifies or updates the official WiiCompiled base and Retro Rewind as needed
- Applies the voice patch to a copy, downloads pinned dependency sources and compiles a separate build locally
- Installs to `%APPDATA%\CT-MKWII\Recomp\MKWVoiceChat` (or Wheel Wizard's configured AppData location)
- Creates the desktop shortcut and installs the updater

Installer menu: **Install / Update**, **Repair / Repatch** (forces a clean rebuild), **Uninstall** (removes only the voice build and shortcut). Logs are written to `...\MKWVoiceChat\logs`.

CLI: `WiiCompiled-VoiceChat-Installer.exe [run|check|reconcile|patch|repair|update|launch|unpatch] [--wait-pid PID]`

## Usage

Press **F10** in-game and open **Voice Chat**. Everything is configured there: enable/disable, input/output device, processing, activation mode, hotkeys, mute presets, overlay and the per-player list. Settings are saved in the `[voicechat]` section of WiiCompiled's `Config.toml`.

## Privacy

- Voice is sent peer-to-peer, like Retro Rewind's own online play. The client only connects to players in your current Retro Rewind room, who can already see your IP address through in-game traffic. Room membership is not authenticated yet though, so a modified client could join a voice room by claiming a player ID from the public RWFC room list and see the IP addresses of its voice participants.
- Your Retro Rewind profile ID, name and friend code are sent to the signaling server to match you with your room. The session key is kept in memory only and is never saved or logged.

## Known limitations

- Windows only.
- Room membership is based on the public RWFC room list and is **not authenticated** yet. The [`rr_verifier`](rr_verifier/) bridge exists, but RR GPSP is not publicly reachable.
- The signaling server runs on Cloudflare Workers Free and can run out of daily requests.

## Building

### Installer

Requires .NET 8 SDK, Python 3.10+, CMake and Visual Studio 2022 with the C++ workload.

```powershell
dotnet publish patcher/MKWVoiceChat.Patcher/MKWVoiceChat.Patcher.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -o dist
```

The build publishes and embeds the updater and downloads the pinned RNNoise model (SHA-verified) via `tools/prepare_rnnoise_model.py`.

Self-tests (same as CI):

```powershell
dotnet run --project patcher/MKWVoiceChat.Patcher -c Release -- selftest-version
dotnet run --project patcher/MKWVoiceChat.Patcher -c Release -- selftest-bootstrap
git clone --depth 1 --branch v0.2.32 https://github.com/patchzyy/Wiicompiled.git upstream
dotnet run --project patcher/MKWVoiceChat.Patcher -c Release -- selftest-patch upstream
```

### Standalone test client

Dev/debug client (ImGui) with LAN UDP, room codes, manual ICE, network simulation and codec diagnostics. Dependencies come from `vcpkg.json` (manifest mode).

- Windows: `msbuild MKW-VoiceChat.sln -p:Configuration=Release -p:Platform=x64` (requires `vcpkg integrate install`)
- macOS: `./build-macos.sh` (output: `bin/mkw_voicechat`)

### Audio regression tests

```sh
cmake -S tests -B audio-tests -DMKWVC_TEST_SPEECH=path/to/speech.wav -DCMAKE_BUILD_TYPE=Release
cmake --build audio-tests --config Release
ctest --test-dir audio-tests -C Release --output-on-failure
```

### Releases

Push a tag `vX.Y.Z` matching `productVersion` in `version.json`. The release workflow runs all tests, builds the installer and publishes it together with `mkwvc-release.json` and `SHA256SUMS.txt`.

## Server components

| Directory | Purpose |
|---|---|
| [`signaling_worker/`](signaling_worker/) | Production signaling (Cloudflare Workers + Durable Objects) |
| [`signaling_server/`](signaling_server/) | Local Python signaling server for development |
| [`turn_server/`](turn_server/) | coturn deployment (Docker) for relay fallback |
| [`rr_verifier/`](rr_verifier/) | Retro Rewind session verifier (not usable in production yet) |

Each directory has its own README with deployment steps.

## Source layout

| Path | Content |
|---|---|
| `include/`, `src/` | Voice core (audio, codec, jitter buffer, transports, signaling) and standalone client |
| `patcher/MKWVoiceChat.Patcher/` | Installer and WiiCompiled integration |
| `patcher/MKWVoiceChat.Updater/` | Update bootstrap |
| `patcher/payload/runtime/` | Code injected into WiiCompiled's runtime |
| `third_party/rnnoise/` | Vendored RNNoise |
| `tests/` | Audio regression tests |
| `version.json` | Single source of version identity |

## License

GPL-3.0, same as WiiCompiled. Third-party components keep their own licenses, see [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md) and [`third_party/rnnoise/COPYING`](third_party/rnnoise/COPYING).

No Nintendo code, assets or game data are included in this repository or its releases.
