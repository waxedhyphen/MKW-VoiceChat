set(MKWVC_RNNOISE_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/src/rnnoise_data.c")
if(NOT EXISTS "${MKWVC_RNNOISE_MODEL}")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/prepare_rnnoise_model.py" --fetch "${MKWVC_RNNOISE_MODEL}"
        RESULT_VARIABLE MKWVC_RNNOISE_PREPARE_RESULT)
    if(NOT MKWVC_RNNOISE_PREPARE_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to prepare RNNoise model data")
    endif()
endif()

add_library(mkwvc_rnnoise STATIC
    src/denoise.c
    src/rnn.c
    src/rnnoise_data.c
    src/rnnoise_tables.c
    src/nnet.c
    src/nnet_default.c
    src/parse_lpcnet_weights.c
    src/pitch.c
    src/celt_lpc.c
    src/kiss_fft.c)
target_include_directories(mkwvc_rnnoise PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(mkwvc_rnnoise PRIVATE RNNOISE_BUILD _USE_MATH_DEFINES DISABLE_DEBUG_FLOAT)
set_target_properties(mkwvc_rnnoise PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS ON UNITY_BUILD OFF POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_link_libraries(mkwvc_rnnoise PUBLIC m)
endif()

if(MSVC)
    target_compile_definitions(mkwvc_rnnoise PRIVATE restrict=__restrict OPUS_X86_MAY_HAVE_SSE OPUS_X86_MAY_HAVE_SSE2)
endif()
