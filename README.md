# AT28BV64B EEPROM Driver for Raspberry Pi Pico 2

C library for reading and writing the AT28BV64B-20JU-T parallel EEPROM
(8 KiB, 5 V, TSOP-28) from a Raspberry Pi Pico 2 (RP2350) via GPIO bit-banging.

## Prerequisites

### 1. ARM cross-compiler

pico-sdk requires both `arm-none-eabi-gcc` **and** `arm-none-eabi-g++` (version 10+, 13/14 recommended). Even for a pure-C project, the SDK's own internals use C++, so the C++ frontend must be present or CMake will abort with `No CMAKE_CXX_COMPILER could be found`.

**Linux x86_64 / aarch64 (Debian/Ubuntu)**
```bash
sudo apt update
sudo apt install gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi
```

> On Debian/Ubuntu the `gcc-arm-none-eabi` package bundles both `arm-none-eabi-gcc` and `arm-none-eabi-g++`. There is no separate `g++-arm-none-eabi` package.

**macOS**
```bash
brew install gcc-arm-embedded
```

The Homebrew cask installs the complete toolchain (gcc + g++ + binutils).

Verify both compilers are available:
```bash
arm-none-eabi-gcc --version   # arm-none-eabi-gcc 13.x.x ...
arm-none-eabi-g++ --version   # arm-none-eabi-g++ 13.x.x ...
```

If either command returns `command not found`, the toolchain is not in PATH — re-run the install step or check `echo $PATH`.

### 2. Native build toolchain

pico-sdk builds `picotool` as a host-side binary during compilation. This requires a **native** C/C++ compiler on the build machine (separate from the ARM cross-compiler).

**Linux x86_64 / aarch64 (Debian/Ubuntu)**
```bash
sudo apt install build-essential
```

**macOS** — Xcode Command Line Tools already provide `clang++`:
```bash
xcode-select --install
```

### 3. CMake ≥ 3.13

**Linux**
```bash
sudo apt install cmake
```

**macOS**
```bash
brew install cmake
```

### 4. Python 3 (via uv) and Git

