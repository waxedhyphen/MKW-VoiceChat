# MKW Voice Chat

Voice chat integration for Mario Kart Wii / Retro Rewind running through WiiCompiled.

The project provides real-time peer-to-peer voice, room-aware player handling, per-player audio controls, noise suppression, mute/deafen controls and an installer/update path that builds a separate WiiCompiled Voice Chat product without replacing the normal WiiCompiled installation.

## Current release identity

The build identity is maintained in `version.json`.

- MKW Voice Chat: 0.14.2
- Protocol: 1
- Supported WiiCompiled base: 0.2.32

## Features

- 48 kHz mono voice with Opus
- WebRTC/ICE peer connectivity through libdatachannel
- Cloudflare Workers + Durable Objects signaling
- RNNoise-based noise suppression
- microphone gain, compressor and voice activity controls
- mute, deafen, push-to-talk and push-to-mute
- per-player volume and mute controls
- friend-aware player speaker display
- automatic Retro Rewind room integration
- persistent voice settings
- release compatibility checks
- separate lightweight updater with SHA-256 verification
- separate WiiCompiled Voice Chat product and desktop shortcut

## Installation

Published builds are distributed through GitHub Releases.

Run `WiiCompiled-VoiceChat-Installer.exe` and choose **Install / Update**. The installer detects the supported WiiCompiled/Retro Rewind installation, builds the Voice Chat product and creates a separate **Wiicompiled (Voicechat)** desktop shortcut.

The project does not include Mario Kart Wii, Nintendo, Retro Rewind or other proprietary game assets.

## Building the installer

Requirements:

- Windows x64
- .NET 8 SDK
- Visual Studio 2022 or newer with Desktop development with C++
- CMake
- Python 3.10 or newer
- Git
- vcpkg dependencies used by the native voice runtime

Publish the installer:

```powershell
dotnet publish ".\patcher\MKWVoiceChat.Patcher\MKWVoiceChat.Patcher.csproj" `
    -c Release `
    -r win-x64 `
    --self-contained true `
    -p:PublishSingleFile=true `
    -o ".\patcher-build"
```

The installer embeds the small `MKWVoiceChat-Updater.exe` bootstrap. The bootstrap only waits for the running game to exit, retrieves the latest release manifest, verifies the advertised installer SHA-256 and starts the verified full installer.

## Standalone native client

The repository also contains the native voice implementation and test client.

Open `MKW-VoiceChat.sln`, select `x64` and build `Debug` or `Release`.

Dependencies are declared in `vcpkg.json`.

## Signaling

The production signaling implementation is under `signaling_worker/` and targets Cloudflare Workers + Durable Objects.

No proprietary game files are required by the signaling service.

## Source layout

- `include/`, `src/` — native voice implementation
- `patcher/MKWVoiceChat.Patcher/` — installer and WiiCompiled integration
- `patcher/MKWVoiceChat.Updater/` — lightweight update bootstrap
- `patcher/payload/` — runtime integration payload applied to the supported WiiCompiled source
- `signaling_worker/` — Cloudflare signaling service
- `tests/` — native audio regression tests
- `third_party/rnnoise/` — pinned RNNoise source and license
- `version.json` — build identity source

## Licensing

This repository is distributed under the GNU General Public License v3.0. The WiiCompiled integration is based on GPL-3.0-licensed WiiCompiled source.

Third-party components retain their own licenses. The vendored RNNoise source is covered by the license in `third_party/rnnoise/COPYING`. Additional attribution and redistribution notes are listed in `THIRD-PARTY-NOTICES.md`.

## Disclaimer

This project is not affiliated with Nintendo, Retro Rewind or the WiiCompiled project. No copyrighted game files or game assets are distributed by this repository.
