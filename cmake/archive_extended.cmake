# Only these read formats are registered by archive_extended.cpp. Existing ZIP
# and RAR implementations deliberately do not pass through libarchive.
foreach(_dep libarchive xz)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party_sources/${_dep}/CMakeLists.txt")
    string(TOUPPER "FETCHCONTENT_SOURCE_DIR_TTPCOMM_${_dep}_SOURCE" _override)
    set(${_override} "${CMAKE_CURRENT_SOURCE_DIR}/third_party_sources/${_dep}" CACHE PATH "Bundled source")
  endif()
endforeach()
FetchContent_Declare(ttpcomm_libarchive_source
  URL https://github.com/libarchive/libarchive/releases/download/v3.8.9/libarchive-3.8.9.tar.gz
  URL_HASH SHA256=f5a6539059cf5e597dbeda37bfa4874b1e8dea063c8d93bf85a2b44af90a5bd4
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR unused)
FetchContent_Declare(ttpcomm_xz_source
  URL https://github.com/tukaani-project/xz/releases/download/v5.8.4/xz-5.8.4.tar.gz
  URL_HASH SHA256=0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR unused)
FetchContent_MakeAvailable(ttpcomm_libarchive_source ttpcomm_xz_source)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Static dependencies" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Private tests are built separately" FORCE)
set(XZ_THREADS no CACHE STRING "The archive service owns its worker" FORCE)
set(XZ_SMALL ON CACHE BOOL "Size-first decoder" FORCE)
set(XZ_ENCODERS "" CACHE STRING "Decode only" FORCE)
set(XZ_MATCH_FINDERS "" CACHE STRING "Decode only" FORCE)
foreach(_opt ASM_I386 NLS EXTERNAL_SHA256 MICROLZMA_ENCODER MICROLZMA_DECODER
    LZIP_DECODER TOOL_XZDEC TOOL_LZMADEC TOOL_LZMAINFO TOOL_XZ TOOL_SCRIPTS DOC DOXYGEN)
  set(XZ_${_opt} OFF CACHE BOOL "Not used by ttpcomm" FORCE)
endforeach()
add_subdirectory("${ttpcomm_xz_source_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/xz-static" EXCLUDE_FROM_ALL)

set(archive_adapted "${CMAKE_CURRENT_BINARY_DIR}/libarchive-adapted")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/adapt_libarchive.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/adapt_libarchive.py"
  "${ttpcomm_libarchive_source_SOURCE_DIR}" "${archive_adapted}" COMMAND_ERROR_IS_FATAL ANY)
foreach(_opt MBEDTLS NETTLE OPENSSL LIBB2 LZ4 LZO LZMA ZSTD ZLIB BZip2 LIBXML2
    EXPAT WIN32_XMLLITE PCREPOSIX PCRE2POSIX LIBGCC CNG TAR CPIO CAT UNZIP XATTR
    ACL ICONV TEST COVERAGE INSTALL)
  set(ENABLE_${_opt} OFF CACHE BOOL "Minimal read-only archive backend" FORCE)
endforeach()
# Dependencies are our in-tree static targets, not an installed DLL discovered
# by FindZLIB/FindLibLZMA. Seed the feature macros and link the targets explicitly.
set(HAVE_ZLIB_H 1)
set(HAVE_LZMA_H 1)
set(HAVE_LIBLZMA 1)
set(MSVC_USE_STATIC_CRT ON CACHE BOOL "Match the XP CRT adapter" FORCE)
set(WINDOWS_VERSION WINXP CACHE STRING "XP API floor" FORCE)
set(POSIX_REGEX_LIB NONE CACHE STRING "No archive pathname matcher" FORCE)
add_subdirectory("${archive_adapted}" "${CMAKE_CURRENT_BINARY_DIR}/archive-static" EXCLUDE_FROM_ALL)
target_compile_definitions(archive_static PRIVATE LZMA_API_STATIC _CRT_SECURE_NO_WARNINGS)
target_link_libraries(archive_static PRIVATE ttpcomm_zlib liblzma)
target_include_directories(archive_static PRIVATE "${ttpcomm_xz_source_SOURCE_DIR}/src/liblzma/api")
foreach(_dependency libarchive xz)
  file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/dependency-sources.txt"
    "${_dependency}=${ttpcomm_${_dependency}_source_SOURCE_DIR}\n")
endforeach()
