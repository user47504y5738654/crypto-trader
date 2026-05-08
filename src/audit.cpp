/*
 * audit.cpp — Реализация модуля аудита
 * 
 * Обеспечивает полную прозрачность каждого действия:
 *   - Каждый шаг (запрос LLM → валидация → ответ биржи) пишется атомарно
 *   - JSONL-лог не модифицируется, только append
 *   - SQLite для структурированных данных
 *   - CSV-экспорт для анализа в Excel/Google Sheets
 */

#include "audit.h"
#include <iostream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <sqlite3.h>

// ============================================================================
// Вспомогательная: текущее время в строку
// ============================================================================
static std::string currentTimeStr() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm* tm = std::localtime(&now_time);
    
    std::ostringstream oss;
    oss << std::put_time(tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// ============================================================================
// Конструктор
// ============================================================================
AuditLogger::AuditLogger(const std::string& db_path, const std::string& jsonl_path)
    : m_db_path(db_path), m_jsonl_path(jsonl_path)
{
    // Открываем JSONL файл (append mode)
    m_jsonl_file.open(m_jsonl_path, std::ios::app);
    if (!m_jsonl_file.is_open()) {
        std::cerr << "[AUDIT] Не удалось открыть JSONL файл: " << m_jsonl_path << "\n";
    }
    
    // Инициализируем SQLite
    initDatabase();
    
    std::cout << "  [AUDIT] Логирование инициализировано:\n";
    std::cout << "  [AUDIT]   JSONL: " << m_jsonl_path << "\n";
    std::cout << "  [AUDIT]   SQLite: " << m_db_path << "\n";
}

// ============================================================================
// Деструктор
// ============================================================================
AuditLogger::~AuditLogger() {
    if (m_jsonl_file.is_open()) {
        m_jsonl_file.close();
    }
    
    if (m_db) {
        sqlite3_close(static_cast<sqlite3*>(m_db));
    }
}

// ============================================================================
// Инициализация SQLite
// ============================================================================
void AuditLogger::initDatabase() {
    sqlite3* db = nullptr;
    int rc = sqlite3_open(m_db_path.c_str(), &db);
    
    if (rc != SQLITE_OK) {
        std::cerr << "[AUDIT] Ошибка открытия SQLite: " << sqlite3_errmsg(db) << "\n";
        if (db) sqlite3_close(db);
        return;
    }
    
    m_db = db;
    
    // Создаём таблицы
    const char* create_tables = R"(
        CREATE TABLE IF NOT EXISTS orders (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp TEXT NOT NULL,
            action TEXT NOT NULL,
            symbol TEXT NOT NULL,
            amount REAL NOT NULL,
            price_type TEXT,
            price REAL,
            take_profit REAL,
            stop_loss REAL,
            status TEXT NOT NULL,
            message TEXT,
            order_id TEXT
        );
        
        CREATE TABLE IF NOT EXISTS config (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
        
        CREATE TABLE IF NOT EXISTS errors (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp TEXT NOT NULL,
            context TEXT,
            error_msg TEXT NOT NULL
        );
        
        CREATE TABLE IF NOT EXISTS events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp TEXT NOT NULL,
            event TEXT NOT NULL,
            details TEXT
        );
        
        -- Лимиты по умолчанию
        INSERT OR IGNORE INTO config (key, value) VALUES ('max_order_usd', '1000');
        INSERT OR IGNORE INTO config (key, value) VALUES ('daily_loss_limit', '200');
        INSERT OR IGNORE INTO config (key, value) VALUES ('max_open_positions', '5');
        INSERT OR IGNORE INTO config (key, value) VALUES ('max_market_deviation', '2');
    )";
    
    char* err_msg = nullptr;
    rc = sqlite3_exec(db, create_tables, nullptr, nullptr, &err_msg);
    
    if (rc != SQLITE_OK) {
        std::cerr << "[AUDIT] Ошибка создания таблиц: " << err_msg << "\n";
        sqlite3_free(err_msg);
    } else {
        std::cout << "  [AUDIT] Таблицы SQLite созданы/проверены.\n";
    }
}

// ============================================================================
// Запись в JSONL
// ============================================================================
void AuditLogger::writeJSONL(const json& record) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_jsonl_file.is_open()) {
        m_jsonl_file << record.dump() << std::endl;
        m_jsonl_file.flush();
    }
}

// ============================================================================
// Логирование команды
// ============================================================================
void AuditLogger::logCommand(const OrderCommand& cmd, const std::string& status,
                              const std::string& message) {
    std::string timestamp = currentTimeStr();
    
    // JSONL запись
    json jsonl_record = {
        {"type", "order"},
        {"timestamp", timestamp},
        {"action", cmd.action},
        {"symbol", cmd.symbol},
        {"amount", cmd.amount},
        {"price_type", cmd.price_type},
        {"price", cmd.price},
        {"take_profit", cmd.take_profit},
        {"stop_loss", cmd.stop_loss},
        {"status", status},
        {"message", message}
    };
    
    writeJSONL(jsonl_record);
    
    // SQLite запись
    if (m_db) {
        sqlite3* db = static_cast<sqlite3*>(m_db);
        
        std::string sql = "INSERT INTO orders (timestamp, action, symbol, amount, "
                         "price_type, price, take_profit, stop_loss, status, message) "
                         "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
        
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, timestamp.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, cmd.action.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, cmd.symbol.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 4, cmd.amount);
            sqlite3_bind_text(stmt, 5, cmd.price_type.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 6, cmd.price);
            sqlite3_bind_double(stmt, 7, cmd.take_profit);
            sqlite3_bind_double(stmt, 8, cmd.stop_loss);
            sqlite3_bind_text(stmt, 9, status.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 10, message.c_str(), -1, SQLITE_TRANSIENT);
            
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    
    std::cout << "  [AUDIT] Команда записана: " << status << "\n";
}

