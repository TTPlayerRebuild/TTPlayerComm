# Decoder-only static dependencies, pinned and fetched at build time.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party_sources/bzip2/bzlib.c")
  set(FETCHCONTENT_SOURCE_DIR_TTPCOMM_BZIP2_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party_sources/bzip2" CACHE PATH "Bundled bzip2")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party_sources/zstd/lib/zstd.h")
  set(FETCHCONTENT_SOURCE_DIR_TTPCOMM_ZSTD_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party_sources/zstd" CACHE PATH "Bundled Zstandard")
endif()
FetchContent_Declare(ttpcomm_bzip2_source
  URL https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz
  URL_HASH SHA256=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR unused)
FetchContent_Declare(ttpcomm_zstd_source
  URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
  URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR unused)
FetchContent_MakeAvailable(ttpcomm_bzip2_source ttpcomm_zstd_source)
set(bzip_dir "${ttpcomm_bzip2_source_SOURCE_DIR}")
set(zstd_dir "${ttpcomm_zstd_source_SOURCE_DIR}/lib")
add_library(ttpcomm_archive_codecs STATIC
  "${bzip_dir}/bzlib.c" "${bzip_dir}/decompress.c" "${bzip_dir}/huffman.c"
  "${bzip_dir}/crctable.c" "${bzip_dir}/randtable.c"
  "${bzip_dir}/compress.c" "${bzip_dir}/blocksort.c"
  "${zstd_dir}/common/debug.c" "${zstd_dir}/common/entropy_common.c"
  "${zstd_dir}/common/error_private.c" "${zstd_dir}/common/fse_decompress.c"
  "${zstd_dir}/common/xxhash.c" "${zstd_dir}/common/zstd_common.c"
  "${zstd_dir}/decompress/huf_decompress.c" "${zstd_dir}/decompress/zstd_ddict.c"
  "${zstd_dir}/decompress/zstd_decompress.c" "${zstd_dir}/decompress/zstd_decompress_block.c")
target_include_directories(ttpcomm_archive_codecs PUBLIC "${bzip_dir}" "${zstd_dir}")
target_compile_definitions(ttpcomm_archive_codecs PRIVATE BZ_NO_STDIO ZSTD_LEGACY_SUPPORT=0
  ZSTD_DISABLE_ASM ZSTD_NO_UNUSED_FUNCTIONS ZSTD_LIB_DEPRECATED=0
  _WIN32_WINNT=0x0501 _CRT_SECURE_NO_WARNINGS)
target_compile_options(ttpcomm_archive_codecs PRIVATE /O1 /arch:IA32 /Gy)
target_sources(ttpcomm_seven PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/archive_seven_extra.cpp")
target_link_libraries(ttpcomm_seven PUBLIC ttpcomm_archive_codecs)
file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/dependency-sources.txt"
  "bzip2=${bzip_dir}\nzstd=${ttpcomm_zstd_source_SOURCE_DIR}\n")
