cmake_minimum_required(VERSION 3.20)
project(SwiftNet VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform detection
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(SWIFTNET_PLATFORM_LINUX TRUE)
    add_compile_definitions(SWIFTNET_BACKEND_IOURING=1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(SWIFTNET_PLATFORM_MACOS TRUE)
    add_compile_definitions(SWIFTNET_BACKEND_KQUEUE=1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(SWIFTNET_PLATFORM_WINDOWS TRUE)
    add_compile_definitions(SWIFTNET_BACKEND_IOCP=1)
endif()

# Compiler-specific flags
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O3")
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4 /O2")
endif()

# Find required packages
find_package(Threads REQUIRED)

# Find nlohmann/json
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# Find spdlog for logging
find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.12.0
    )
    FetchContent_MakeAvailable(spdlog)
endif()

# Platform-specific libraries
if(SWIFTNET_PLATFORM_LINUX)
    # Try to find liburing for Linux
    find_library(LIBURING_LIBRARY NAMES uring)
    if(LIBURING_LIBRARY)
        add_compile_definitions(SWIFTNET_HAS_LIBURING=1)
    endif()
elseif(SWIFTNET_PLATFORM_WINDOWS)
    # Windows-specific libraries
    set(PLATFORM_LIBS ws2_32 wsock32)
endif()

# SwiftNet library sources
set(SWIFTNET_SOURCES
    src/swiftnet.cpp
    src/vthread.cpp
    src/vthread_scheduler.cpp
    src/io_context.cpp
    src/io_awaitable.cpp
    src/event_loop.cpp
    src/net/tcp_socket.cpp
    src/net/acceptor.cpp
    src/http/http_server.cpp
    src/detail/platform_utils.cpp
)

# SwiftNet library headers
set(SWIFTNET_HEADERS
    include/swiftnet.hpp
    include/vthread.hpp
    include/vthread_scheduler.hpp
    include/io_context.hpp
    include/io_awaitable.hpp
    include/event_loop.hpp
    include/net/tcp_socket.hpp
    include/net/acceptor.hpp
    include/http/http_server.hpp
    include/detail/os_backend.hpp
    include/detail/mpsc_queue.hpp
    include/detail/cpu_affinity.hpp
)

# Create the SwiftNet library
add_library(swiftnet STATIC ${SWIFTNET_SOURCES} ${SWIFTNET_HEADERS})

# Set include directories
target_include_directories(swiftnet 
    PUBLIC 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Link libraries
target_link_libraries(swiftnet 
    PUBLIC 
        Threads::Threads
    PRIVATE
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)

# Platform-specific linking
if(SWIFTNET_PLATFORM_LINUX AND LIBURING_LIBRARY)
    target_link_libraries(swiftnet PUBLIC ${LIBURING_LIBRARY})
elseif(SWIFTNET_PLATFORM_WINDOWS)
    target_link_libraries(swiftnet PUBLIC ${PLATFORM_LIBS})
endif()

# Enable examples
option(SWIFTNET_BUILD_EXAMPLES "Build SwiftNet examples" ON)
if(SWIFTNET_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Future: Add installation and packaging configuration
