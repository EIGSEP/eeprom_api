#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "at28bv64b.h"

/*
 * On-device test suite for AT28BV64B.
 *
 * The onboard LED blinks at ~1 Hz throughout execution so you can confirm
 * the firmware is running without a serial terminal.
 *
 * Flash test_eeprom.uf2, then open a USB serial terminal for detailed output:
 *   macOS/Linux: minicom -D /dev/tty.usbmodem*  (or screen, picocom)
 *   Windows:     PuTTY, baud 115200
 *
 * All tests print [PASS] or [FAIL].
 */

/* ── LED heartbeat (1 Hz, non-blocking) ─────────────────────────────── */

static bool led_blink_cb(struct repeating_timer *t) {
    (void)t;
    gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
    return true;  /* keep repeating */
}

static at28bv64b_t dev = {
    .addr_base   = 0,
    .data_base   = 13,
    .ce_pin      = 21,
    .oe_pin      = 22,
    .we_pin      = 23,
    .sdp_enabled = false,
};

#define TEST(name, expr) \
    do { if (expr) printf("[PASS] %s\n", name); \
         else      printf("[FAIL] %s\n", name); } while(0)

/* ── Test cases ──────────────────────────────────────────────────────── */

static void test_init(void) {
    TEST("init returns true", at28bv64b_init(&dev));
}

static void test_read_write_byte(void) {
    TEST("write_byte 0xA5", at28bv64b_write_byte(&dev, 0x0010, 0xA5));
    TEST("read_byte 0xA5",  at28bv64b_read_byte(&dev, 0x0010) == 0xA5);
    TEST("write_byte 0x00", at28bv64b_write_byte(&dev, 0x0010, 0x00));
    TEST("read_byte 0x00",  at28bv64b_read_byte(&dev, 0x0010) == 0x00);
}

static void test_read_write_buf(void) {
    uint8_t tx[64], rx[64];
    memset(tx, 0x5A, sizeof(tx));
    memset(rx, 0x00, sizeof(rx));
    TEST("write_buf 64B",     at28bv64b_write_buf(&dev, 0x0040, tx, 64));
    TEST("read_buf 64B",      at28bv64b_read_buf(&dev, 0x0040, rx, 64));
    TEST("buf content match", memcmp(tx, rx, 64) == 0);
}

static void test_page_boundary(void) {
    uint8_t tx[128], rx[128];
    for (int i = 0; i < 128; i++) tx[i] = (uint8_t)i;
    memset(rx, 0, sizeof(rx));
    /* 0x00C0 = 192; spans page [192,255] and [256,319] */
    TEST("write 128B across page boundary",
         at28bv64b_write_buf(&dev, 0x00C0, tx, 128));
    TEST("read 128B",
         at28bv64b_read_buf(&dev, 0x00C0, rx, 128));
    TEST("page boundary content match", memcmp(tx, rx, 128) == 0);
}

static void test_write_page_rejects_bad_args(void) {
    uint8_t buf[65];
    memset(buf, 0xFF, sizeof(buf));
    TEST("write_page rejects len=0",
         !at28bv64b_write_page(&dev, 0x0000, buf, 0));
    TEST("write_page rejects len=65",
         !at28bv64b_write_page(&dev, 0x0000, buf, 65));
    /* addr=0x003F, len=2 crosses page boundary [0,63] → [64,127] */
    TEST("write_page rejects cross-page",
         !at28bv64b_write_page(&dev, 0x003F, buf, 2));
}

static void test_write_buf_rejects_overflow(void) {
    uint8_t buf[1] = {0};
    /* Address 0x1FFF + 2 bytes overflows 8 KiB */
    TEST("write_buf rejects overflow",
         !at28bv64b_write_buf(&dev, 0x1FFF, buf, 2));
}

static void test_sdp(void) {
    at28bv64b_sdp_enable(&dev);
    TEST("write_byte with SDP",
         at28bv64b_write_byte(&dev, 0x0200, 0xC3));
    TEST("read_byte after SDP write",
         at28bv64b_read_byte(&dev, 0x0200) == 0xC3);
    at28bv64b_sdp_disable(&dev);
    /* After disabling SDP, a normal write should still work. */
    TEST("write_byte after SDP disable",
         at28bv64b_write_byte(&dev, 0x0201, 0x7E));
    TEST("read_byte after SDP disable",
         at28bv64b_read_byte(&dev, 0x0201) == 0x7E);
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    stdio_init_all();

    /* Start LED heartbeat before anything else so the blink is visible
     * even if the serial terminal is not connected.
     * Timer fires every 500 ms → LED toggles at 1 Hz. */
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    struct repeating_timer led_timer;
    add_repeating_timer_ms(500, led_blink_cb, NULL, &led_timer);

    sleep_ms(2000);  /* wait for USB CDC enumeration on host */

    printf("\n=== AT28BV64B Test Suite ===\n");
    test_init();
    test_read_write_byte();
    test_read_write_buf();
    test_page_boundary();
    test_write_page_rejects_bad_args();
    test_write_buf_rejects_overflow();
    test_sdp();
    printf("=== Done ===\n\n");

    for (;;) tight_loop_contents();
}
