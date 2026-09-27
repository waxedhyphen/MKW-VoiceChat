# Third-Party Notices

MKW VoiceChat is distributed under the GNU General Public License version 3. See `LICENSE`.

## WiiCompiled

The WiiCompiled integration targets the official WiiCompiled project by patchzyy and is designed against WiiCompiled 0.2.32.

Upstream: https://github.com/patchzyy/Wiicompiled

WiiCompiled is licensed under GPL v3.0. This repository does not redistribute Mario Kart Wii, Nintendo code, Nintendo assets, game data, or Retro Rewind content.

## RNNoise

RNNoise source is vendored under `third_party/rnnoise/`.

The complete RNNoise redistribution terms and copyright notices are preserved in `third_party/rnnoise/COPYING`. Release builds also publish that license text as `RNNoise-LICENSE.txt`.

## Other dependencies

The project also uses dependencies obtained from their official upstream sources or through vcpkg/build tooling, including Dear ImGui, GLFW, miniaudio, Opus, SpeexDSP, libdatachannel, libjuice, usrsctp, plog, and Mbed TLS.

Those components retain their respective upstream licenses. Version and acquisition details are defined in `vcpkg.json` and the installer dependency provisioning source. Their upstream license terms remain applicable to any redistributed source or binary that includes them.
