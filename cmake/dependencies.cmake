include(FetchContent)
# Release source archives carry these exact upstream trees. Ordinary repository
# builds still download the pinned archives below; no vendor sources are tracked.
foreach(_dependency zlib id3 kissfft ssrc mpeg dream detours)
  set(_bundled "${CMAKE_CURRENT_SOURCE_DIR}/third_party_sources/${_dependency}")
  string(TOUPPER "FETCHCONTENT_SOURCE_DIR_TTPCOMM_${_dependency}_SOURCE" _override)
  if(EXISTS "${_bundled}" AND NOT DEFINED ${_override})
    set(${_override} "${_bundled}" CACHE PATH "Source archive dependency")
  endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/adapt_id3.py" "${CMAKE_CURRENT_LIST_DIR}/adapt_fft.py")
FetchContent_Declare(ttpcomm_zlib_source
  URL https://codeload.github.com/madler/zlib/zip/refs/tags/v1.3.2
  URL_HASH SHA256=31fd9fee98812abcf147d0e103bc4d2f983c35a8d7a807a328a299f3a74e0050
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR unused)
FetchContent_Declare(ttpcomm_id3_source
  URL https://codeberg.org/tenacityteam/libid3tag/archive/0.16.4.tar.gz
  URL_HASH SHA256=2e9058af51e5f3881c13c55a9790abb9870812cc0f5917b6f3e825c6ae9b9f39
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR unused)
FetchContent_MakeAvailable(ttpcomm_zlib_source ttpcomm_id3_source)
FetchContent_Declare(ttpcomm_kissfft_source
  URL https://codeload.github.com/mborgerding/kissfft/zip/refs/tags/131.2.0
  URL_HASH SHA256=0fd8757f845acfdf178470be3435e6e5a65e8bfa2564bf2e5d3163be166121c1
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR unused)
FetchContent_MakeAvailable(ttpcomm_kissfft_source)
set(fft_dir "${CMAKE_CURRENT_BINARY_DIR}/fft-adapted")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/adapt_fft.py"
  "${ttpcomm_kissfft_source_SOURCE_DIR}" "${fft_dir}"
  COMMAND_ERROR_IS_FATAL ANY)
add_library(ttpcomm_fft STATIC "${fft_dir}/kiss_fft.c" "${fft_dir}/kiss_fftr.c")
target_include_directories(ttpcomm_fft PUBLIC "${ttpcomm_kissfft_source_SOURCE_DIR}")
target_include_directories(ttpcomm_fft PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(ttpcomm_fft PRIVATE /arch:IA32 /fp:precise)
set(zlib_sources adler32.c crc32.c deflate.c infback.c inffast.c inflate.c
  inftrees.c trees.c zutil.c compress.c uncompr.c)
list(TRANSFORM zlib_sources PREPEND "${ttpcomm_zlib_source_SOURCE_DIR}/")
add_library(ttpcomm_zlib STATIC ${zlib_sources})
target_include_directories(ttpcomm_zlib PUBLIC "${ttpcomm_zlib_source_SOURCE_DIR}")
target_compile_definitions(ttpcomm_zlib PRIVATE _CRT_SECURE_NO_WARNINGS)

# Adapt a build-tree copy; never change a shared dependency/source cache.
set(id3_dir "${CMAKE_CURRENT_BINARY_DIR}/id3-adapted")
execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/adapt_id3.py"
  "${ttpcomm_id3_source_SOURCE_DIR}" "${id3_dir}"
  COMMAND_ERROR_IS_FATAL ANY)
set(id3_sources ucs4.c latin1.c utf16.c utf8.c parse.c render.c field.c
  frametype.c compat.c genre.c frame.c crc.c util.c tag.c)
list(TRANSFORM id3_sources PREPEND "${id3_dir}/")
add_library(ttpcomm_id3 STATIC ${id3_sources})
target_include_directories(ttpcomm_id3 PUBLIC "${id3_dir}")
target_compile_definitions(ttpcomm_id3 PRIVATE HAVE_ASSERT_H=1 HAVE_ZLIB_H=1
  _CRT_SECURE_NO_WARNINGS)
target_link_libraries(ttpcomm_id3 PUBLIC ttpcomm_zlib)

FetchContent_Declare(ttpcomm_ssrc_source
 URL https://codeload.github.com/AviSynth/AviSynthPlus/zip/6c02f0dfac72678b80176e0aeefb7e1fde205b97
 URL_HASH SHA256=db396143f72e330a38e91bfeb371b6b3a9419fea84642f30bfbd4ee4a9551e9a
 DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR unused)
FetchContent_MakeAvailable(ttpcomm_ssrc_source)
set(ssrc_dir "${CMAKE_CURRENT_BINARY_DIR}/ssrc-adapted")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/adapt_ssrc.py"
 "${ttpcomm_ssrc_source_SOURCE_DIR}" "${ssrc_dir}" COMMAND_ERROR_IS_FATAL ANY)
