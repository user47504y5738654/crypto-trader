#ifndef EXCHANGE_CLIENT_H
#define EXCHANGE_CLIENT_H

/*
 * exchange_client.h — Клиент для работы с CoinEx API
 * 
 * Отвечает за:
 *   - Подпись запросов HMAC-SHA256
 *   - Получение баланса и цен
 *   - Отправку ордеров
 *   - Обработку ошибок API
 *   - Dry-run симуляцию
 */

#include "config.h"
#include "llm_agent.h"

#include <string>
#include <map>
#include <vector>
#include <mutex>

// ============================================================================
// Структура баланса
// ============================================================================
struct Balance {
    double available = 0.0;
    double frozen = 0.0;
};

// ============================================================================
// Статистика dry-run симуляции
// ============================================================================
struct DryRunStats {
    int total_trades = 0;
    int successful_trades = 0;
    double total_profit = 0.0;
};

// ============================================================================
// Класс клиента биржи
// ============================================================================
class ExchangeClient {
public:
    ExchangeClient(const std::string& api_key, const std::string& api_secret);
    ~ExchangeClient();
    
    // Получение баланса всех валют
    std::map<std::string, Balance> getBalance();
    
    // Получение текущей цены тикера
    double getTickerPrice(const std::string& symbol);
    
    // Получение контекста рынка (для LLM)
    MarketContext getMarketContext();
    
    // Размещение ордера
    OrderResult placeOrder(const OrderCommand& cmd);
    
    // Получение статуса ордера
    OrderStatus getOrderStatus(const std::string& order_id);
    
    // Отмена ордера
    bool cancelOrder(const std::string& order_id);
    
    // Проверка соединения с API
    bool isConnected();
    
    // Получение статистики dry-run
    DryRunStats getDryRunStats();
    
private:
    // Создание подписи HMAC-SHA256
    std::string signRequest(const std::string& method, const std::string& path,
                            const std::string& query_string, const std::string& body,
                            const std::string& timestamp);
    
    // Формирование заголовков для запроса
    std::map<std::string, std::string> buildHeaders(const std::string& method,
                                                     const std::string& path,
                                                     const std::string& body = "");
    
    // Отправка HTTP запроса
    std::string sendHttpRequest(const std::string& method, const std::string& endpoint,
                                 const std::string& body = "",
                                 const std::map<std::string, std::string>& headers = {});
    
    // Симуляция торговли (dry-run)
    OrderResult simulateOrder(const OrderCommand& cmd);
    
    // API ключи
    std::string m_api_key;
    std::string m_api_secret;
    
    // Имитация баланса для dry-run
    std::map<std::string, Balance> m_simulated_balance;
    
    // Статистика dry-run
    DryRunStats m_dry_run_stats;
    
    // Мьютекс
    std::mutex m_mutex;
};

#endif // EXCHANGE_CLIENT_H
