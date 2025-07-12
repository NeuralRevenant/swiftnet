# Basic HTTP server example
add_executable(basic_server basic_server.cpp)
target_link_libraries(basic_server 
    PRIVATE 
        swiftnet
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)
target_include_directories(basic_server 
    PRIVATE 
        ${CMAKE_SOURCE_DIR}/include
)

# File server example
add_executable(file_server file_server.cpp)
target_link_libraries(file_server 
    PRIVATE 
        swiftnet
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)
target_include_directories(file_server 
    PRIVATE 
        ${CMAKE_SOURCE_DIR}/include
)

# REST API example
add_executable(rest_api rest_api.cpp)
target_link_libraries(rest_api 
    PRIVATE 
        swiftnet
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)
target_include_directories(rest_api 
    PRIVATE 
        ${CMAKE_SOURCE_DIR}/include
)

# Simple test for virtual threads
add_executable(simple_test simple_test.cpp)
target_link_libraries(simple_test 
    PRIVATE 
        swiftnet
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)
target_include_directories(simple_test 
    PRIVATE 
        ${CMAKE_SOURCE_DIR}/include
)

# Performance test for virtual threads
add_executable(performance_test performance_test.cpp)
target_link_libraries(performance_test 
        PRIVATE 
            swiftnet
            nlohmann_json::nlohmann_json
            spdlog::spdlog
    )
target_include_directories(performance_test 
        PRIVATE 
            ${CMAKE_SOURCE_DIR}/include
    )