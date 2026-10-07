set(TTPCOMM_BUILD_VERSION "" CACHE STRING "Beijing yyyy.MM.dd[pN] version; empty selects the current date")
find_program(TTPCOMM_POWERSHELL NAMES powershell pwsh REQUIRED)
set(_ttpcomm_version_args)
if(NOT TTPCOMM_BUILD_VERSION STREQUAL "")
  list(APPEND _ttpcomm_version_args -Version "${TTPCOMM_BUILD_VERSION}")
endif()
add_custom_target(ttpcomm_version
  COMMAND "${TTPCOMM_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake/write_version.ps1"
    -Template "${CMAKE_CURRENT_SOURCE_DIR}/src/version.rc.in"
    -OutputPath "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc"
    ${_ttpcomm_version_args}
  BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc"
  COMMENT "Updating TTPCOMM file and product versions"
  VERBATIM)
