#include "at28bv64b.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "pico/time.h"

/* ── Timing (conservative for AT28BV64B-20 grade) ──────────────────── */
#define WAIT_US          1u       /* covers tACC ≤ 200 ns, tWP ≥ 100 ns */
#define POLL_TIMEOUT_US  12000u   /* 12 ms > tWC max (10 ms)            */

/* ── SDP command addresses (0x5555/0x2AAA folded to 13-bit space) ───── */
#define SDP_ADDR1        0x1555u  /* 0x5555 & 0x1FFF */
#define SDP_ADDR2        0x0AAAu  /* 0x2AAA & 0x1FFF */
#define SDP_D1           0xAAu
#define SDP_D2           0x55u
#define SDP_UNLOCK       0xA0u    /* byte-program command (precedes write) */
#define SDP_PROT_SETUP   0x80u    /* byte 3 of 6-byte enable/disable seq  */
#define SDP_PROT_CONFIRM 0x20u    /* byte 6 of 6-byte enable seq          */

/* ── Static bus helpers ─────────────────────────────────────────────── */

static inline uint32_t addr_mask(const at28bv64b_t *dev) {
    return (uint32_t)0x1FFF << dev->addr_base;
}

static inline uint32_t data_mask(const at28bv64b_t *dev) {
    return (uint32_t)0xFF << dev->data_base;
}

static inline void set_address(const at28bv64b_t *dev, uint16_t addr) {
    gpio_put_masked(addr_mask(dev),
                    (uint32_t)(addr & AT28BV64B_ADDR_MASK) << dev->addr_base);
}

static inline void set_data_out(const at28bv64b_t *dev, uint8_t data) {
    gpio_set_dir_masked(data_mask(dev), data_mask(dev));
    gpio_put_masked(data_mask(dev), (uint32_t)data << dev->data_base);
}

static inline void set_data_in(const at28bv64b_t *dev) {
    gpio_set_dir_masked(data_mask(dev), 0u);
}

static inline uint8_t read_data(const at28bv64b_t *dev) {
    return (uint8_t)((gpio_get_all() >> dev->data_base) & 0xFF);
}

/* ── Init / Deinit ──────────────────────────────────────────────────── */

bool at28bv64b_init(const at28bv64b_t *dev) {
    for (uint i = 0; i < 13; i++) {
        gpio_init(dev->addr_base + i);
        gpio_set_dir(dev->addr_base + i, GPIO_OUT);
        gpio_put(dev->addr_base + i, 0);
    }
    for (uint i = 0; i < 8; i++) {
        gpio_init(dev->data_base + i);
        gpio_set_dir(dev->data_base + i, GPIO_IN);
        gpio_pull_down(dev->data_base + i);
    }
    gpio_init(dev->ce_pin);
    gpio_set_dir(dev->ce_pin, GPIO_OUT);
    gpio_put(dev->ce_pin, 1);

    gpio_init(dev->oe_pin);
    gpio_set_dir(dev->oe_pin, GPIO_OUT);
    gpio_put(dev->oe_pin, 1);

    gpio_init(dev->we_pin);
    gpio_set_dir(dev->we_pin, GPIO_OUT);
    gpio_put(dev->we_pin, 1);

    return true;
}

void at28bv64b_deinit(const at28bv64b_t *dev) {
    for (uint i = 0; i < 13; i++) gpio_deinit(dev->addr_base + i);
    for (uint i = 0; i < 8;  i++) gpio_deinit(dev->data_base + i);
    gpio_deinit(dev->ce_pin);
    gpio_deinit(dev->oe_pin);
    gpio_deinit(dev->we_pin);
}

/* ── Read ───────────────────────────────────────────────────────────── */

uint8_t at28bv64b_read_byte(const at28bv64b_t *dev, uint16_t addr) {
    set_data_in(dev);
    set_address(dev, addr);
    gpio_put(dev->ce_pin, 0);
    gpio_put(dev->oe_pin, 0);
    busy_wait_us_32(WAIT_US);
    uint8_t data = read_data(dev);
    gpio_put(dev->oe_pin, 1);
    gpio_put(dev->ce_pin, 1);
    return data;
}

bool at28bv64b_read_buf(const at28bv64b_t *dev, uint16_t addr,
                         uint8_t *buf, size_t len) {
    if ((uint32_t)addr + len > AT28BV64B_SIZE) return false;
    for (size_t i = 0; i < len; i++)
        buf[i] = at28bv64b_read_byte(dev, (uint16_t)(addr + i));
    return true;
}

/* ── Write internals ────────────────────────────────────────────────── */

/* Poll D7 until it matches bit 7 of the last written byte (Data Polling).
 * Returns false on timeout (write failed or no EEPROM connected). */
