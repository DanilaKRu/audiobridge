# Windows 11 WASAPI → ASIO Audio Bridge (TerraTec Phase 24 FW)

Высокопроизводительный, низколатентный системный аудиомост для Windows 11 на **C++20** с использованием **RtAudio 6.x**, **Steinberg ASIO SDK** и **Windows Core Audio API**.

---

### 💡 Коротко о главном

* **Для каких аудиокарт:**  
  Для профессиональных FireWire и USB-звуковых карт (в первую очередь **TerraTec Phase 24 FW**, а также *E-MU, M-Audio, TC Electronic, Echo* и др.), у которых нет полноценных современных WDM-драйверов для Windows 10/11 и которые работают только через протокол **ASIO**.

* **Какую проблему решает:**  
  1. **Выводит обычный звук Windows**: даёт возможность слушать YouTube, браузер, игры, фильмы и мессенджеры через профессиональный ASIO-драйвер карты без задержек и заиканий.
  2. **Возвращает регулировку громкости**: системный ползунок Windows в трее и клавиши громкости на клавиатуре начинают реально менять громкость звука в наушниках/колонках (без моста такие карты всегда играют на оглушающих 100%).
  3. **Ограничивает предел громкости - (`--max-volume`)**: позволяет аппаратно ограничить верхний предел громкости (например, не выше 15–25%), сохраняя при этом возможность регулировки громкости.

---

## ⚡ Быстрый старт (для обычных пользователей)

Вам **не нужно** ничего компилировать или устанавливать Visual Studio — в разделе релизов доступен готовый к работе файл.