add_library(ttpcomm_ssrc STATIC "${ssrc_dir}/ssrc.cpp" "${ssrc_dir}/dbesi0.c")
target_include_directories(ttpcomm_ssrc PUBLIC "${ssrc_dir}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(ttpcomm_ssrc PRIVATE /fp:precise /arch:IA32)

# The historical double mpglib path is retained in this fixed LAME release.
# Only the decoding primitives are used; buffering and public ABI are recovered.
FetchContent_Declare(ttpcomm_mpeg_source
 URL https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz
 URL_HASH SHA256=ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e
 DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR unused)
FetchContent_MakeAvailable(ttpcomm_mpeg_source)
set(mpeg_dir "${CMAKE_CURRENT_BINARY_DIR}/mpeg-adapted")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/adapt_mpeg.py"
 "${ttpcomm_mpeg_source_SOURCE_DIR}" "${mpeg_dir}" "${CMAKE_CURRENT_SOURCE_DIR}"
 COMMAND_ERROR_IS_FATAL ANY)
set(mpeg_sources common.c layer1.c layer2.c layer3.c decode_i386.c dct64_i386.c tabinit.c)
list(TRANSFORM mpeg_sources PREPEND "${mpeg_dir}/")
add_library(ttpcomm_mpeg STATIC ${mpeg_sources})
target_include_directories(ttpcomm_mpeg PUBLIC "${mpeg_dir}" "${ttpcomm_mpeg_source_SOURCE_DIR}/include"
 PRIVATE "${ttpcomm_mpeg_source_SOURCE_DIR}/libmp3lame" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(ttpcomm_mpeg PUBLIC STDC_HEADERS HAVE_STDINT_H PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(ttpcomm_mpeg PRIVATE /fp:precise /arch:IA32)

# Goom 1.9.3 matches the recovered Dream control flow. Adapt a build-tree copy;
# TTPlayer's random table, per-instance state and arithmetic differ upstream.
FetchContent_Declare(ttpcomm_dream_source
 URL https://downloads.sourceforge.net/project/goom/OldFiles/wgoom-1.9.3-src.zip
 URL_HASH SHA256=7989966991eb6adeabfa87b2f07e81e47b3eb52e700cd4313ab8e4b429f6609e
 DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR unused)
FetchContent_MakeAvailable(ttpcomm_dream_source)
set(dream_dir "${CMAKE_CURRENT_BINARY_DIR}/dream-adapted")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/adapt_dream.py"
 "${ttpcomm_dream_source_SOURCE_DIR}" "${dream_dir}" COMMAND_ERROR_IS_FATAL ANY)

FetchContent_Declare(ttpcomm_coolsb_source
 URL https://codeload.github.com/jsleroy/CoolSB/zip/472051497f176f5853eae03677f63260ad4eed44
 URL_HASH SHA256=a1e6e5c3850f81cf48aa9e6de974f6763311c14eaf85398c99415e2c5e122697
 DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR unused)
FetchContent_Declare(ttpcomm_detours_source
 URL https://codeload.github.com/microsoft/Detours/zip/refs/tags/v4.0.1
 URL_HASH SHA256=5ab84eb08fb9befeb16ffd04ca283731b1e0e1e53b1947ce7868d4b9654e43fc
 DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR unused)
FetchContent_MakeAvailable(ttpcomm_coolsb_source ttpcomm_detours_source)
set(coolsb_dir "${CMAKE_CURRENT_BINARY_DIR}/coolsb-adapted")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/adapt_coolsb.py"
 "${ttpcomm_coolsb_source_SOURCE_DIR}" "${coolsb_dir}" COMMAND_ERROR_IS_FATAL ANY)
add_library(ttpcomm_coolsb STATIC "${coolsb_dir}/coolscroll.c" "${coolsb_dir}/coolsblib.c")
target_include_directories(ttpcomm_coolsb PUBLIC "${coolsb_dir}")
target_compile_definitions(ttpcomm_coolsb PRIVATE _CRT_SECURE_NO_WARNINGS)
# The original subclass explicitly uses the ANSI window procedure API.
target_compile_options(ttpcomm_coolsb PRIVATE /UUNICODE /U_UNICODE)
set(detours_dir "${CMAKE_CURRENT_BINARY_DIR}/detours-adapted")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/adapt_detours.py"
 "${ttpcomm_detours_source_SOURCE_DIR}" "${detours_dir}" COMMAND_ERROR_IS_FATAL ANY)
set(detours_sources modules.cpp disasm.cpp image.cpp creatwth.cpp)
list(TRANSFORM detours_sources PREPEND "${ttpcomm_detours_source_SOURCE_DIR}/src/")
add_library(ttpcomm_detours STATIC ${detours_sources} "${detours_dir}/detours.cpp")
target_include_directories(ttpcomm_detours PUBLIC "${ttpcomm_detours_source_SOURCE_DIR}/src")
target_compile_definitions(ttpcomm_detours PRIVATE _WIN32_WINNT=0x0501 WIN32_LEAN_AND_MEAN)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/dependency-sources.txt" "")
foreach(_dependency zlib id3 kissfft ssrc mpeg dream detours)
  file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/dependency-sources.txt"
    "${_dependency}=${ttpcomm_${_dependency}_source_SOURCE_DIR}\n")
endforeach()
