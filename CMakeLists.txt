cmake_minimum_required(VERSION 3.16)
project(crypto_trader VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# Кроссплатформенная сборка (Linux / Windows / macOS)
# ============================================================================

if(WIN32)
    # Windows: MSVC или MinGW
    add_definitions(-D_WIN32)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
    
    # vcpkg — рекомендуемый способ установки зависимостей
    # find_package(OpenSSL REQUIRED)
    # find_package(CURL REQUIRED)
    # find_package(SQLite3 REQUIRED)
    # find_package(nlohmann_json REQUIRED)
    
    # Если зависимости установлены вручную — раскомментируйте пути:
    # set(OPENSSL_ROOT_DIR "C:/openssl")
    # set(CURL_ROOT_DIR "C:/curl")
else()
    # Linux / macOS
    find_package(OpenSSL REQUIRED)
    find_package(CURL REQUIRED)
    find_package(SQLite3 REQUIRED)
    find_package(nlohmann_json REQUIRED)
endif()

# ============================================================================
# Пути для поиска заголовков
# ============================================================================
include_directories(${CMAKE_SOURCE_DIR}/include)

# ============================================================================
# Исходные файлы
# ============================================================================
file(GLOB_RECURSE SOURCES ${CMAKE_SOURCE_DIR}/src/*.cpp)
file(GLOB_RECURSE HEADERS 
    ${CMAKE_SOURCE_DIR}/src/*.h
    ${CMAKE_SOURCE_DIR}/include/*.h)

# ============================================================================
# Исполняемый файл
# ============================================================================
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

# Основные библиотеки
target_link_libraries(${PROJECT_NAME} PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    SQLite::SQLite3
    nlohmann_json::nlohmann_json
)

# Платформозависимые библиотеки
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 crypt32)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE pthread ${CMAKE_DL_LIBS})
endif()

# Флаги компиляции
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W3 /O2 /EHsc)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -O2 -g)
endif()

# ============================================================================
message(STATUS "========================================")
message(STATUS "  CryptoTrader — AI-Стратег v2.0")
message(STATUS "  DeepSeek + CoinEx API v2")
message(STATUS "  Платформа: ${CMAKE_SYSTEM_NAME}")
message(STATUS "========================================")
