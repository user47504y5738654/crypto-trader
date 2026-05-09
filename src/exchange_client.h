#ifndef EXCHANGE_CLIENT_H
#define EXCHANGE_CLIENT_H

/*
 * exchange_client.h — Клиент CoinEx API v2
 * 
 * Полностью совместим с API v2 (документация: https://docs.coinex.com/api/v2/):
 *   - HMAC-SHA256 подпись: method + path[?query] + body + timestamp (без \n)
 *   - Все значения amount/price — строки
 *   - Обязательный market_type = "SPOT"
 *   - Заголовки: X-COINEX-KEY, X-COINEX-SIGN, X-COINEX-TIMESTAMP
 */

#include "config.h"

#include <string>
#include <map>
#include <vector>
#include <mutex>

// ============================================================================
// Класс клиента CoinEx API v2
// ============================================================================
class ExchangeClient {
public:
    ExchangeClient(const std::string& access_id, const std::string& secret_key);
    ~ExchangeClient();
    
    // ------------------------------------------------------------------------
    // Публичные методы
    // ------------------------------------------------------------------------
    
    // Получение баланса спотового аккаунта
    std::map<std::string, Balance> getSpotBalance();
    
    // Получение тикеров (один или несколько)
    std::map<std::string, Ticker> getTickers(const std::vector<std::string>& symbols = {});
    
    // Получение списка доступных рынков
    std::vector<std::string> getMarketList();
    
    // Получение открытых ордеров
    std::vector<OrderResult> getPendingOrders(const std::string& market = "",
                                               const std::string& side = "");
    
    // Получение завершённых ордеров
    std::vector<OrderResult> getFinishedOrders(const std::string& market = "",
                                                const std::string& side = "",
                                                int limit = 20);
    
    // Размещение ордера
    OrderResult placeOrder(const OrderCommand& cmd);
    
    // Отмена всех ордеров (опционально по рынку)
    bool cancelAllOrders(const std::string& market = "");
    
    // Отмена ордера по ID
    bool cancelOrder(const std::string& order_id, const std::string& market);
    
    // Получение полного контекста рынка (для стратега)
    MarketContext getMarketContext();
    
    // Проверка соединения
    bool isConnected();
    
    // Получение времени сервера
    int64_t getServerTime();
    
    // Dry-run: симуляция ордера
    OrderResult simulateOrder(const OrderCommand& cmd);
    
    // Статистика dry-run
    struct DryRunStats {
        int total_trades = 0;
        int successful_trades = 0;
        double total_profit = 0.0;
        double total_fees = 0.0;
    };
    DryRunStats getDryRunStats() const;
    
    // Сброс dry-run статистики
    void resetDryRunStats();
    
private:
    // ------------------------------------------------------------------------
    // Приватные методы
    // ------------------------------------------------------------------------
    
    // HMAC-SHA256 подпись (CoinEx API v2 формат)
    // Формат: method + request_path[?query] + body + timestamp
    // Результат: lowercase hex (64 символа)
    std::string createSignature(const std::string& method,
                                 const std::string& request_path,
                                 const std::string& body,
                                 const std::string& timestamp);
    
    // Отправка HTTP-запроса
    std::string httpRequest(const std::string& method,
                             const std::string& path,
                             const std::string& body = "",
                             const std::string& query_string = "");
    
    // Парсинг строки в double (безопасно)
    static double parseDouble(const std::string& s);
    
    // Генерация ID клиента
    static std::string generateClientId();
    
    // ------------------------------------------------------------------------
    // Поля
    // ------------------------------------------------------------------------
    
    std::string m_access_id;       // CoinEx Access ID (API Key)
    std::string m_secret_key;      // CoinEx Secret Key
    
    // Симулированный баланс для dry-run
    std::map<std::string, Balance> m_sim_balance;
    
    // Статистика dry-run
    DryRunStats m_stats;
    
    // Мьютекс
    mutable std::mutex m_mutex;
};

#endif // EXCHANGE_CLIENT_H
