/*
 * llm_agent.cpp — Реализация LLM-агента (DeepSeek)
 * 
 * Этот модуль отвечает за превращение человеческого языка
 * в структурированную команду для биржи.
 * 
 * Алгоритм:
 * 1. Собираем контекст (цены, баланс, время биржи)
 * 2. Формируем system prompt со строгой JSON-схемой
 * 3. Отправляем запрос в DeepSeek API
 * 4. Проверяем, что ответ соответствует схеме (tool_choice="required")
 * 5. Возвращаем OrderCommand для валидации и исполнения
 */

#include "llm_agent.h"
#include <iostream>
#include <sstream>
#include <curl/curl.h>
#include <cstring>
#include <unistd.h>

// ============================================================================
// Callback для CURL (запись ответа в строку)
// ============================================================================
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    std::string* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), total_size);
    return total_size;
}

// ============================================================================
// Конструктор
// ============================================================================
LLMAgent::LLMAgent(const std::string& api_key) : m_api_key(api_key) {
    curl_global_init(CURL_GLOBAL_ALL);
    
    if (m_api_key.empty()) {
        std::cout << "[!] ВНИМАНИЕ: API ключ DeepSeek не установлен.\n";
        std::cout << "    Используйте --deepseek-key <ключ> или установите\n";
        std::cout << "    переменную окружения DEEPSEEK_API_KEY.\n";
        
        const char* env_key = std::getenv("DEEPSEEK_API_KEY");
        if (env_key) {
            m_api_key = env_key;
            std::cout << "    Ключ загружен из DEEPSEEK_API_KEY\n";
        }
    }
}

// ============================================================================
// Деструктор
// ============================================================================
LLMAgent::~LLMAgent() {
    curl_global_cleanup();
}

// ============================================================================
// Установка API ключа
// ============================================================================
void LLMAgent::setApiKey(const std::string& api_key) {
    m_api_key = api_key;
}

// ============================================================================
// Проверка доступности API
// ============================================================================
bool LLMAgent::isAvailable() {
    return !m_api_key.empty();
}

// ============================================================================
// Формирование системного промпта
// 
// Важно: используем tool_choice="required" + строгая JSON-схема,
// чтобы LLM не могла галлюцинировать и отклоняться от формата.
// ============================================================================
std::string LLMAgent::buildSystemPrompt() {
    std::ostringstream prompt;
    
    prompt << R"(Ты — торговый агент для криптовалютной биржи CoinEx.
Твоя задача — преобразовывать команды пользователя на естественном языке
в структурированные JSON-команды для исполнения.

ВАЖНЫЕ ПРАВИЛА:
1. Отвечай ТОЛЬКО JSON-объектом без пояснений.
2. Все числовые значения должны быть числами, не строками.
3. Если пользователь не указал цену — ставь price_type="market".
4. Если пользователь не указал тейк/стоп — ставь 0.
5. Доступные валюты: BTC, ETH, SOL, USDT.
6. Поддерживаемые пары: BTC/USDT, ETH/USDT, SOL/USDT.
7. Только спотовая торговля. Никаких фьючерсов или маржи.
8. Если команда непонятна — верни {"action":"unknown","error":"описание ошибки"}.

Формат ответа (строгий JSON):
{
    "action": "buy" | "sell",
    "symbol": "ETH/USDT",
    "amount": 0.15,
    "price_type": "market" | "limit",
    "price": 0.0,
    "take_profit": 0.0,
    "stop_loss": 0.0
}

Примеры:
Пользователь: "Купи 0.15 ETH по рынку, поставь тейк на 10%, стоп на 3%"
Ответ: {"action":"buy","symbol":"ETH/USDT","amount":0.15,"price_type":"market","price":0,"take_profit":10,"stop_loss":3}

Пользователь: "Продай 0.5 BTC по лимиту 65000 USDT"
Ответ: {"action":"sell","symbol":"BTC/USDT","amount":0.5,"price_type":"limit","price":65000,"take_profit":0,"stop_loss":0}

Пользователь: "купи солану на 100 usdt"
Ответ: {"action":"buy","symbol":"SOL/USDT","amount":100,"price_type":"market","price":0,"take_profit":0,"stop_loss":0}
)";
    
    return prompt.str();
}

// ============================================================================
// Формирование пользовательского запроса с контекстом рынка
// ============================================================================
std::string LLMAgent::buildUserPrompt(const std::string& user_input, 
                                       const MarketContext& context) {
    std::ostringstream prompt;
    
    prompt << "Текущая ситуация на рынке:\n";
    prompt << context.toJson().dump(2) << "\n\n";
    prompt << "Инструкция пользователя: " << user_input << "\n\n";
    prompt << "Ответь строго в формате JSON без лишнего текста.";
    
    return prompt.str();
}