// ============================================================================
// Логирование системного события
// ============================================================================
void AuditLogger::logSystemEvent(const std::string& event, const std::string& details) {
    std::string timestamp = currentTimeStr();
    
    // JSONL
    json jsonl_record = {
        {"type", "event"},
        {"timestamp", timestamp},
        {"event", event},
        {"details", details}
    };
    
    writeJSONL(jsonl_record);
    
    // SQLite
    if (m_db) {
        sqlite3* db = static_cast<sqlite3*>(m_db);
        
        std::string sql = "INSERT INTO events (timestamp, event, details) VALUES (?, ?, ?)";
        sqlite3_stmt* stmt;
        
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, timestamp.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, event.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, details.c_str(), -1, SQLITE_TRANSIENT);
            
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
}

// ============================================================================
// Логирование ошибки
// ============================================================================
void AuditLogger::logError(const std::string& context, const std::string& error_msg) {
    std::string timestamp = currentTimeStr();
    
    // JSONL
    json jsonl_record = {
        {"type", "error"},
        {"timestamp", timestamp},
        {"context", context},
        {"error", error_msg}
    };
    
    writeJSONL(jsonl_record);
    
    // SQLite
    if (m_db) {
        sqlite3* db = static_cast<sqlite3*>(m_db);
        
        std::string sql = "INSERT INTO errors (timestamp, context, error_msg) VALUES (?, ?, ?)";
        sqlite3_stmt* stmt;
        
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, timestamp.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, context.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, error_msg.c_str(), -1, SQLITE_TRANSIENT);
            
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
}

// ============================================================================
// Получение последних ордеров
// ============================================================================
std::vector<OrderRecord> AuditLogger::getRecentOrders(int limit) {
    std::vector<OrderRecord> orders;
    
    if (!m_db) return orders;
    
    sqlite3* db = static_cast<sqlite3*>(m_db);
    std::string sql = "SELECT timestamp, action, symbol, amount, price, status, order_id "
                      "FROM orders ORDER BY id DESC LIMIT ?";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, limit);
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            OrderRecord rec;
            rec.time = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            rec.side = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            rec.symbol = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            rec.amount = sqlite3_column_double(stmt, 3);
            rec.price = sqlite3_column_double(stmt, 4);
            rec.status_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            
            if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
                rec.order_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            }
            
            orders.push_back(rec);
        }
        
        sqlite3_finalize(stmt);
    }
    
    return orders;
}

// ============================================================================
// Получение статистики
// ============================================================================
json AuditLogger::getStatistics() {
    json stats;
    
    if (!m_db) {
        stats["error"] = "База данных не инициализирована";
        return stats;
    }
    
    sqlite3* db = static_cast<sqlite3*>(m_db);
    sqlite3_stmt* stmt;
    
    // Общее количество ордеров
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM orders", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            stats["total_orders"] = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    
    // По статусам
    if (sqlite3_prepare_v2(db, "SELECT status, COUNT(*) FROM orders GROUP BY status", -1, &stmt, nullptr) == SQLITE_OK) {
        json status_counts;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            status_counts[status] = sqlite3_column_int(stmt, 1);
        }
        stats["status_counts"] = status_counts;
        sqlite3_finalize(stmt);
    }
    
    // Количество ошибок
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM errors", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            stats["total_errors"] = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    
    return stats;
}

// ============================================================================
// Экспорт в CSV
// ============================================================================
bool AuditLogger::exportToCSV(const std::string& filepath) {
    if (!m_db) return false;
    
    sqlite3* db = static_cast<sqlite3*>(m_db);
    
    // Используем SQLite CSV virtual table или просто запрашиваем и пишем
    std::ofstream csv(filepath);
    if (!csv.is_open()) return false;
    
    // Заголовки
    csv << "timestamp,action,symbol,amount,price_type,price,take_profit,stop_loss,status,message\n";
    
    sqlite3_stmt* stmt;
    std::string sql = "SELECT timestamp, action, symbol, amount, price_type, price, "
                      "take_profit, stop_loss, status, message FROM orders ORDER BY id";
    
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            for (int i = 0; i < 10; ++i) {
                if (i > 0) csv << ",";
                
                if (sqlite3_column_type(stmt, i) == SQLITE_NULL) {
                    csv << "";
                } else if (sqlite3_column_type(stmt, i) == SQLITE_FLOAT) {
                    csv << sqlite3_column_double(stmt, i);
                } else if (sqlite3_column_type(stmt, i) == SQLITE_INTEGER) {
                    csv << sqlite3_column_int(stmt, i);
                } else {
                    const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
                    // Экранируем кавычки
                    std::string s(val);
                    size_t pos = 0;
                    while ((pos = s.find('"', pos)) != std::string::npos) {
                        s.insert(pos, "\"");
                        pos += 2;
                    }
                    csv << "\"" << s << "\"";
                }
            }
            csv << "\n";
        }
        sqlite3_finalize(stmt);
    }
    
    csv.close();
    std::cout << "  [AUDIT] CSV экспорт завершён: " << filepath << "\n";
    return true;
}
