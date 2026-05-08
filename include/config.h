#ifndef CONFIG_H
#define CONFIG_H

/*
 * config.h — Файл конфигурации приложения
 * Содержит ключевые настройки и макросы для работы трейдера.
 * Комментарии на русском для удобства разработки.
 */

#include <string>
#include <vector>
#include <chrono>

// ============================================================================
// Версия приложения
// ============================================================================
#define APP_VERSION "1.0.0"
#define APP_NAME "CryptoTrader — AI-трейдер"

// ============================================================================
// Режимы работы
// ============================================================================
enum class TradingMode {
    DRY_RUN,  // Тестовый режим: анализ без реальных торгов
    LIVE      // Боевой режим: активная торговля
};

// ============================================================================
// Типы ордеров
// ============================================================================
enum class OrderType {
    MARKET,  // Рыночный ордер
    LIMIT    // Лимитный ордер
};

enum class OrderSide {
    BUY,   // Покупка
    SELL   // Продажа
};

enum class OrderStatus {
    PENDING,      // Ожидает исполнения
    FILLED,       // Исполнен полностью
    PARTIALLY,    // Исполнен частично
    CANCELLED,    // Отменён
    REJECTED,     // Отклонён
    EXPIRED       // Истёк
};

// ============================================================================
// Структура ордера (то, что возвращает LLM)
// ============================================================================
struct OrderCommand {
    std::string action;        // "buy" или "sell"
    std::string symbol;        // "ETH/USDT", "BTC/USDT" и т.д.
    double amount;             // Количество
    std::string price_type;    // "market" или "limit"
    double price;              // Цена (для лимитных ордеров)
    double take_profit;        // Тейк-профит в %
    double stop_loss;          // Стоп-лосс в %
};

// ============================================================================
// Структура результата ордера
// ============================================================================
struct OrderResult {
    std::string order_id;      // ID ордера на бирже
    OrderStatus status;        // Статус
    double filled_amount;      // Исполненный объём
    double filled_price;       // Средняя цена исполнения
    double fee;                // Комиссия
    std::string error_msg;     // Сообщение об ошибке (если есть)
};

// ============================================================================
// Лимиты безопасности (можно загружать из SQLite)
// ============================================================================
struct RiskLimits {
    double max_order_usd = 1000.0;        // Максимальный ордер в USD
    double daily_loss_limit = 200.0;      // Лимит дневного убытка
    int max_open_positions = 5;           // Макс. открытых позиций
    double max_market_deviation = 2.0;    // Макс. отклонение от рынка для лимитных ордеров (%)
    int circuit_breaker_count = 3;        // Кол-во ошибок для автопаузы
    int circuit_breaker_seconds = 300;    // Длительность паузы (5 мин)
};

// ============================================================================
// Настройки CoinEx API
// ============================================================================
namespace CoinExConfig {
    const std::string BASE_URL = "https://api.coinex.com";
    const std::string SPOT_ORDER_ENDPOINT = "/v2/spot/order";
    const std::string BALANCE_ENDPOINT = "/v2/spot/balance";
    const std::string TICKER_ENDPOINT = "/v2/spot/ticker";
    const std::string MARKET_ENDPOINT = "/v2/spot/market";
};

// ============================================================================
// Настройки DeepSeek API
// ============================================================================
namespace DeepSeekConfig {
    const std::string API_URL = "https://api.deepseek.com/v1/chat/completions";
    const std::string MODEL = "deepseek-chat";
    const int MAX_TOKENS = 500;
    const double TEMPERATURE = 0.1;  // Низкая температура для детерминированных ответов
};

#endif // CONFIG_H