pico-sdk requires Python 3 during CMake configuration. Use [uv](https://github.com/astral-sh/uv) to manage the Python version.

**Install uv**

```bash
# Linux / macOS
curl -LsSf https://astral.sh/uv/install.sh | sh
```

Or on macOS via Homebrew:
```bash
brew install uv
```

**Install Python and create a project environment**

```bash
# In the repository root
uv python install 3.12
uv venv --python 3.12
```

This creates a `.venv/` directory. Activate it before building so that `python3` is available in PATH for CMake and pico-sdk scripts:

```bash
# Linux / macOS
source .venv/bin/activate
```

The prompt changes to `(.venv) $`. Keep this shell active for all build steps below.

**Install Git**

```bash
# Linux
sudo apt install git

# macOS — included with Xcode Command Line Tools
xcode-select --install
```

### 4. pico-sdk ≥ 2.0.0

Version 2.0.0+ is required for RP2350 (Pico 2) support.

```bash
git clone https://github.com/raspberrypi/pico-sdk.git --branch 2.1.1 --depth 1
cd pico-sdk
git submodule update --init
```

Then export the path (add to your shell profile to make permanent):

```bash
export PICO_SDK_PATH=/path/to/pico-sdk
```

### 5. (Optional) Serial terminal for test output

**Linux**
```bash
sudo apt install minicom
```

**macOS**
```bash
brew install minicom
```

Or use any other USB serial terminal (screen, picocom, PuTTY on Windows).

---

## Hardware Wiring

| Signal | AT28BV64B pin | Pico 2 GPIO |
|--------|--------------|-------------|
| A0–A12 | Address bus | GPIO 0–12 |
| D0–D7 | Data bus | GPIO 13–20 |
| /CE | Chip Enable | GPIO 21 |
| /OE | Output Enable | GPIO 22 |
| /WE | Write Enable | GPIO 26 |
| VCC | Power | **See note** |
| GND | Ground | GND |

> **GPIO note:** GPIO 23, 24, and 25 are internal on Pico 2 (power management and
> onboard LED) and are not available on the header. /WE is therefore wired to
> GPIO 26 (physical pin 31), which is the next available GPIO after 22.

> **Voltage note:** The AT28BV64B is rated for 5 V. Address and control lines driven
> at 3.3 V satisfy its VIH ≥ 2.2 V requirement and need no level shifting.
> The **data bus (D0–D7) is bidirectional**: when the EEPROM drives it at 5 V VCC,
> the voltage can exceed the Pico GPIO maximum (~3.6 V).
>
> - **Prototype:** power the EEPROM from 3.3 V (out of spec, but commonly works).
> - **Production:** add a 74AHCT245 8-bit bidirectional level shifter on D0–D7,
>   or a 1 kΩ series resistor + Schottky diode clamp to 3.3 V on each data line.

The pin mapping is fully configurable via the `at28bv64b_t` struct — see API usage below.

---

## Building as a Library

The project produces a static library (`libat28bv64b.a`) that another CMake-based
Pico project can link against.

### Step 1 — Activate the Python environment

```bash
source .venv/bin/activate   # required so CMake can find python3
```

### Step 2 — Configure

```bash
mkdir build && cd build
cmake .. -DPICO_BOARD=pico2
```

### Step 3 — Build the library only

```bash
make at28bv64b
```

Build outputs:
```
build/libat28bv64b.a      ← static library
include/at28bv64b.h       ← public header (in the source tree)
```

### Step 3 — Use from another project

**Option A — CMake subdirectory (recommended)**

Add this repository as a subdirectory of your project:

```cmake
# your project's CMakeLists.txt
add_subdirectory(path/to/eeprom)          # exposes the 'at28bv64b' target

add_executable(my_app main.c)
target_link_libraries(my_app at28bv64b pico_stdlib)
```

**Option B — CMake FetchContent**

```cmake
include(FetchContent)
FetchContent_Declare(
    at28bv64b
    GIT_REPOSITORY https://github.com/your-org/eeprom.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(at28bv64b)

add_executable(my_app main.c)
target_link_libraries(my_app at28bv64b pico_stdlib)
```

**Option C — pre-built `.a` file**

```cmake
add_library(at28bv64b STATIC IMPORTED)
set_target_properties(at28bv64b PROPERTIES
    IMPORTED_LOCATION /path/to/build/libat28bv64b.a
    INTERFACE_INCLUDE_DIRECTORIES /path/to/eeprom/include)

target_link_libraries(my_app at28bv64b)
```

### API Quick Reference

```c
#include "at28bv64b.h"

// 1. Declare and initialise a device (use your actual GPIO numbers)
at28bv64b_t dev = {
    .addr_base   = 0,    // A0 on GPIO 0, A1 on GPIO 1, …, A12 on GPIO 12
    .data_base   = 13,   // D0 on GPIO 13, …, D7 on GPIO 20
    .ce_pin      = 21,
    .oe_pin      = 22,
    .we_pin      = 26,
    .sdp_enabled = false,
};

at28bv64b_init(&dev);

// 2. Read
uint8_t byte = at28bv64b_read_byte(&dev, 0x0100);
at28bv64b_read_buf(&dev, 0x0000, buf, 256);

// 3. Write (blocks until complete; returns false on timeout)
at28bv64b_write_byte(&dev, 0x0100, 0xAB);
at28bv64b_write_buf(&dev, 0x0000, data, 256);  // auto page-splits

// 4. Software Data Protection
at28bv64b_sdp_enable(&dev);   // writes then require the 3-byte unlock sequence
at28bv64b_sdp_disable(&dev);  // verify disable sequence against your datasheet

at28bv64b_deinit(&dev);
```

---

## Testing

The `test/` directory contains an on-device test application that runs all tests on
real hardware and prints results over USB CDC (virtual serial port).

### Build the test binary

```bash
source .venv/bin/activate   # if not already active
mkdir build && cd build
cmake .. -DPICO_BOARD=pico2
make test_eeprom
```

Output: `build/test/test_eeprom.uf2`

### Flash to Pico 2

1. Hold the **BOOTSEL** button on the Pico 2 while plugging in USB.
2. It mounts as a USB mass-storage device (`RPI-RP2`).
3. Copy the `.uf2` file to the drive:

```bash
# Linux
cp build/test/test_eeprom.uf2 /media/$USER/RPI-RP2/

# macOS
cp build/test/test_eeprom.uf2 /Volumes/RPI-RP2/
```

The Pico reboots automatically after copying.

### View test output

Open a serial terminal at the USB CDC port (no baud rate setting needed for CDC):

```bash
# Linux — device is usually /dev/ttyACM0
minicom -D /dev/ttyACM0

# macOS — device is usually /dev/tty.usbmodem*
minicom -D /dev/tty.usbmodem*
```

**With EEPROM connected (all tests pass):**

```
=== AT28BV64B Test Suite ===
[PASS] init returns true
[PASS] write_byte 0xA5
[PASS] read_byte 0xA5
[PASS] write_byte 0x00
[PASS] read_byte 0x00
[PASS] write_buf 64B
[PASS] read_buf 64B
[PASS] buf content match
[PASS] write 128B across page boundary
[PASS] read 128B
[PASS] page boundary content match
[PASS] write_page rejects len=0
[PASS] write_page rejects len=65
[PASS] write_page rejects cross-page
[PASS] write_buf rejects overflow
[PASS] write_byte with SDP
[PASS] read_byte after SDP write
[PASS] write_byte after SDP disable
[PASS] read_byte after SDP disable
=== Done ===
```

**Without EEPROM connected (expected partial failure):**

```
=== AT28BV64B Test Suite ===
[PASS] init returns true
[FAIL] write_byte 0xA5
[FAIL] read_byte 0xA5
[FAIL] write_byte 0x00
[PASS] read_byte 0x00
[FAIL] write_buf 64B
[PASS] read_buf 64B
[FAIL] buf content match
[FAIL] write 128B across page boundary
[PASS] read 128B
[FAIL] page boundary content match
[PASS] write_page rejects len=0
[PASS] write_page rejects len=65
[PASS] write_page rejects cross-page
[PASS] write_buf rejects overflow
[FAIL] write_byte with SDP
[FAIL] read_byte after SDP write
[FAIL] write_byte after SDP disable
[FAIL] read_byte after SDP disable
=== Done ===
```

The PASSes without hardware are expected:
- `init returns true` — GPIO initialisation does not require an external device.
- `write_page rejects *`, `write_buf rejects overflow` — argument validation runs before any bus activity.
- `read_buf 64B`, `read 128B` — these functions return `true` to indicate no address overflow; content correctness is checked by the separate `buf content match` / `page boundary content match` tests, which correctly fail.
- `read_byte 0x00` — the pull-down resistors on the data lines happen to read `0x00`, which matches the expected value; this is an unavoidable false positive for a zero-value read with no hardware present.

### Logic analyser (optional)

Attach a logic analyser to `/WE`, `/CE`, and `D0–D7` to verify:
- `/WE` pulse width ≥ 100 ns (tWP)
- Data lines are stable before the rising edge of `/WE`
- Write cycle completes within 10 ms (Data Polling on D7)
