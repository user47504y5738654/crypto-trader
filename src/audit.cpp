/*
 * audit.cpp — Реализация аудита и логирования
 * 
 * SQLite: структурированные данные (ордера, события, ошибки)
 * JSONL:  append-only лог (каждая строка — JSON)
 * CSV:    экспорт для анализа
 */

#include "audit.h"

#include <iostream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <sqlite3.h>

// ============================================================================
static std::string nowStr() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm* tm = std::localtime(&t);
    std::ostringstream oss;
    oss << std::put_time(tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// ============================================================================
AuditLogger::AuditLogger(const std::string& db_path, const std::string& jsonl_path)
    : m_db_path(db_path), m_jsonl_path(jsonl_path)
{
    m_jsonl_file.open(m_jsonl_path, std::ios::app);
    if (!m_jsonl_file.is_open()) {
        std::cerr << "[AUDIT] Не удалось открыть JSONL: " << m_jsonl_path << "\n";
    }
    
    initDatabase();
    
    std::cout << "  [AUDIT] Логирование: JSONL=" << m_jsonl_path 
              << ", SQLite=" << m_db_path << "\n";
}

AuditLogger::~AuditLogger() {
    if (m_jsonl_file.is_open()) m_jsonl_file.close();
    if (m_db) sqlite3_close(static_cast<sqlite3*>(m_db));
}

// ============================================================================
void AuditLogger::initDatabase() {
    sqlite3* db = nullptr;
    int rc = sqlite3_open(m_db_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        std::cerr << "[AUDIT] Ошибка SQLite: " << sqlite3_errmsg(db) << "\n";
        if (db) sqlite3_close(db);
        return;
    }
    m_db = db;
    
    const char* sql = R"(
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
            order_id TEXT,
            reason TEXT
        );
        
        CREATE TABLE IF NOT EXISTS strategist_decisions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp TEXT NOT NULL,
            action TEXT NOT NULL,
            symbol TEXT,
            amount REAL,
            confidence REAL,
            reasoning TEXT,
            strategy_name TEXT,
            status TEXT
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
        
        INSERT OR IGNORE INTO events (timestamp, event, details) 
        VALUES (datetime('now'), 'db_init', 'v2.0.0');
    )";
    
    char* err = nullptr;
    rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "[AUDIT] Ошибка таблиц: " << err << "\n";
        sqlite3_free(err);
    }
}

// ============================================================================
void AuditLogger::writeJSONL(const json& record) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_jsonl_file.is_open()) {
        m_jsonl_file << record.dump() << std::endl;
        m_jsonl_file.flush();
    }
}

