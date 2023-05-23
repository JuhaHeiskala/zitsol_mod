cmake_minimum_required(VERSION 3.16)
project(ZITSOL_1 C)
enable_language(Fortran)

if(NOT JLL_BUILD)
  find_package(LAPACK)
endif()

file(GLOB SRCS *.c)
file(GLOB SRCS2 LIB/*.c)

add_library(ZITSOL_1 SHARED ${SRCS} ${SRCS2} LIB/ztools.f)
set_property(TARGET ZITSOL_1 PROPERTY C_STANDARD 99)

if(JLL_BUILD)
  if($ENV{target} MATCHES "w64-mingw32$")
    target_link_libraries(ZITSOL_1  blastrampoline-5)
  else()
    target_link_libraries(ZITSOL_1  blastrampoline)
  endif()
else()
  target_link_libraries(ZITSOL_1  ${LAPACK_LIBRARIES})
endif()

target_include_directories(ZITSOL_1 PUBLIC LIB)

install(TARGETS ZITSOL_1)
