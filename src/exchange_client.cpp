/*
 * exchange_client.cpp — Реализация клиента CoinEx API
 * 
 * Обеспечивает взаимодействие с биржей через REST API.
 * Использует HMAC-SHA256 для подписи запросов.
 * 
 * В режиме dry-run симулирует торговлю без отправки на биржу.
 */

#include "exchange_client.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cstring>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <curl/curl.h>

// ============================================================================
// Callback для CURL
// ============================================================================
static size_t WriteCb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    std::string* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), total);
    return total;
}

// ============================================================================
// Вспомогательная: текущее время в ISO8601 для CoinEx
// ============================================================================
static std::string getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return std::to_string(ms);
}

// ============================================================================
// Конструктор
// ============================================================================
ExchangeClient::ExchangeClient(const std::string& api_key, const std::string& api_secret)
    : m_api_key(api_key), m_api_secret(api_secret)
{
    curl_global_init(CURL_GLOBAL_ALL);
    
    // Инициализируем симулированный баланс для dry-run
    m_simulated_balance["USDT"] = {10000.0, 0.0};  // $10,000 для тестов
    m_simulated_balance["BTC"] = {0.5, 0.0};
    m_simulated_balance["ETH"] = {5.0, 0.0};
    m_simulated_balance["SOL"] = {50.0, 0.0};
}

// ============================================================================
// Деструктор
// ============================================================================
ExchangeClient::~ExchangeClient() {
    curl_global_cleanup();
}

// ============================================================================
// Создание HMAC-SHA256 подписи
// ============================================================================
std::string ExchangeClient::signRequest(const std::string& method,
                                         const std::string& path,
                                         const std::string& query_string,
                                         const std::string& body,
                                         const std::string& timestamp) {
    // Формируем строку для подписи
    std::string sign_str = method + "\n" + path + "\n" + query_string + "\n" + body + "\n" + timestamp;
    
    // HMAC-SHA256
    unsigned char hash[SHA256_DIGEST_LENGTH];
    HMAC(EVP_sha256(), m_api_secret.c_str(), m_api_secret.length(),
         reinterpret_cast<const unsigned char*>(sign_str.c_str()), sign_str.length(),
         hash, nullptr);
    
    // Конвертируем в hex
    std::ostringstream hex;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        hex << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    
    return hex.str();
}

// ============================================================================
// Формирование заголовков для CoinEx API v2
// ============================================================================
std::map<std::string, std::string> ExchangeClient::buildHeaders(const std::string& method,
                                                                  const std::string& path,
                                                                  const std::string& body) {
    std::map<std::string, std::string> headers;
    
    if (m_api_key.empty() || m_api_secret.empty()) {
        // В dry-run режиме ключи могут отсутствовать
        return headers;
    }
    
    std::string timestamp = getCurrentTimestamp();
    std::string signature = signRequest(method, path, "", body, timestamp);
    
    headers["Content-Type"] = "application/json";
    headers["X-COINEX-KEY"] = m_api_key;
    headers["X-COINEX-SIGN"] = signature;
    headers["X-COINEX-TIMESTAMP"] = timestamp;
    
    return headers;
}

// ============================================================================
// Отправка HTTP запроса
// ============================================================================
std::string ExchangeClient::sendHttpRequest(const std::string& method,
                                             const std::string& endpoint,
                                             const std::string& body,
                                             const std::map<std::string, std::string>& headers) {
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Не удалось инициализировать CURL");
    }
    
    std::string url = CoinExConfig::BASE_URL + endpoint;
    std::string response_string;
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    
    // Добавляем заголовки
    struct curl_slist* curl_headers = nullptr;
    for (const auto& [key, value] : headers) {
        std::string header = key + ": " + value;
        curl_headers = curl_slist_append(curl_headers, header.c_str());
    }
    
    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    }
    
    if (curl_headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curl_headers);
    }
    
    CURLcode res = curl_easy_perform(curl);
    
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    curl_slist_free_all(curl_headers);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        throw std::runtime_error("Ошибка HTTP: " + std::string(curl_easy_strerror(res)));
    }
    
    return response_string;
}

