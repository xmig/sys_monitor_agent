# [AI] Анализ проекта sys_monitor_agent

> Документ сгенерирован автоматически. Пометка `[AI]` означает наблюдение,
> добавленное ИИ-ассистентом без изменения логики кода.

---

## 1. Назначение и архитектура

**sys_monitor_agent** — демон мониторинга системных ресурсов, написанный на C++20.
Собирает метрики хоста (CPU, память, диски, сеть, процессы, Docker, TCP-порты)
и отправляет их по **UDP/multicast** в формате **JSON** с заданным интервалом.

Целевые платформы: **Linux** (полный набор функций) и **macOS** (частичная поддержка).
**Windows не поддерживается** намеренно.

Исполняемый файл: `dsp`.  
Единственный исходный файл: `sys_monitor_agent.cpp` (~1300 строк).  
Сборка: CMake 3.25+, C++20, компилятор g++.

---

## 2. Зависимости

| Библиотека | Версия | Назначение | Обязательна |
|---|---|---|---|
| Boost | ≥1.88.0 | asio, beast, algorithm, json, filesystem | Да |
| OpenSSL | 3.x | TLS/HTTPS для Slack (опционально) | Нет (`USESSL`) |
| C++ STL | C++20 | Базовые структуры данных | Да |

Установка зависимостей (Linux):
```bash
sudo apt-get install libssl-dev libboost-all-dev
```

macOS (Homebrew):
```bash
brew install boost openssl@3
cmake -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3) .
```

---

## 3. Поддержка платформ: Linux vs macOS

### 3.1 Работает на обеих платформах
| Функция | Источник данных |
|---|---|
| `disk_space()` | `statvfs()` — POSIX |
| `get_hostname()` | `gethostname()` — POSIX |
| `get_loadavg()` | `getloadavg()` — POSIX |
| `exec_command()` | `popen()` — POSIX |
| UDP-транспорт | Boost.ASIO |
| `daemonize()` | double-fork — POSIX |

### 3.2 Linux-only (на macOS вернут пустые/нулевые данные)
| Функция | Источник | macOS-эквивалент |
|---|---|---|
| `getNetworkStats()` | `/proc/net/dev` | `netstat -ibn` |
| `get_memory_info()` | `/proc/meminfo` | `vm_stat`, `sysctl hw.memsize` |
| `get_uptime()` | `/proc/uptime` | `sysctl kern.boottime` |
| `read_CPU_stats()` | `/proc/stat` | Mach API `host_statistics64()` |
| `hostinfo` action | `hostnamectl` (systemd) | `scutil --get ComputerName` |
| `tcp` action | `ss -tlnp` (iproute2) | `netstat -anp tcp \| grep LISTEN` |

### 3.3 Условно-совместимые
| Функция | Особенность |
|---|---|
| `host` action (`netcat`) | Флаги `-zvw1` работают на Linux и macOS; на некоторых системах это `nc`, не `netcat` |
| `df` action | GNU df (Linux): `1K-blocks`; macOS df: другой формат — парсинг по индексу колонки хрупок |
| `iftop` action | На macOS iftop может требовать root или отсутствовать |
| `docker` action | Работает везде, где установлен Docker CLI |

---

## 4. Ключевые компоненты

### 4.1 Класс `Agent`
Центральный объект. Управляет:
- UDP-сокетом и endpoint-ом для отправки
- Таймерами для каждой метрики (`times_`, `timeouts_`)
- Дополнительными параметрами метрик (`extra_params_`, например: `disk[/;/data]`)
- Дедупликацией Slack-сообщений (`message_sent_map_`)

### 4.2 Цикл опроса (`handle_timeout`)
Вызывается как ASIO-callback, но внутри содержит бесконечный `while(true)` с
`sleep(500ms)`. Фактически это polling-loop, блокирующий поток `io_context`.
Корректно работает в текущей однопоточной модели, но несовместимо с добавлением
других async-операций в тот же `io_context`.

### 4.3 Формат JSON-сообщений
```json
{
  "action":  "monitor-<ACTION_TYPE>",
  "appkey":  "<APP_KEY>",
  "host":    "<HOSTNAME>",
  "time":    "<UNIX_TIMESTAMP>",
  "data":    { ... }
}
```

### 4.4 Slack-интеграция (только с `USESSL`)
- Класс `SlackStatusSender`: синхронный HTTPS POST через Boost.Beast + TLS 1.2.
- Используется для: `kill_cpu`, `kill_mem`, `host` (при недоступности), `dailyreport`.
- Дедупликация через `DEBOUNCE_TIME_SEC` (по умолчанию 10 минут).

---

## 5. Поддерживаемые метрики и их параметры

| Action | Интервал | Параметры `[...]` | Описание |
|---|---|---|---|
| `cpu:N` | N сек | — | CPU usage по ядрам + loadavg + uptime |
| `memory:N` | N сек | — | Первые 20 строк /proc/meminfo |
| `disk:N[/;/data]` | N сек | mount points через `;` | statvfs() на каждую точку монтирования |
| `df:N[/;/data]` | N сек | mount points через `;` | Вывод `df -l` с фильтрацией |
| `net:N` | N сек | — | RX/TX bytes/sec по интерфейсам |
| `iftop:N` | N сек | — | Парсинг файла `iftop-result.txt` |
| `tcp:N` | N сек | — | TCP listening ports (`ss -tlnp`) |
| `ps:N` | N сек | — | Все процессы (`ps -eo`) |
| `python:N` | N сек | — | Только Python-процессы |
| `xtest:N` | N сек | — | Процессы с именем XTEST |
| `docker:N` | N сек | — | `docker ps --no-trunc -a` |
| `hostinfo:N` | N сек | — | `hostnamectl` (Linux/systemd only) |
| `host:N[h1;h2]` | N сек | хосты через `;` | Проверка SSH-доступности (порт 22) |
| `date:N` | N сек | — | Текущее время хоста с таймзоной |
| `kill_cpu:N[%]` | N сек | максимальный % CPU | Убивает процессы, превысившие лимит |
| `kill_mem:N[%]` | N сек | максимальный % RAM | Убивает процессы, превысившие лимит |
| `dailyreport[HH-MM]` | раз в сутки | время UTC | Slack: "HOST AGENT OK" |

