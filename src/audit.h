#ifndef AUDIT_H
#define AUDIT_H

/*
 * audit.h — Модуль аудита и логирования
 * 
 * Отвечает за:
 *   - JSONL-логирование каждого действия
 *   - SQLite для конфигурации, истории ордеров, статистики
 *   - Экспорт в CSV для анализа
 *   - Воспроизводимость сессий
 */

#include "config.h"

#include <string>
#include <fstream>
#include <mutex>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ============================================================================
// Структура записи ордера в истории
// ============================================================================
struct OrderRecord {
    std::string time;
    std::string side;        // "buy" или "sell"
    std::string symbol;
    double amount;
    double price;
    std::string status_str;
    std::string order_id;
};

// ============================================================================
// Класс логгера аудита
// ============================================================================
class AuditLogger {
public:
    AuditLogger(const std::string& db_path, const std::string& jsonl_path);
    ~AuditLogger();
    
    // Логирование команды (LLM → валидация → ответ)
    void logCommand(const OrderCommand& cmd, const std::string& status, 
                    const std::string& message);
    
    // Логирование системного события
    void logSystemEvent(const std::string& event, const std::string& details);
    
    // Логирование ошибки
    void logError(const std::string& context, const std::string& error_msg);
    
    // Получение последних ордеров
    std::vector<OrderRecord> getRecentOrders(int limit = 20);
    
    // Получение статистики
    json getStatistics();
    
    // Экспорт в CSV
    bool exportToCSV(const std::string& filepath);
    
private:
    // Инициализация SQLite
    void initDatabase();
    
    // Запись в JSONL
    void writeJSONL(const json& record);
    
    // Пути к файлам
    std::string m_db_path;
    std::string m_jsonl_path;
    
    // Файловый поток для JSONL
    std::ofstream m_jsonl_file;
    
    // SQLite3 (используем через C callback или простой формат)
    // TODO: Полноценная SQLite интеграция
    void* m_db = nullptr;
    
    // Мьютекс
    std::mutex m_mutex;
};

#endif // AUDIT_H