// ============================================================================
// Получение баланса
// ============================================================================
std::map<std::string, Balance> ExchangeClient::getBalance() {
    // В dry-run или если нет ключей — возвращаем симулированный баланс
    if (m_api_key.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::cout << "  [EXCHANGE] Режим симуляции: возвращаю тестовый баланс\n";
        return m_simulated_balance;
    }
    
    try {
        auto headers = buildHeaders("GET", CoinExConfig::BALANCE_ENDPOINT);
        std::string response = sendHttpRequest("GET", CoinExConfig::BALANCE_ENDPOINT, "", headers);
        
        // Парсим ответ
        auto json_response = nlohmann::json::parse(response);
        
        std::map<std::string, Balance> balance_map;
        
        if (json_response.contains("data")) {
            for (const auto& item : json_response["data"]) {
                Balance bal;
                bal.available = std::stod(item["available"].get<std::string>());
                bal.frozen = std::stod(item["frozen"].get<std::string>());
                balance_map[item["ccy"].get<std::string>()] = bal;
            }
        }
        
        return balance_map;
        
    } catch (const std::exception& e) {
        std::cerr << "  [EXCHANGE] Ошибка получения баланса: " << e.what() << "\n";
        throw;
    }
}

// ============================================================================
// Получение цены тикера
// ============================================================================
double ExchangeClient::getTickerPrice(const std::string& symbol) {
    // Если нет API ключа — симулируем цены (без HTTP запросов)
    if (m_api_key.empty()) {
        if (symbol == "BTC/USDT") return 65000.0;
        if (symbol == "ETH/USDT") return 3500.0;
        if (symbol == "SOL/USDT") return 150.0;
        return 0.0;
    }
    
    // Преобразуем символ: BTC/USDT → BTCUSDT (для CoinEx API)
    std::string market_symbol = symbol;
    size_t slash_pos = market_symbol.find('/');
    if (slash_pos != std::string::npos) {
        market_symbol.erase(slash_pos, 1);
    }
    
    std::string endpoint = CoinExConfig::TICKER_ENDPOINT + "?market=" + market_symbol;
    
    try {
        std::string response = sendHttpRequest("GET", endpoint);
        auto json_response = nlohmann::json::parse(response);
        
        if (json_response.contains("data") && json_response["data"].contains("last")) {
            return std::stod(json_response["data"]["last"].get<std::string>());
        }
        
        // Альтернативный формат ответа
        if (json_response.contains("data") && json_response["data"].is_array() && !json_response["data"].empty()) {
            auto& ticker = json_response["data"][0];
            if (ticker.contains("last")) {
                return std::stod(ticker["last"].get<std::string>());
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "  [EXCHANGE] Ошибка получения цены " << symbol << ": " << e.what() << "\n";
    }
    
    // Если не удалось — симулированные цены
    if (symbol == "BTC/USDT") return 65000.0;
    if (symbol == "ETH/USDT") return 3500.0;
    if (symbol == "SOL/USDT") return 150.0;
    
    return 0.0;
}

// ============================================================================
// Получение контекста рынка
// ============================================================================
MarketContext ExchangeClient::getMarketContext() {
    MarketContext ctx;
    
    // Заполняем из симуляции (без HTTP запросов, если нет ключей)
    ctx.btc_price = getTickerPrice("BTC/USDT");
    ctx.eth_price = getTickerPrice("ETH/USDT");
    ctx.sol_price = getTickerPrice("SOL/USDT");
    
    auto balance = getBalance();
    ctx.usdt_balance = balance["USDT"].available;
    ctx.btc_balance = balance["BTC"].available;
    ctx.eth_balance = balance["ETH"].available;
    
    ctx.server_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    // Если есть API ключи, пытаемся получить реальные данные
    if (!m_api_key.empty()) {
        try {
            ctx.btc_price = getTickerPrice("BTC/USDT");
            ctx.eth_price = getTickerPrice("ETH/USDT");
            ctx.sol_price = getTickerPrice("SOL/USDT");
            
            auto balance = getBalance();
            ctx.usdt_balance = balance["USDT"].available;
            ctx.btc_balance = balance["BTC"].available;
            ctx.eth_balance = balance["ETH"].available;
        } catch (const std::exception& e) {
            std::cerr << "  [EXCHANGE] Ошибка получения контекста: " << e.what() << "\n";
            std::cerr << "  [EXCHANGE] Использую симулированные данные.\n";
        }
    }
    
    return ctx;
}

// ============================================================================
// Размещение ордера
// ============================================================================
OrderResult ExchangeClient::placeOrder(const OrderCommand& cmd) {
    // Если нет API ключей — симуляция
    if (m_api_key.empty()) {
        return simulateOrder(cmd);
    }
    
    try {
        // Формируем тело запроса для CoinEx API v2
        nlohmann::json order_body = {
            {"market", cmd.symbol},
            {"side", cmd.action == "buy" ? "buy" : "sell"},
            {"type", cmd.price_type == "market" ? "market" : "limit"},
            {"amount", std::to_string(cmd.amount)}
        };
        
        if (cmd.price_type == "limit" && cmd.price > 0) {
            order_body["price"] = std::to_string(cmd.price);
        }
        
        std::string body_str = order_body.dump();
        auto headers = buildHeaders("POST", CoinExConfig::SPOT_ORDER_ENDPOINT, body_str);
        std::string response = sendHttpRequest("POST", CoinExConfig::SPOT_ORDER_ENDPOINT, body_str, headers);
        
        auto json_resp = nlohmann::json::parse(response);
        
        OrderResult result;
        if (json_resp.contains("data")) {
            result.order_id = json_resp["data"].value("order_id", "");
            result.status = OrderStatus::FILLED;
            result.filled_amount = cmd.amount;
            
            if (json_resp["data"].contains("avg_price")) {
                result.filled_price = std::stod(json_resp["data"]["avg_price"].get<std::string>());
            }
        } else {
            result.status = OrderStatus::REJECTED;
            result.error_msg = json_resp.value("message", "Неизвестная ошибка");
        }
        
        return result;
        
    } catch (const std::exception& e) {
        OrderResult error_result;
        error_result.status = OrderStatus::REJECTED;
        error_result.error_msg = e.what();
        return error_result;
    }
}

// ============================================================================
// Симуляция ордера (dry-run)
// ============================================================================
OrderResult ExchangeClient::simulateOrder(const OrderCommand& cmd) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    OrderResult result;
    result.order_id = "SIM-" + std::to_string(std::rand());
    result.status = OrderStatus::FILLED;
    result.filled_amount = cmd.amount;
    result.fee = cmd.amount * 0.002;  // Симуляция комиссии 0.2%
    
    // Получаем текущую цену для расчёта
    double price = 0.0;
    if (cmd.symbol == "BTC/USDT") price = 65000.0;
    else if (cmd.symbol == "ETH/USDT") price = 3500.0;
    else if (cmd.symbol == "SOL/USDT") price = 150.0;
    
    result.filled_price = price;
    
    // Обновляем симулированный баланс
    if (cmd.action == "buy") {
        double cost = cmd.amount * price;
        m_simulated_balance["USDT"].available -= cost;
        
        std::string currency = cmd.symbol.substr(0, cmd.symbol.find('/'));
        m_simulated_balance[currency].available += cmd.amount;
    } else {
        std::string currency = cmd.symbol.substr(0, cmd.symbol.find('/'));
        m_simulated_balance[currency].available -= cmd.amount;
        m_simulated_balance["USDT"].available += cmd.amount * price;
    }
    
    // Обновляем статистику
    m_dry_run_stats.total_trades++;
    m_dry_run_stats.successful_trades++;
    
    return result;
}

// ============================================================================
// Получение статуса ордера
// ============================================================================
OrderStatus ExchangeClient::getOrderStatus(const std::string& order_id) {
    std::string endpoint = CoinExConfig::SPOT_ORDER_ENDPOINT + "/" + order_id;
    
    try {
        auto headers = buildHeaders("GET", endpoint);
        std::string response = sendHttpRequest("GET", endpoint, "", headers);
        
        auto json_resp = nlohmann::json::parse(response);
        
        if (json_resp.contains("data")) {
            std::string status_str = json_resp["data"].value("status", "");
            
            if (status_str == "filled") return OrderStatus::FILLED;
            if (status_str == "partially_filled") return OrderStatus::PARTIALLY;
            if (status_str == "cancelled") return OrderStatus::CANCELLED;
            if (status_str == "rejected") return OrderStatus::REJECTED;
            if (status_str == "expired") return OrderStatus::EXPIRED;
        }
    } catch (const std::exception& e) {
        std::cerr << "  [EXCHANGE] Ошибка получения статуса: " << e.what() << "\n";
    }
    
    return OrderStatus::PENDING;
}

// ============================================================================
// Отмена ордера
// ============================================================================
bool ExchangeClient::cancelOrder(const std::string& order_id) {
    std::string endpoint = CoinExConfig::SPOT_ORDER_ENDPOINT + "/" + order_id;
    
    try {
        auto headers = buildHeaders("DELETE", endpoint);
        std::string response = sendHttpRequest("DELETE", endpoint, "", headers);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  [EXCHANGE] Ошибка отмены ордера: " << e.what() << "\n";
        return false;
    }
}

// ============================================================================
// Проверка соединения
// ============================================================================
bool ExchangeClient::isConnected() {
    if (m_api_key.empty()) {
        return true;  // В симуляции всегда "подключены"
    }
    
    try {
        auto headers = buildHeaders("GET", CoinExConfig::MARKET_ENDPOINT);
        std::string response = sendHttpRequest("GET", CoinExConfig::MARKET_ENDPOINT, "", headers);
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Получение статистики dry-run
// ============================================================================
DryRunStats ExchangeClient::getDryRunStats() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_dry_run_stats;
}
