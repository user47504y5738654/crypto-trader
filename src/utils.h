#ifndef UTILS_H
#define UTILS_H

/*
 * utils.h — Вспомогательные функции
 * 
 * Содержит общие утилиты, используемые в разных модулях:
 *   - Форматирование чисел и валют
 *   - Работа с временем
 *   - Обработка строк
 *   - Безопасное хранение ключей (заглушка для OS Keyring)
 */

#include "config.h"
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>

// ============================================================================
// Форматирование
// ============================================================================
namespace Format {
    // Форматирование числа с указанной точностью
    inline std::string price(double value, int precision = 2) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(precision) << value;
        return oss.str();
    }
    
    // Форматирование валюты
    inline std::string usdt(double value) {
        return "$" + price(value, 2);
    }
    
    // Форматирование процентов
    inline std::string percent(double value) {
        return price(value, 1) + "%";
    }
    
    // Форматирование объёма криптовалюты
    inline std::string crypto(double value) {
        if (value < 0.001) return price(value, 8);
        if (value < 1.0) return price(value, 6);
        if (value < 100.0) return price(value, 4);
        return price(value, 2);
    }
}

// ============================================================================
// Работа со строками
// ============================================================================
namespace StrUtil {
    // Разделение строки по разделителю
    inline std::vector<std::string> split(const std::string& s, char delimiter) {
        std::vector<std::string> tokens;
        std::string token;
        std::istringstream tokenStream(s);
        while (std::getline(tokenStream, token, delimiter)) {
            tokens.push_back(token);
        }
        return tokens;
    }
    
    // Обрезка пробелов
    inline std::string trim(const std::string& s) {
        size_t start = s.find_first_not_of(" \t\n\r");
        size_t end = s.find_last_not_of(" \t\n\r");
        if (start == std::string::npos) return "";
        return s.substr(start, end - start + 1);
    }
    
    // Приведение к нижнему регистру
    inline std::string toLower(const std::string& s) {
        std::string result = s;
        std::transform(result.begin(), result.end(), result.begin(), ::tolower);
        return result;
    }
}

// ============================================================================
// Управление ключами (заглушка для OS Keyring)
// ============================================================================
namespace KeyManager {
    // Сохранение ключа (в реальном приложении — OS Keyring)
    inline bool saveKey(const std::string& service, const std::string& key, const std::string& value) {
        // TODO: Использовать libsecret или keyring
        // Пока сохраняем в файл с ограниченными правами
        std::ofstream file(service + "_" + key + ".key");
        if (file.is_open()) {
            file << value;
            file.close();
            chmod((service + "_" + key + ".key").c_str(), 0600);  // Только владелец
            return true;
        }
        return false;
    }
    
    // Загрузка ключа
    inline std::string loadKey(const std::string& service, const std::string& key) {
        std::ifstream file(service + "_" + key + ".key");
        std::string value;
        if (file.is_open()) {
            std::getline(file, value);
            file.close();
        }
        return value;
    }
    
    // Проверка, что ключи не попадают в логи
    inline std::string maskKey(const std::string& key) {
        if (key.length() <= 8) return "****";
        return key.substr(0, 4) + "****" + key.substr(key.length() - 4);
    }
}

#endif // UTILS_H