### Шаг 1. Скачайте готовый файл
Перейдите на страницу **[Релизы (Releases)](https://github.com/DanilaKRu/audiobridge/releases/latest)** и скачайте архив **`audiobridge-v1.2.0-win64.zip`**.

### Шаг 2. Установите виртуальный кабель
1. Скачайте и установите бесплатный драйвер **[VB-Audio Virtual Cable](https://vb-audio.com/Cable/)**.
2. В Windows откройте настройки звука (`Win + R` → `mmsys.cpl`):
   - На вкладке «Воспроизведение» установите **CABLE Input (VB-Audio Virtual Cable)** устройством по умолчанию.
   - В свойствах `CABLE Input` (вкладка «Дополнительно») выберите формат: **24 бит, 48000 Гц (Студийная запись)**.
   - На вкладке «Запись» в свойствах `CABLE Output` также выберите: **24 бит, 48000 Гц**.

### Шаг 3. Разместите программу
Распакуйте файл `audiobridge.exe` из архива в постоянную папку, например:  
`C:\Program Files\TerraTec\audiobridge.exe`

### Шаг 4. Настройте автозапуск в 1 клик
Откройте **PowerShell от имени Администратора** и выполните команду настройки автозапуска через Планировщик заданий (см. блок ниже). Мост начнет работать в фоне и будет автоматически запускаться при включении компьютера и выходе из сна.

---

## ⚙️ Настройка автозапуска (Планировщик заданий / Task Scheduler)

Планировщик заданий Windows — **наилучший способ запуска**, так как он:
- Запускает процесс с **наивысшими правами (Highest privileges)**, необходимыми для прямого доступа к ASIO и приоритета реального времени (Pro Audio MMCSS).
- Бесшовно восстанавливает аудиопоток **при выходе компьютера из спящего режима** (событие Wake from Sleep).
- Работает полностью незаметно в фоне без мигающих черных окон.

### Автоматическая настройка через PowerShell:
Откройте **PowerShell от имени Администратора** и вставьте скрипт:

```powershell
# 1. Задача для панели управления TerraTec Phase Control (инициализация микшера карты)
$ActionCpl = New-ScheduledTaskAction `
    -Execute "C:\Program Files\TerraTec\PHASE 24 FireWire\CPanel\Phase24Cpl.exe" `
    -WorkingDirectory "C:\Program Files\TerraTec\PHASE 24 FireWire\CPanel\"

$TriggerLogon5s = New-ScheduledTaskTrigger -AtLogOn
$TriggerLogon5s.Delay = "PT5S"

Register-ScheduledTask `
    -TaskName "Terratec Phase Control" `
    -Action $ActionCpl `
    -Trigger $TriggerLogon5s `
    -RunLevel Highest `
    -Force

# 2. Задача для аудиомоста TerraTec Phase AudioBridge
$ActionBridge = New-ScheduledTaskAction `
    -Execute "C:\Program Files\TerraTec\audiobridge.exe" `
    -Argument '--capture "CABLE Output" --device "TerraTec" --max-volume 0.15 --headless' `
    -WorkingDirectory "C:\Program Files\TerraTec"

# Триггер 1: При входе пользователя (задержка 10 секунд для инициализации FireWire шины)
$TriggerLogon10s = New-ScheduledTaskTrigger -AtLogOn
$TriggerLogon10s.Delay = "PT10S"

# Триггер 2: При выходе из спящего режима (EventID 1 от Power-Troubleshooter)
$TriggerWake10s = Get-CimClass -Namespace root\Microsoft\Windows\TaskScheduler -ClassName MSFT_TaskEventTrigger
$WakeTrigger = New-CimInstance -CimClass $TriggerWake10s -ClientOnly
$WakeTrigger.Enabled = $true
$WakeTrigger.Delay = "PT10S"
$WakeTrigger.Subscription = '<QueryList><Query Id="0" Path="System"><Select Path="System">*[System[Provider[@Name=''Microsoft-Windows-Power-Troubleshooter''] and EventID=1]]</Select></Query></QueryList>'

Register-ScheduledTask `
    -TaskName "TerraTec Phase AudioBridge" `
    -Action $ActionBridge `
    -Trigger @($TriggerLogon10s, $WakeTrigger) `
    -RunLevel Highest `
    -Force

# Запуск прямо сейчас
Start-ScheduledTask -TaskName "TerraTec Phase AudioBridge"

Write-Host "AudioBridge успешно запущен и настроен в автозагрузке!" -ForegroundColor Green
```

---

## 📋 Параметры командной строки

Если вы хотите запустить программу вручную или изменить параметры задачи:

```text
Usage: audiobridge.exe [options]

Options:
  -c, --capture <pattern>  Имя/подстрока устройства записи Windows (например: "CABLE Output")
  -d, --device <pattern>   Имя/подстрока ASIO драйвера (по умолчанию: "TerraTec" или "Phase")
  -m, --max-volume <gain>  Ограничение максимальной громкости (0.0 .. 1.0, по умолчанию: 1.0)
  -b, --buffer <samples>   Размер буфера ASIO в сэмплах (192, 256, 512; по умолчанию: 256)
  -ch, --channel <firstCh> Индекс первого выходного канала (по умолчанию: 0 = Out 1 / Left, 1 = Out 2 / Right)
  -f, --format <fmt>       Формат вывода: 'int32', 'float32', или 'auto' (по умолчанию: 'auto')
  -s, --silent, --headless Запуск в скрытом фоновом режиме (без окна консоли)
  -l, --list               Показать список всех доступных ASIO-драйверов и выйти
  --list-endpoints         Показать все устройства воспроизведения и записи Windows и выйти
  -h, --help               Показать справку
```

---

## 🚀 Архитектура и принцип работы

```
[ Любые приложения Windows (YouTube, плееры, игры, мессенджеры) ]
                           │
                           ▼
 [ Устройство вывода по умолчанию: "CABLE Input" (VB-Audio Virtual Cable) ]
                           │ (виртуальная петля)
                           ▼
 [ Устройство записи: "CABLE Output" (eCapture) ]
                           │
                           ▼
 ┌──────────────────────────────────────────────────────────────┐
 │                      AudioBridge                             │
 │  1. WASAPI захват (Float32, 48000 Hz, 2ch, без loopback)     │
 │  2. Мониторинг системной громкости через Core Audio API      │
 │  3. Lock-free SPSC Ring Buffer (нулевые аллокации в real-time)│
 │  4. Линейная покадровая интерполяция усиления (Gain Ramp)    │
 │  5. Конвертация Float32 -> Int32 PCM с лимитом (--max-volume)│
 └──────────────────────────────┬───────────────────────────────┘
                                │
                                ▼
 [ Steinberg ASIO SDK: "ASIO for TerraTec FW Series" (Каналы 0 & 1) ]
                                │
                                ▼
 [ TerraTec PHASE 24 FW (Аналоговые выходы 1 / 2, Наушники, Мониторы) ]
```

---

## 🛠️ Сборка из исходников (для разработчиков)

Если вы хотите собрать бинарник самостоятельно из исходного кода:

### Требования:
- Windows 10 / Windows 11 (x64)
- Visual Studio 2022 (компилятор MSVC с поддержкой C++20, Toolset v143 или v145)
- CMake 3.20 или новее

### Инструкция по сборке:

```powershell
# 1. Клонирование репозитория
git clone https://github.com/DanilaKRu/audiobridge.git
cd audiobridge

# 2. Генерация проекта CMake под Visual Studio (x64)
cmake -B build -G "Visual Studio 17 2022" -A x64

# 3. Компиляция в конфигурации Release
cmake --build build --config Release
```

Скомпилированный файл появится по пути:  
`build/Release/audiobridge.exe`

---

## 🔍 Устранение неполадок (FAQ)

### Звук плоский, тонкий, «как из динамика телефона» (нет баса)
- **Причина**: В системе одновременно запущено более одной копии `audiobridge.exe`. Два процесса параллельно отдают сэмплы в звуковую карту с микроскопической разницей во времени, вызывая гребенчатую фильтрацию (Comb Filtering) и фазовое вычитание басовых частот.
- **Решение**: Начиная с версии 1.2.0, в мост встроен системный мьютекс, который физически блокирует запуск второй копии. Если в системе остались старые фоновые процессы, завершите их:
  ```powershell
  Stop-Process -Name audiobridge -Force
  ```

### Звук отсутствует или ASIO устройство не найдено
1. Убедитесь, что FireWire-кабель подключен, карта включена и светодиод на передней панели горит устойчиво.
2. Проверьте список обнаруженных ASIO устройств:
   ```powershell
   & "C:\Program Files\TerraTec\audiobridge.exe" --list
   ```
3. Убедитесь, что звук в приложениях Windows идёт в `CABLE Input`.

### Мост закрывается при закрытии окна PowerShell
- При обычном запуске из консоли процесс завершается вместе с ней. Для независимой фоновой работы используйте задачу Планировщика заданий либо команду:
  ```powershell
  Start-Process "C:\Program Files\TerraTec\audiobridge.exe" -ArgumentList '--capture "CABLE Output" --device "TerraTec" --max-volume 0.15 --headless'
  ```

---

## 📄 Лицензия

Проект распространяется под лицензией MIT. Исходный код библиотек RtAudio и Steinberg ASIO SDK подчиняется их соответствующим лицензиям.
