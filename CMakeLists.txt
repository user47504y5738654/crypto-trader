cmake_minimum_required(VERSION 3.16)
project(crypto_trader VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# Cross-platform build
# ============================================================================

if(WIN32)
    add_definitions(-D_WIN32)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(nlohmann_json REQUIRED)

# ============================================================================
# Include paths
# ============================================================================
include_directories(${CMAKE_SOURCE_DIR}/include)

# ============================================================================
# Source files
# ============================================================================
file(GLOB_RECURSE SOURCES ${CMAKE_SOURCE_DIR}/src/*.cpp)
file(GLOB_RECURSE HEADERS 
    ${CMAKE_SOURCE_DIR}/src/*.h
    ${CMAKE_SOURCE_DIR}/include/*.h)

# ============================================================================
# Executable
# ============================================================================
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

target_link_libraries(${PROJECT_NAME} PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    SQLite::SQLite3
    nlohmann_json::nlohmann_json
)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 crypt32)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE pthread ${CMAKE_DL_LIBS})
endif()

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W3 /O2 /EHsc)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -O2 -g)
endif()

# ============================================================================
message(STATUS "========================================")
message(STATUS "  CryptoTrader - AI-Strategist v2.0")
message(STATUS "  Platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "========================================")