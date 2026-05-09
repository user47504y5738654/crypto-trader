#ifndef AUDIT_H
#define AUDIT_H

/*
 * audit.h — Аудит и логирование
 * 
 * Записывает все действия в SQLite + JSONL для полной прозрачности.
 */

#include "config.h"

#include <string>
#include <fstream>
#include <mutex>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ============================================================================
// Запись в истории ордеров
// ============================================================================
struct OrderRecord {
    std::string time;
    std::string side;
    std::string symbol;
    double amount = 0.0;
    double price = 0.0;
    std::string status_str;
    std::string order_id;
    std::string reason;
};

// ============================================================================
// Логгер аудита
// ============================================================================
class AuditLogger {
public:
    AuditLogger(const std::string& db_path, const std::string& jsonl_path);
    ~AuditLogger();
    
    // Логирование ордера
    void logOrder(const OrderCommand& cmd, const std::string& status,
                  const std::string& message, const std::string& order_id = "");
    
    // Логирование решения стратега
    void logStrategistDecision(const StrategistDecision& decision,
                                 const std::string& status);
    
    // Логирование системного события
    void logSystemEvent(const std::string& event, const std::string& details);
    
    // Логирование ошибки
    void logError(const std::string& context, const std::string& error_msg);
    
    // Получение истории
    std::vector<OrderRecord> getRecentOrders(int limit = 20);
    
    // Статистика
    json getStatistics();
    
    // CSV экспорт
    bool exportToCSV(const std::string& filepath);
    
private:
    void initDatabase();
    void writeJSONL(const json& record);
    
    std::string m_db_path;
    std::string m_jsonl_path;
    std::ofstream m_jsonl_file;
    void* m_db = nullptr;
    std::mutex m_mutex;
};

#endif // AUDIT_H
