#ifndef UTILS_H
#define UTILS_H

/*
 * utils.h — Кроссплатформенные утилиты
 * 
 * Linux, Windows (MSVC/MinGW), macOS.
 * Заменяет: unistd.h (sleep), sys/stat.h (chmod), locale.h.
 */

#include "config.h"

#include <string>
#include <sstream>
#include <iomanip>
#include <vector>
#include <fstream>
#include <algorithm>

#ifdef _WIN32
    #include <windows.h>
    #include <io.h>
    #define WIN32_LEAN_AND_MEAN
#else
    #include <sys/stat.h>
#endif

// ============================================================================
// Системные утилиты
// ============================================================================
namespace SysUtil {

// Кроссплатформенный sleep (секунды)
inline void sleepSec(int seconds) {
#ifdef _WIN32
    Sleep(static_cast<DWORD>(seconds) * 1000);
#else
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
#endif
}

// Кроссплатформенный sleep (миллисекунды)
inline void sleepMs(int milliseconds) {
#ifdef _WIN32
    Sleep(static_cast<DWORD>(milliseconds));
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
#endif
}

// Кроссплатформенная установка прав файла (600 — только владелец)
inline bool setFilePermissions(const std::string& path, int mode = 0600) {
#ifdef _WIN32
    // Windows: _chmod использует другой формат
    (void)path;
    (void)mode;
    return true;  // На Windows пропускаем
#else
    return chmod(path.c_str(), static_cast<mode_t>(mode)) == 0;
#endif
}

// Получение переменной окружения (кроссплатформенно)
inline std::string getEnv(const std::string& name) {
#ifdef _WIN32
    char* buf = nullptr;
    size_t sz = 0;
    if (_dupenv_s(&buf, &sz, name.c_str()) == 0 && buf != nullptr) {
        std::string result(buf);
        free(buf);
        return result;
    }
    return "";
#else
    const char* val = std::getenv(name.c_str());
    return val ? std::string(val) : "";
#endif
}

} // namespace SysUtil

// ============================================================================
// Форматирование
// ============================================================================
namespace Format {

inline std::string price(double value, int precision = 2) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << value;
    return oss.str();
}

inline std::string usdt(double value) {
    return "$" + price(value, 2);
}

inline std::string pct(double value) {
    std::ostringstream oss;
    oss << (value >= 0 ? "+" : "") << std::fixed << std::setprecision(2) << value << "%";
    return oss.str();
}

inline std::string crypto(double value) {
    if (value < 0.001)  return price(value, 8);
    if (value < 1.0)    return price(value, 6);
    if (value < 100.0)  return price(value, 4);
    return price(value, 2);
}

inline std::string volume(double value) {
    if (value >= 1e9)  return price(value / 1e9, 2) + "B";
    if (value >= 1e6)  return price(value / 1e6, 2) + "M";
    if (value >= 1e3)  return price(value / 1e3, 2) + "K";
    return price(value, 2);
}

} // namespace Format

// ============================================================================
// Работа со строками
// ============================================================================
namespace StrUtil {

inline std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream ss(s);
    while (std::getline(ss, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

inline std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\n\r");
    size_t end   = s.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

inline std::string toLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

inline bool contains(const std::string& haystack, const std::string& needle) {
    return toLower(haystack).find(toLower(needle)) != std::string::npos;
}

inline bool containsAny(const std::string& s, const std::vector<std::string>& keys) {
    std::string lower = toLower(s);
    for (const auto& key : keys) {
        if (lower.find(toLower(key)) != std::string::npos) return true;
    }
    return false;
}

} // namespace StrUtil

// ============================================================================
// Безопасность ключей
// ============================================================================
namespace KeyManager {

inline std::string maskKey(const std::string& key) {
    if (key.length() <= 8) return "****";
    return key.substr(0, 4) + "****" + key.substr(key.length() - 4);
}

} // namespace KeyManager

#endif // UTILS_H
