#ifndef VALIDATOR_H
#define VALIDATOR_H

/*
 * validator.h — Модуль валидации и риск-контроля
 * 
 * Отвечает за:
 *   - Проверку схемы ордера (Pydantic-подобная валидация)
 *   - Сверку с лимитами (баланс, дневной убыток, макс. позиции)
 *   - Проверку цен на отклонение от рынка
 *   - Circuit breaker (автопауза при ошибках)
 *   - Блокировку опасных действий (фьючерсы, маржа)
 */

#include "config.h"
#include "llm_agent.h"

#include <string>
#include <chrono>
#include <mutex>

// ============================================================================
// Результат валидации
// ============================================================================
struct ValidationResult {
    bool is_valid = true;
    std::string error_message;
    std::vector<std::string> warnings;  // Предупреждения (не блокирующие)
};

// ============================================================================
// Класс валидатора
// ============================================================================
class Validator {
public:
    Validator();
    ~Validator();
    
    // Основной метод валидации
    ValidationResult validate(const OrderCommand& cmd, const MarketContext& context);
    
    // Загрузка лимитов из SQLite
    void loadLimits();
    
    // Сохранение лимитов
    void saveLimits(const RiskLimits& limits);
    
    // Получение текущих лимитов
    RiskLimits getLimits() const;
    
    // Обновление лимитов
    void updateLimits(const RiskLimits& limits);
    
    // Проверка circuit breaker
    bool isCircuitBreakerActive();
    
    // Регистрация ошибки для circuit breaker
    void registerError();
    
    // Сброс circuit breaker
    void resetCircuitBreaker();
    
    // Проверка дневного лимита убытков
    bool checkDailyLossLimit(double potential_loss);
    
    // Регистрация сделки для дневного трекинга
    void registerTrade(double profit_loss);
    
private:
    // Проверка базовой схемы
    ValidationResult validateSchema(const OrderCommand& cmd);
    
    // Проверка лимитов
    ValidationResult validateLimits(const OrderCommand& cmd, const MarketContext& context);
    
    // Проверка цен
    ValidationResult validatePrices(const OrderCommand& cmd, const MarketContext& context);
    
    // Блокировка опасных действий
    ValidationResult validateSafety(const OrderCommand& cmd);
    
    // Лимиты безопасности
    RiskLimits m_limits;
    
    // Circuit breaker
    int m_error_count = 0;
    std::chrono::steady_clock::time_point m_circuit_breaker_until;
    bool m_circuit_breaker_active = false;
    
    // Дневной трекинг
    double m_daily_pnl = 0.0;
    std::chrono::system_clock::time_point m_day_start;
    
    // Мьютекс для потокобезопасности
    mutable std::mutex m_mutex;
};

#endif // VALIDATOR_H