// ============================================================================
void AuditLogger::logOrder(const OrderCommand& cmd, const std::string& status,
                             const std::string& message, const std::string& order_id) {
    std::string ts = nowStr();
    
    json j = {
        {"type", "order"},
        {"timestamp", ts},
        {"action", cmd.action},
        {"symbol", cmd.symbol},
        {"amount", cmd.amount},
        {"price_type", cmd.price_type},
        {"price", cmd.price},
        {"take_profit", cmd.take_profit},
        {"stop_loss", cmd.stop_loss},
        {"status", status},
        {"message", message},
        {"order_id", order_id}
    };
    writeJSONL(j);
    
    if (!m_db) return;
    auto* db = static_cast<sqlite3*>(m_db);
    
    const char* sql = "INSERT INTO orders (timestamp,action,symbol,amount,price_type,"
                      "price,take_profit,stop_loss,status,message,order_id,reason) "
                      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, cmd.action.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, cmd.symbol.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 4, cmd.amount);
        sqlite3_bind_text(stmt, 5, cmd.price_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 6, cmd.price);
        sqlite3_bind_double(stmt, 7, cmd.take_profit);
        sqlite3_bind_double(stmt, 8, cmd.stop_loss);
        sqlite3_bind_text(stmt, 9, status.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 10, message.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 11, order_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 12, cmd.reason.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void AuditLogger::logStrategistDecision(const StrategistDecision& decision,
                                          const std::string& status) {
    std::string ts = nowStr();
    
    json j = {
        {"type", "strategist"},
        {"timestamp", ts},
        {"action", decision.action},
        {"symbol", decision.symbol},
        {"amount", decision.amount},
        {"confidence", decision.confidence},
        {"reasoning", decision.reasoning},
        {"strategy", decision.strategy_name},
        {"status", status}
    };
    writeJSONL(j);
    
    if (!m_db) return;
    auto* db = static_cast<sqlite3*>(m_db);
    
    const char* sql = "INSERT INTO strategist_decisions (timestamp,action,symbol,amount,"
                      "confidence,reasoning,strategy_name,status) "
                      "VALUES (?,?,?,?,?,?,?,?)";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, decision.action.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, decision.symbol.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 4, decision.amount);
        sqlite3_bind_double(stmt, 5, decision.confidence);
        sqlite3_bind_text(stmt, 6, decision.reasoning.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, decision.strategy_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, status.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void AuditLogger::logSystemEvent(const std::string& event, const std::string& details) {
    std::string ts = nowStr();
    
    json j = {{"type","event"},{"timestamp",ts},{"event",event},{"details",details}};
    writeJSONL(j);
    
    if (!m_db) return;
    auto* db = static_cast<sqlite3*>(m_db);
    const char* sql = "INSERT INTO events (timestamp,event,details) VALUES (?,?,?)";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, event.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, details.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void AuditLogger::logError(const std::string& context, const std::string& error_msg) {
    std::string ts = nowStr();
    
    json j = {{"type","error"},{"timestamp",ts},{"context",context},{"error",error_msg}};
    writeJSONL(j);
    
    if (!m_db) return;
    auto* db = static_cast<sqlite3*>(m_db);
    const char* sql = "INSERT INTO errors (timestamp,context,error_msg) VALUES (?,?,?)";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, context.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, error_msg.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// ============================================================================
std::vector<OrderRecord> AuditLogger::getRecentOrders(int limit) {
    std::vector<OrderRecord> orders;
    if (!m_db) return orders;
    
    auto* db = static_cast<sqlite3*>(m_db);
    const char* sql = "SELECT timestamp,action,symbol,amount,price,status,order_id,reason "
                      "FROM orders ORDER BY id DESC LIMIT ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, limit);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            OrderRecord r;
            r.time = (const char*)sqlite3_column_text(stmt, 0);
            r.side = (const char*)sqlite3_column_text(stmt, 1);
            r.symbol = (const char*)sqlite3_column_text(stmt, 2);
            r.amount = sqlite3_column_double(stmt, 3);
            r.price = sqlite3_column_double(stmt, 4);
            r.status_str = (const char*)sqlite3_column_text(stmt, 5);
            if (sqlite3_column_type(stmt, 6) != SQLITE_NULL)
                r.order_id = (const char*)sqlite3_column_text(stmt, 6);
            if (sqlite3_column_type(stmt, 7) != SQLITE_NULL)
                r.reason = (const char*)sqlite3_column_text(stmt, 7);
            orders.push_back(r);
        }
        sqlite3_finalize(stmt);
    }
    return orders;
}

json AuditLogger::getStatistics() {
    json stats;
    if (!m_db) return {{"error","no db"}};
    
    auto* db = static_cast<sqlite3*>(m_db);
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM orders", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) stats["total_orders"] = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM strategist_decisions", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) stats["strategist_decisions"] = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM errors", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) stats["total_errors"] = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return stats;
}

bool AuditLogger::exportToCSV(const std::string& filepath) {
    if (!m_db) return false;
    auto* db = static_cast<sqlite3*>(m_db);
    
    std::ofstream csv(filepath);
    if (!csv.is_open()) return false;
    
    csv << "timestamp,action,symbol,amount,price_type,price,status,reason\n";
    
    const char* sql = "SELECT timestamp,action,symbol,amount,price_type,price,status,reason "
                      "FROM orders ORDER BY id";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            for (int i = 0; i < 8; ++i) {
                if (i > 0) csv << ",";
                if (sqlite3_column_type(stmt, i) == SQLITE_NULL)
                    csv << "";
                else if (sqlite3_column_type(stmt, i) == SQLITE_FLOAT)
                    csv << sqlite3_column_double(stmt, i);
                else
                    csv << "\"" << (const char*)sqlite3_column_text(stmt, i) << "\"";
            }
            csv << "\n";
        }
        sqlite3_finalize(stmt);
    }
    csv.close();
    std::cout << "  [AUDIT] CSV: " << filepath << "\n";
    return true;
}
