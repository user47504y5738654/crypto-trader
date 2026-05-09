#ifndef LLM_AGENT_H
#define LLM_AGENT_H

/*
 * llm_agent.h — DeepSeek LLM агент (парсер + стратег)
 * 
 * Два режима работы:
 *   1. PARSER (MANUAL) — преобразует команды пользователя в структуру OrderCommand
 *   2. STRATEGIST (SEMI_AUTO / FULL_AUTO) — анализирует рынок и принимает
 *      самостоятельные торговые решения, возвращает StrategistDecision
 */

#include "config.h"

#include <string>
#include <functional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ============================================================================
// Класс LLM-агента
// ============================================================================
class LLMAgent {
public:
    explicit LLMAgent(const std::string& api_key);
    ~LLMAgent();
    
    // ------------------------------------------------------------------------
    // Режим 1: Парсер команд (MANUAL)
    // ------------------------------------------------------------------------
    
    // Парсит команду на естественном языке в OrderCommand
    OrderCommand parseCommand(const std::string& user_input,
                               const MarketContext& context);
    
    // ------------------------------------------------------------------------
    // Режим 2: Стратег (SEMI_AUTO / FULL_AUTO)
    // ------------------------------------------------------------------------
    
    // Стратег анализирует рынок и возвращает решение
    StrategistDecision analyzeMarket(const MarketContext& context,
                                      const StrategyConfig& strategy,
                                      const RiskLimits& limits);
    
    // ------------------------------------------------------------------------
    // Режим 3: Чат (свободное общение + торговые инструкции)
    // ------------------------------------------------------------------------
    
    // Свободный диалог с DeepSeek: отвечает на вопросы, может распознать торговую команду
    ChatResponse chat(const std::string& user_message,
                       const MarketContext& context);
    
    // ------------------------------------------------------------------------
    // Общие методы
    // ------------------------------------------------------------------------
    
    // Проверка доступности API
    bool isAvailable() const;
    
    // Установка API ключа
    void setApiKey(const std::string& api_key);
    
private:
    // ------------------------------------------------------------------------
    // Внутренние методы
    // ------------------------------------------------------------------------
    
    // Формирование системного промпта для парсера
    std::string buildParserSystemPrompt();
    
    // Формирование системного промпта для стратега
    std::string buildStrategistSystemPrompt(const StrategyConfig& strategy,
                                              const RiskLimits& limits);
    
    // Формирование пользовательского промпта с контекстом рынка
    std::string buildMarketContextPrompt(const MarketContext& context);
    
    // Отправка запроса к DeepSeek API
    json sendDeepSeekRequest(const std::string& system_prompt,
                               const std::string& user_prompt,
                               int max_tokens,
                               double temperature);
    
    // Парсинг ответа в OrderCommand
    OrderCommand parseOrderCommand(const json& response);
    
    // Парсинг ответа в StrategistDecision
    StrategistDecision parseStrategistDecision(const json& response);
    
    // Валидация JSON-схемы ответа парсера
    bool validateParserSchema(const json& data);
    
    // Валидация JSON-схемы ответа стратега
    bool validateStrategistSchema(const json& data);
    
    // Формирование системного промпта для чата
    std::string buildChatSystemPrompt();
    
    // Парсинг ответа чата
    ChatResponse parseChatResponse(const json& response);
    
    // ------------------------------------------------------------------------
    // Поля
    // ------------------------------------------------------------------------
    
    std::string m_api_key;
    static constexpr int MAX_RETRIES = 3;
};

#endif // LLM_AGENT_H
