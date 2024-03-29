project(Micah)
cmake_minimum_required(VERSION 3.2)

add_definitions("-Wall")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(MICAH_VERSION 2.0)
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DVERSION=\"\\\"${MICAH_VERSION}\\\"\"")

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer -fstack-protector-strong")
set(CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer")

add_executable(
  Micah
  eval.cpp
  eval_par.cpp
  Micah.cpp
  psq.cpp
  search.cpp
  syzygy.cpp
  tt.cpp
  utils.cpp
  Fathom/src/tbprobe.c
)

target_include_directories(Micah PRIVATE Fathom/src)

include(FindPkgConfig)

find_package(OpenMP REQUIRED)
target_link_libraries(Micah PRIVATE OpenMP::OpenMP_CXX)

set_target_properties(Micah PROPERTIES OUTPUT_NAME Micah)
