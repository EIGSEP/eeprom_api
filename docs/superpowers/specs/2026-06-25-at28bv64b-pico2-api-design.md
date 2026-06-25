# AT28BV64B EEPROM C API for Raspberry Pi Pico 2 — Design Spec

**Date:** 2026-06-25  
**Status:** Approved

## Context

This library provides a C API for reading and writing the AT28BV64B-20JU-T parallel EEPROM
(8K × 8, 5V) from a Raspberry Pi Pico 2 (RP2350, 3.3V). The goal is a clean, complete
reference implementation for learning and testing, covering all key chip features.

## Hardware Interface

### Voltage Level Consideration

The AT28BV64B is rated for 5V VCC. The Pico 2 operates at 3.3V.

- **Address lines (A0–A12) and control lines (/CE, /OE, /WE):** Pico drives them as outputs.
  Pico output = 3.3V; EEPROM VIH min = 2.2V. **No level shifting needed** — 3.3V satisfies
  the input threshold.
- **Data lines (D0–D7):** Bidirectional. When the EEPROM drives the bus (reads), it can
  output up to VCC (5V), which exceeds the Pico GPIO absolute maximum (~3.63V).

**Recommended mitigations (choose one):**
1. Power the EEPROM from 3.3V (out-of-spec but commonly works for prototyping).
2. Add a 74AHCT245 8-bit bidirectional level shifter on D0–D7 for a production-quality design.
3. Series resistor (1kΩ) + Schottky diode clamp to 3.3V on each data line.

### Default GPIO Pin Mapping

| Signal | EEPROM Pin | Pico 2 GPIO |
|--------|-----------|-------------|
| A0–A12 | Address bus | GPIO 0–12 (consecutive) |
| D0–D7  | Data bus    | GPIO 13–20 (consecutive) |
| /CE    | Chip Enable | GPIO 21 (active low) |
| /OE    | Output Enable | GPIO 22 (active low) |
| /WE    | Write Enable | GPIO 23 (active low) |

GPIO 24–28 remain free for other use.

The pin mapping is fully configurable via the `at28bv64b_t` struct, not hardcoded.

## API Design

### Configuration Structure

```c
typedef struct {
    uint addr_base;   // GPIO number of A0; A0–A12 occupy addr_base to addr_base+12
    uint data_base;   // GPIO number of D0; D0–D7 occupy data_base to data_base+7
    uint ce_pin;      // /CE, active low
    uint oe_pin;      // /OE, active low
    uint we_pin;      // /WE, active low
    bool sdp_enabled; // tracks whether SDP is currently active on the device
} at28bv64b_t;
```

### Public API (`include/at28bv64b.h`)

```c
// Initialization
bool at28bv64b_init(const at28bv64b_t *dev);
void at28bv64b_deinit(const at28bv64b_t *dev);

// Read
uint8_t at28bv64b_read_byte(const at28bv64b_t *dev, uint16_t addr);
bool    at28bv64b_read_buf(const at28bv64b_t *dev, uint16_t addr,
                           uint8_t *buf, size_t len);

// Write (block until complete, return false on timeout)
// dev is non-const: write functions may update sdp_enabled state
bool at28bv64b_write_byte(at28bv64b_t *dev, uint16_t addr, uint8_t data);
bool at28bv64b_write_page(at28bv64b_t *dev, uint16_t addr,
                          const uint8_t *buf, size_t len);  // len ≤ 64, must not cross page
bool at28bv64b_write_buf(at28bv64b_t *dev, uint16_t addr,
                         const uint8_t *buf, size_t len);   // splits into pages automatically

// Software Data Protection (also updates dev->sdp_enabled)
void at28bv64b_sdp_enable(at28bv64b_t *dev);
void at28bv64b_sdp_disable(at28bv64b_t *dev);
```

### Address Constraints

- Valid address range: `0x0000`–`0x1FFF` (13-bit, 8192 bytes)
- Page size: 64 bytes; page boundary at `addr & ~0x3F`
- SDP command addresses: `0x5555` and `0x2AAA` fold to `0x1555` and `0x0AAA` in 13-bit space

## Implementation Details

### Read Cycle

1. Assert /CE (low), /OE (low), /WE (high)
2. Drive address on A0–A12
3. Data GPIOs configured as inputs
4. Wait tACC (≥ 200ns for -20 grade; use `busy_wait_us(1)`)
5. Sample D0–D7
6. Deassert /CE, /OE

### Byte Write Cycle

1. Assert /CE (low), /OE (high), /WE (high)
2. Drive address on A0–A12
3. Set data GPIOs to output, drive D0–D7
4. Assert /WE (low) — hold ≥ 100ns (tWP)
5. Deassert /WE (high)
6. Deassert /CE (high)
7. Switch data GPIOs back to input
8. **Data Polling**: read D7 until it matches bit 7 of written data (timeout = 12ms → return false)

### Page Write Cycle (up to 64 bytes, same page)

1. Assert /CE, deassert /OE
2. For each byte in sequence:
   a. Drive address and data
   b. Pulse /WE (low→high, ≥ tWP each pulse)
   c. Next byte must follow within tBLC (150µs)
3. After last byte, deassert /CE → triggers internal write
4. Data Polling on last byte written

### SDP Unlock Sequence (precedes each write when SDP is enabled)

```
write 0xAA → addr 0x1555
write 0x55 → addr 0x0AAA
write 0xA0 → addr 0x1555
```
Then immediately write data (entire sequence + write must complete within tBLC = 150µs).

`at28bv64b_sdp_enable` and `at28bv64b_sdp_disable` use alternate command sequences per
datasheet. After enable/disable, all subsequent write functions prepend the unlock bytes
automatically (tracked by a `bool sdp_enabled` field in the struct).

### Write Completion Detection

Data Polling (D7 method): while EEPROM write in progress, D7 outputs the complement of the
programmed D7 bit. Poll with `/CE` and `/OE` asserted until D7 matches expected value.
Timeout after 12ms (1.2× the 10ms tWC max) and return `false`.

### `write_buf` Page Splitting

`at28bv64b_write_buf` splits the input buffer into page-aligned chunks:
- First chunk: from `addr` to end of its page
- Middle chunks: full 64-byte pages
- Last chunk: remainder

Each chunk calls `at28bv64b_write_page` and waits for completion before the next.

## File Structure

```
eeprom/
├── CMakeLists.txt
├── include/
│   └── at28bv64b.h
└── src/
    └── at28bv64b.c
```

`CMakeLists.txt` exposes an `at28bv64b` interface library target linking against
`pico_stdlib` and `hardware_gpio`.

## Verification

1. **Read after write**: Write a known pattern to the full 8K range, read back and compare byte-by-byte.
2. **Page boundary test**: Write 128 bytes spanning two consecutive pages, verify no corruption.
3. **Data Polling timeout**: Pull /CE high prematurely (simulate), confirm `write_byte` returns `false`.
4. **SDP round-trip**: Enable SDP, attempt a write (should succeed via unlock sequence), disable SDP.
5. **Hardware**: Use a logic analyzer on D0–D7 and /WE to verify write pulse timing meets tWP ≥ 100ns.
