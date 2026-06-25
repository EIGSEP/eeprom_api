#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#if __has_include("pico/stdlib.h")
#include "pico/stdlib.h"
#else
/* Fallback for host-side tooling (clangd, etc.) without pico-sdk. */
typedef unsigned int uint;
#endif

#define AT28BV64B_SIZE       8192u
#define AT28BV64B_PAGE_SIZE  64u
#define AT28BV64B_ADDR_MASK  0x1FFFu

/*
 * Pin configuration for one AT28BV64B device.
 *
 * Default wiring:
 *   addr_base = 0   → A0–A12 on GPIO 0–12
 *   data_base = 13  → D0–D7  on GPIO 13–20
 *   ce_pin    = 21  → /CE
 *   oe_pin    = 22  → /OE
 *   we_pin    = 23  → /WE
 *
 * The EEPROM is rated for 5 V. Address and control lines driven by the
 * Pico at 3.3 V satisfy VIH ≥ 2.2 V and need no level shifting.
 * Data lines (bidirectional) may reach 5 V when the EEPROM drives them;
 * add a 74AHCT245 or Schottky clamp on D0–D7 for a robust design, or
 * power the EEPROM from 3.3 V for prototype use.
 */
typedef struct {
    uint addr_base;   /* GPIO of A0; A0–A12 occupy addr_base … addr_base+12 */
    uint data_base;   /* GPIO of D0; D0–D7  occupy data_base … data_base+7  */
    uint ce_pin;      /* /CE — active low */
    uint oe_pin;      /* /OE — active low */
    uint we_pin;      /* /WE — active low */
    bool sdp_enabled; /* true when Software Data Protection is active on chip */
} at28bv64b_t;

/* Configure GPIO pins. Call once before any other function. */
bool    at28bv64b_init(const at28bv64b_t *dev);

/* Release all GPIO pins back to their default state. */
void    at28bv64b_deinit(const at28bv64b_t *dev);

/* Read a single byte. */
uint8_t at28bv64b_read_byte(const at28bv64b_t *dev, uint16_t addr);

/* Read len bytes into buf starting at addr. Returns false if range exceeds 8 KiB. */
bool    at28bv64b_read_buf(const at28bv64b_t *dev, uint16_t addr,
                            uint8_t *buf, size_t len);

/* Write a single byte. Blocks until write completes (Data Polling).
 * Returns false on timeout (> 12 ms). */
bool    at28bv64b_write_byte(at28bv64b_t *dev, uint16_t addr, uint8_t data);

/* Write up to 64 bytes within one 64-byte aligned page.
 * len must be 1–64 and all bytes must lie within the same page.
 * Returns false on invalid arguments or write timeout. */
bool    at28bv64b_write_page(at28bv64b_t *dev, uint16_t addr,
                              const uint8_t *buf, size_t len);

/* Write len bytes starting at addr, automatically splitting across page
 * boundaries. Returns false if range exceeds 8 KiB or a write times out. */
bool    at28bv64b_write_buf(at28bv64b_t *dev, uint16_t addr,
                             const uint8_t *buf, size_t len);

/* Send the 6-byte Software Data Protection enable command.
 * After this, every write must be preceded by the 3-byte unlock sequence
 * (handled automatically by write_byte / write_page / write_buf).
 * Verify exact command bytes against the AT28BV64B datasheet. */
void    at28bv64b_sdp_enable(at28bv64b_t *dev);

/* Send the 6-byte Software Data Protection disable command.
 * Note: some AT28BV64B revisions require a hardware/voltage method to
 * disable SDP. Verify against the datasheet before relying on this. */
void    at28bv64b_sdp_disable(at28bv64b_t *dev);
