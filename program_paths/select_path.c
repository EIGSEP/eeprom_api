/*
 * select_path.c  —  Runtime RF-switch path selector.
 *
 * ROLE
 * ----
 * Runs on the RUNTIME board's Pico (not the programmer). This Pico's only job
 * on the EEPROM bus is to present a 5-bit address on A0..A4; the EEPROM's read
 * cycle (A5..A12, /CE, /OE, /WE) is handled elsewhere on the board, and the
 * byte stored at the selected address drives the ADGM1004 switches.
 *
 * So this program does exactly one thing when you pick a path: it drives the
 * five address GPIOs to that path's address number. No EEPROM read is done
 * here. (Thermistor ADC inputs exist on this Pico but are not used here.)
 *
 * ADDRESS LINES  (must match how A0..A4 are wired to this Pico)
 *   A0 = GP8   (LSB)
 *   A1 = GP9
 *   A2 = GP10
 *   A3 = GP11
 *   A4 = GP12  (MSB)
 *   address value = sum(bit_i << i), i = 0..4  -> GP(8+i)
 *
 * The path list and address numbers match the table burned by program_paths.c:
 * address 0x00 = LNA->Feed (default), etc.
 *
 * USE
 *   Flash select_path.uf2, open USB serial (115200). The menu lists paths;
 *   type the address number (hex like 0x04 or decimal) and press enter. The
 *   Pico sets A0..A4 and reports the driven level on each line. The selection
 *   persists until you choose another (GPIOs hold their output state).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"

/* ── Address-line GPIO mapping: A0..A4 -> GP8..GP12 ─────────────────────
 * The ONE place that defines address bit -> GPIO. Edit here only. */
#define ADDR_LINES   5
#define ADDR_GPIO0   8              /* A0 = GP8; A_i = GP(ADDR_GPIO0 + i) */
#define ADDR_MAX     ((1u << ADDR_LINES) - 1u)   /* 31 */

/* ── Path table: address -> human-readable name (matches burned EEPROM) ── */
typedef struct { uint8_t addr; const char *name; } path_t;
static const path_t PATHS[] = {
    {0x00, "LNA -> Feed  (DEFAULT)"},
    {0x01, "VNA -> VNA Cal Load"},
    {0x02, "VNA -> VNA Cal Open"},
    {0x03, "VNA -> VNA Cal Short"},
    {0x04, "VNA -> Feed"},
    {0x05, "VNA -> Noise Diode ON"},
    {0x06, "VNA -> Noise Diode OFF"},
    {0x07, "VNA -> LNA"},
    {0x08, "VNA -> Amb/Hot Load"},
    {0x09, "VNA -> Spare 1"},
    {0x0A, "VNA -> Spare 2"},
    {0x0B, "LNA -> Noise Diode ON"},
    {0x0C, "LNA -> Noise Diode OFF"},
    {0x0D, "LNA -> Amb/Hot Load"},
    {0x0E, "LNA -> Spare 1"},
    {0x0F, "LNA -> Spare 2"},
};
#define N_PATHS (sizeof(PATHS) / sizeof(PATHS[0]))

static int current_addr = -1;   /* -1 = nothing driven yet */

/* Heartbeat LED so you can see the firmware is alive. */
static bool led_cb(struct repeating_timer *t) {
    (void)t; gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN); return true;
}

/* Look up a path name for an address, or NULL if it isn't a defined path. */
static const char *name_for(unsigned addr) {
    for (unsigned i = 0; i < N_PATHS; i++)
        if (PATHS[i].addr == addr) return PATHS[i].name;
    return NULL;
}

/* Drive A0..A4 (GP8..GP12) to the low 5 bits of addr. */
static void set_address(unsigned addr) {
    addr &= ADDR_MAX;
    for (unsigned i = 0; i < ADDR_LINES; i++)
        gpio_put(ADDR_GPIO0 + i, (addr >> i) & 1u);
    current_addr = (int)addr;

    const char *nm = name_for(addr);
    printf("\n>> Address set to 0x%02X (%u) : %s\n",
           addr, addr, nm ? nm : "(no defined path at this address)");
    printf("   Lines: ");
    for (int i = ADDR_LINES - 1; i >= 0; i--)          /* print A4..A0 */
        printf("A%d(GP%d)=%u  ", i, ADDR_GPIO0 + i, (addr >> i) & 1u);
    putchar('\n');
}

static void print_menu(void) {
    printf("\n===== RF Switch Path Selector =====\n");
    if (current_addr >= 0) {
        const char *nm = name_for((unsigned)current_addr);
        printf("Current: 0x%02X  %s\n", current_addr, nm ? nm : "(undefined)");
    } else {
        printf("Current: (none set yet)\n");
    }
    printf("Pick a path by its address number:\n");
    for (unsigned i = 0; i < N_PATHS; i++)
        printf("  0x%02X  %s\n", PATHS[i].addr, PATHS[i].name);
    printf("Type address (e.g. 0x04 or 4), or 'm' to reprint this menu.\n> ");
}

/* Read a whitespace-delimited token from USB serial (blocking, with echo). */
static bool read_token(char *buf, size_t buflen) {
    size_t n = 0;
    while (n < buflen - 1) {
        int c = getchar_timeout_us(60u * 1000u * 1000u);   /* 60 s */
        if (c == PICO_ERROR_TIMEOUT) { if (n) break; else return false; }
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            if (n) break; else continue;
        }
        putchar(c);            /* echo so you can see what you typed */
        buf[n++] = (char)c;
    }
    buf[n] = '\0';
    putchar('\n');
    return n > 0;
}

int main(void) {
    stdio_init_all();

    /* LED heartbeat. */
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    static struct repeating_timer timer;
    add_repeating_timer_ms(500, led_cb, NULL, &timer);

    /* Configure A0..A4 as outputs, default LOW (address 0 = LNA->Feed). */
    for (unsigned i = 0; i < ADDR_LINES; i++) {
        gpio_init(ADDR_GPIO0 + i);
        gpio_set_dir(ADDR_GPIO0 + i, GPIO_OUT);
        gpio_put(ADDR_GPIO0 + i, 0);
    }

    while (!stdio_usb_connected()) sleep_ms(100);

    /* Drive the safe default immediately on boot. */
    set_address(0x00);

    char tok[16];
    while (true) {
        print_menu();
        if (!read_token(tok, sizeof(tok))) continue;      /* timeout -> reloop */

        if (tok[0] == 'm' || tok[0] == 'M') continue;     /* just reprint */

        char *end = NULL;
        long v = strtol(tok, &end, 0);                    /* 0x.. or decimal */
        if (end == tok || *end != '\0') {
            printf("Unrecognised input: '%s'\n", tok);
            continue;
        }
        if (v < 0 || (unsigned long)v > ADDR_MAX) {
            printf("Address out of range 0x00..0x%02X\n", ADDR_MAX);
            continue;
        }
        if (!name_for((unsigned)v))
            printf("Note: 0x%02lX is not a defined path (setting it anyway).\n", v);
        set_address((unsigned)v);
    }
}
