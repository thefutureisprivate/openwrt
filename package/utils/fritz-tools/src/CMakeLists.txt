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

INSTALL(TARGETS fritz_tffs_read fritz_tffs_nand_read fritz_tffs3 fritz_cal_extract fritz_caldata RUNTIME DESTINATION bin)