---

## 6. Найденные проблемы

### 6.1 Баг: жёстко заданный `nproc = 1`
**Файл:** `sys_monitor_agent.cpp`, функция `cpu_usage()`

```cpp
nproc = atoi(exec_command("nproc").c_str()); // получаем реальное значение
std::cout << "nproc:::" << "\n";
nproc = 1; // ПЕРЕЗАПИСЫВАЕМ! Всегда 1.
```

Агент **всегда читает только общую строку CPU + cpu0**, игнорируя остальные ядра.
На многоядерных машинах статистика по ядрам неполная.

### 6.2 Баг: `argv[5]` вместо `argv[i]`
**Файл:** `sys_monitor_agent.cpp`, функция `main()`

```cpp
std::cout << "unrecognised key '" << std::string(argv[5]) << "'\n";
```

При неизвестном аргументе на позиции ≠ 5 выводится неверный аргумент.
Если `argc <= 5` — неопределённое поведение (выход за границы массива).

### 6.3 Мёртвый код: блок `CRYPTO_USESSL`
Содержит собственную функцию `main()` и несуществующий заголовок `<boost/crypto.hpp>`.
Никогда не компилируется как часть проекта. Является незавершённым прототипом AES-шифрования.

### 6.4 `split_trim()` не делает trim
Функция идентична `split()` — пробелы вокруг токенов не удаляются, несмотря на название.
Применяется в критических местах (парсинг параметров команды, mount points).

### 6.5 `SYSTEM_ROOT` не применяется к `/proc`
Параметр `--sysroot=PATH` сохраняется в глобальную переменную `SYSTEM_ROOT`, но
функции `get_memory_info()`, `read_CPU_stats()`, `getNetworkStats()`, `get_uptime()`
открывают файлы по абсолютным путям (`/proc/...`) без учёта `SYSTEM_ROOT`.
Поддержка контейнеризации формально заявлена, но не реализована.

### 6.6 Блокирующий sleep в асинхронном контексте
В `send_message_and_next()` после `async_send_to()` следует `sleep(1s)`, блокирующий
поток `io_context`. Аналогично в `cpu_usage()` и `network_usage()` при первом вызове.

### 6.7 Утечка памяти `SlackStatusSender`
Объект создаётся через `new` в `getSlackInfraChannel()`, `delete` не вызывается.
Некритично для демона (один объект за весь срок жизни процесса), но нарушает RAII.

### 6.8 `tick()` — только отладочный вывод
Функция создаёт таймер, который каждые 10 секунд печатает временную метку в stdout.
В продакшне stdout закрыт (daemon mode). Полезна только при `-p` флаге для отладки.

### 6.9 Hardcoded путь к файлу iftop
Имя файла `iftop-result.txt` прописано в коде без возможности конфигурации.
Файл должен существовать в рабочей директории процесса, иначе метрика iftop
вернёт пустой массив без предупреждения.

### 6.10 Отсутствие обработки ошибок открытия `/proc` файлов
`std::ifstream` открывается без проверки `is_open()` в `get_memory_info()`,
`read_CPU_stats()`, `get_uptime()`. На macOS или в ограниченных контейнерах
файлы не существуют — чтение из закрытого потока вернёт нули без диагностики.

---

## 7. Режимы работы

```
# Тестовый режим (вывод в консоль):
./dsp 127.0.0.1 9999 30 --actions='cpu:30,memory:30,disk:30[/]' -p

# Демон-режим:
./dsp 127.0.0.1 9999 30 --actions='cpu:30,memory:30,disk:30[/]' -d

# С аутентификацией и Slack:
./dsp 127.0.0.1 9999 30 \
  --actions='cpu:30,kill_mem:60[20],dailyreport[8-00]' \
  --apikey=MY_KEY \
  --slackpath=/services/XXXX/YYYY/ZZZZ \
  -d
```

---

## 8. Рекомендации (без изменения текущей архитектуры)

1. **`nproc = 1`** — убрать строку для корректного сбора статистики по всем ядрам.
2. **`argv[5]` → `argv[i]`** — однострочное исправление явного бага.
3. **`SYSTEM_ROOT`** — применить в путях `/proc/` для полноценной поддержки `--sysroot`.
4. **`split_trim`** — либо добавить реальный trim, либо переименовать в `split` и убрать дубликат.
5. **Блок `CRYPTO_USESSL`** — вынести в отдельный файл `crypto_prototype.cpp` или удалить.
6. **Проверка `is_open()`** — добавить для всех `/proc` файлов, чтобы macOS выдавал понятную ошибку.

---

## 9. История изменений (git log)

| Коммит | Описание |
|---|---|
| `a8858fd` | Build OK for MAC but without SSL |
| `87d4692` | Something was done about a year ago |
| `c700ffb` | kill_mem & kill_cpu added |
| `6c825fd` | argument apikey added |
| `bef164a` | xtest added |
| `2f0c120` | date metric added |
| `5fa0b49` | iftop flush script added |
| `3a993df` | Big improvement of agent |

---

*Документ создан: 2026-04-24. Версия агента: 1.14.2*
