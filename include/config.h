#ifndef CONFIG_H
#define CONFIG_H

/*
 * config.h — Конфигурация всего проекта
 * 
 * Содержит: структуры данных, enum-ы, лимиты безопасности,
 * константы API (CoinEx v2, DeepSeek), типы стратегий.
 * Комментарии на русском.
 */

#include <string>
#include <vector>
#include <map>

// ============================================================================
// Версия приложения
// ============================================================================
#define APP_VERSION "2.0.0"
#define APP_NAME    "CryptoTrader — AI-Стратег"

// ============================================================================
// Режимы работы
// ============================================================================
enum class TradingMode {
    DRY_RUN,     // Симуляция без реальных торгов
    LIVE         // Реальная торговля
};

// ============================================================================
// Режимы стратега (DeepSeek)
// ============================================================================
enum class StrategistMode {
    MANUAL,      // Пользователь даёт команды, DeepSeek только парсит
    SEMI_AUTO,   // DeepSeek предлагает сделки, пользователь подтверждает
    FULL_AUTO    // DeepSeek торгует самостоятельно (с лимитами)
};

// ============================================================================
// Типы ордеров
// ============================================================================
enum class OrderType {
    MARKET,      // Рыночный
    LIMIT        // Лимитный
};

enum class OrderSide {
    BUY,
    SELL
};

enum class OrderStatus {
    PENDING,
    FILLED,
    PARTIALLY_FILLED,
    CANCELLED,
    REJECTED,
    EXPIRED
};

// ============================================================================
// Структура ордера (от LLM или стратега)
// ============================================================================
struct OrderCommand {
    std::string action;         // "buy" / "sell"
    std::string symbol;         // "ETHUSDT" (формат CoinEx: без "/")
    std::string market_type;    // "SPOT" (обязательно для CoinEx API v2)
    double amount = 0.0;
    std::string price_type;     // "market" / "limit"
    double price = 0.0;
    double take_profit = 0.0;   // Тейк-профит (%)
    double stop_loss = 0.0;     // Стоп-лосс (%)
    std::string client_id;      // Пользовательский ID ордера
    std::string reason;         // Причина сделки (от стратега)
};

// ============================================================================
// Результат исполнения ордера
// ============================================================================
struct OrderResult {
    std::string order_id;
    OrderStatus status = OrderStatus::PENDING;
    double filled_amount = 0.0;
    double filled_price = 0.0;
    double filled_value = 0.0;
    double base_fee = 0.0;      // Комиссия в базовой валюте
    double quote_fee = 0.0;     // Комиссия в котируемой валюте
    std::string error_msg;
    int64_t created_at = 0;
};

// ============================================================================
// Баланс валюты
// ============================================================================
struct Balance {
    double available = 0.0;
    double frozen = 0.0;
    
    double total() const { return available + frozen; }
};

// ============================================================================
// Тикер (рыночные данные)
// ============================================================================
struct Ticker {
    std::string market;         // "BTCUSDT"
    double last = 0.0;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double volume = 0.0;
    double value = 0.0;         // Объём в USDT
    double change_pct = 0.0;    // Изменение за 24ч (%)
};

// ============================================================================
// Контекст рынка (передаётся стратегу DeepSeek)
// ============================================================================
struct MarketContext {
    std::map<std::string, Ticker> tickers;          // Все тикеры
    std::map<std::string, Balance> balances;        // Балансы
    std::vector<OrderResult> open_orders;           // Открытые ордера
    std::map<std::string, double> positions;        // Открытые позиции (symbol -> amount)
    int64_t server_time = 0;                        // Время сервера (UNIX мс)
    
    // Дневная статистика
    double daily_pnl = 0.0;
    int daily_trades = 0;
};

// ============================================================================
// Решение стратега (DeepSeek)
// ============================================================================
struct StrategistDecision {
    std::string action;          // "buy" / "sell" / "hold" / "cancel"
    std::string symbol;          // "ETHUSDT"
    double amount = 0.0;
    std::string price_type;      // "market" / "limit"
    double price = 0.0;
    double take_profit = 0.0;
    double stop_loss = 0.0;
    double confidence = 0.0;     // Уверенность стратега (0.0–1.0)
    std::string reasoning;       // Объяснение решения
    std::string strategy_name;   // Название стратегии
    
    // Для отмены ордеров
    std::vector<std::string> cancel_order_ids;
    
    // Статус: нужно ли действовать
    bool should_act = false;
};

