/*
 * program_paths.c  —  Burn the RF-switch path lookup table into an
 *                     AT28BV64B-20JU-T EEPROM using the eeprom_api library.
 *
 * BOARD CONTEXT
 * -------------
 * Two EEPROMs feed three ADGM1004 4PST switches. At runtime, 5 external
 * address lines (A0..A4) select one of 32 entries; the stored byte's bits
 * drive switch-control I/O 0..7 across a D-sub. This tool writes that table.
 *
 * BIT CONVENTION (locked with the user)
 * -------------------------------------
 *   I/O numbering : 0-based, I/O0..I/O7
 *   Bit weight    : I/O0 = bit0 = LSB   →   byte = sum(1 << io)
 *   This MUST match (a) the D-sub pin order and (b) the pull-resistor
 *   default on the switch board. Change ONLY io_bit() if the mapping differs;
 *   every path byte then regenerates correctly.
 *
 * EEPROM I/O MAP (from the user's board description)
 *   EEPROM1:
 *     I/O0 = SwA IN1  (RF1 Spare 1)
 *     I/O1 = SwA IN2  (RF2 Onboard noise diode  — RF path leg)
 *     I/O2 = SwA IN3  (RF3 Amb/Hot Load)
 *     I/O3 = SwA IN4  (RF4 Feed)
 *     I/O4 = SwB IN1  (RF1 VNA Cal Load)
 *     I/O5 = SwB IN2  (RF2 Spare 2)
 *     I/O6 = SwB IN3  (RF3 VNA Cal Open)
 *     I/O7 = SwB IN4  (RF4 VNA Cal Short)
 *   EEPROM2:
 *     I/O0 = SwC IN1  (RF1 RFSWA Common)
 *     I/O1 = SwC IN2  (RF2 VNA)
 *     I/O2 = SwC IN3  (RF3 LNA)
 *     I/O3 = SwC IN4  (RF4 RFSWB Common)
 *     I/O4 = Noise diode bias ON(1)/OFF(0)
 *     I/O5..7 = grounded (always 0)
 *
 * DEFAULT / FAIL-SAFE
 *   Address 0x00 holds LNA -> Feed so that if the 5 select lines are pulled
 *   low on comms loss, the presented byte is the safe default. NOTE: this only
 *   protects the "select lines low" case. Protection against the Pico/cable
 *   being unplugged (D-sub goes high-Z) MUST come from pull resistors on the
 *   switch board that bias each I/O line to the LNA->Feed pattern:
 *       EEPROM1 default byte = 0x08   (only I/O3 Feed high)
 *       EEPROM2 default byte = 0x05   (I/O0 RFSWA + I/O2 LNA high)
 *   Firmware cannot create that guarantee; it is a hardware requirement.
 *
 * USAGE
 *   Flash program_paths.uf2, open USB serial (115200), follow the prompt to
 *   pick which chip is currently seated, program, and read-back verify.
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "at28bv64b.h"

/* ── Device pin config: identical to the library's documented default ──── */
static at28bv64b_t dev = {
    .addr_pins   = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12},
    .data_pins   = {13, 14, 15, 16, 17, 18, 19, 20},
    .ce_pin      = 21,
    .oe_pin      = 22,
    .we_pin      = 26,
    .sdp_enabled = false,
};

/* ── Bit convention ────────────────────────────────────────────────────
 * The ONE place that defines I/O-number -> bit position. Edit here only.
 * IO(n) is a compile-time constant (usable in static initializers below);
 * io_bit() is the runtime equivalent for use inside functions.            */
#define IO(n)  ((uint8_t)(1u << (n)))
static inline uint8_t io_bit(unsigned io_number) { return IO(io_number); }

/* Named I/O helpers so path definitions read like the switch table, not hex.
 * EEPROM1 (Switch A + Switch B) */
#define E1_A_SPARE1     IO(0)
#define E1_A_NOISELEG   IO(1)   /* RF leg to the noise diode on Switch A */
#define E1_A_AMBHOT     IO(2)
#define E1_A_FEED       IO(3)
#define E1_B_CALLOAD    IO(4)
#define E1_B_SPARE2     IO(5)
#define E1_B_CALOPEN    IO(6)
#define E1_B_CALSHORT   IO(7)

/* EEPROM2 (Switch C + noise-diode bias) */
#define E2_C_RFSWA      IO(0)   /* SwC -> Switch A branch  */
#define E2_C_VNA        IO(1)
#define E2_C_LNA        IO(2)
#define E2_C_RFSWB      IO(3)   /* SwC -> Switch B branch  */
#define E2_DIODE_ON     IO(4)   /* noise diode bias enable */

/* ── Path table ────────────────────────────────────────────────────────
 * Index = runtime address presented by the 5 select lines (A0..A4).
 * Address 0 is the LNA->Feed default. */
typedef struct { const char *name; uint8_t e1; uint8_t e2; } path_t;

