# RAR decoding only. Keep upstream source out of Git; download a pinned archive.
FetchContent_Declare(ttpcomm_unrar_source
  URL https://www.rarlab.com/rar/unrarsrc-7.3.1.tar.gz
  URL_HASH SHA256=634900842a3737d9cc15bbcc71d4c74cc713437e0bca296a573424fe5f2660ab
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR unused)
FetchContent_MakeAvailable(ttpcomm_unrar_source)
set(unrar_adapted "${CMAKE_CURRENT_BINARY_DIR}/unrar-adapted")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/adapt_unrar.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/adapt_unrar.py"
  "${ttpcomm_unrar_source_SOURCE_DIR}" "${unrar_adapted}" COMMAND_ERROR_IS_FATAL ANY)
set(unrar_sources strlist.cpp strfn.cpp pathfn.cpp smallfn.cpp global.cpp file.cpp
  filefn.cpp filcreat.cpp archive.cpp arcread.cpp unicode.cpp system.cpp crypt.cpp
  crc.cpp rawread.cpp encname.cpp resource.cpp match.cpp timefn.cpp
  consio.cpp options.cpp errhnd.cpp rarvm.cpp secpassword.cpp rijndael.cpp
  getbits.cpp sha1.cpp sha256.cpp blake2s.cpp hash.cpp extinfo.cpp extract.cpp
  volume.cpp list.cpp find.cpp unpack.cpp headers.cpp threadpool.cpp rs16.cpp
  cmddata.cpp ui.cpp largepage.cpp filestr.cpp scantree.cpp dll.cpp qopen.cpp isnt.cpp motw.cpp)
list(TRANSFORM unrar_sources PREPEND "${ttpcomm_unrar_source_SOURCE_DIR}/")
add_library(ttpcomm_unrar STATIC ${unrar_sources} "${unrar_adapted}/rdwrfn.cpp")
target_compile_definitions(ttpcomm_unrar PRIVATE RARDLL UNRAR SILENT UNICODE
  _UNICODE NOMINMAX _WIN32_WINNT=0x0501 _CRT_SECURE_NO_WARNINGS)
target_compile_options(ttpcomm_unrar PRIVATE /EHsc /utf-8 /arch:IA32)
target_include_directories(ttpcomm_unrar PUBLIC "${ttpcomm_unrar_source_SOURCE_DIR}")
