CMAKE_MINIMUM_REQUIRED(VERSION 3.22)

PROJECT(UEFIReplace LANGUAGES C CXX)

SET(CMAKE_CXX_STANDARD 11)
SET(CMAKE_CXX_STANDARD_REQUIRED ON)
SET(CMAKE_CXX_EXTENSIONS OFF)

# Shared sources: parser + builder + compressors (no Qt required)
# Shared sources: see common/edit_sources.cmake
INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/../common/edit_sources.cmake)
SET(UEFIREPLACE_COMMON_SOURCES
 uefireplace.cpp
 ${UEFI_EDIT_COMMON_SOURCES}
)

ADD_DEFINITIONS(
 -DU_ENABLE_NVRAM_PARSING_SUPPORT
 -DU_ENABLE_ME_PARSING_SUPPORT
 -DU_ENABLE_FIT_PARSING_SUPPORT
 -DU_ENABLE_GUID_DATABASE_SUPPORT
)

ADD_EXECUTABLE(UEFIReplace uefireplace_main.cpp ${UEFIREPLACE_COMMON_SOURCES})

IF(UNIX)
 SET_TARGET_PROPERTIES(UEFIReplace PROPERTIES OUTPUT_NAME uefireplace)
ENDIF()

# Test harness (identity rebuild, replace, size-change rejection), run with: ctest or ./uefireplace_test image.bin
OPTION(UEFIREPLACE_BUILD_TESTS "Build the UEFIReplace/builder test harness" ON)
IF(UEFIREPLACE_BUILD_TESTS)
 ADD_EXECUTABLE(UEFIReplaceTest tests/ffsedit_test.cpp ../UEFIPatch/uefipatch.cpp ${UEFIREPLACE_COMMON_SOURCES})
 SET_TARGET_PROPERTIES(UEFIReplaceTest PROPERTIES OUTPUT_NAME uefireplace_test)
ENDIF()

INSTALL(
 TARGETS UEFIReplace
 RUNTIME DESTINATION bin
)
