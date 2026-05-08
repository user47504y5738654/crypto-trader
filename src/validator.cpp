/*
 * validator.cpp — Реализация модуля валидации
 * 
 * Защита от ошибок и рисков перед отправкой ордера.
 * Работает как "предохранитель" между LLM и биржей.
 */

#include "validator.h"
#include <iostream>
#include <algorithm>
#include <cmath>

// ============================================================================
// Конструктор
// ============================================================================
Validator::Validator() {
    m_day_start = std::chrono::system_clock::now();
}

// ============================================================================
// Деструктор
// ============================================================================
Validator::~Validator() = default;

// ============================================================================
// Получение текущих лимитов
// ============================================================================
RiskLimits Validator::getLimits() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_limits;
}

// ============================================================================
// Обновление лимитов
// ============================================================================
void Validator::updateLimits(const RiskLimits& limits) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_limits = limits;
}

// ============================================================================
// Загрузка лимитов (заглушка — позже подключим SQLite)
// ============================================================================
void Validator::loadLimits() {
    std::lock_guard<std::mutex> lock(m_mutex);
    // TODO: Загружать из SQLite таблицы config
    // Пока используем значения по умолчанию из config.h
    std::cout << "  [VALIDATOR] Лимиты загружены (по умолчанию)\n";
}

// ============================================================================
// Сохранение лимитов (заглушка)
// ============================================================================
void Validator::saveLimits(const RiskLimits& limits) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_limits = limits;
    // TODO: Сохранять в SQLite
}

// ============================================================================
// Проверка circuit breaker
// ============================================================================
bool Validator::isCircuitBreakerActive() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_circuit_breaker_active) {
        return false;
    }
    
    // Проверяем, не прошло ли время паузы
    auto now = std::chrono::steady_clock::now();
    if (now >= m_circuit_breaker_until) {
        m_circuit_breaker_active = false;
        m_error_count = 0;
        std::cout << "  [VALIDATOR] Circuit breaker сброшен. Торговля возобновлена.\n";
        return false;
    }
    
    return true;
}

// ============================================================================
// Регистрация ошибки
// ============================================================================
void Validator::registerError() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    m_error_count++;
    std::cout << "  [VALIDATOR] Ошибка #" << m_error_count 
              << "/" << m_limits.circuit_breaker_count << "\n";
    
    if (m_error_count >= m_limits.circuit_breaker_count) {
        m_circuit_breaker_active = true;
        m_circuit_breaker_until = std::chrono::steady_clock::now() + 
            std::chrono::seconds(m_limits.circuit_breaker_seconds);
        
        std::cout << "  [VALIDATOR] ⚠ CIRCUIT BREAKER АКТИВИРОВАН!\n";
        std::cout << "  [VALIDATOR] Пауза " << m_limits.circuit_breaker_seconds 
                  << " секунд.\n";
    }
}

// ============================================================================
// Сброс circuit breaker
// ============================================================================
void Validator::resetCircuitBreaker() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_error_count = 0;
    m_circuit_breaker_active = false;
}

// ============================================================================
// Проверка дневного лимита убытков
// ============================================================================
bool Validator::checkDailyLossLimit(double potential_loss) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Сброс в начале нового дня
    auto now = std::chrono::system_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::hours>(now - m_day_start);
    
    if (diff.count() >= 24) {
        m_daily_pnl = 0.0;
        m_day_start = now;
    }
    
    double projected_pnl = m_daily_pnl - potential_loss;
    return projected_pnl >= -m_limits.daily_loss_limit;
}

// ============================================================================
// Регистрация сделки для дневного трекинга
// ============================================================================
void Validator::registerTrade(double profit_loss) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_daily_pnl += profit_loss;
}

// ============================================================================
// Основной метод валидации
// ============================================================================
ValidationResult Validator::validate(const OrderCommand& cmd, 
                                      const MarketContext& context) {
    
    // Проверяем circuit breaker
    if (isCircuitBreakerActive()) {
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            m_circuit_breaker_until - std::chrono::steady_clock::now()).count();
        
        return {false, "Circuit breaker активен. Осталось " + std::to_string(remaining) + "с паузы.", {}};
    }
    
    // 1. Проверка схемы
    auto schema_result = validateSchema(cmd);
    if (!schema_result.is_valid) {
        registerError();
        return schema_result;
    }
    
    // 2. Проверка безопасности
    auto safety_result = validateSafety(cmd);
    if (!safety_result.is_valid) {
        registerError();
        return safety_result;
    }
    
    // 3. Проверка цен
    auto price_result = validatePrices(cmd, context);
    if (!price_result.is_valid) {
        return price_result;
    }
    
    // 4. Проверка лимитов
    auto limits_result = validateLimits(cmd, context);
    if (!limits_result.is_valid) {
        return limits_result;
    }
    
    // Сброс счётчика ошибок при успехе
    resetCircuitBreaker();
    
    // Собираем все предупреждения
    ValidationResult result;
    result.is_valid = true;
    result.warnings = schema_result.warnings;
    for (const auto& w : safety_result.warnings) result.warnings.push_back(w);
    for (const auto& w : price_result.warnings) result.warnings.push_back(w);
    for (const auto& w : limits_result.warnings) result.warnings.push_back(w);
    
    return result;
}

