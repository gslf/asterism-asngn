# A runtime bundle has no model weights, credentials, development libraries or state.
install(TARGETS asngn asngn-mcp RUNTIME DESTINATION bin)
# Sibling directories are EXCLUDE_FROM_ALL; install their explicitly built tools here.
install(PROGRAMS $<TARGET_FILE:asper-mcp> $<TARGET_FILE:astools-mcp>
                 $<TARGET_FILE:astools-check> $<TARGET_FILE:astools-jail> DESTINATION bin)
add_custom_target(asngn_distribution_tools ALL
  DEPENDS asper-mcp astools-mcp astools-check astools-jail astools_packages)
install(DIRECTORY "${CMAKE_BINARY_DIR}/packages/" DESTINATION share/asterism/tools
  USE_SOURCE_PERMISSIONS PATTERN ".*" EXCLUDE)
install(DIRECTORY examples/ DESTINATION share/asterism/examples
  PATTERN "__pycache__" EXCLUDE)
install(FILES release.json docs/distribution.md DESTINATION share/asterism)
foreach(component asngn asper asmodel astools)
  if(component STREQUAL "asngn")
    set(component_source "${CMAKE_CURRENT_SOURCE_DIR}")
  else()
    string(TOUPPER "${component}" component_upper)
    set(component_source "${ASNGN_${component_upper}_DIR}")
  endif()
  install(FILES "${component_source}/LICENSE" DESTINATION share/asterism/licenses RENAME "${component}.txt")
endforeach()
install(FILES "${ASNGN_XCDN_DIR}/LICENSE" DESTINATION share/asterism/licenses RENAME xcdn.txt)

# Derive the package version from the public header rather than another release default.
foreach(part MAJOR MINOR PATCH)
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/asngn.h" version_line
    REGEX "^#define ASNGN_VERSION_${part} [0-9]+$")
  string(REGEX REPLACE ".* " "" CPACK_PACKAGE_VERSION_${part} "${version_line}")
endforeach()
set(CPACK_PACKAGE_NAME asterism)
set(CPACK_PACKAGE_VENDOR Asterism)
set(CPACK_GENERATOR TGZ)
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_PACKAGE_FILE_NAME "asterism-${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}-linux-${CMAKE_SYSTEM_PROCESSOR}")
include(CPack)
