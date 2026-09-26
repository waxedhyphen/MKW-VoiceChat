# MKW VoiceChat source integration stage 3.
#
# The installer provisions pinned Opus, SpeexDSP, miniaudio, libdatachannel,
# libjuice, usrsctp, plog and Mbed TLS source trees under MKWVC_DEP_ROOT.
# WiiCompiled's shipped LLVM-MinGW toolchain builds the existing MKW-VoiceChat
# audio, signaling and ICE sources locally. libdatachannel media transport is
# disabled because voice media already travels as our own Opus packets over a
# WebRTC DataChannel.

if(NOT MKW_PLATFORM_WINDOWS)
    return()
endif()

set(MKWVC_DEP_ROOT "$ENV{MKWVC_DEP_ROOT}")
if(NOT MKWVC_DEP_ROOT)
    message(FATAL_ERROR "MKWVC_DEP_ROOT is not set; run the WiiCompiled Voicechat installer")
endif()

set(MKWVC_OPUS_DIR "${MKWVC_DEP_ROOT}/opus")
set(MKWVC_SPEEXDSP_DIR "${MKWVC_DEP_ROOT}/speexdsp")
set(MKWVC_MINIAUDIO_DIR "${MKWVC_DEP_ROOT}/miniaudio")
set(MKWVC_DATACHANNEL_DIR "${MKWVC_DEP_ROOT}/libdatachannel")
set(MKWVC_MBEDTLS_DIR "${MKWVC_DEP_ROOT}/mbedtls")

foreach(required
    "${MKWVC_OPUS_DIR}/CMakeLists.txt"
    "${MKWVC_SPEEXDSP_DIR}/libspeexdsp/preprocess.c"
    "${MKWVC_MINIAUDIO_DIR}/miniaudio.h"
    "${MKWVC_DATACHANNEL_DIR}/CMakeLists.txt"
    "${MKWVC_DATACHANNEL_DIR}/deps/libjuice/CMakeLists.txt"
    "${MKWVC_DATACHANNEL_DIR}/deps/usrsctp/CMakeLists.txt"
    "${MKWVC_DATACHANNEL_DIR}/deps/plog/CMakeLists.txt"
    "${MKWVC_MBEDTLS_DIR}/CMakeLists.txt")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "MKW VoiceChat dependency source is missing: ${required}")
    endif()
endforeach()

# Opus 1.6.1.
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(
    "${MKWVC_OPUS_DIR}"
    "${CMAKE_BINARY_DIR}/mkwvc-opus"
    EXCLUDE_FROM_ALL)

# SpeexDSP 1.2.1 floating-point build.
add_library(mkwvc_speexdsp STATIC
    "${MKWVC_SPEEXDSP_DIR}/libspeexdsp/buffer.c"
    "${MKWVC_SPEEXDSP_DIR}/libspeexdsp/fftwrap.c"
    "${MKWVC_SPEEXDSP_DIR}/libspeexdsp/filterbank.c"
    "${MKWVC_SPEEXDSP_DIR}/libspeexdsp/jitter.c"
    "${MKWVC_SPEEXDSP_DIR}/libspeexdsp/mdf.c"
    "${MKWVC_SPEEXDSP_DIR}/libspeexdsp/preprocess.c"
    "${MKWVC_SPEEXDSP_DIR}/libspeexdsp/resample.c"
    "${MKWVC_SPEEXDSP_DIR}/libspeexdsp/scal.c"
    "${MKWVC_SPEEXDSP_DIR}/libspeexdsp/smallft.c")
target_include_directories(mkwvc_speexdsp
    PUBLIC "${MKWVC_SPEEXDSP_DIR}/include"
    PRIVATE "${MKWVC_SPEEXDSP_DIR}/libspeexdsp")
target_compile_definitions(mkwvc_speexdsp PRIVATE
    FLOATING_POINT
    USE_SMALLFT
    VAR_ARRAYS
    EXPORT=)
target_compile_options(mkwvc_speexdsp PRIVATE -O2 -w -pipe)
set_target_properties(mkwvc_speexdsp PROPERTIES
    UNITY_BUILD OFF
    C_EXTENSIONS ON)

