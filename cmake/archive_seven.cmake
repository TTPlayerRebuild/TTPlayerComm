# Public-domain, pinned decoder sources. No runtime 7z.exe/7z.dll dependency.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party_sources/lzmasdk/CPP/7zip/Archive/7z/7zIn.cpp")
  set(FETCHCONTENT_SOURCE_DIR_TTPCOMM_LZMASDK_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party_sources/lzmasdk" CACHE PATH "Bundled SDK")
endif()
FetchContent_Declare(ttpcomm_lzmasdk_source
  URL https://github.com/ip7z/7zip/releases/download/26.04/lzma2604.7z
  URL_HASH SHA256=bd98058d0e12c5da970dd4e5e1d8fa24f2d704914d96e69c28f92ed45421a439
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR unused)
FetchContent_MakeAvailable(ttpcomm_lzmasdk_source)
set(seven_dir "${CMAKE_CURRENT_BINARY_DIR}/seven-adapted")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/adapt_seven.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/adapt_seven.py"
  "${ttpcomm_lzmasdk_source_SOURCE_DIR}" "${seven_dir}" COMMAND_ERROR_IS_FATAL ANY)
set(seven_sources)
function(seven_files directory)
  foreach(name IN LISTS ARGN)
    list(APPEND seven_sources "${seven_dir}/${directory}/${name}")
  endforeach()
  set(seven_sources "${seven_sources}" PARENT_SCOPE)
endfunction()
seven_files(CPP/Common CRC.cpp IntToString.cpp MyString.cpp MyVector.cpp StringConvert.cpp StringToInt.cpp)
seven_files(CPP/Windows PropVariant.cpp System.cpp)
seven_files(CPP/7zip/Common CreateCoder.cpp CWrappers.cpp InBuffer.cpp FilterCoder.cpp LimitedStreams.cpp
  MethodId.cpp MethodProps.cpp OutBuffer.cpp ProgressUtils.cpp PropId.cpp StreamObjects.cpp StreamUtils.cpp)
seven_files(CPP/7zip/Archive/Common CoderMixer2.cpp HandlerOut.cpp ItemNameUtils.cpp OutStreamWithCRC.cpp ParseProperties.cpp)
seven_files(CPP/7zip/Archive/7z 7zCompressionMode.cpp 7zDecode.cpp 7zExtract.cpp 7zHandler.cpp 7zHeader.cpp 7zIn.cpp 7zProperties.cpp)
seven_files(CPP/7zip/Compress Bcj2Coder.cpp Bcj2Register.cpp BcjCoder.cpp BcjRegister.cpp BranchMisc.cpp BranchRegister.cpp
  CopyCoder.cpp CopyRegister.cpp DeltaFilter.cpp Lzma2Decoder.cpp Lzma2Register.cpp LzmaDecoder.cpp LzmaRegister.cpp PpmdDecoder.cpp PpmdRegister.cpp)
seven_files(CPP/7zip/Crypto 7zAes.cpp 7zAesRegister.cpp MyAes.cpp)
seven_files(C 7zStream.c Alloc.c Bcj2.c Bra.c Bra86.c BraIA64.c CpuArch.c Delta.c Lzma2Dec.c Lzma2DecMt.c LzmaDec.c
  Ppmd7.c Ppmd7Dec.c Aes.c AesOpt.c 7zCrc.c 7zCrcOpt.c Sha256.c Sha256Opt.c)
# Object library retains codec registrations without exporting SDK factories.
add_library(ttpcomm_seven OBJECT ${seven_sources})
target_compile_definitions(ttpcomm_seven PUBLIC Z7_EXTRACT_ONLY Z7_ST Z7_NO_READ_FROM_CODER Z7_NO_LARGE_PAGES
  PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0501 _CRT_SECURE_NO_WARNINGS)
target_compile_options(ttpcomm_seven PRIVATE /EHsc /O1 /arch:IA32)
target_include_directories(ttpcomm_seven PUBLIC "${seven_dir}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/dependency-sources.txt" "lzmasdk=${ttpcomm_lzmasdk_source_SOURCE_DIR}\n")
include("${CMAKE_CURRENT_LIST_DIR}/archive_extra_codecs.cmake")
