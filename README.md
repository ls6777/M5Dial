# M5Dial: Heat Troller

[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20Me%20a%20Coffee-ffdd00?logo=buy-me-a-coffee&logoColor=black)](https://buymeacoffee.com/ls6777)

Heat Troller for the [M5Stack Dial](https://docs.m5stack.com/en/core/M5Dial), built with ESP-IDF on
[PrintfTask](https://github.com/ls6777/PrintfTask) and [TaskMonitor](https://github.com/ls6777/TaskMonitor).

## Status
This is the project setup. The application is still a placeholder:

- `HelloTask` prints a message once a second through the printf buffer.
- `DisplayTask` shows "Heat Troller" and the CPU usage, updated every 500 ms, on the round LCD.

Both check in with the task monitor when asked. The Heat Troller features come next.

## Hardware
The M5Stack Dial is an M5StampS3 (ESP32-S3, 8 MB flash, no PSRAM) with a 1.28" 240x240 round LCD, a rotary encoder
and a push button. The pins below are the ones M5Stack's own libraries use (M5GFX, M5Unified and M5Dial).

| Function | Pins | Used by |
|---|---|---|
| LCD, GC9A01 on SPI2 | MOSI 5, SCLK 6, DC 4, CS 7, RST 8 (no MISO) | `DisplayPort` |
| LCD backlight | 9 (high = on) | `DisplayPort` |
| Power hold | 46 (high keeps the power on when running from battery) | `main.cpp` |
| Rotary encoder | A 41, B 40 | not used yet |
| Dial button (BtnA) | 42 | not used yet |
| StampS3 button (BtnB) | 0 (active low) | not used yet |
| Buzzer | 3 | not used yet |
| Internal I2C: touch (FT3267, address 0x38, INT 14), RTC | SDA 11, SCL 12 | not used yet |
| Port A (I2C) | SDA 13, SCL 15 | not used yet |
| Port B | 1, 2 | not used yet |

The power hold pin matters on battery: the WAKE button powers the board only while it's pressed, so `app_main()`
drives pin 46 high first. On USB power the board stays on anyway.

## Project layout
```
CMakeLists.txt                ESP-IDF project
sdkconfig.defaults            Settings the project relies on (target, flash, console, tick rate, task watchdog, run-time stats)
components/
  common/                     Shared by all tasks
    Inc/TargetPort.hpp        Port layer: queues, task IDs, time, watchdog, task statistics, CPU time
    Src/TargetPort.cpp        Port layer implementation for FreeRTOS on ESP-IDF
    Inc/Message.hpp           Message posted to a task's queue
    Inc/Utils.hpp             Small shared helpers
    Inc/CpuUsage.hpp          Generic CPU usage calculation from idle and total time counters
  printf_task/                From PrintfTask
    Inc/PrintfTask.hpp        The printf task: sends buffered output to the real stdout
    Inc/PrintUtils.hpp        Generic stdout API that owns the printf buffer, with an optional output mirror
    Inc/PrintBuffer.hpp       Generic circular buffer (no RTOS/target code)
    Inc/StdOutPort.hpp        Port layer: stdout redirect and real stdout write
    Src/StdOutPort.cpp        Port layer implementation for ESP-IDF (Picolibc and Newlib)
  task_monitor/               From TaskMonitor
    Inc/TaskMonitor.hpp       The task monitor task (message handling, watchdog, violation action)
    Inc/CheckinTracker.hpp    Generic monitor logic: checkin timing, requests, violations (no RTOS/target code)
  display/                    Text on the LCD
    Inc/Display.hpp           Generic display API: clear the screen, draw text
    Inc/TextRenderer.hpp      Generic text to RGB565 pixel rendering, and colors (no RTOS/target code)
    Inc/TextConsole.hpp       Generic scrolling text console: lines, wrapping, scrolling, changed cells
    Inc/Font5x7.hpp           Generic 5x7 bitmap font for printable ASCII
    Inc/DisplayPort.hpp       Port layer: panel init, backlight, fill and draw pixel rectangles
    Src/DisplayPort.cpp       Port layer implementation for the M5Stack Dial LCD (GC9A01 over SPI)
main/
  main.cpp                    Holds the power on, sets up stdout buffering, the display and the task monitor, then creates the tasks
  Inc|Src/HelloTask.*         Hello world: prints every second, 5 s task monitor timeout
  Inc|Src/DisplayTask.*       Owns the LCD: title and CPU usage every 500 ms, 2 s task monitor timeout
test/                         Host unit tests (Unity)
  fakes/                      Host OS port layer (TargetPort) and test helpers
```

## Tasks
| Task | Priority | Purpose |
|---|---|---|
| TaskMonitor | Highest | Checks every registered task and kicks the watchdog |
| HelloTask | 5 | Prints "Hello world!" every second. Every 10 seconds it also prints the FreeRTOS task list and run-time stats, and the printf buffer high water mark |
| DisplayTask | 3 | The only task that draws on the LCD: the title, and the CPU usage every 500 ms |
| PrintfTask | 1 (lowest) | Sends buffered printf output to the console every 10 ms |

`app_main()` holds the power on, then redirects stdout into the printf buffer before creating any task, so all task
output is buffered. It also initializes the display and creates the task monitor's queue first, so tasks can draw and
register however they get scheduled.

## Display
The LCD is a 240x240 GC9A01 on SPI. `DisplayPort` sends the same initialization sequence as M5Stack's M5GFX library
(`Panel_GC9A01`), with the settings M5GFX uses for the Dial: rotation 0, BGR color order, 16 bits per pixel and
color inversion on. The SPI clock is 40 MHz (M5GFX uses 80 MHz; `SPI_CLOCK_HZ` in `DisplayPort.cpp`).

Coordinates are the panel's: (0, 0) at the top left, x to the right, y down. The screen is round, so only pixels
within 120 pixels of the center (120, 120) are visible. Keep content inside that circle; the corners of the
240 x 240 memory are hidden.

Draw text with `Display::DrawString()`:

```cpp
Display::DrawString(x, y, "Heat Troller", Color::WHITE, Color::BLACK, 2);
```

The scale multiplies the 6x8 pixel character cell (1 to `Display::MAX_SCALE`). `Display::TextWidth()` gives a
string's width, for centering. `Display` is not thread safe: only `DisplayTask` draws.

## Adding a task
Follow `HelloTask`:

1. Create a message queue in `Initialize()` and post `Message::INITIALIZE` to it.
2. On `INITIALIZE`, register with the task monitor: `TaskMonitor::Register(CHECKIN_TIMEOUT, &TaskCheckin);`
3. `TaskCheckin()` posts `Message::TASK_CHECKIN` to the task's own queue. On that message, call `TaskMonitor::TaskCheckin()`.
4. Use `printf` as usual. The output goes through the printf buffer.
5. Don't draw on the LCD directly: only `DisplayTask` does, since `Display` isn't thread safe. Send what to show to `DisplayTask` instead.

Use `PrintUtils::GetHighWaterMark()` to size the printf buffer (`PrintUtils::BUFFER_SIZE`, 15000 bytes by default).

## Building
Tested with ESP-IDF v6.1. From an ESP-IDF terminal:

```
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

The Dial connects over the ESP32-S3's own USB Serial/JTAG port (there is no USB to UART bridge), which is also the
console. Use the COM port Windows assigns to it.

Expected output:
```
Task Monitor Initialized
HelloTask Initialized
DisplayTask Initialized
Hello world! 1
Hello world! 2
...
Hello world! 10

TASK INFO**********************************************
Name            State  Priority  Stack  Num
...
TASK STATS INFO****************************************
Name            Abs Time (us)  % Time
...
Printf buffer high water mark: ...
```

`sdkconfig.defaults` holds the settings the project relies on, so they survive `idf.py set-target` and deleting `sdkconfig`:

- `CONFIG_IDF_TARGET="esp32s3"` and `CONFIG_ESPTOOLPY_FLASHSIZE_8MB`: the StampS3
- `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`: the console on the USB port
- `CONFIG_FREERTOS_HZ=1000`: 1 ms tick, for 10 ms task monitor evaluation and clean wrap-around of `GetTimeMs()`
- `CONFIG_ESP_TASK_WDT_PANIC=y`: the task watchdog resets the device. Without it, a task monitor violation only prints a warning.
- `CONFIG_FREERTOS_USE_TRACE_FACILITY`, `..._USE_STATS_FORMATTING_FUNCTIONS`, `..._GENERATE_RUN_TIME_STATS`: task statistics (`TargetPort::GetTaskList()`, `TargetPort::GetRunTimeStats()`)

The project includes a shared VS Code setup (`.vscode/c_cpp_properties.json` and `extensions.json`) for the ESP-IDF
and C/C++ extensions. Code navigation uses `build/compile_commands.json`, so build once first.

## Unit tests
All the target independent code has host unit tests in `test/`, using the Unity framework that ships with ESP-IDF.
They need `IDF_PATH` set and a host gcc, CMake and Ninja (e.g. MinGW from WinLibs on Windows):

```
cmake -S test -B test/build -G Ninja
cmake --build test/build
ctest --test-dir test/build --output-on-failure
```

| Test | Code under test | Fakes |
|---|---|---|
| `test_PrintBuffer` | `PrintBuffer` | none |
| `test_PrintUtils` | `PrintUtils`, including the output mirror | `StdOutPort` |
| `test_PrintfTask` | `PrintfTask`, with the real `PrintUtils` | `StdOutPort`, OS |
| `test_CheckinTracker` | `CheckinTracker` | none |
| `test_TaskMonitor` | `TaskMonitor` task: requests, violations, watchdog, statistics | OS |
| `test_TextRenderer` | `TextRenderer`, `Font5x7` (including glyph shapes as ASCII art) | none |
| `test_TextConsole` | `TextConsole`: lines, wrapping, scrolling, changed cells, control characters | none |
| `test_Display` | `Display`: positions, scaling, clipping | `DisplayPort` |
| `test_CpuUsage` | `CpuUsage`: rounding, limits, counter wrap | none |
| `test_HelloTask` | `HelloTask`, with the real `TaskMonitor` and `PrintUtils` | `StdOutPort`, OS |
| `test_DisplayTask` | `DisplayTask`: title, CPU usage, layout inside the round screen, 500 ms schedule, with the real `TaskMonitor`, `Display` and `CpuUsage` | `DisplayPort`, OS (with a simulated CPU load) |

The tasks run their real `Run()` loops on a host implementation of the OS port layer
([test/fakes/TargetPort.hpp](test/fakes/TargetPort.hpp)): in-memory queues and a clock that advances when a task
waits. Since `Run()` never returns, the fake ends it by throwing when a call budget runs out or when the task would
block forever.

The port layer implementations (`TargetPort.cpp`, `StdOutPort.cpp`, `DisplayPort.cpp`) and `main.cpp` call ESP-IDF
and FreeRTOS directly, so they are verified on the board instead.

## Targets
The application runs on the M5Stack Dial (ESP32-S3) with ESP-IDF and FreeRTOS. Porting to another target or RTOS
only means changing the port layers: `TargetPort.hpp/.cpp`, `StdOutPort.cpp`, `DisplayPort.cpp`, and the power hold
in `main.cpp`.
