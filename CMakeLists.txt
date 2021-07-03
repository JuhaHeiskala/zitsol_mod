cmake_minimum_required(VERSION 3.16)
project(ZITSOL_1 C)
enable_language(Fortran)

if(JLL_BUILD)
  include($ENV{prefix}/lib/cmake/lapack-3.9.0/lapack-config.cmake)
else()
  find_package(LAPACK)
endif()

file(GLOB SRCS *.c)
file(GLOB SRCS2 LIB/*.c)

add_library(ZITSOL_1 SHARED ${SRCS} ${SRCS2} LIB/ztools.f)
set_property(TARGET ZITSOL_1 PROPERTY C_STANDARD 99)

target_link_libraries(ZITSOL_1  ${LAPACK_LIBRARIES})
target_include_directories(ZITSOL_1 PUBLIC LIB)

install(TARGETS ZITSOL_1)