static const path_t PATHS[] = {
 /* 0x00 */ {"LNA -> Feed  (DEFAULT)",   E1_A_FEED,     E2_C_RFSWA | E2_C_LNA},
 /* 0x01 */ {"VNA -> VNA Cal Load",      E1_B_CALLOAD,  E2_C_VNA   | E2_C_RFSWB},
 /* 0x02 */ {"VNA -> VNA Cal Open",      E1_B_CALOPEN,  E2_C_VNA   | E2_C_RFSWB},
 /* 0x03 */ {"VNA -> VNA Cal Short",     E1_B_CALSHORT, E2_C_VNA   | E2_C_RFSWB},
 /* 0x04 */ {"VNA -> Feed",              E1_A_FEED,     E2_C_VNA   | E2_C_RFSWA},
 /* 0x05 */ {"VNA -> Noise Diode ON",    E1_A_NOISELEG, E2_C_VNA   | E2_C_RFSWA | E2_DIODE_ON},
 /* 0x06 */ {"VNA -> Noise Diode OFF",   E1_A_NOISELEG, E2_C_VNA   | E2_C_RFSWA},
 /* 0x07 */ {"VNA -> LNA",               0,             E2_C_VNA   | E2_C_LNA},
 /* 0x08 */ {"VNA -> Amb/Hot Load",      E1_A_AMBHOT,   E2_C_VNA   | E2_C_RFSWA},
 /* 0x09 */ {"VNA -> Spare 1",           E1_A_SPARE1,   E2_C_VNA   | E2_C_RFSWA},
 /* 0x0A */ {"VNA -> Spare 2",           E1_B_SPARE2,   E2_C_VNA   | E2_C_RFSWB},
 /* 0x0B */ {"LNA -> Noise Diode ON",    E1_A_NOISELEG, E2_C_RFSWA | E2_C_LNA | E2_DIODE_ON},
 /* 0x0C */ {"LNA -> Noise Diode OFF",   E1_A_NOISELEG, E2_C_RFSWA | E2_C_LNA},
 /* 0x0D */ {"LNA -> Amb/Hot Load",      E1_A_AMBHOT,   E2_C_RFSWA | E2_C_LNA},
 /* 0x0E */ {"LNA -> Spare 1",           E1_A_SPARE1,   E2_C_RFSWA | E2_C_LNA},
 /* 0x0F */ {"LNA -> Spare 2",           E1_B_SPARE2,   E2_C_RFSWB | E2_C_LNA},
};
#define N_PATHS (sizeof(PATHS) / sizeof(PATHS[0]))

/* Which chip's column to burn. */
typedef enum { CHIP_EEPROM1 = 1, CHIP_EEPROM2 = 2 } chip_t;

/* ── LED heartbeat so you can see the firmware is alive ────────────────── */
static bool led_cb(struct repeating_timer *t) {
    (void)t; gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN); return true;
}

/* Program all 32 addresses for one chip, then read-back verify. */
static bool program_chip(chip_t chip) {
    uint8_t image[32];
    memset(image, 0xFF, sizeof(image));   /* unused addrs -> 0xFF (recognisable) */

    for (unsigned i = 0; i < N_PATHS; i++)
        image[i] = (chip == CHIP_EEPROM1) ? PATHS[i].e1 : PATHS[i].e2;

    printf("\nProgramming EEPROM%d — 32 bytes:\n", (int)chip);
    for (unsigned i = 0; i < N_PATHS; i++)
        printf("  [0x%02X] 0x%02X  %s\n", i, image[i], PATHS[i].name);

    /* Write. write_buf auto page-splits; 32 bytes fits inside one 64B page. */
    if (!at28bv64b_write_buf(&dev, 0x0000, image, sizeof(image))) {
        printf("[FAIL] write_buf timed out — check wiring/power.\n");
        return false;
    }

    /* Verify. */
    uint8_t rb[32];
    if (!at28bv64b_read_buf(&dev, 0x0000, rb, sizeof(rb))) {
        printf("[FAIL] read_buf range error.\n");
        return false;
    }
    bool ok = true;
    for (unsigned i = 0; i < sizeof(image); i++) {
        if (rb[i] != image[i]) {
            printf("[FAIL] verify @0x%02X: wrote 0x%02X read 0x%02X\n",
                   i, image[i], rb[i]);
            ok = false;
        }
    }
    printf(ok ? "[PASS] EEPROM%d verified (32/32 bytes match)\n"
              : "[FAIL] EEPROM%d verify mismatch — do NOT trust this chip\n",
           (int)chip);
    return ok;
}

int main(void) {
    stdio_init_all();
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    struct repeating_timer timer;
    add_repeating_timer_ms(500, led_cb, NULL, &timer);

    /* Wait for a USB serial connection before prompting. */
    while (!stdio_usb_connected()) sleep_ms(100);

    if (!at28bv64b_init(&dev)) {
        printf("[FAIL] at28bv64b_init\n");
        while (true) tight_loop_contents();
    }

    printf("\n=== RF Switch Path Programmer ===\n");
    printf("Bit convention: I/O0 = bit0 (LSB), byte = sum(1<<io)\n");
    printf("Seat ONE chip, then choose which it is.\n");

    while (true) {
        printf("\nSeated chip?  [1] EEPROM1 (SwA+SwB)   [2] EEPROM2 (SwC+diode)   [r] re-print menu\n> ");
        int c = getchar_timeout_us(60u * 1000u * 1000u); /* 60 s */
        if (c == '1') program_chip(CHIP_EEPROM1);
        else if (c == '2') program_chip(CHIP_EEPROM2);
        else if (c == 'r' || c == 'R' || c == PICO_ERROR_TIMEOUT) continue;
        else printf("Unrecognised input.\n");
    }
}