# Mbed TLS 3.6.5 supplies libdatachannel's TLS/DTLS crypto backend. Keep this
# library-only/static; the upstream GitHub archive is prepared by the installer
# with the same empty framework placeholder used by vcpkg.
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
# The release archive intentionally omits mbedtls-framework. With GEN_FILES
# off, the library build does not need it; suppress Mbed TLS' optional Python
# config-warning probe so scripts/config.py never imports the absent framework.
# Python may already have been found by WiiCompiled/Aurora, so hide both the
# package lookup and any inherited result while Mbed TLS configures.
set(_MKWVC_SAVED_PYTHON3_FOUND "${Python3_Interpreter_FOUND}")
set(_MKWVC_SAVED_PYTHON3_EXECUTABLE "${Python3_EXECUTABLE}")
set(CMAKE_DISABLE_FIND_PACKAGE_Python3 TRUE CACHE BOOL "" FORCE)
set(Python3_Interpreter_FOUND FALSE)
set(Python3_EXECUTABLE "")
set(MBEDTLS_PYTHON_EXECUTABLE FALSE)
add_subdirectory(
    "${MKWVC_MBEDTLS_DIR}"
    "${CMAKE_BINARY_DIR}/mkwvc-mbedtls"
    EXCLUDE_FROM_ALL)
set(CMAKE_DISABLE_FIND_PACKAGE_Python3 FALSE CACHE BOOL "" FORCE)
set(Python3_Interpreter_FOUND "${_MKWVC_SAVED_PYTHON3_FOUND}")
set(Python3_EXECUTABLE "${_MKWVC_SAVED_PYTHON3_EXECUTABLE}")
target_compile_definitions(mbedtls PUBLIC MBEDTLS_SSL_DTLS_SRTP)
if(TARGET mbedtls AND NOT TARGET MbedTLS::MbedTLS)
    add_library(MbedTLS::MbedTLS ALIAS mbedtls)
endif()

# libdatachannel 0.24.5. Its three git-submodule dependencies are provisioned
# into deps/ by the installer. NO_MEDIA removes the libSRTP/media path; WebSocket
# support stays enabled because the existing SignalingClient uses rtc::WebSocket.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BUILD_SHARED_DEPS_LIBS OFF CACHE BOOL "" FORCE)
set(USE_GNUTLS OFF CACHE BOOL "" FORCE)
set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(USE_NICE OFF CACHE BOOL "" FORCE)
set(PREFER_SYSTEM_LIB OFF CACHE BOOL "" FORCE)
set(NO_WEBSOCKET OFF CACHE BOOL "" FORCE)
set(NO_MEDIA ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(RTC_UPDATE_VERSION_HEADER OFF CACHE BOOL "" FORCE)
set(PLOG_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(PLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(sctp_build_shared_lib OFF CACHE BOOL "" FORCE)
set(sctp_build_programs OFF CACHE BOOL "" FORCE)
set(sctp_werror OFF CACHE BOOL "" FORCE)
# CMake 4 removed compatibility with projects declaring a minimum below 3.5.
# The installer normalizes usrsctp to 3.5 as well, and this policy floor keeps
# nested dependency configuration deterministic if upstream metadata differs.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
add_subdirectory(
    "${MKWVC_DATACHANNEL_DIR}"
    "${CMAKE_BINARY_DIR}/mkwvc-datachannel"
    EXCLUDE_FROM_ALL)

# Existing standalone implementation sources. Nothing is reimplemented here.
add_library(mkw_voicechat_core_stage3 STATIC
    src/AdaptiveJitterBuffer.cpp
    src/AudioEngine.cpp
    src/AudioProcessor.cpp
    src/EmbeddedCore.cpp
    src/IcePeerTransport.cpp
    src/IceSignal.cpp
    src/MicrophoneTest.cpp
    src/NetworkSimulator.cpp
    src/OpusCodec.cpp
    src/SignalingClient.cpp
    src/UdpVoiceTransport.cpp
    src/VoiceClient.cpp)

target_include_directories(mkw_voicechat_core_stage3
    PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../include"
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../third_party/toml11"
    PRIVATE "${MKWVC_MINIAUDIO_DIR}")
target_compile_features(mkw_voicechat_core_stage3 PRIVATE cxx_std_23)
target_compile_definitions(mkw_voicechat_core_stage3 PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(mkw_voicechat_core_stage3 PRIVATE -O2 -w -pipe)
add_subdirectory(third_party/rnnoise)

target_link_libraries(mkw_voicechat_core_stage3 PUBLIC
    mkwvc_rnnoise
    Opus::opus
    mkwvc_speexdsp
    LibDataChannel::LibDataChannelStatic
    ole32
    winmm)
set_target_properties(mkw_voicechat_core_stage3 PROPERTIES
    UNITY_BUILD OFF
    CXX_EXTENSIONS OFF)

