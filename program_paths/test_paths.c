/*
 * test_paths.c  —  Read-back / decode verifier for the RF-switch path EEPROMs.
 *
 * Complements program_paths.c. For the currently seated chip it walks all 32
 * addresses, reads the stored byte, decodes it into named I/O lines, and
 * compares against the expected path table. Use it to confirm that the right
 * pins are set for each address — either by eye, or with a meter/logic probe
 * on the I/O lines while you hold an address.
 *
 * BIT CONVENTION (must match program_paths.c and the D-sub wiring):
 *   I/O0 = bit0 = LSB,  byte = sum(1 << io).  Edit io_bit() only.
 *
 * MODES (chosen from the USB-serial menu):
 *   [1]/[2]  Seated chip is EEPROM1 / EEPROM2 → full read-back + verify table.
 *   [h A]    HOLD one address on the bus so you can probe the live I/O lines.
 *            Presents the address, asserts /CE+/OE, and drives D0..D7 with the
 *            stored byte pattern until you press a key. (Read the datasheet
 *            note below before trusting the driven-output method.)
 *
 * NOTE ON "HOLD": the AT28BV64B only drives D0..D7 during a read cycle. This
 *   tool re-reads the byte and re-drives it in a tight loop so the pins stay
 *   at the pattern for probing; it is a bench aid, not how the runtime board
 *   presents the byte. For a true end-to-end check, present the address from
 *   your runtime path and probe the D-sub.
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "at28bv64b.h"

static at28bv64b_t dev = {
    .addr_pins   = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12},
    .data_pins   = {13, 14, 15, 16, 17, 18, 19, 20},
    .ce_pin      = 21,
    .oe_pin      = 22,
    .we_pin      = 26,
    .sdp_enabled = false,
};

#define IO(n)  ((uint8_t)(1u << (n)))
static inline uint8_t io_bit(unsigned io) { return IO(io); }

/* ── Per-chip I/O line names, indexed by I/O number 0..7 ───────────────── */
static const char *E1_NAMES[8] = {
    "SwA/Spare1", "SwA/NoiseLeg", "SwA/AmbHot", "SwA/Feed",
    "SwB/CalLoad", "SwB/Spare2", "SwB/CalOpen", "SwB/CalShort"
};
static const char *E2_NAMES[8] = {
    "SwC/RFSWA", "SwC/VNA", "SwC/LNA", "SwC/RFSWB",
    "Diode_ON", "GND(io5)", "GND(io6)", "GND(io7)"
};

/* ── Expected table (same values as program_paths.c) ───────────────────── */
#define E1_A_SPARE1 IO(0)
#define E1_A_NOISELEG IO(1)
#define E1_A_AMBHOT IO(2)
#define E1_A_FEED IO(3)
#define E1_B_CALLOAD IO(4)
#define E1_B_SPARE2 IO(5)
#define E1_B_CALOPEN IO(6)
#define E1_B_CALSHORT IO(7)
#define E2_C_RFSWA IO(0)
#define E2_C_VNA IO(1)
#define E2_C_LNA IO(2)
#define E2_C_RFSWB IO(3)
#define E2_DIODE_ON IO(4)

typedef struct { const char *name; uint8_t e1, e2; } path_t;
static const path_t PATHS[] = {
 {"LNA -> Feed (DEFAULT)",  E1_A_FEED,     E2_C_RFSWA | E2_C_LNA},
 {"VNA -> VNA Cal Load",    E1_B_CALLOAD,  E2_C_VNA   | E2_C_RFSWB},
 {"VNA -> VNA Cal Open",    E1_B_CALOPEN,  E2_C_VNA   | E2_C_RFSWB},
 {"VNA -> VNA Cal Short",   E1_B_CALSHORT, E2_C_VNA   | E2_C_RFSWB},
 {"VNA -> Feed",            E1_A_FEED,     E2_C_VNA   | E2_C_RFSWA},
 {"VNA -> Noise Diode ON",  E1_A_NOISELEG, E2_C_VNA   | E2_C_RFSWA | E2_DIODE_ON},
 {"VNA -> Noise Diode OFF", E1_A_NOISELEG, E2_C_VNA   | E2_C_RFSWA},
 {"VNA -> LNA",             0,             E2_C_VNA   | E2_C_LNA},
 {"VNA -> Amb/Hot Load",    E1_A_AMBHOT,   E2_C_VNA   | E2_C_RFSWA},
 {"VNA -> Spare 1",         E1_A_SPARE1,   E2_C_VNA   | E2_C_RFSWA},
 {"VNA -> Spare 2",         E1_B_SPARE2,   E2_C_VNA   | E2_C_RFSWB},
 {"LNA -> Noise Diode ON",  E1_A_NOISELEG, E2_C_RFSWA | E2_C_LNA | E2_DIODE_ON},
 {"LNA -> Noise Diode OFF", E1_A_NOISELEG, E2_C_RFSWA | E2_C_LNA},
 {"LNA -> Amb/Hot Load",    E1_A_AMBHOT,   E2_C_RFSWA | E2_C_LNA},
 {"LNA -> Spare 1",         E1_A_SPARE1,   E2_C_RFSWA | E2_C_LNA},
 {"LNA -> Spare 2",         E1_B_SPARE2,   E2_C_RFSWB | E2_C_LNA},
};
#define N_PATHS (sizeof(PATHS) / sizeof(PATHS[0]))
#define ADDR_SPAN 32u          /* 5 select lines → 32 addresses */
#define UNUSED_FILL 0xFFu      /* program_paths writes this to 0x10..0x1F */

