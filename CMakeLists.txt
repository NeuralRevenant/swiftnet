cmake_minimum_required(VERSION 3.19)
project(swiftnet LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable LTO / IPO if the compiler supports it
include(CheckIPOSupported)
check_ipo_supported(RESULT ipoOK)
if(ipoOK)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

# ---------------- jemalloc / tcmalloc ----------------
option(SWIFTNET_WITH_JEMALLOC "Link jemalloc for per-core arenas" ON)
if(SWIFTNET_WITH_JEMALLOC)
  find_library(JEMALLOC_LIB jemalloc REQUIRED)
endif()

# ---------------- liburing --------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED liburing)

include_directories(include)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_library(swiftnet STATIC ${SOURCES})
target_compile_options(swiftnet PRIVATE -O3 -march=native -fno-omit-frame-pointer -pthread)
target_link_libraries(swiftnet PRIVATE ${LIBURING_LIBRARIES} pthread
                      $<$<BOOL:${SWIFTNET_WITH_JEMALLOC}>:${JEMALLOC_LIB}>)
target_include_directories(swiftnet PRIVATE ${LIBURING_INCLUDE_DIRS})
