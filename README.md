# 🤖 CryptoTrader — AI Trading Tool

**Консольный AI-агент для торговли на бирже CoinEx с использованием DeepSeek LLM**

> Версия: 1.0.0 | Язык: C++17 | Лицензия: MIT

---

## 📋 Оглавление

1. [О проекте](#-о-проекте)
2. [Архитектура](#-архитектура)
   - [Схема работы](#схема-работы)
   - [Модули](#модули)
   - [Поток данных](#поток-данных)
3. [Установка и сборка](#-установка-и-сборка)
   - [Зависимости](#зависимости)
   - [Сборка](#сборка)
   - [Проверка сборки](#проверка-сборки)
4. [Использование](#-использование)
   - [Режимы работы](#режимы-работы)
   - [Команды](#команды)
   - [Примеры](#примеры)
5. [API ключи](#-api-ключи)
   - [DeepSeek](#deepseek)
   - [CoinEx](#coinex)
6. [Структура кода](#-структура-кода)
   - [Описание файлов](#описание-файлов)
   - [Ключевые структуры данных](#ключевые-структуры-данных)
   - [Подробный разбор модулей](#подробный-разбор-модулей)
7. [Безопасность](#-безопасность)
   - [Механизмы защиты](#механизмы-защиты)
   - [Circuit Breaker](#circuit-breaker)
   - [Валидация](#валидация)
8. [Аудит и логирование](#-аудит-и-логирование)
   - [SQLite](#sqlite)
   - [JSONL](#jsonl)
   - [CSV экспорт](#csv-экспорт)
9. [Разработка](#-разработка)
   - [Добавление новых бирж](#добавление-новых-бирж)
   - [Добавление новых LLM](#добавление-новых-llm)
   - [Тестирование](#тестирование)
   - [План развития](#план-развития)
10. [Устранение неполадок](#-устранение-неполадок)
11. [FAQ](#-faq)

---

## 🔍 О проекте

CryptoTrader — это безопасный, аудируемый и контролируемый мост между **естественным языком** и **биржевым исполнением**.

### Концепция

Пользователь пишет команду на русском или английском языке, например:
```
купи 0.15 ETH по рынку, тейк 10%, стоп 3%
```

Программа:
1. **Отправляет команду в DeepSeek** — LLM анализирует текст и возвращает строгий JSON
2. **Проверяет риски** — валидатор проверяет баланс, лимиты, цены, безопасность
3. **Исполняет на CoinEx** — подписанный HMAC-SHA256 запрос к REST API биржи
4. **Логирует всё** — каждое действие сохраняется в SQLite + JSONL

### Философия

| Принцип | Описание |
|---------|----------|
| **LLM — исполнитель, не стратег** | DeepSeek не придумывает стратегии, а только структурирует команды пользователя |
| **Dry-run по умолчанию** | Случайная потеря средств невозможна — LIVE требует явного флага |
| **Полная прозрачность** | Каждый шаг логируется: запрос → валидация → ответ → статус |
| **Безопасность в коде** | Фьючерсы и маржа заблокированы, лимиты проверяются, circuit breaker активен |

---

## 🏗 Архитектура

### Схема работы

```
┌─────────────────────────────────────────────────────────────────┐
│                      ПОЛЬЗОВАТЕЛЬ                               │
│         "купи 0.15 ETH по рынку, тейк 10%, стоп 3%"             │
└──────────────────────┬──────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────────┐
│                    CLI (cli.cpp / cli.h)                         │
│  Принимает ввод, определяет тип команды, управляет режимами     │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌───────┐ │
│  │ Торговля │ │ Баланс   │ │ Статус   │ │ История  │ │Помощь │ │
│  └────┬─────┘ └──────────┘ └──────────┘ └──────────┘ └───────┘ │
└──────────────────────┬──────────────────────────────────────────┘
                       │ (торговая команда)
                       ▼
┌─────────────────────────────────────────────────────────────────┐
│                 LLM AGENT (llm_agent.cpp / .h)                  │
│                                                                 │
│  1. Собирает контекст: цены, баланс, время биржи               │
│  2. Формирует system prompt со строгой JSON-схемой              │
│  3. Отправляет POST-запрос к DeepSeek API                      │
│  4. Проверяет, что ответ — валидный JSON                       │
│  5. Парсит в структуру OrderCommand                            │
│                                                                 │
│  Получает:  "купи 0.15 ETH по рынку..."                        │
│  Возвращает: {"action":"buy","symbol":"ETH/USDT","amount":0.15}│
└──────────────────────┬──────────────────────────────────────────┘
                       │ (OrderCommand)
                       ▼
┌─────────────────────────────────────────────────────────────────┐
│               VALIDATOR (validator.cpp / .h)                    │
│                                                                 │
│  Четыре уровня проверки:                                        │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ 1. Схема → поля существуют, типы верны, значения ок      │   │
│  │ 2. Безопасность → только спот, нет фьючерсов/маржи       │   │
│  │ 3. Цены → отклонение от рынка ≤ 2% (warning) / ≤ 10%     │   │
│  │ 4. Лимиты → баланс, макс. ордер, дневной убыток          │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                 │
│  Также: Circuit Breaker (3 ошибки → 5 мин паузы)                │
└──────────────────────┬──────────────────────────────────────────┘
                       │ (если is_valid = true)
                       ▼
┌─────────────────────────────────────────────────────────────────┐
│             EXCHANGE CLIENT (exchange_client.cpp / .h)          │
│                                                                 │
│  ┌────────────┐     ┌──────────────┐    ┌──────────────────┐   │
│  │ DRY RUN    │  или│ LIVE         │    │                   │   │
│  │ (симуляция)│     │ (CoinEx API) │    │                   │   │
│  │ - нет HTTP │     │ - HMAC-SHA256│    │                   │   │
│  │ - тест.баланс│   │ - REST запрос│    │                   │   │
│  │ - расчёт   │     │ - парсинг    │    │                   │   │
│  │   комиссии │     │   ответа     │    │                   │   │
│  └────────────┘     └──────────────┘    └──────────────────┘   │
└──────────────────────┬──────────────────────────────────────────┘
                       │ (OrderResult)
                       ▼
┌─────────────────────────────────────────────────────────────────┐
│               AUDIT LOGGER (audit.cpp / .h)                     │
│                                                                 │
│  Сохраняет всё в:                                               │
│  ┌────────────────────┐  ┌────────────────────┐                 │
│  │ 📁 trades.jsonl    │  │ 🗄 crypto_trader.db│                 │
│  │ (JSONL — append    │  │ (SQLite — таблицы) │                 │
│  │ only, never modify)│  │ - orders           │                 │
│  └────────────────────┘  │ - config           │                 │
│                          │ - errors           │                 │
│                          │ - events           │                 │
│                          └────────────────────┘                 │
└──────────────────────┬──────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────────┐
│                     ВЫВОД ПОЛЬЗОВАТЕЛЮ                          │
│                                                                 │
│  ✅ Ордер создан #123456 | Ожидает исполнения | Комиссия: 0.0003│
│  ⚪ (DRY RUN: Предпросмотр — торгов не было)                    │
└─────────────────────────────────────────────────────────────────┘
```

### Модули

| Модуль | Файлы | Ответственность |
|--------|-------|-----------------|
| **config** | `include/config.h` | Конфигурация: структуры, enum-ы, лимиты, API endpoints |
| **main** | `src/main.cpp` | Точка входа, парсинг аргументов, инициализация компонентов |
| **cli** | `src/cli.cpp/.h` | CLI-оболочка, ввод команд, цветной вывод, управление режимами |
| **llm_agent** | `src/llm_agent.cpp/.h` | Общение с DeepSeek, формирование промптов, парсинг JSON |
| **validator** | `src/validator.cpp/.h` | Валидация схемы, рисков, цен; circuit breaker |
| **exchange_client** | `src/exchange_client.cpp/.h` | CoinEx REST API, HMAC-SHA256, симуляция dry-run |
| **audit** | `src/audit.cpp/.h` | SQLite + JSONL логирование, CSV экспорт |
| **utils** | `src/utils.h` | Форматирование чисел/валют, работа со строками, управление ключами |

### Поток данных

```
Ввод пользователя
    │
    ▼
┌──────────┐    ┌───────────┐    ┌──────────┐    ┌───────────────┐    ┌──────────┐
│  CLI     │───►│ LLM Agent │───►│Validator │───►│ExchangeClient │───►│  Audit   │
│ (ввод)   │    │ (DeepSeek)│    │(проверка)│    │(исполнение)   │    │(логи)    │
└──────────┘    └───────────┘    └──────────┘    └───────────────┘    └──────────┘
     │               │               │               │                  │
     │               │               │               ▼                  │
     │               │               │          ┌──────────┐            │
     │               │               │          │  CoinEx  │            │
     │               │               │          │  Server  │            │
     │               │               │          └──────────┘            │
     ▼               ▼               ▼                                 ▼
  Пользователь    DeepSeek API    Лимиты,        REST API            SQLite +
  видит ответ                     риски          запросы             JSONL
```

---

## 📦 Установка и сборка

### Зависимости

| Пакет | Назначение | Команда установки |
|-------|-----------|------------------|
| **cmake** (≥3.16) | Система сборки | `sudo apt install cmake` |
| **g++** (≥8) | Компилятор C++17 | `sudo apt install g++` |
| **libssl-dev** | OpenSSL (HMAC-SHA256) | `sudo apt install libssl-dev` |
| **libsqlite3-dev** | SQLite3 (база данных) | `sudo apt install libsqlite3-dev` |
| **libcurl4-openssl-dev** | libcurl (HTTP запросы) | `sudo apt install libcurl4-openssl-dev` |
| **nlohmann-json3-dev** | JSON парсинг | `sudo apt install nlohmann-json3-dev` |
| **git** | Контроль версий | `sudo apt install git` |
| **gh** | GitHub CLI | `sudo apt install gh` |

**Одной командой:**
```bash
sudo apt update && sudo apt install -y \
    cmake g++ libssl-dev libsqlite3-dev \
    libcurl4-openssl-dev nlohmann-json3-dev
```

### Сборка

```bash
# 1. Клонируем репозиторий (если ещё нет)
git clone https://github.com/user47504y5738654/crypto-trader.git
cd crypto-trader

# 2. Создаём папку для сборки
mkdir build && cd build

# 3. Конфигурируем CMake
cmake ..

# 4. Собираем
make -j$(nproc)
```

### Проверка сборки

```bash
# Проверяем, что бинарник создан
ls -la crypto_trader

# Запускаем
./crypto_trader
```

Ожидаемый вывод:
```
CryptoTrader — AI-трейдер v1.0.0
Режим: анализ инструкций через DeepSeek + исполнение на CoinEx

⚪ Режим: DRY RUN (симуляция без реальной торговли)
   Чтобы переключиться в LIVE, используйте флаг --live
...
crypto> [DRY-RUN] █
```

---

## 🎮 Использование

### Режимы работы

#### ⚪ DRY RUN (по умолчанию) — симуляция

```bash
./crypto_trader
# или явно:
./crypto_trader --dry-run
```

**Что происходит:**
- ✅ Команда анализируется DeepSeek
- ✅ Проходит все проверки валидатора
- ✅ Выводится предпросмотр: детали ордера, комиссия, тейк/стоп
- ❌ **Ордер НЕ отправляется на биржу**
- ✅ Всё логируется в SQLite + JSONL

**Для чего:** Тестирование, отладка, проверка стратегий без риска.

#### 🔴 LIVE — реальная торговля

```bash
./crypto_trader --live
```

**Дополнительные меры безопасности:**
1. При запуске: запрос подтверждения `yes/no`
2. Перед каждым ордером: запрос подтверждения
3. Все лимиты активны

**Внимание:** Используются реальные средства!

### Команды

#### Торговые инструкции (естественный язык)

DeepSeek понимает русский и английский:

```bash
# Русский
> купи 0.15 ETH по рынку, тейк 10%, стоп 3%
> продай 0.5 BTC по лимиту 65000 USDT
> купи 100 USDT солану
> продай 2 ETH

# English
> buy 0.15 ETH at market, take profit 10%, stop loss 3%
> sell 0.5 BTC limit 65000 USDT
> buy 100 USDT of SOL
```

#### Системные команды

| Команда | Русский | Описание |
|---------|---------|----------|
| `help` | `помощь` / `?` | Показать справку |
| `balance` | `баланс` | Показать баланс всех валют |
| `status` | `статус` | Статус системы + статистика dry-run |
| `history` | `история` | Последние 20 ордеров |
| `limits` | `лимиты` | Текущие лимиты безопасности |
| `clear` | `очистить` | Очистить экран |
| `exit` / `quit` | `выход` | Завершить программу |

### Примеры

#### Пример 1: Проверка баланса

```
crypto> [DRY-RUN] баланс

📊 Запрос баланса...
  [EXCHANGE] Режим симуляции: возвращаю тестовый баланс

Баланс:
┌──────────┬──────────────┬────────────────┐
│ Валюта   │ Доступно      │ Заморожено     │
├──────────┼──────────────┼────────────────┤
│ BTC      │       0.5000 │         0.0000 │
│ ETH      │       5.0000 │         0.0000 │
│ SOL      │      50.0000 │         0.0000 │
│ USDT     │   10000.0000 │         0.0000 │
└──────────┴──────────────┴────────────────┘
```

#### Пример 2: DRY RUN торговля (с DeepSeek API ключом)

```
crypto> [DRY-RUN] купи 0.15 ETH по рынку, тейк 10%, стоп 3%

🤖 Анализ инструкции через DeepSeek...
  [LLM] Формирую промпт с контекстом рынка...
  [LLM] Отправляю запрос к DeepSeek...
  [LLM] Проверяю схему ответа...
  [LLM] Команда успешно распознана.

✅ Команда распознана:
  Действие:   buy
  Пара:       ETH/USDT
  Объём:      0.15
  Тип цены:   market
  Тейк-профит: 10%
  Стоп-лосс:  3%

🔍 Проверка рисков...
✅ Валидация пройдена

╔══════════════════════════════════════════════╗
║        ПРЕДПРОСМОТР ОРДЕРА (DRY-RUN)        ║
╚══════════════════════════════════════════════╝

  📋 Детали ордера:
     Покупка 0.15 ETH/USDT
     Тип: Рыночный

  💰 Расчёт комиссии:
     Ставка: 0.2%
     Комиссия: ≈ 0.000300 ETH

  🎯 Уровни:
     Тейк-профит: +10%
     Стоп-лимит:  -3%

  ✅ Ордер проверен. Реальных торгов не производилось.
```

#### Пример 3: Проверка статуса

```
crypto> [DRY-RUN] статус

ℹ️  Статус системы:
  Режим:      DRY RUN (симуляция)
  Время:      2025-06-15 14:30:00
  Версия:     1.0.0
  Статус API: OK

📈 Статистика симуляции:
  Всего сделок:  5
  Успешных:      5
  Прибыль:       +125.42 USDT
```

---

## 🔑 API ключи

### DeepSeek

1. Зарегистрируйтесь на [platform.deepseek.com](https://platform.deepseek.com)
2. Перейдите в **API Keys** → **Create API key**
3. Скопируйте ключ (формат: `sk-xxx...`)
4. Пополните баланс (~$0.14 за 1M токенов, $1 ≈ 1000 команд)

**Передача ключа:**
```bash
# Переменная окружения (рекомендуется)
export DEEPSEEK_API_KEY="sk-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"

# Аргумент CLI
./crypto_trader --deepseek-key "sk-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
```

### CoinEx

1. Зарегистрируйтесь на [coinex.com](https://www.coinex.com)
2. **Security** → **API Management** → **Create API Key**
3. Включите разрешения: Spot Trading, Spot Order Query, Asset Query
4. **Отключите:** Futures, Withdraw
5. Сохраните Access ID и Secret Key

**Передача ключей:**
```bash
export COINEX_API_KEY="XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"
export COINEX_API_SECRET="xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
./crypto_trader --live
```

---

## 📁 Структура кода

### Описание файлов

```
crypto_trader/
│
├── 📄 CMakeLists.txt              # Система сборки CMake
├── 📄 README.md                   # Этот файл — документация
├── 📄 INSTRUCTIONS.md             # Инструкция по API ключам
├── 📄 .gitignore                  # Что не попадает в git
│
├── 📁 include/
│   └── 📄 config.h                # 🎯 Конфигурация всего проекта
│
├── 📁 src/
│   ├── 📄 main.cpp                # 🚀 Точка входа
│   ├── 📄 cli.cpp / cli.h         # 💻 CLI-оболочка
│   ├── 📄 llm_agent.cpp / .h      # 🤖 DeepSeek интеграция
│   ├── 📄 validator.cpp / .h      # 🛡 Валидация и риски
│   ├── 📄 exchange_client.cpp / .h # 🔄 CoinEx API
│   ├── 📄 audit.cpp / audit.h     # 📝 Логирование
│   └── 📄 utils.h                 # 🔧 Утилиты
│
└── 📁 build/                      # Папка сборки (gitignored)
```

### Ключевые структуры данных

#### `AppConfig` (config.h + main.cpp)
Конфигурация приложения, создаётся из аргументов командной строки:
```cpp
struct AppConfig {
    TradingMode mode;          // DRY_RUN или LIVE
    std::string config_file;   // Путь к файлу конфигурации
    std::string api_key;       // CoinEx API Key (Access ID)
    std::string api_secret;    // CoinEx Secret Key
    std::string deepseek_key;  // DeepSeek API Key
};
```

#### `OrderCommand` (config.h)
Структурированная команда, которую возвращает DeepSeek:
```cpp
struct OrderCommand {
    std::string action;        // "buy" или "sell"
    std::string symbol;        // "ETH/USDT", "BTC/USDT"
    double amount;             // Количество (0.15 ETH)
    std::string price_type;    // "market" или "limit"
    double price;              // Цена для лимитных ордеров
    double take_profit;        // Тейк-профит в % (10.0 = 10%)
    double stop_loss;          // Стоп-лосс в % (3.0 = 3%)
};
```

#### `MarketContext` (llm_agent.h)
Контекст рынка, который передаётся DeepSeek для принятия решения:
```cpp
struct MarketContext {
    double btc_price, eth_price, sol_price;  // Текущие цены
    double usdt_balance, btc_balance, eth_balance;  // Балансы
    int64_t server_time;  // Время биржи
};
```

#### `RiskLimits` (config.h)
Настраиваемые лимиты безопасности:
```cpp
struct RiskLimits {
    double max_order_usd = 1000.0;       // Макс. $1000 на ордер
    double daily_loss_limit = 200.0;     // Макс. $200 убытка в день
    int max_open_positions = 5;          // Не более 5 открытых позиций
    double max_market_deviation = 2.0;   // Отклонение от рынка ≤ 2%
    int circuit_breaker_count = 3;       // 3 ошибки → пауза
    int circuit_breaker_seconds = 300;   // Пауза на 5 минут
};
```

#### `ValidationResult` (validator.h)
Результат проверки рисков:
```cpp
struct ValidationResult {
    bool is_valid = true;                     // true = можно торговать
    std::string error_message;                // Причина отказа
    std::vector<std::string> warnings;        // Предупреждения (не блокирующие)
};
```

#### `OrderResult` (config.h)
Результат исполнения ордера на бирже:
```cpp
struct OrderResult {
    std::string order_id;       // ID ордера (например, "SIM-123456")
    OrderStatus status;         // FILLED, REJECTED, PENDING...
    double filled_amount;       // Сколько реально купили
    double filled_price;        // Средняя цена исполнения
    double fee;                 // Комиссия сети
    std::string error_msg;      // Текст ошибки, если есть
};
```

### Подробный разбор модулей

#### 1. `config.h` — Конфигурация

Файл содержит всё, что может меняться при настройке: enum-ы (TradingMode, OrderType, OrderSide, OrderStatus), структуры данных (OrderCommand, OrderResult, RiskLimits), и константы для API (CoinExConfig::BASE_URL, DeepSeekConfig::API_URL).

**Зачем это отдельно?** Чтобы при добавлении новой биржи или новой LLM нужно было менять только этот файл и соответствующий модуль.

```cpp
// Пример: как задаются API endpoints
namespace CoinExConfig {
    const std::string BASE_URL = "https://api.coinex.com";
    const std::string SPOT_ORDER_ENDPOINT = "/v2/spot/order";
    const std::string BALANCE_ENDPOINT = "/v2/spot/balance";
};
```

---

#### 2. `main.cpp` — Точка входа

```cpp
int main(int argc, char* argv[]) {
    // 1. Устанавливаем обработчики сигналов (Ctrl+C)
    signal(SIGINT, signalHandler);
    
    // 2. Парсим аргументы командной строки
    auto config = parseArgs(argc, argv);
    
    // 3. Если LIVE — запрашиваем подтверждение
    if (config->mode == TradingMode::LIVE) {
        std::cout << "Подтвердите переход в LIVE режим (yes/no): ";
        // ...
    }
    
    // 4. Создаём компоненты (Dependency Injection)
    auto audit = std::make_shared<AuditLogger>("crypto_trader.db", "trades.jsonl");
    auto exchange = std::make_shared<ExchangeClient>(config->api_key, config->api_secret);
    auto validator = std::make_shared<Validator>();
    auto llm_agent = std::make_shared<LLMAgent>(config->deepseek_key);
    
    // 5. Запускаем CLI
    auto cli = std::make_unique<CLI>(config, llm_agent, validator, exchange, audit);
    cli->run();
}
```

**Почему shared_ptr?** Компоненты используются разными модулями. Например, `audit` нужен и CLI (для логирования команд), и exchange_client (для логирования ответов).

---

#### 3. `cli.cpp` / `cli.h` — CLI-оболочка

**Что делает:**
1. Печатает приглашение `crypto> [DRY-RUN] `
2. Читает строку ввода
3. Определяет тип команды (торговая или системная)
4. Маршрутизирует к соответствующему обработчику
5. Выводит результат с цветным форматированием

**Распознавание команд:**
```cpp
void CLI::processCommand(const std::string& input) {
    // Приводим к нижнему регистру
    std::string lower = toLower(input);
    
    // Системные команды
    if (lower == "выход" || lower == "exit") { ... }
    else if (lower == "помощь" || lower == "help") { ... }
    else if (lower == "баланс" || lower == "balance") { ... }
    else if (lower == "статус" || lower == "status") { ... }
    
    // Торговые команды (по ключевым словам)
    else if (containsAny(lower, {"купи", "продай", "buy", "sell"})) {
        processTradingCommand(input);  // → LLM → валидация → исполнение
    }
}
```

**Цветной вывод:**
```cpp
namespace Color {
    const std::string GREEN   = "\033[32m";  // Успех
    const std::string RED     = "\033[31m";  // Ошибка
    const std::string YELLOW  = "\033[33m";  // Предупреждение
    const std::string BLUE    = "\033[34m";  // Информация
    const std::string CYAN    = "\033[36m";  // Системные сообщения
    const std::string BOLD    = "\033[1m";   // Жирный
};
```

**Dry-run предпросмотр:**
```cpp
void CLI::printOrderPreview(const OrderCommand& cmd) {
    // Расчёт симулированной комиссии
    double fee_rate = 0.002;  // 0.2% — стандартная комиссия CoinEx
    double estimated_fee = cmd.amount * fee_rate;
    
    // Вывод таблицы
    std::cout << "  📋 Детали ордера:\n";
    std::cout << "     Покупка " << cmd.amount << " " << cmd.symbol << "\n";
    std::cout << "     Комиссия: ≈ " << estimated_fee << "\n";
    std::cout << "     Тейк: +" << cmd.take_profit << "%\n";
    std::cout << "     Стоп: -" << cmd.stop_loss << "%\n";
}
```

---

#### 4. `llm_agent.cpp` / `llm_agent.h` — DeepSeek интеграция

**Системный промпт** — самая важная часть. Он определяет, как DeepSeek будет обрабатывать команды:

```cpp
std::string LLMAgent::buildSystemPrompt() {
    // Этот промпт отправляется DeepSeek как system message
    // Он задаёт правила, формат ответа и примеры
    prompt << R"(Ты — торговый агент для CoinEx.
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
Пример:
Пользователь: "Купи 0.15 ETH по рынку, тейк 10%, стоп 3%"
Ответ: {"action":"buy","symbol":"ETH/USDT","amount":0.15,...})";
}
```

**Пользовательский промпт** содержит контекст:
```
Текущая ситуация на рынке:
{
  "prices": {"BTC/USDT": 65000, "ETH/USDT": 3500},
  "balances": {"USDT": 10000, "BTC": 0.5},
  "server_time": 1718467200000
}

Инструкция пользователя: купи 0.15 ETH по рынку
```

**HTTP запрос к DeepSeek:**
```cpp
json LLMAgent::sendRequest(const std::string& system_prompt,
                            const std::string& user_prompt) {
    // 1. Формируем тело запроса
    json body = {
        {"model", "deepseek-chat"},
        {"messages", {
            {{"role", "system"}, {"content", system_prompt}},
            {{"role", "user"}, {"content", user_prompt}}
        }},
        {"temperature", 0.1},  // Низкая = детерминированный вывод
        {"max_tokens", 500}
    };
    
    // 2. Отправляем POST запрос с CURL
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.deepseek.com/v1/chat/completions");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.dump().c_str());
    
    // ... обрабатываем ответ ...
    
    // 3. Парсим ответ LLM как JSON
    return json::parse(response["choices"][0]["message"]["content"]);
}
```

**Валидация JSON-схемы ответа:**
```cpp
bool LLMAgent::validateSchema(const json& data) {
    if (!data.contains("action")) return false;
    if (!data.contains("symbol")) return false;
    if (!data.contains("amount")) return false;
    
    std::string action = data["action"];
    if (action != "buy" && action != "sell") return false;
    
    return true;
}
```

**Повторные попытки (retries):**
```cpp
for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
    try {
        response = sendRequest(system_prompt, user_prompt);
        break;  // Успех
    } catch (const std::exception& e) {
        std::cerr << "Попытка " << attempt << "/3 не удалась\n";
        sleep(1);  // Ждём 1 секунду перед повтором
    }
}
```

---

#### 5. `validator.cpp` / `validator.h` — Валидация

**4 уровня проверки:**

**Уровень 1 — Схема:**
```cpp
ValidationResult Validator::validateSchema(const OrderCommand& cmd) {
    // Проверка: действие должно быть buy/sell
    if (cmd.action != "buy" && cmd.action != "sell")
        return {false, "Неизвестное действие: " + cmd.action};
    
    // Проверка: символ должен содержать "/"
    if (cmd.symbol.find('/') == std::string::npos)
        return {false, "Неверный формат пары"};
    
    // Проверка: объём должен быть положительным
    if (cmd.amount <= 0)
        return {false, "Объём должен быть > 0"};
}
```

**Уровень 2 — Безопасность:**
```cpp
ValidationResult Validator::validateSafety(const OrderCommand& cmd) {
    // Блокировка фьючерсов
    if (cmd.symbol.find("PERP") != std::string::npos ||
        cmd.symbol.find("FUTURES") != std::string::npos)
        return {false, "Фьючерсы запрещены. Только спот."};
}
```

**Уровень 3 — Цены:**
```cpp
ValidationResult Validator::validatePrices(const OrderCommand& cmd,
                                            const MarketContext& context) {
    double deviation = abs(cmd.price - market_price) / market_price * 100;
    
    if (deviation > limits.max_market_deviation) {
        // > 2% — предупреждение
        warnings.push_back("Цена отклоняется на " + deviation + "%");
    }
    if (deviation > 10.0) {
        // > 10% — блокировка
        return {false, "Отклонение > 10%. Заблокировано."};
    }
}
```

**Уровень 4 — Лимиты:**
```cpp
ValidationResult Validator::validateLimits(const OrderCommand& cmd,
                                            const MarketContext& context) {
    // Максимальный ордер
    if (estimated_order_usd > limits.max_order_usd)
        return {false, "Ордер превышает лимит $" + limits.max_order_usd};
    
    // Дневной убыток
    if (!checkDailyLossLimit(estimated_order_usd))
        return {false, "Дневной лимит убытков исчерпан"};
    
    // Недостаточно средств
    if (required_usdt > context.usdt_balance)
        return {false, "Недостаточно USDT"};
}
```

---

#### 6. `exchange_client.cpp` / `exchange_client.h` — CoinEx API

**HMAC-SHA256 подпись запросов:**
```cpp
std::string ExchangeClient::signRequest(const std::string& method,
                                         const std::string& path,
                                         const std::string& query_string,
                                         const std::string& body,
                                         const std::string& timestamp) {
    // Формируем строку для подписи
    std::string sign_str = method + "\n" + path + "\n" + 
                          query_string + "\n" + body + "\n" + timestamp;
    
    // HMAC-SHA256 через OpenSSL
    unsigned char hash[SHA256_DIGEST_LENGTH];
    HMAC(EVP_sha256(), secret.c_str(), secret.length(),
         (unsigned char*)sign_str.c_str(), sign_str.length(),
         hash, nullptr);
    
    // Конвертируем в hex
    return hex_string;
}
```

**Dry-run симуляция:**
```cpp
OrderResult ExchangeClient::simulateOrder(const OrderCommand& cmd) {
    OrderResult result;
    result.order_id = "SIM-" + random_string();
    result.status = OrderStatus::FILLED;
    result.fee = cmd.amount * 0.002;  // 0.2% комиссия
    
    // Обновляем симулированный баланс
    m_simulated_balance["USDT"].available -= cmd.amount * price;
    m_simulated_balance["ETH"].available += cmd.amount;
    
    return result;
}
```

**LIVE запрос к CoinEx:**
```cpp
OrderResult ExchangeClient::placeOrder(const OrderCommand& cmd) {
    // 1. Формируем JSON тело
    json order = {
        {"market", cmd.symbol},
        {"side", cmd.action},
        {"type", cmd.price_type == "market" ? "market" : "limit"},
        {"amount", cmd.amount}
    };
    
    // 2. Подписываем заголовки
    auto headers = buildHeaders("POST", "/v2/spot/order", order.dump());
    
    // 3. Отправляем POST запрос
    std::string response = sendHttpRequest("POST", "/v2/spot/order", 
                                           order.dump(), headers);
    
    // 4. Парсим ответ
    auto json = nlohmann::json::parse(response);
    result.order_id = json["data"]["order_id"];
}
```

---

#### 7. `audit.cpp` / `audit.h` — Логирование

**Инициализация SQLite:**
```cpp
void AuditLogger::initDatabase() {
    sqlite3_open(m_db_path.c_str(), &db);
    
    // Создаём таблицы
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS orders (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp TEXT,
            action TEXT, symbol TEXT, amount REAL,
            status TEXT, message TEXT
        );
        CREATE TABLE IF NOT EXISTS config (
            key TEXT PRIMARY KEY, value TEXT
        );
        CREATE TABLE IF NOT EXISTS errors (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp TEXT, context TEXT, error_msg TEXT
        );
        CREATE TABLE IF NOT EXISTS events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp TEXT, event TEXT, details TEXT
        );
    )";
    sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
}
```

**JSONL запись (append-only):**
```cpp
void AuditLogger::writeJSONL(const json& record) {
    // Открываем файл в режиме append
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Каждая строка — один JSON-объект
    m_jsonl_file << record.dump() << std::endl;
    m_jsonl_file.flush();
}
```

**Пример JSONL записи:**
```json
{"type":"order","timestamp":"2025-06-15 14:30:00","action":"buy","symbol":"ETH/USDT","amount":0.15,"status":"DRY_RUN","message":"Симуляция"}
{"type":"event","timestamp":"2025-06-15 14:30:00","event":"Запуск приложения","details":"DRY_RUN"}
{"type":"error","timestamp":"2025-06-15 14:30:05","context":"купи 0.1 ETH","error":"API ключ не установлен"}
```

**CSV экспорт:**
```cpp
bool AuditLogger::exportToCSV(const std::string& filepath) {
    // SQL запрос → запись в CSV
    std::ofstream csv(filepath);
    csv << "timestamp,action,symbol,amount,status\n";
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        csv << sqlite3_column_text(stmt, 0) << ","
            << sqlite3_column_text(stmt, 1) << ","
            << sqlite3_column_double(stmt, 3) << "\n";
    }
}
```

---

#### 8. `utils.h` — Утилиты

**Форматирование:**
```cpp
namespace Format {
    std::string price(double value, int precision = 2);    // "1234.56"
    std::string usdt(double value);                         // "$1,234.56"
    std::string percent(double value);                      // "10.0%"
    std::string crypto(double value);                       // "0.150000"
}
```

**Работа со строками:**
```cpp
namespace StrUtil {
    std::vector<std::string> split(const std::string& s, char delimiter);
    std::string trim(const std::string& s);
    std::string toLower(const std::string& s);
}
```

**Управление ключами (заглушка для OS Keyring):**
```cpp
namespace KeyManager {
    bool saveKey(const std::string& service, const std::string& key, 
                 const std::string& value);
    std::string loadKey(const std::string& service, const std::string& key);
    std::string maskKey(const std::string& key);  // "abcd****wxyz"
}
```

---

## 🛡 Безопасность

### Механизмы защиты

| Механизм | Где реализован | Описание |
|----------|----------------|----------|
| **Dry-run по умолчанию** | main.cpp | LIVE требует флаг `--live` + подтверждение |
| **Подтверждение LIVE** | main.cpp, cli.cpp | Два запроса: при запуске и перед каждым ордером |
| **Валидация схемы** | validator.cpp | Проверка всех полей JSON от LLM |
| **Блокировка фьючерсов** | validator.cpp | Поиск слов "perp", "futures", "margin" |
| **Лимит ордера** | validator.cpp | `max_order_usd` (по умолч. $1000) |
| **Дневной лимит** | validator.cpp | `daily_loss_limit` (по умолч. $200) |
| **Отклонение цены** | validator.cpp | >10% — блокировка, >2% — предупреждение |
| **Circuit breaker** | validator.cpp | 3 ошибки → 5 мин пауза |
| **Маскировка ключей** | utils.h | Ключи никогда не выводятся в консоль |
| **Хранение ключей** | — | В OS Keyring (TODO) или переменных окружения |

### Circuit Breaker

```
[VALIDATOR] Ошибка #1/3
[VALIDATOR] Ошибка #2/3
[VALIDATOR] Ошибка #3/3
[VALIDATOR] ⚠ CIRCUIT BREAKER АКТИВИРОВАН!
[VALIDATOR] Пауза 300 секунд.
   ... 5 минут ...
[VALIDATOR] Circuit breaker сброшен. Торговля возобновлена.
```

Реализация:
```cpp
void Validator::registerError() {
    m_error_count++;
    
    if (m_error_count >= m_limits.circuit_breaker_count) {
        m_circuit_breaker_active = true;
        m_circuit_breaker_until = now + 300_seconds;
    }
}

bool Validator::isCircuitBreakerActive() {
    if (!m_circuit_breaker_active) return false;
    if (now >= m_circuit_breaker_until) {
        m_circuit_breaker_active = false;
        m_error_count = 0;
        return false;
    }
    return true;
}
```

### Валидация

Валидатор запускается **перед каждым ордером** и проверяет 4 уровня:

```
Уровень 1: Схема
  ✅ action = "buy" | "sell"
  ✅ symbol содержит "/" (например "ETH/USDT")
  ✅ amount > 0
  ✅ price_type = "market" | "limit"

Уровень 2: Безопасность
  ❌ Блокировка: "PERP", "FUTURES", "MARGIN" в symbol
  ✅ Только спот-торговля

Уровень 3: Цены (для лимитных ордеров)
  ✅ Отклонение от рынка < 2% — ок
  ⚠ Отклонение 2-10% — предупреждение
  ❌ Отклонение > 10% — блокировка

Уровень 4: Лимиты
  ✅ Стоимость ордера ≤ max_order_usd
  ✅ Дневной P&L ≥ -daily_loss_limit
  ✅ Достаточно средств на балансе
```

---

## 📝 Аудит и логирование

### SQLite

База данных `crypto_trader.db` содержит 4 таблицы:

| Таблица | Назначение | Пример записи |
|---------|-----------|---------------|
| **orders** | История всех ордеров | `(1, "2025-06-15 14:30:00", "buy", "ETH/USDT", 0.15, "DRY_RUN", "Симуляция")` |
| **config** | Конфигурация и лимиты | `("max_order_usd", "1000")` |
| **errors** | Лог ошибок | `(1, "2025-06-15 14:30:05", "купи 0.1 ETH", "API ключ не установлен")` |
| **events** | Системные события | `(1, "2025-06-15 14:30:00", "Запуск приложения", "DRY_RUN")` |

**Просмотр базы вручную:**
```bash
sqlite3 crypto_trader.db
.tables
SELECT * FROM orders;
SELECT * FROM config;
SELECT * FROM errors;
.quit
```

### JSONL

Файл `trades.jsonl` — это **append-only** лог. Каждая строка — один JSON-объект. Преимущество: нельзя изменить уже записанные данные (аудит).

```json
{"type":"order","timestamp":"2025-06-15 14:30:00","action":"buy","symbol":"ETH/USDT","amount":0.15,"status":"DRY_RUN"}
{"type":"event","timestamp":"2025-06-15 14:30:00","event":"Запуск приложения","details":"DRY_RUN"}
{"type":"error","timestamp":"2025-06-15 14:30:05","context":"купи 0.1 ETH","error":"API ключ не установлен"}
```

**Анализ JSONL:**
```bash
# Сколько всего команд
wc -l trades.jsonl

# Все успешные ордера
grep '"status":"FILLED"' trades.jsonl

# Все ошибки
grep '"type":"error"' trades.jsonl

# Красивый вывод одной записи
cat trades.jsonl | head -1 | python3 -m json.tool
```

### CSV экспорт

Для анализа в Excel/Google Sheets:
```cpp
audit.exportToCSV("orders_export.csv");
```

Создаёт файл с заголовками:
```csv
timestamp,action,symbol,amount,price_type,price,take_profit,stop_loss,status,message
2025-06-15 14:30:00,buy,ETH/USDT,0.15,market,0,10,3,DRY_RUN,Симуляция
```

---

## 🔧 Разработка

### Добавление новых бирж

Чтобы добавить новую биржу (например, Binance):

1. **config.h** — добавить конфигурацию:
```cpp
namespace BinanceConfig {
    const std::string BASE_URL = "https://api.binance.com";
    const std::string ORDER_ENDPOINT = "/api/v3/order";
};
```

2. **exchange_client.h** — наследовать интерфейс:
```cpp
class BinanceClient : public ExchangeClient {
    // Реализовать те же методы: getBalance(), placeOrder() и т.д.
    // Подпись запросов через Binance API (Ed25519)
};
```

3. **main.cpp** — добавить выбор биржи:
```cpp
if (config.exchange == "coinex") {
    exchange = std::make_shared<CoinExClient>(...);
} else if (config.exchange == "binance") {
    exchange = std::make_shared<BinanceClient>(...);
}
```

### Добавление новых LLM

Чтобы добавить другую LLM (например, OpenAI GPT):

1. **llm_agent.h** — наследовать интерфейс:
```cpp
class OpenAILLM : public LLMAgent {
    // buildSystemPrompt() — адаптировать под OpenAI
    // sendRequest() — изменить endpoint на api.openai.com
    // parseResponse() — адаптировать под формат ответа OpenAI
};
```

2. **config.h** — добавить:
```cpp
namespace OpenAIConfig {
    const std::string API_URL = "https://api.openai.com/v1/chat/completions";
    const std::string MODEL = "gpt-4";
};
```

### Тестирование

Проект поддерживает ручное тестирование (автоматические тесты можно добавить в `tests/`):

```bash
# 1. Базовая проверка
./crypto_trader --help
./crypto_trader --version

# 2. Проверка всех команд (через echo)
echo -e "помощь\nбаланс\nстатус\nлимиты\nистория\nвыход" | ./crypto_trader

# 3. Проверка торговой команды (требует DeepSeek ключ)
echo -e "купи 0.001 BTC по рынку\nвыход" | ./crypto_trader --deepseek-key "sk-xxx"

# 4. LIVE режим (требует все ключи)
echo -e "баланс\nвыход" | ./crypto_trader --live --key "xxx" --secret "xxx"
```

### План развития

- [x] **v1.0** — Базовая архитектура: CLI → LLM → Validator → Exchange → Audit
- [x] **v1.0** — DRY RUN / LIVE режимы
- [ ] **v1.1** — OS Keyring для хранения ключей
- [ ] **v1.2** — История в реальном времени (WebSocket)
- [ ] **v1.3** — Юнит-тесты (Google Test)
- [ ] **v2.0** — Поддержка нескольких бирж (Binance, Bybit)
- [ ] **v2.0** — Поддержка нескольких LLM (GPT-4, Claude)
- [ ] **v2.1** — Интерактивный дашборд (ncurses)

---

## ❓ Устранение неполадок

### "fatal error: json.hpp: No such file or directory"

```bash
# Установите nlohmann-json
sudo apt install nlohmann-json3-dev

# Или проверьте путь
find /usr -name "json.hpp"
# Должен быть: /usr/include/nlohmann/json.hpp
```

### "cannot find -lssl" / "cannot find -lcrypto"

```bash
sudo apt install libssl-dev
```

### "cannot find -lsqlite3"

```bash
sudo apt install libsqlite3-dev
```

### "API ключ DeepSeek не установлен"

```bash
export DEEPSEEK_API_KEY="sk-ваш_ключ_здесь"
# Запустите программу заново
```

### "Ошибка HTTP: SSL certificate problem"

```bash
# Установите корневые сертификаты
sudo apt install ca-certificates
```

### Программа не запускается (ошибка сегментации)

```bash
# Пересоберите с отладочной информацией
cd build && cmake -DCMAKE_BUILD_TYPE=Debug .. && make
# Запустите через GDB
gdb ./crypto_trader
```

---

## 💡 FAQ

**Q: Нужен ли мне API ключ DeepSeek для работы программы?**
A: Да, для торговых команд (купи/продай). Системные команды (баланс, статус, помощь) работают без ключа.

**Q: Можно ли использовать бесплатную LLM?**
A: DeepSeek — платный, но очень дешёвый (~$0.14/1M токенов). $1 ≈ 1000 команд. Альтернативы: OpenAI GPT, Claude (потребуется адаптация кода).

**Q: Безопасно ли вводить API ключи?**
A: Ключи передаются как переменные окружения (не попадают в логи). В будущем будет OS Keyring.

**Q: Сколько стоит комиссия на CoinEx?**
A: 0.2% за сделку (спот). Для VIP клиентов — от 0.12%.

**Q: Может ли программа потерять все мои деньги?**
A: Нет, потому что:
- DRY RUN по умолчанию (нужен флаг --live)
- Подтверждение каждого LIVE ордера
- Лимиты (макс. $1000/ордер, $200/день)
- Только спот (без фьючерсов)
- Circuit breaker при ошибках

**Q: Работает ли программа на Windows/Mac?**
A: Код кроссплатформенный (C++17, CMake). Требуется адаптация:
- Windows: MSVC или MinGW
- Mac: Homebrew для установки зависимостей

**Q: Где хранятся данные?**
A: В папке запуска: `crypto_trader.db` (SQLite) и `trades.jsonl` (JSON-лог).

---

## 📄 Лицензия

MIT License. Используйте, модифицируйте, распространяйте свободно.

Проект создан в образовательных целях. Автор не несёт ответственности за финансовые потери, возникшие при использовании данного программного обеспечения.

---

*Сделано с ❤️ и C++17*