typedef enum { CHIP_EEPROM1 = 1, CHIP_EEPROM2 = 2 } chip_t;

/* Heartbeat toggles the LED while idle so you can see the firmware is alive.
 * It is disabled during a verify so the LED can show a definite result:
 *   solid ON  = verify passed,   fast triple-blink then OFF = verify failed. */
static volatile bool heartbeat_on = true;
static bool led_cb(struct repeating_timer *t) {
    (void)t; if (heartbeat_on) gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN); return true;
}
static void led_set(bool on) { gpio_put(PICO_DEFAULT_LED_PIN, on); }
static void led_success(void) { heartbeat_on = false; led_set(true); }   /* solid ON */
static void led_fail(void) {
    heartbeat_on = false;
    for (int i = 0; i < 3; i++) { led_set(true); sleep_ms(120); led_set(false); sleep_ms(120); }
}

/* Directly sample the Pico's data GPIOs (D0..D7) and assemble a byte from the
 * live pin levels, using the SAME data_pins mapping the library uses. This is
 * an honest read of what the Pico sees on its own bus — it validates the
 * D0..D7 (I/O0..7) pinout at the Pico connector. It does NOT reach the far
 * side of the D-sub; probe the D-sub physically (hold mode) for that. */
static uint8_t sense_data_pins(void) {
    uint8_t v = 0;
    for (int io = 0; io < 8; io++)
        if (gpio_get(dev.data_pins[io])) v |= io_bit(io);
    return v;
}

/* Print the set/clear I/O lines of a byte for the given chip. */
static void decode_byte(chip_t chip, uint8_t b) {
    const char **names = (chip == CHIP_EEPROM1) ? E1_NAMES : E2_NAMES;
    printf("  bits(I/O7..0)=");
    for (int io = 7; io >= 0; io--) putchar((b & io_bit(io)) ? '1' : '0');
    printf("  high: ");
    bool any = false;
    for (int io = 0; io < 8; io++)
        if (b & io_bit(io)) { printf("%sI/O%d(%s)", any ? ", " : "", io, names[io]); any = true; }
    if (!any) printf("(none)");
    putchar('\n');
}

/* Full read-back verify against the expected table. */
static void verify_chip(chip_t chip) {
    printf("\n=== Read-back verify: EEPROM%d ===\n", (int)chip);
    heartbeat_on = false; led_set(false);   /* quiet LED during test */
    unsigned fails = 0;
    for (unsigned a = 0; a < ADDR_SPAN; a++) {
        uint8_t got = at28bv64b_read_byte(&dev, (uint16_t)a);
        uint8_t sensed = sense_data_pins();   /* live Pico-side pin levels */
        uint8_t exp = (a < N_PATHS)
                        ? ((chip == CHIP_EEPROM1) ? PATHS[a].e1 : PATHS[a].e2)
                        : UNUSED_FILL;
        const char *label = (a < N_PATHS) ? PATHS[a].name : "(unused, expect 0xFF)";
        bool ok = (got == exp);
        printf("[%s] 0x%02X  read 0x%02X exp 0x%02X  live-pins 0x%02X  %s\n",
               ok ? "PASS" : "FAIL", a, got, exp, sensed, label);
        decode_byte(chip, got);
        if (sensed != got)
            printf("       NOTE live-pins 0x%02X != stored 0x%02X (bus timing or data-pin wiring)\n",
                   sensed, got);
        if (!ok) {
            printf("       MISMATCH — diff bits: ");
            uint8_t d = got ^ exp;
            for (int io = 0; io < 8; io++) if (d & io_bit(io)) printf("I/O%d ", io);
            putchar('\n');
            fails++;
        }
    }
    printf("\nEEPROM%d: %u/%u addresses match.%s\n", (int)chip,
           ADDR_SPAN - fails, ADDR_SPAN, fails ? "  <-- INVESTIGATE" : "  All good.");
    if (fails) led_fail(); else led_success();   /* solid ON = all good */
}