// ============================================================================
// Конфигурация стратегии
// ============================================================================
struct StrategyConfig {
    std::string name;
    bool enabled = true;
    
    // Торгуемые пары
    std::vector<std::string> symbols = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    
    // Лимиты
    double max_position_percent = 999999.0;     // Макс. позиция (снят)
    double max_order_usd = 999999999.0;         // Макс. ордер (снят)
    double daily_loss_limit = 999999999.0;      // Дневной лимит (снят)
    int max_daily_trades = 999999;              // Макс. сделок (снят)
    
    // Параметры стратегии
    double min_confidence = 0.0;                // Мин. уверенность (снят)
    double take_profit_default = 5.0;      // Тейк-профит по умолчанию (%)
    double stop_loss_default = 3.0;        // Стоп-лосс по умолчанию (%)
    
    // Интервал анализа (секунды)
    int analysis_interval_sec = 300;       // 5 минут
};

// ============================================================================
// Лимиты безопасности (глобальные)
// ============================================================================
struct RiskLimits {
    double max_order_usd = 999999999.0;       // Макс. ордер (снят)
    double daily_loss_limit = 999999999.0;     // Дневной лимит (снят)
    double max_total_exposure_pct = 999999.0;  // Макс. экспозиция (снят)
    int max_open_positions = 999999;           // Макс. позиций (снят)
    double max_market_deviation_pct = 999999.0;
    int circuit_breaker_errors = 999999;       // Circuit breaker (снят)
    int circuit_breaker_seconds = 0;           // Circuit breaker (снят)
    
    // Минимальная уверенность стратега для автоматической сделки
    double min_auto_confidence = 0.0;          // (снят)
};

// ============================================================================
// Ответ чата с DeepSeek (свободное общение + опциональная торговля)
// ============================================================================
struct ChatResponse {
    std::string message;            // Ответ на естественном языке
    bool wants_to_trade = false;    // Хочет ли пользователь торговать
    OrderCommand trade;             // Ордер (если wants_to_trade)
};

// ============================================================================
// CoinEx API v2 — конфигурация
// ============================================================================
namespace CoinExConfig {
    // Базовый URL API v2 (документация: https://docs.coinex.com/api/v2/)
    const std::string BASE_URL = "https://api.coinex.com/v2";
    
    // Spot — ордера
    // POST /spot/order              — Создать ордер
    // DELETE /spot/order             — Отменить все ордера
    // DELETE /spot/order?order_id=X  — Отменить ордер по ID
    const std::string SPOT_ORDER = "/spot/order";
    
    // Spot — открытые/завершённые ордера
    const std::string SPOT_PENDING_ORDERS  = "/spot/pending-order";   // GET
    const std::string SPOT_FINISHED_ORDERS = "/spot/finished-order";  // GET
    
    // Spot — статус ордера
    // GET /spot/order-status?order_id=X&market=Y
    const std::string SPOT_ORDER_STATUS = "/spot/order-status";
    
    // Spot — рынок (тикеры)
    // GET /spot/ticker?market=BTCUSDT,ETHUSDT
    const std::string SPOT_TICKER = "/spot/ticker";
    
    // Spot — список рынков
    // GET /spot/market
    const std::string SPOT_MARKET = "/spot/market";
    
    // Активы — баланс
    // GET /assets/spot/balance
    const std::string SPOT_BALANCE = "/assets/spot/balance";
    
    // Тип рынка для спота (обязательный параметр)
    const std::string MARKET_TYPE_SPOT = "SPOT";
    
    // Комиссия по умолчанию
    const double DEFAULT_FEE_RATE = 0.002;   // 0.2%
};

// ============================================================================
// DeepSeek API — конфигурация
// ============================================================================
namespace DeepSeekConfig {
    // API endpoint (OpenAI-совместимый)
    // Документация: https://platform.deepseek.com/api-docs
    const std::string API_URL = "https://api.deepseek.com/v1/chat/completions";
    
    // Модель
    const std::string MODEL = "deepseek-chat";
    
    // Параметры для режима «парсер команд»
    const int PARSER_MAX_TOKENS = 500;
    const double PARSER_TEMPERATURE = 0.1;
    
    // Параметры для режима «стратег»
    const int STRATEGIST_MAX_TOKENS = 2000;
    const double STRATEGIST_TEMPERATURE = 0.3;  // Немного креативности для анализа
};

#endif // CONFIG_H
