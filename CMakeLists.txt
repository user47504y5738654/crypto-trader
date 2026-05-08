cmake_minimum_required(VERSION 3.16)
project(crypto_trader VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# Подключение внешних библиотек
# ============================================================================

# OpenSSL — для HMAC-SHA256 подписи запросов к CoinEx
find_package(OpenSSL REQUIRED)

# SQLite3 — для хранения аудита и конфигурации
find_package(SQLite3 REQUIRED)

# nlohmann/json — header-only библиотека
find_package(nlohmann_json REQUIRED)

# ============================================================================
# Пути для поиска заголовочных файлов
# ============================================================================
include_directories(${CMAKE_SOURCE_DIR}/include)

# ============================================================================
# Исходные файлы
# ============================================================================

file(GLOB_RECURSE SOURCES
    ${CMAKE_SOURCE_DIR}/src/*.cpp
)

file(GLOB_RECURSE HEADERS
    ${CMAKE_SOURCE_DIR}/src/*.h
    ${CMAKE_SOURCE_DIR}/include/*.h
)

# ============================================================================
# Исполняемый файл
# ============================================================================

add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

target_link_libraries(${PROJECT_NAME}
    PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    SQLite::SQLite3
    ${CMAKE_DL_LIBS}
    pthread
    curl
    nlohmann_json::nlohmann_json
)

# Флаги компиляции
target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -O2 -g)

# Сообщение пользователю
message(STATUS "========================================")
message(STATUS "  Crypto Trader - AI Trading Tool v1.0")
message(STATUS "========================================")
message(STATUS "  Для сборки: mkdir build && cd build && cmake .. && make")