// ============================================================================
// Проверка схемы ордера
// ============================================================================
ValidationResult Validator::validateSchema(const OrderCommand& cmd) {
    ValidationResult result;
    
    // Проверка действия
    if (cmd.action != "buy" && cmd.action != "sell") {
        result.is_valid = false;
        result.error_message = "Неизвестное действие: " + cmd.action + ". Допустимо: buy/sell.";
        return result;
    }
    
    // Проверка символа
    if (cmd.symbol.empty()) {
        result.is_valid = false;
        result.error_message = "Не указана торговая пара (symbol).";
        return result;
    }
    
    // Проверка: только спот (нет USDT/USDT и т.п.)
    if (cmd.symbol.find('/') == std::string::npos) {
        result.is_valid = false;
        result.error_message = "Неверный формат пары. Нужно: BTC/USDT, ETH/USDT и т.д.";
        return result;
    }
    
    // Проверка объёма
    if (cmd.amount <= 0) {
        result.is_valid = false;
        result.error_message = "Объём должен быть положительным числом.";
        return result;
    }
    
    // Проверка типа цены
    if (cmd.price_type != "market" && cmd.price_type != "limit") {
        result.is_valid = false;
        result.error_message = "Тип цены должен быть 'market' или 'limit'.";
        return result;
    }
    
    // Для лимитных ордеров цена обязательна
    if (cmd.price_type == "limit" && cmd.price <= 0) {
        result.is_valid = false;
        result.error_message = "Для лимитного ордера укажите цену.";
        return result;
    }
    
    // Проверка тейков/стопов
    if (cmd.take_profit < 0) {
        result.warnings.push_back("Тейк-профит не может быть отрицательным. Установлен в 0.");
    }
    if (cmd.stop_loss < 0) {
        result.warnings.push_back("Стоп-лосс не может быть отрицательным. Установлен в 0.");
    }
    
    return result;
}

// ============================================================================
// Проверка безопасности
// ============================================================================
ValidationResult Validator::validateSafety(const OrderCommand& cmd) {
    ValidationResult result;
    
    // Запрет фьючерсов и маржи
    std::string lower_symbol = cmd.symbol;
    std::transform(lower_symbol.begin(), lower_symbol.end(), lower_symbol.begin(), ::tolower);
    
    if (lower_symbol.find("perp") != std::string::npos ||
        lower_symbol.find("futures") != std::string::npos ||
        lower_symbol.find("margin") != std::string::npos) {
        result.is_valid = false;
        result.error_message = "БЛОКИРОВКА: Фьючерсы и маржинальная торговля запрещены. Только спот.";
        return result;
    }
    
    return result;
}

// ============================================================================
// Проверка цен
// ============================================================================
ValidationResult Validator::validatePrices(const OrderCommand& cmd, 
                                            const MarketContext& context) {
    ValidationResult result;
    
    if (cmd.price_type != "limit") {
        return result;  // Рыночные ордера не проверяем по цене
    }
    
    // Получаем текущую рыночную цену
    double market_price = 0.0;
    if (cmd.symbol == "BTC/USDT") market_price = context.btc_price;
    else if (cmd.symbol == "ETH/USDT") market_price = context.eth_price;
    else if (cmd.symbol == "SOL/USDT") market_price = context.sol_price;
    
    if (market_price <= 0) {
        result.warnings.push_back("Не удалось получить рыночную цену для " + cmd.symbol);
        return result;
    }
    
    // Проверяем отклонение
    double deviation = std::abs(cmd.price - market_price) / market_price * 100.0;
    
    if (deviation > m_limits.max_market_deviation) {
        result.warnings.push_back(
            "⚠ Цена лимитного ордера отклоняется от рынка на " + 
            std::to_string(deviation) + "% (макс. " + 
            std::to_string(m_limits.max_market_deviation) + "%)");
        
        // Если отклонение слишком большое (>10%) — блокируем
        if (deviation > 10.0) {
            result.is_valid = false;
            result.error_message = "Цена ордера отклоняется от рынка более чем на 10%. Заблокировано.";
            return result;
        }
    }
    
    return result;
}

// ============================================================================
// Проверка лимитов
// ============================================================================
ValidationResult Validator::validateLimits(const OrderCommand& cmd, 
                                            const MarketContext& context) {
    ValidationResult result;
    
    // Оцениваем стоимость ордера в USDT
    double estimated_order_usd = 0.0;
    double market_price = 0.0;
    
    if (cmd.symbol == "BTC/USDT") market_price = context.btc_price;
    else if (cmd.symbol == "ETH/USDT") market_price = context.eth_price;
    else if (cmd.symbol == "SOL/USDT") market_price = context.sol_price;
    
    if (cmd.action == "buy") {
        // Для покупки: amount в базовой валюте * цена
        estimated_order_usd = cmd.amount * market_price;
    } else {
        // Для продажи: amount в базовой валюте * цена (получаем USDT)
        estimated_order_usd = cmd.amount * market_price;
    }
    
    // Проверка макс. размера ордера
    if (estimated_order_usd > m_limits.max_order_usd) {
        result.is_valid = false;
        result.error_message = "Стоимость ордера ($" + std::to_string(estimated_order_usd) + 
            ") превышает лимит ($" + std::to_string(m_limits.max_order_usd) + ").";
        return result;
    }
    
    // Проверка дневного лимита убытков
    if (!checkDailyLossLimit(estimated_order_usd)) {
        result.is_valid = false;
        result.error_message = "Достигнут дневной лимит убытков.";
        return result;
    }
    
    // Проверка баланса (для покупки)
    if (cmd.action == "buy" && market_price > 0) {
        double required_usdt = cmd.amount * market_price;
        if (required_usdt > context.usdt_balance) {
            result.is_valid = false;
            result.error_message = "Недостаточно USDT. Нужно: " + 
                std::to_string(required_usdt) + ", доступно: " + 
                std::to_string(context.usdt_balance);
            return result;
        }
    }
    
    return result;
}
