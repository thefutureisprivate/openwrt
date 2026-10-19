cmake_minimum_required(VERSION 3.10)

PROJECT(fritz-tools C)
ADD_DEFINITIONS(-Wall -Werror --std=gnu99 -Wmissing-declarations)

SET(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")

FIND_PATH(zlib_include_dir zlib.h)
INCLUDE_DIRECTORIES(${zlib_include_dir})

ADD_EXECUTABLE(fritz_tffs_read fritz_tffs_read.c)
ADD_EXECUTABLE(fritz_tffs_nand_read fritz_tffs_nand_read.c)
ADD_EXECUTABLE(fritz_tffs3 fritz_tffs3.c)
ADD_EXECUTABLE(fritz_cal_extract fritz_cal_extract.c)
TARGET_LINK_LIBRARIES(fritz_cal_extract z)
ADD_EXECUTABLE(fritz_caldata fritz_caldata.c)
TARGET_LINK_LIBRARIES(fritz_caldata z)

OPTION(BUILD_FRITZ_FIT "Build the AVM FIT reader" OFF)
IF(BUILD_FRITZ_FIT)
  FIND_PATH(libfdt_include_dir libfdt.h REQUIRED)
  INCLUDE_DIRECTORIES(${libfdt_include_dir})
  ADD_EXECUTABLE(fritz_fit fritz_fit.c)
  TARGET_LINK_LIBRARIES(fritz_fit fdt z)
  INSTALL(TARGETS fritz_fit RUNTIME DESTINATION bin)
ENDIF()

INSTALL(TARGETS fritz_tffs_read fritz_tffs_nand_read fritz_tffs3 fritz_cal_extract fritz_caldata RUNTIME DESTINATION bin)
