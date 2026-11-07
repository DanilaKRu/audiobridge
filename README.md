# Windows 11 WASAPI → ASIO Audio Bridge (TerraTec Phase 24 FW)

Высокопроизводительный, низколатентный системный аудиомост для Windows 11 на **C++20** с использованием **RtAudio 6.x**, **Steinberg ASIO SDK** и **Windows Core Audio API**.

Проект решает фундаментальную проблему профессиональных FireWire/USB звуковых карт с ASIO-драйверами (в частности, **TerraTec Phase 24 FW**): отсутствие поддержки разделяемого WDM-звука в современных версиях Windows, невозможность регулировки громкости с клавиатуры и ползунка Windows, а также задержки и нестабильность стандартных системных драйверов.

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

## 🌟 Ключевые возможности

1. **Захват через виртуальный кабель (eCapture)**:
   - Захват звука с виртуального аудиокабеля (`CABLE Output`) в стандартном режиме записи без флага loopback.
   - Полное отсутствие проблемы "Silent Packets" (засыпания потока при тишине/паузе видео).

2. **Синхронизация системной громкости и мультимедиа-клавиш**:
   - Перехват изменений мастер-громкости Windows и клавиш клавиатуры (Volume Up / Down / Mute) через `IAudioEndpointVolumeCallback`.
   - Плавная линейная интерполяция коэффициента громкости (Gain Ramp) между буферами исключает щелчки и артефакты («zipper noise»).
   - Аппаратное ограничение максимальной громкости через аргумент `--max-volume` (защита ушей и акустики при высокой мощности ЦАП).

3. **Lock-Free SPSC кольцевой буфер**:
   - Полностью потокобезопасная передача сэмплов между WASAPI-потоком и ASIO-потоком на `std::atomic` с семантикой acquire/release.
   - Никаких блокировок (mutex), аллокаций динамической памяти и вызовов I/O внутри real-time аудио-колбэка.

4. **Защита от запуска дубликатов (Single-Instance Mutex)**:
   - Использование системного мьютекса `Global\TerraTecAudioBridgeSingleInstance`.
   - Исключает одновременную работу двух процессов моста в фоне при выходе из спящего режима, полностью предотвращая фазовые искажения (гребенчатую фильтрацию).

5. **Отказоустойчивость FireWire (Auto-Reconnect)**:
   - При сбросе шины IEEE 1394, отключении питания карты или кратковременной потере пакетов программа не аварийно завершается, а плавно ожидает восстановления связи с устройством и восстанавливает ASIO-сессию.

6. **Фоновый режим (`--headless`)**:
   - Полное скрытие консольного окна для бесшумной фоновой работы через автозапуск.

---

## 🛠️ Сборка проекта

### Требования:
- Windows 10 / Windows 11 (x64)
- Visual Studio 2022 (компилятор MSVC с поддержкой C++20, Toolset v143 или v145)
- CMake 3.20 или новее

### Инструкция по сборке из консоли:

```powershell
# 1. Клонирование репозитория
git clone https://github.com/DanilaKRu/audiobridge.git
cd audiobridge

# 2. Генерация проекта CMake под Visual Studio (x64)
cmake -B build -G "Visual Studio 17 2022" -A x64

# 3. Компиляция в конфигурации Release
cmake --build build --config Release
```

Скомпилированный файл будет находиться по пути:
`build/Release/audiobridge.exe`

---

## 📋 Параметры командной строки

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

## ⚙️ Настройка системы и автозапуска

Для обеспечения стабильного звука в режиме 24/7 без необходимости запускать мост вручную настройте систему по следующей схеме:

### Шаг 1. Установка и настройка VB-Audio Virtual Cable
1. Скачайте и установите бесплатный драйвер [VB-Audio Virtual Cable](https://vb-audio.com/Cable/).
2. В Windows перейдите в **Параметры → Система → Звук** (или выполните `mmsys.cpl`):
   - Установите **"CABLE Input (VB-Audio Virtual Cable)"** в качестве **устройства воспроизведения по умолчанию** (Default Playback Device).
   - В свойствах устройства `CABLE Input` (вкладка «Дополнительно») задайте формат: **24 бит, 48000 Гц (Студийная запись)**.
   - В свойствах устройства записи `CABLE Output` также задайте формат: **24 бит, 48000 Гц**.

### Шаг 2. Размещение бинарных файлов
Создайте рабочую папку программы (например, `C:\Program Files\TerraTec`) и скопируйте туда собранный `audiobridge.exe`.

---

### Шаг 3. Настройка автозапуска через Планировщик заданий (Task Scheduler)

Планировщик заданий Windows — **наилучший способ автозапуска**, поскольку он:
- Запускает процесс с **наивысшими правами (Run with highest privileges)**, что необходимо для приоритета реального времени (Pro Audio MMCSS) и прямого доступа к ASIO драйверу.
- Корректно восстанавливает работу моста **при выходе из спящего режима** (событие Wake from Sleep).
- Не оставляет видимых окон терминала.

Настраиваются **две задачи**:
1. **Панель управления картой (Phase24Cpl.exe)** — инициализирует FireWire-микшер при входе в систему.
2. **Аудиомост (audiobridge.exe)** — связывает поток Windows с ASIO при входе и при пробуждении от сна.

#### Автоматическая настройка через PowerShell (от имени Администратора)

Откройте **PowerShell от имени Администратора** и выполните следующий скрипт:

```powershell
# 1. Задача для панели управления TerraTec Phase Control
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

# Триггер 2: При пробуждении от спящего режима (EventID 1 от Power-Troubleshooter)
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

Write-Host "Задачи автозапуска успешно настроены в Планировщике заданий!" -ForegroundColor Green
```

---

## 🔍 Устранение неполадок (FAQ)

### Звук плоский, тонкий, «как из динамика телефона» (нет баса)
- **Причина**: В системе одновременно запущено более одной копии `audiobridge.exe` (например, вручную и через автозапуск). Два процесса параллельно отдают сэмплы в драйвер с микроскопической разницей во времени, вызывая гребенчатую фильтрацию (Comb Filtering) и взаимное вычитание низких частот.
- **Решение**: Начиная с версии 1.2.0, в мост встроен **системный мьютекс**, который физически запрещает запуск второй копии. Если в системе остались старые фоновые процессы, завершите их через диспетчер задач или выполните в PowerShell:
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
- При запуске из консоли напрямую процесс привязан к терминалу. Для независимой работы используйте запуск через `Start-ScheduledTask` либо команду `Start-Process`:
  ```powershell
  Start-Process "C:\Program Files\TerraTec\audiobridge.exe" -ArgumentList '--capture "CABLE Output" --device "TerraTec" --max-volume 0.15 --headless'
  ```

---

## 📄 Лицензия

Проект распространяется под лицензией MIT. Исходный код библиотек RtAudio и Steinberg ASIO SDK подчиняется их соответствующим лицензиям.