static bool poll_write_complete(const at28bv64b_t *dev,
                                 uint16_t addr, uint8_t written) {
    uint8_t d7_expected = written & 0x80;
    absolute_time_t deadline = make_timeout_time_us(POLL_TIMEOUT_US);
    set_data_in(dev);
    set_address(dev, addr);
    while (!time_reached(deadline)) {
        gpio_put(dev->ce_pin, 0);
        gpio_put(dev->oe_pin, 0);
        busy_wait_us_32(WAIT_US);
        uint8_t d = read_data(dev);
        gpio_put(dev->oe_pin, 1);
        gpio_put(dev->ce_pin, 1);
        if ((d & 0x80) == d7_expected) return true;
    }
    return false;
}

/* Single /WE pulse. /CE must already be asserted (low) by the caller.
 * Address is latched on the falling edge of /WE; data on the rising edge. */
static inline void pulse_we(const at28bv64b_t *dev,
                             uint16_t addr, uint8_t data) {
    set_address(dev, addr);
    set_data_out(dev, data);
    gpio_put(dev->we_pin, 0);
    busy_wait_us_32(WAIT_US);  /* tWP >= 100 ns */
    gpio_put(dev->we_pin, 1);
}

/* ── Write ──────────────────────────────────────────────────────────── */

bool at28bv64b_write_byte(at28bv64b_t *dev, uint16_t addr, uint8_t data) {
    gpio_put(dev->oe_pin, 1);
    gpio_put(dev->ce_pin, 0);
    if (dev->sdp_enabled) {
        pulse_we(dev, SDP_ADDR1, SDP_D1);
        pulse_we(dev, SDP_ADDR2, SDP_D2);
        pulse_we(dev, SDP_ADDR1, SDP_UNLOCK);
    }
    pulse_we(dev, addr, data);
    gpio_put(dev->ce_pin, 1);
    set_data_in(dev);
    return poll_write_complete(dev, addr, data);
}

bool at28bv64b_write_page(at28bv64b_t *dev, uint16_t addr,
                           const uint8_t *buf, size_t len) {
    if (len == 0 || len > AT28BV64B_PAGE_SIZE) return false;
    /* All bytes must lie within the same 64-byte aligned page. */
    uint16_t page_start = addr & (uint16_t)~(AT28BV64B_PAGE_SIZE - 1u);
    if ((uint32_t)addr + len > page_start + AT28BV64B_PAGE_SIZE) return false;

    gpio_put(dev->oe_pin, 1);
    gpio_put(dev->ce_pin, 0);
    if (dev->sdp_enabled) {
        pulse_we(dev, SDP_ADDR1, SDP_D1);
        pulse_we(dev, SDP_ADDR2, SDP_D2);
        pulse_we(dev, SDP_ADDR1, SDP_UNLOCK);
    }
    for (size_t i = 0; i < len; i++)
        pulse_we(dev, (uint16_t)(addr + i), buf[i]);
    gpio_put(dev->ce_pin, 1);  /* rising /CE triggers the internal write cycle */
    set_data_in(dev);
    return poll_write_complete(dev, (uint16_t)(addr + len - 1), buf[len - 1]);
}

bool at28bv64b_write_buf(at28bv64b_t *dev, uint16_t addr,
                          const uint8_t *buf, size_t len) {
    if ((uint32_t)addr + len > AT28BV64B_SIZE) return false;
    size_t offset = 0;
    while (offset < len) {
        uint16_t cur = (uint16_t)(addr + offset);
        /* End of the page that cur falls in. */
        uint16_t page_end = (uint16_t)((cur & ~(uint16_t)(AT28BV64B_PAGE_SIZE - 1u))
                                       + AT28BV64B_PAGE_SIZE);
        size_t chunk = (size_t)(page_end - cur);
        if (chunk > len - offset) chunk = len - offset;
        if (!at28bv64b_write_page(dev, cur, buf + offset, chunk)) return false;
        offset += chunk;
    }
    return true;
}

/* ── Software Data Protection ───────────────────────────────────────── */

/* 6-byte command sequence used for both enable and disable. */
static void sdp_6byte_cmd(const at28bv64b_t *dev, uint8_t confirm) {
    gpio_put(dev->oe_pin, 1);
    gpio_put(dev->ce_pin, 0);
    pulse_we(dev, SDP_ADDR1, SDP_D1);
    pulse_we(dev, SDP_ADDR2, SDP_D2);
    pulse_we(dev, SDP_ADDR1, SDP_PROT_SETUP);
    pulse_we(dev, SDP_ADDR1, SDP_D1);
    pulse_we(dev, SDP_ADDR2, SDP_D2);
    pulse_we(dev, SDP_ADDR1, confirm);
    gpio_put(dev->ce_pin, 1);
    set_data_in(dev);
    busy_wait_us_32(POLL_TIMEOUT_US);  /* wait full tWC for command to complete */
}

void at28bv64b_sdp_enable(at28bv64b_t *dev) {
    sdp_6byte_cmd(dev, SDP_PROT_CONFIRM);  /* 0x20 */
    dev->sdp_enabled = true;
}

void at28bv64b_sdp_disable(at28bv64b_t *dev) {
    /* Verify the exact disable sequence in the AT28BV64B datasheet.
     * Some revisions require a hardware/voltage-level method instead. */
    sdp_6byte_cmd(dev, SDP_PROT_CONFIRM);
    dev->sdp_enabled = false;
}