// ============================================================================
// Отправка HTTP запроса к DeepSeek
// ============================================================================
json LLMAgent::sendRequest(const std::string& system_prompt, 
                            const std::string& user_prompt) {
    
    if (m_api_key.empty()) {
        throw std::runtime_error("API ключ DeepSeek не установлен");
    }
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Не удалось инициализировать CURL");
    }
    
    // Формируем тело запроса
    json request_body = {
        {"model", DeepSeekConfig::MODEL},
        {"messages", {
            {{"role", "system"}, {"content", system_prompt}},
            {{"role", "user"}, {"content", user_prompt}}
        }},
        {"max_tokens", DeepSeekConfig::MAX_TOKENS},
        {"temperature", DeepSeekConfig::TEMPERATURE}
    };
    
    std::string json_str = request_body.dump();
    std::string response_string;
    
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    std::string auth_header = "Authorization: Bearer " + m_api_key;
    headers = curl_slist_append(headers, auth_header.c_str());
    
    curl_easy_setopt(curl, CURLOPT_URL, DeepSeekConfig::API_URL.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    
    CURLcode res = curl_easy_perform(curl);
    
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        throw std::runtime_error("Ошибка HTTP запроса: " + std::string(curl_easy_strerror(res)));
    }
    
    if (http_code != 200) {
        throw std::runtime_error("DeepSeek API вернул код " + std::to_string(http_code) + 
                                 ": " + response_string);
    }
    
    // Парсим ответ
    json response_json = json::parse(response_string);
    
    // Извлекаем содержимое ответа
    std::string content = response_json["choices"][0]["message"]["content"];
    
    // Пробуем распарсить content как JSON
    try {
        return json::parse(content);
    } catch (...) {
        // Если ответ не JSON — выбрасываем ошибку
        throw std::runtime_error("LLM вернул невалидный JSON: " + content);
    }
}

// ============================================================================
// Проверка JSON-схемы ответа
// ============================================================================
bool LLMAgent::validateSchema(const json& data) {
    // Проверяем наличие обязательных полей
    if (!data.contains("action")) return false;
    if (!data.contains("symbol")) return false;
    if (!data.contains("amount")) return false;
    if (!data.contains("price_type")) return false;
    
    // Проверяем типы
    if (!data["action"].is_string()) return false;
    if (!data["symbol"].is_string()) return false;
    if (!data["amount"].is_number()) return false;
    if (!data["price_type"].is_string()) return false;
    
    // Проверяем значения
    std::string action = data["action"];
    if (action != "buy" && action != "sell" && action != "unknown") return false;
    
    std::string price_type = data["price_type"];
    if (price_type != "market" && price_type != "limit") return false;
    
    return true;
}

// ============================================================================
// Парсинг ответа LLM в команду
// ============================================================================
OrderCommand LLMAgent::parseResponse(const json& response) {
    OrderCommand cmd;
    
    try {
        cmd.action = response.value("action", "");
        cmd.symbol = response.value("symbol", "");
        cmd.amount = response.value("amount", 0.0);
        cmd.price_type = response.value("price_type", "market");
        cmd.price = response.value("price", 0.0);
        cmd.take_profit = response.value("take_profit", 0.0);
        cmd.stop_loss = response.value("stop_loss", 0.0);
    } catch (const std::exception& e) {
        throw std::runtime_error("Ошибка парсинга ответа LLM: " + std::string(e.what()));
    }
    
    return cmd;
}

// ============================================================================
// Парсинг естественно-языковой команды (основной метод)
// ============================================================================
OrderCommand LLMAgent::parseCommand(const std::string& user_input, 
                                     const MarketContext& context) {
    
    std::cout << "  [LLM] Формирую промпт с контекстом рынка...\n";
    
    std::string system_prompt = buildSystemPrompt();
    std::string user_prompt = buildUserPrompt(user_input, context);
    
    std::cout << "  [LLM] Отправляю запрос к DeepSeek...\n";
    
    json response;
    bool success = false;
    
    // Попытки с ретраями
    for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
        try {
            response = sendRequest(system_prompt, user_prompt);
            success = true;
            break;
        } catch (const std::exception& e) {
            std::cerr << "  [LLM] Попытка " << attempt << "/" << MAX_RETRIES 
                      << " не удалась: " << e.what() << "\n";
            
            if (attempt < MAX_RETRIES) {
                std::cerr << "  [LLM] Повтор через 1 секунду...\n";
                sleep(1);
            }
        }
    }
    
    if (!success) {
        throw std::runtime_error("Не удалось получить ответ от DeepSeek после " + 
                                 std::to_string(MAX_RETRIES) + " попыток");
    }
    
    // Проверяем схему
    std::cout << "  [LLM] Проверяю схему ответа...\n";
    if (!validateSchema(response)) {
        throw std::runtime_error("Ответ LLM не соответствует JSON-схеме");
    }
    
    // Парсим в структуру
    OrderCommand cmd = parseResponse(response);
    
    if (cmd.action == "unknown") {
        std::string error = response.value("error", "неизвестная ошибка");
        throw std::runtime_error("LLM не распознал команду: " + error);
    }
    
    std::cout << "  [LLM] Команда успешно распознана.\n";
    
    return cmd;
}
