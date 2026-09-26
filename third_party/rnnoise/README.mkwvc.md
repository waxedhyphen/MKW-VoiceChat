# RNNoise

Vendored runtime sources and built-in speech model from Xiph RNNoise v0.2, commit `904a876dce1f9ab8860c0a5000ed151f9f6eef58`:
https://github.com/xiph/rnnoise/tree/904a876dce1f9ab8860c0a5000ed151f9f6eef58

`CMakeLists.txt` is the project's static-library build adapter. Upstream copyright notices, AUTHORS and COPYING are retained. No model download, Python runtime or online service is needed by the voice client.

AudioProcessor passes 48 kHz mono floating-point PCM in the original int16 amplitude range, in 480-sample blocks. Suppression runs after the existing capture processing and final microphone gain. At partial strength the dry path is delayed by the same 960 samples as RNNoise before mixing; 0% and disabled remain exact immediate bypasses. The VAD return value does not gate or mute speech.

RNNoise is warmed once before microphone processing starts. Each processor owns independent recurrent state and fixed-size input/output buffers.

The full quantized model is reproduced at build time from https://media.xiph.org/rnnoise/models/rnnoise_data-0b50c45.tar.gz (SHA-256 `4ac81c5c0884ec4bd5907026aaae16209b7b76cd9d7f71af582094a2f98f4b43`). `tools/prepare_rnnoise_model.py` removes only the optional `#ifndef DISABLE_DEBUG_FLOAT` debugging arrays and verifies the generated source against the pinned Git blob SHA-1 `04638db60c30177c5f79c7b32ad429b2d435b691`. The model is built into the program; there is no runtime model download.

`src/os_support.h` is a project compatibility shim mapping the scalar/NEON vector helper to RNNoise's existing clear macro; the upstream v0.2 release references this header without shipping it. MSVC builds select the supported x64 SSE2 path and spell C restrict as __restrict.
