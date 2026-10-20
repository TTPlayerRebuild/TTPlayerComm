cmake_minimum_required(VERSION 3.24)
project(TtpComm LANGUAGES C CXX RC)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "ttpcomm requires MSVC and -A Win32")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(cmake/legacy_windows.cmake)
include(cmake/size_release.cmake)
# Apply the x86 baseline before creating any dependency target. Modern MSVC
# defaults to SSE2 even when the PE imports and subsystem remain XP-compatible.
# Explicit intrinsic fast paths in archive codecs still use their CPU dispatch;
# ordinary C/C++ code, including DSP and runtime wrappers, must work without SSE2.
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/arch:IA32>")
include(cmake/version.cmake)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
include(cmake/dependencies.cmake)
include(cmake/unrar.cmake)
include(cmake/archive_extended.cmake)
include(cmake/archive_seven.cmake)

# All 67 exports are recovered. Keep the CMake target name for build continuity;
# the release filename and PE module identity now match the original host ABI.
add_library(ttpcomm_core SHARED src/archive.cpp src/runtime.cpp src/extensions.cpp src/runtime_services.cpp src/runtime_pcm.cpp src/runtime_pcm_sse2.cpp src/runtime_quantizer.cpp src/security.cpp src/identity.cpp src/digest.cpp src/compression.cpp src/mpeg.cpp src/dream.cpp src/coolsb.cpp
  src/resampler.cpp src/replaygain.cpp src/equalizer.cpp src/surround.cpp src/dts.cpp src/spectrum.cpp src/abi_checks.cpp
  src/ttpcomm_core.def "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")
add_dependencies(ttpcomm_core ttpcomm_version)
target_sources(ttpcomm_core PRIVATE src/archive_zip.cpp src/archive_stream.cpp src/archive_extended.cpp src/archive_seven.cpp src/archive_group.cpp)
target_sources(ttpcomm_core PRIVATE src/audio_encoder.cpp src/audio_effects.cpp)
target_compile_definitions(ttpcomm_core PRIVATE UNICODE _UNICODE NOMINMAX
  WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0501 _CRT_SECURE_NO_WARNINGS)
target_compile_options(ttpcomm_core PRIVATE /W4 /permissive- /EHsc /utf-8 /fp:precise)
target_include_directories(ttpcomm_core PRIVATE include sdk/include "${dream_dir}")
set_source_files_properties(src/dream.cpp PROPERTIES COMPILE_OPTIONS "/arch:IA32")
# Preserve the host's hot PCM compilation policy, including the math intrinsic
# path used by gain/quantization. Cold tag/compression wrappers remain /O1.
set_source_files_properties(src/runtime_pcm.cpp src/runtime_quantizer.cpp PROPERTIES COMPILE_OPTIONS "/O2;/Ob2")
# Only PcmGain's CPU-checked branch can enter this separate implementation.
# Disable LTCG for it as well as inlining, so SSE2 instructions cannot be
# propagated into the IA32 dispatcher or its scalar fallback.
set_source_files_properties(src/runtime_pcm_sse2.cpp PROPERTIES COMPILE_OPTIONS "/arch:SSE2;/O2;/Ob2;/GL-")
# Only the terminal /GS reporter must not recursively instrument itself.
set_source_files_properties(src/security.cpp PROPERTIES COMPILE_OPTIONS "/GS-")
target_link_options(ttpcomm_core PRIVATE "/MAP:$<TARGET_FILE_DIR:ttpcomm_core>/ttpcomm.map")
target_link_libraries(ttpcomm_core PRIVATE ttpcomm_unrar ttpcomm_id3 ttpcomm_zlib ttpcomm_fft ttpcomm_ssrc ttpcomm_mpeg ttpcomm_coolsb ttpcomm_detours iphlpapi user32 gdi32 ole32 uuid)
target_link_libraries(ttpcomm_core PRIVATE archive_static ttpcomm_seven oleaut32 advapi32)
set_target_properties(ttpcomm_core PROPERTIES PREFIX "" OUTPUT_NAME "ttpcomm")
add_custom_command(TARGET ttpcomm_core POST_BUILD
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_abi.py"
    "$<TARGET_FILE:ttpcomm_core>" "${CMAKE_CURRENT_SOURCE_DIR}/abi/exports.json"
    --require-complete
    --report "$<TARGET_FILE_DIR:ttpcomm_core>/abi-report.json"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_legacy_imports.py"
    "$<TARGET_FILE:ttpcomm_core>"
    --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/5.1.2600.txt"
    --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/6.1.7600.txt"
    --report "$<TARGET_FILE_DIR:ttpcomm_core>/legacy-imports.json"
  COMMAND "${CMAKE_COMMAND}" "-DINPUT=$<TARGET_FILE:ttpcomm_core>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/write_hash.cmake" VERBATIM)
# Local differential tests live outside this repository, in rebuild/tests.
install(TARGETS ttpcomm_core RUNTIME DESTINATION . COMPONENT Runtime)