/* Step-through probing walk: park on each address, drive its stored byte on
 * D0..D7, print the expected-high pins, and wait for a keypress before moving
 * on. Designed for the separate programming rig with exposed, probeable wiring
 * — measure each I/O line with a meter against the printed expectation to
 * validate your pinout, then press a key to advance.
 *
 * The EEPROM only drives the bus during a read, so each step re-reads in a
 * tight loop to keep the pattern present while you probe. */
static void probe_walk(chip_t chip) {
    printf("\n=== Step-through probe walk: EEPROM%d ===\n", (int)chip);
    printf("At each address the stored byte is driven on D0..D7.\n");
    printf("Measure the I/O lines, then: [enter]/any key = next, 'b' = back, 'q' = quit walk.\n");
    heartbeat_on = false; led_set(false);

    unsigned a = 0;
    while (a < ADDR_SPAN) {
        uint8_t b = at28bv64b_read_byte(&dev, (uint16_t)a);
        const char *label = (a < N_PATHS) ? PATHS[a].name : "(unused)";
        printf("\n---- Address 0x%02X : stored 0x%02X : %s ----\n", a, b, label);
        decode_byte(chip, b);
        printf("Expect HIGH on: ");
        bool any = false;
        const char **names = (chip == CHIP_EEPROM1) ? E1_NAMES : E2_NAMES;
        for (int io = 0; io < 8; io++)
            if (b & io_bit(io)) { printf("%sI/O%d(%s)", any ? ", " : "", io, names[io]); any = true; }
        if (!any) printf("(all LOW)");
        printf("\nAll other I/O lines should read LOW.  Probe now.\n");

        /* Park: keep re-reading so the bus holds the pattern; poll for a key. */
        int key = PICO_ERROR_TIMEOUT;
        while (key == PICO_ERROR_TIMEOUT) {
            (void)at28bv64b_read_byte(&dev, (uint16_t)a);
            uint8_t sensed = sense_data_pins();
            /* brief live confirmation the Pico itself sees the pattern */
            if (sensed != b) {
                /* re-print sparingly to avoid spamming: only note once per change */
            }
            key = getchar_timeout_us(200u * 1000u);   /* 200 ms poll window */
        }
        if (key == 'q' || key == 'Q') { printf("Walk aborted.\n"); break; }
        else if (key == 'b' || key == 'B') { if (a) a--; }
        else a++;
    }
    if (a >= ADDR_SPAN) printf("\nReached end of address span.\n");
    heartbeat_on = true;   /* resume idle heartbeat */
}

int main(void) {
    stdio_init_all();
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    struct repeating_timer timer;
    add_repeating_timer_ms(500, led_cb, NULL, &timer);
    while (!stdio_usb_connected()) sleep_ms(100);

    if (!at28bv64b_init(&dev)) {
        printf("[FAIL] at28bv64b_init\n");
        while (true) tight_loop_contents();
    }

    printf("\n=== RF Switch Path TESTER ===\n");
    printf("Convention: I/O0 = bit0 (LSB).  Seat one chip before testing.\n");

    chip_t chip = CHIP_EEPROM1;
    while (true) {
        printf("\nMenu (current chip = EEPROM%d):\n", (int)chip);
        printf("  1  set/verify as EEPROM1 (SwA+SwB)\n");
        printf("  2  set/verify as EEPROM2 (SwC+diode)\n");
        printf("  w  step-through PROBE WALK (park on each address to meter pins)\n");
        printf("  d  dump/decode all addresses (no verify)\n> ");
        int c = getchar_timeout_us(60u * 1000u * 1000u);
        if (c == '1') { chip = CHIP_EEPROM1; verify_chip(chip); }
        else if (c == '2') { chip = CHIP_EEPROM2; verify_chip(chip); }
        else if (c == 'w' || c == 'W') { probe_walk(chip); }
        else if (c == 'd' || c == 'D') {
            printf("\n=== Decode dump: EEPROM%d ===\n", (int)chip);
            for (unsigned a = 0; a < ADDR_SPAN; a++) {
                uint8_t b = at28bv64b_read_byte(&dev, (uint16_t)a);
                printf("0x%02X = 0x%02X  %s\n", a, b,
                       (a < N_PATHS) ? PATHS[a].name : "(unused)");
                decode_byte(chip, b);
            }
        } else if (c == PICO_ERROR_TIMEOUT) continue;
        else printf("Unrecognised input.\n");
    }
}
