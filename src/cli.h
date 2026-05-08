#ifndef CLI_H
#define CLI_H

/*
 * cli.h — Заголовочный файл CLI-оболочки
 * 
 * Отвечает за:
 *   - Ввод команд пользователя
 *   - Отображение статусов и логов
 *   - Управление режимами (dry-run/live)
 *   - Цветной вывод и форматирование
 */

#include "config.h"
#include "llm_agent.h"
#include "validator.h"
#include "exchange_client.h"
#include "audit.h"

#include <memory>
#include <string>
#include <atomic>

// ============================================================================
// Структура конфигурации приложения (из аргументов CLI)
// ============================================================================
struct AppConfig {
    TradingMode mode = TradingMode::DRY_RUN;
    std::string config_file;
    std::string api_key;
    std::string api_secret;
    std::string deepseek_key;
};

// ============================================================================
// ANSI цвета для красивого вывода
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
// Класс CLI
// ============================================================================
class CLI {
public:
    CLI(std::shared_ptr<AppConfig> config,
        std::shared_ptr<LLMAgent> llm,
        std::shared_ptr<Validator> validator,
        std::shared_ptr<ExchangeClient> exchange,
        std::shared_ptr<AuditLogger> audit);
    
    ~CLI();
    
    // Запуск основного цикла обработки команд
    void run();
    
    // Остановка CLI (вызывается из обработчика сигналов)
    void shutdown();
    
private:
    // Обработка одной команды
    void processCommand(const std::string& input);
    
    // Вывод приглашения к вводу
    void printPrompt();
    
    // Вывод справки по командам
    void printHelp();
    
    // Вывод текущего баланса
    void printBalance();
    
    // Вывод статуса системы
    void printStatus();
    
    // Вывод истории ордеров
    void printHistory();
    
    // Вывод текущих лимитов
    void printLimits();
    
    // Обработка торговой команды (через LLM)
    void processTradingCommand(const std::string& natural_language_input);
    
    // Вывод предпросмотра ордера (dry-run)
    void printOrderPreview(const OrderCommand& cmd);
    
    // Исполнение ордера (live)
    OrderResult executeOrder(const OrderCommand& cmd);
    
    // Компоненты системы
    std::shared_ptr<AppConfig> m_config;
    std::shared_ptr<LLMAgent> m_llm;
    std::shared_ptr<Validator> m_validator;
    std::shared_ptr<ExchangeClient> m_exchange;
    std::shared_ptr<AuditLogger> m_audit;
    
    // Флаг работы
    std::atomic<bool> m_running{true};
    
    // История ввода (для стрелочек вверх/вниз)
    std::vector<std::string> m_history;
    size_t m_history_pos = 0;
};

#endif // CLI_H
