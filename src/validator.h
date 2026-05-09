#ifndef VALIDATOR_H
#define VALIDATOR_H

/*
 * validator.h — Валидатор ордеров и риск-контроль
 * 
 * Проверяет ордера перед отправкой на биржу:
 *   1. Схема ордера
 *   2. Безопасность (только спот)
 *   3. Лимиты (баланс, дневной убыток, экспозиция)
 *   4. Решения стратега (min confidence, max exposure)
 *   5. Circuit breaker
 */

#include "config.h"

#include <string>
#include <chrono>
#include <mutex>

// ============================================================================
// Результат валидации
// ============================================================================
struct ValidationResult {
    bool is_valid = true;
    std::string error_message;
    std::vector<std::string> warnings;
};

// ============================================================================
// Валидатор
// ============================================================================
class Validator {
public:
    Validator();
    ~Validator();
    
    // Валидация ордера (для MANUAL режима)
    ValidationResult validateOrder(const OrderCommand& cmd,
                                    const MarketContext& context);
    
    // Валидация решения стратега (для SEMI_AUTO / FULL_AUTO)
    ValidationResult validateStrategistDecision(const StrategistDecision& decision,
                                                  const MarketContext& context,
                                                  const StrategyConfig& strategy);
    
    // Загрузка/сохранение лимитов
    void loadLimits();
    void saveLimits(const RiskLimits& limits);
    
    // Получение/установка лимитов
    RiskLimits getLimits() const;
    void updateLimits(const RiskLimits& limits);
    
    // Circuit breaker
    bool isCircuitBreakerActive();
    void registerError();
    void resetCircuitBreaker();
    
    // Дневной P&L
    bool checkDailyLossLimit(double potential_loss);
    void registerTrade(double pnl);
    
    // Сброс дневной статистики
    void resetDailyStats();
    
    // Текущий дневной P&L
    double getDailyPnL() const { return m_daily_pnl; }
    int getDailyTrades() const { return m_daily_trades; }
    
private:
    // Уровни валидации
    ValidationResult validateSchema(const OrderCommand& cmd);
    ValidationResult validateSafety(const OrderCommand& cmd);
    ValidationResult validateLimits(const OrderCommand& cmd,
                                     const MarketContext& context);
    
    // Лимиты
    RiskLimits m_limits;
    
    // Circuit breaker
    int m_error_count = 0;
    bool m_circuit_breaker_active = false;
    std::chrono::steady_clock::time_point m_circuit_breaker_until;
    
    // Дневная статистика
    double m_daily_pnl = 0.0;
    int m_daily_trades = 0;
    std::chrono::system_clock::time_point m_day_start;
    
    mutable std::mutex m_mutex;
};

#endif // VALIDATOR_H
