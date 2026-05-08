#ifndef LLM_AGENT_H
#define LLM_AGENT_H

/*
 * llm_agent.h — Заголовочный файл LLM-агента
 * 
 * Отвечает за:
 *   - Формирование промпта для DeepSeek
 *   - Отправку запросов к DeepSeek API
 *   - Парсинг JSON-ответа в структуру OrderCommand
 *   - Обработку ошибок и повторные попытки
 */

#include "config.h"
#include <string>
#include <functional>
#include <nlohmann/json.hpp>  // nlohmann/json

using json = nlohmann::json;

// ============================================================================
// Структура контекста рынка (передаётся в LLM)
// ============================================================================
struct MarketContext {
    double btc_price = 0.0;
    double eth_price = 0.0;
    double sol_price = 0.0;
    // Можно добавить другие активы
    
    double usdt_balance = 0.0;
    double btc_balance = 0.0;
    double eth_balance = 0.0;
    
    int64_t server_time = 0;  // Время биржи (UNIX timestamp)
    
    // Сериализация в JSON для промпта
    json toJson() const {
        return {
            {"prices", {
                {"BTC/USDT", btc_price},
                {"ETH/USDT", eth_price},
                {"SOL/USDT", sol_price}
            }},
            {"balances", {
                {"USDT", usdt_balance},
                {"BTC", btc_balance},
                {"ETH", eth_balance}
            }},
            {"server_time", server_time}
        };
    }
};

// ============================================================================
// Класс LLM-агента
// ============================================================================
class LLMAgent {
public:
    explicit LLMAgent(const std::string& api_key);
    ~LLMAgent();
    
    // Основной метод: парсит естественно-языковую команду в структуру
    OrderCommand parseCommand(const std::string& user_input, 
                              const MarketContext& context);
    
    // Проверка доступности API
    bool isAvailable();
    
    // Установка/обновление API ключа
    void setApiKey(const std::string& api_key);
    
private:
    // Формирование системного промпта
    std::string buildSystemPrompt();
    
    // Формирование пользовательского запроса с контекстом
    std::string buildUserPrompt(const std::string& user_input, 
                                const MarketContext& context);
    
    // Отправка HTTP запроса к DeepSeek
    json sendRequest(const std::string& system_prompt, 
                     const std::string& user_prompt);
    
    // Парсинг ответа LLM в команду
    OrderCommand parseResponse(const json& response);
    
    // Валидация JSON-схемы ответа
    bool validateSchema(const json& data);
    
    // API ключ DeepSeek
    std::string m_api_key;
    
    // Счётчик попыток
    int m_retry_count = 0;
    static constexpr int MAX_RETRIES = 3;
};

#endif // LLM_AGENT_H
