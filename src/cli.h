#ifndef CLI_H
#define CLI_H

/*
 * cli.h — CLI-оболочка
 * 
 * Поддерживает три режима:
 *   MANUAL    — пользователь → DeepSeek-парсер → CoinEx
 *   SEMI_AUTO — DeepSeek-стратег анализирует рынок, пользователь подтверждает
 *   FULL_AUTO — DeepSeek-стратег торгует самостоятельно
 */

#include "config.h"
#include "llm_agent.h"
#include "validator.h"
#include "exchange_client.h"
#include "audit.h"

#include <memory>
#include <string>
#include <atomic>
#include <thread>

// ============================================================================
// Конфигурация приложения
// ============================================================================
struct AppConfig {
    TradingMode mode          = TradingMode::DRY_RUN;
    StrategistMode strat_mode = StrategistMode::MANUAL;
    std::string config_file;
    std::string access_id;     // CoinEx API Key
    std::string secret_key;    // CoinEx Secret Key
    std::string deepseek_key;  // DeepSeek API Key
};

// ============================================================================
// ANSI цвета
// ============================================================================
namespace Color {
    const std::string RESET   = "\033[0m";
    const std::string RED     = "\033[31m";
    const std::string GREEN   = "\033[32m";
    const std::string YELLOW  = "\033[33m";
    const std::string BLUE    = "\033[34m";
    const std::string MAGENTA = "\033[35m";
    const std::string CYAN    = "\033[36m";
    const std::string WHITE   = "\033[37m";
    const std::string BOLD    = "\033[1m";
    const std::string DIM     = "\033[2m";
}

// ============================================================================
// CLI
// ============================================================================
class CLI {
public:
    CLI(std::shared_ptr<AppConfig> config,
        std::shared_ptr<LLMAgent> llm,
        std::shared_ptr<Validator> validator,
        std::shared_ptr<ExchangeClient> exchange,
        std::shared_ptr<AuditLogger> audit);
    
    ~CLI();
    
    // Запуск основного цикла
    void run();
    
    // Остановка
    void shutdown();
    
private:
    // ------------------------------------------------------------------------
    // Команды
    // ------------------------------------------------------------------------
    void processCommand(const std::string& input);
    void printPrompt();
    void printHelp();
    void printBalance();
    void printStatus();
    void printHistory();
    void printLimits();
    
    // ------------------------------------------------------------------------
    // Чат с DeepSeek (свободное общение)
    // ------------------------------------------------------------------------
    void processChat(const std::string& input);
    
    // ------------------------------------------------------------------------
    // Торговля
    // ------------------------------------------------------------------------
    void processTradingCommand(const std::string& input);  // MANUAL
    void printOrderPreview(const OrderCommand& cmd);
    OrderResult executeOrder(const OrderCommand& cmd);
    
    // ------------------------------------------------------------------------
    // Стратег
    // ------------------------------------------------------------------------
    void startStrategistLoop();         // SEMI_AUTO / FULL_AUTO
    void stopStrategistLoop();
    void strategistIteration();         // Одна итерация анализа
    void printStrategistDecision(const StrategistDecision& d);
    bool executeStrategistDecision(const StrategistDecision& d);
    
    // ------------------------------------------------------------------------
    // Вспомогательные
    // ------------------------------------------------------------------------
    void switchToSemiAuto();
    void switchToFullAuto();
    void switchToManual();
    
    // ------------------------------------------------------------------------
    // Компоненты
    // ------------------------------------------------------------------------
    std::shared_ptr<AppConfig>      m_config;
    std::shared_ptr<LLMAgent>       m_llm;
    std::shared_ptr<Validator>      m_validator;
    std::shared_ptr<ExchangeClient> m_exchange;
    std::shared_ptr<AuditLogger>    m_audit;
    
    // Состояние
    std::atomic<bool> m_running{true};
    std::atomic<bool> m_strategist_running{false};
    std::unique_ptr<std::thread> m_strategist_thread;
    
    // Конфигурация стратегии
    StrategyConfig m_strategy;
    
    // История
    std::vector<std::string> m_history;
    size_t m_history_pos = 0;
};

#endif // CLI_H
