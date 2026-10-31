# Windows 11 WASAPI Loopback → ASIO Audio Bridge (TerraTec FW / Phase)

Высокопроизводительный системный аудиомост для Windows 11 на **C++20** с использованием **RtAudio 6.x**, **Steinberg ASIO SDK** и **Windows Core Audio API**.

---

## Возможности и Архитектура

1. **WASAPI Loopback Capture (Источник)**:
   - Захват звукового потока с дефолтного устройства вывода Windows в режиме Loopback (`AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK`).
   - Поддержка Float32 48000 Hz stereo (с автоматическим интерполяционным ресэмплером, если системный микшер работает на иной частоте, например 44100 Hz).

2. **Синхронизация системной громкости**:
   - Реализация интерфейса `IAudioEndpointVolumeCallback` из Windows Core Audio API.
   - Отслеживание изменений положения системного ползунка громкости (0.0 – 1.0) и мультимедийных клавиш клавиатуры (Volume Up / Down / Mute).
   - Мгновенное масштабирование Float32 сэмплов без задержек.

3. **Lock-Free SPSC Ring Buffer**:
   - Однопоточный кольцевой буфер Single-Producer Single-Consumer без блокировок (на `std::atomic<size_t>` с семантикой `acquire`/`release`).
   - Нулевые аллокации памяти внутри критического аудио-колбэка.
   - Размер буфера: 32768 значений (16384 стерео-сэмпла, ~341 мс при 48 кГц).

4. **ASIO Output (TerraTec FW Series / Phase)**:
   - Автоматический поиск и подключение к ASIO-драйверу с именем, содержащим `"TerraTec"` или `"Phase"` (например, `ASIO for TerraTec FW Series`).
   - Настраиваемый размер буфера (256 или 512 сэмплов) через CLI аргументы.
   - Режим реального времени (`RTAUDIO_SCHEDULE_REALTIME | RTAUDIO_MINIMIZE_LATENCY`).

5. **Отказоустойчивость FireWire (Auto-Reconnect)**:
   - Ловит сбросы шины IEEE 1394 (FireWire), отключение питания карты и исключения RtAudio.
   - При сбое поток корректно освобождается, программа ожидает 2 секунды и автоматически переподключается к карте, как только она снова станет доступна.

---

## Структура Проекта

```text
├── CMakeLists.txt              # Скрипт сборки CMake с настройкой WASAPI, ASIO и системных библиотек
├── main.cpp                    # Полная реализация аудиомоста, ring buffer, volume sync, ASIO
├── extern/
│   └── rtaudio/                # Исходники RtAudio 6.x со встроенным Steinberg ASIO SDK
└── build/
    └── Release/
        └── audiobridge.exe     # Собранный бинарный файл
```

---

## Сборка из терминала

Сборка осуществляется через **Visual Studio Build Tools / IDE** и **CMake**:

```powershell
# 1. Генерация проекта (Visual Studio 2022/2026 x64, Toolset v145)
& "F:\_dev\vscode\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -B build -G "Visual Studio 17 2022" -A x64 -T v145

# 2. Компиляция в Release-режиме
& "F:\_dev\vscode\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

*(Если путь к `cmake.exe` уже находится в вашем системном `PATH`, достаточно просто выполнить `cmake -B build` и `cmake --build build --config Release`).*

---

## Запуск и параметры CLI

### 1. Просмотр доступных устройств ASIO и Windows Playback
```powershell
# Показать ASIO-устройства
.\build\Release\audiobridge.exe --list

# Показать все активные устройства вывода Windows (WASAPI endpoints)
.\build\Release\audiobridge.exe --list-endpoints
```

### 2. Запуск в стандартном режиме (автопоиск TerraTec / Phase, буфер 256)
```powershell
.\build\Release\audiobridge.exe
```

### 3. Запуск с выбором конкретного устройства захвата Windows, ASIO и лимитом громкости
```powershell
# Захват с CABLE Output, вывод в TerraTec с лимитом громкости 25%
.\build\Release\audiobridge.exe --capture "CABLE Output" --device "TerraTec" --max-volume 0.25

# Запуск в фоновом режиме (без окна консоли)
.\build\Release\audiobridge.exe --capture "CABLE Output" --device "TerraTec" --max-volume 0.25 --headless
```

### 4. Справка по всем опциям
```powershell
.\build\Release\audiobridge.exe --help
```
