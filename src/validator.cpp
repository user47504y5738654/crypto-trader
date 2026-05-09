/*
 * validator.cpp — Валидация ордеров и риск-контроль
 * 
 * Проверяет каждый ордер и решение стратега перед отправкой на биржу.
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

Validator::~Validator() = default;

// ============================================================================
// Лимиты
// ============================================================================
RiskLimits Validator::getLimits() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_limits;
}

void Validator::updateLimits(const RiskLimits& limits) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_limits = limits;
}

void Validator::loadLimits() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::cout << "  [VALIDATOR] Лимиты загружены (по умолчанию)\n";
}

void Validator::saveLimits(const RiskLimits& limits) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_limits = limits;
}

// ============================================================================
// Circuit breaker
// ============================================================================
bool Validator::isCircuitBreakerActive() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_circuit_breaker_active) return false;
    
    auto now = std::chrono::steady_clock::now();
    if (now >= m_circuit_breaker_until) {
        m_circuit_breaker_active = false;
        m_error_count = 0;
        std::cout << "  [VALIDATOR] Circuit breaker сброшен.\n";
        return false;
    }
    return true;
}

void Validator::registerError() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    m_error_count++;
    std::cout << "  [VALIDATOR] Ошибка #" << m_error_count
              << "/" << m_limits.circuit_breaker_errors << "\n";
    
    if (m_error_count >= m_limits.circuit_breaker_errors) {
        m_circuit_breaker_active = true;
        m_circuit_breaker_until = std::chrono::steady_clock::now()
            + std::chrono::seconds(m_limits.circuit_breaker_seconds);
        std::cout << "  [VALIDATOR] CIRCUIT BREAKER! Пауза "
                  << m_limits.circuit_breaker_seconds << "с\n";
    }
}

void Validator::resetCircuitBreaker() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_error_count = 0;
    m_circuit_breaker_active = false;
}

// ============================================================================
// Дневной P&L
// ============================================================================
bool Validator::checkDailyLossLimit(double potential_loss) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto now = std::chrono::system_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::hours>(now - m_day_start);
    if (diff.count() >= 24) {
        m_daily_pnl = 0.0;
        m_daily_trades = 0;
        m_day_start = now;
    }
    
    double projected = m_daily_pnl - potential_loss;
    return projected >= -m_limits.daily_loss_limit;
}

void Validator::registerTrade(double pnl) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_daily_pnl += pnl;
    m_daily_trades++;
}

void Validator::resetDailyStats() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_daily_pnl = 0.0;
    m_daily_trades = 0;
    m_day_start = std::chrono::system_clock::now();
}

// ============================================================================
// Валидация ордера (MANUAL)
// ============================================================================
ValidationResult Validator::validateOrder(const OrderCommand& cmd,
                                            const MarketContext& context) {
    
    if (isCircuitBreakerActive()) {
        return {false, "Circuit breaker активен.", {}};
    }
    
    auto schema = validateSchema(cmd);
    if (!schema.is_valid) { registerError(); return schema; }
    
    auto safety = validateSafety(cmd);
    if (!safety.is_valid) { registerError(); return safety; }
    
    auto limits = validateLimits(cmd, context);
    if (!limits.is_valid) { return limits; }
    
    resetCircuitBreaker();
    
    ValidationResult result;
    result.is_valid = true;
    for (auto& w : schema.warnings) result.warnings.push_back(w);
    for (auto& w : safety.warnings) result.warnings.push_back(w);
    for (auto& w : limits.warnings) result.warnings.push_back(w);
    
    return result;
}

// ============================================================================
// Валидация решения стратега
// ============================================================================
ValidationResult Validator::validateStrategistDecision(
    const StrategistDecision& decision,
    const MarketContext& context,
    const StrategyConfig& strategy) {
    
    if (isCircuitBreakerActive()) {
        return {false, "Circuit breaker активен.", {}};
    }
    
    ValidationResult result;
    
    // Если стратег решил ничего не делать — ок
    if (!decision.should_act) return result;
    
    // Проверка уверенности
    if (decision.confidence < strategy.min_confidence) {
        result.warnings.push_back(
            "Уверенность стратега (" + std::to_string(decision.confidence) +
            ") ниже порога (" + std::to_string(strategy.min_confidence) + ")");
    }
    
    if (decision.confidence < m_limits.min_auto_confidence && 
        decision.action != "cancel") {
        result.is_valid = false;
        result.error_message = "Уверенность стратега слишком низкая для авто-сделки";
        return result;
    }
    
    // Для cancel — проверяем список ордеров
    if (decision.action == "cancel") {
        if (decision.cancel_order_ids.empty()) {
            result.warnings.push_back("Стратег выбрал cancel, но список пуст");
        }
        return result;
    }
    
    // Для buy/sell — валидация как обычный ордер
    OrderCommand cmd;
    cmd.action = decision.action;
    cmd.symbol = decision.symbol;
    cmd.amount = decision.amount;
    cmd.market_type = CoinExConfig::MARKET_TYPE_SPOT;
    cmd.price_type = decision.price_type;
    cmd.price = decision.price;
    
    auto schema = validateSchema(cmd);
    if (!schema.is_valid) { registerError(); return schema; }
    
    auto safety = validateSafety(cmd);
    if (!safety.is_valid) { registerError(); return safety; }
    
    auto limits = validateLimits(cmd, context);
    if (!limits.is_valid) return limits;
    
    // Проверка дневного количества сделок
    if (m_daily_trades >= strategy.max_daily_trades) {
        result.warnings.push_back("Достигнут лимит дневных сделок");
    }
    
    // Проверка позиции (% от портфеля)
    double total_usd = 0.0;
    for (const auto& [ccy, bal] : context.balances) {
        if (ccy == "USDT" || ccy == "USDC") {
            total_usd += bal.total();
        } else {
            auto it = context.tickers.find(ccy + "USDT");
            if (it != context.tickers.end()) {
                total_usd += bal.total() * it->second.last;
            }
        }
    }
    
    if (total_usd > 0) {
        double order_value = cmd.amount;
        auto it = context.tickers.find(cmd.symbol);
        if (it != context.tickers.end()) {
            order_value *= it->second.last;
        }
        double pct = order_value / total_usd * 100.0;
        if (pct > strategy.max_position_percent) {
            result.warnings.push_back(
                "Размер позиции (" + std::to_string(pct) +
                "%) превышает лимит стратегии (" +
                std::to_string(strategy.max_position_percent) + "%)");
        }
    }
    
    resetCircuitBreaker();
    return result;
}

// ============================================================================
// Валидация схемы
// ============================================================================
ValidationResult Validator::validateSchema(const OrderCommand& cmd) {
    ValidationResult result;
    
    if (cmd.action != "buy" && cmd.action != "sell") {
        result.is_valid = false;
        result.error_message = "Действие должно быть buy/sell: " + cmd.action;
        return result;
    }
    
    if (cmd.symbol.empty()) {
        result.is_valid = false;
        result.error_message = "Не указана торговая пара";
        return result;
    }
    
    if (cmd.amount <= 0) {
        result.is_valid = false;
        result.error_message = "Объём должен быть > 0";
        return result;
    }
    
    if (cmd.price_type != "market" && cmd.price_type != "limit") {
        result.is_valid = false;
        result.error_message = "Тип цены: market или limit";
        return result;
    }
    
    if (cmd.price_type == "limit" && cmd.price <= 0) {
        result.is_valid = false;
        result.error_message = "Для лимитного ордера нужна цена";
        return result;
    }
    
    if (cmd.take_profit < 0) {
        result.warnings.push_back("Тейк-профит < 0 — установлен в 0");
    }
    if (cmd.stop_loss < 0) {
        result.warnings.push_back("Стоп-лосс < 0 — установлен в 0");
    }
    
    return result;
}

// ============================================================================
// Валидация безопасности
// ============================================================================
ValidationResult Validator::validateSafety(const OrderCommand& cmd) {
    ValidationResult result;
    
    std::string lower = cmd.symbol;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    if (lower.find("perp") != std::string::npos ||
        lower.find("futures") != std::string::npos ||
        lower.find("margin") != std::string::npos) {
        result.is_valid = false;
        result.error_message = "Фьючерсы/маржа запрещены. Только спот.";
        return result;
    }
    
    return result;
}

// ============================================================================
// Валидация лимитов
// ============================================================================
ValidationResult Validator::validateLimits(const OrderCommand& cmd,
                                            const MarketContext& context) {
    ValidationResult result;
    
    // Оценка стоимости ордера
    double price = 0.0;
    auto it = context.tickers.find(cmd.symbol);
    if (it != context.tickers.end()) {
        price = it->second.last;
    }
    
    double estimated = cmd.amount * price;
    
    // Макс. ордер
    if (estimated > m_limits.max_order_usd) {
        result.is_valid = false;
        result.error_message = "Ордер $" + std::to_string(estimated) +
            " превышает лимит $" + std::to_string(m_limits.max_order_usd);
        return result;
    }
    
    // Дневной убыток
    if (!checkDailyLossLimit(estimated)) {
        result.is_valid = false;
        result.error_message = "Дневной лимит убытков исчерпан";
        return result;
    }
    
    // Баланс для покупки
    if (cmd.action == "buy") {
        auto bal_it = context.balances.find("USDT");
        double available = (bal_it != context.balances.end()) 
            ? bal_it->second.available : 0.0;
        
        if (estimated > available) {
            result.is_valid = false;
            result.error_message = "Недостаточно USDT: нужно $" +
                std::to_string(estimated) + ", доступно $" +
                std::to_string(available);
            return result;
        }
    } else {
        // Для продажи — проверяем базовую валюту
        std::string base = cmd.symbol;
        if (base.length() > 4) base = base.substr(0, base.length() - 4);
        
        auto bal_it = context.balances.find(base);
        double available = (bal_it != context.balances.end())
            ? bal_it->second.available : 0.0;
        
        if (cmd.amount > available) {
            result.is_valid = false;
            result.error_message = "Недостаточно " + base + ": нужно " +
                std::to_string(cmd.amount) + ", доступно " +
                std::to_string(available);
            return result;
        }
    }
    
    return result;
}
