#include "pit.h"
#include "cpu.h"
#include "pic.h"

#define PIT_CHANNEL0 0x40
#define PIT_CMD      0x43
#define PIT_DIVISOR  1193  /* 1193182 Hz / 1193 ≈ 1000 Hz */

volatile uint64_t g_ticks = 0;
uint64_t g_epoch_base = 0;

static uint8_t cmos_read(uint8_t reg)
{
    outb(0x70, reg);
    io_wait();
    return inb(0x71);
}

static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0xf); }

static uint64_t rtc_read_unix(void)
{
    while (cmos_read(0x0A) & 0x80); /* wait: update-in-progress */

    uint8_t sec = cmos_read(0x00);
    uint8_t min = cmos_read(0x02);
    uint8_t hr  = cmos_read(0x04);
    uint8_t day = cmos_read(0x07);
    uint8_t mon = cmos_read(0x08);
    uint8_t yr  = cmos_read(0x09);
    uint8_t cen = cmos_read(0x32);
    uint8_t sb  = cmos_read(0x0B);

    if (!(sb & 0x04)) { /* BCD mode */
        sec = bcd2bin(sec);
        min = bcd2bin(min);
        hr  = bcd2bin(hr & 0x7f) | (hr & 0x80);
        day = bcd2bin(day);
        mon = bcd2bin(mon);
        yr  = bcd2bin(yr);
        cen = bcd2bin(cen);
    }
    if (!(sb & 0x02) && (hr & 0x80)) /* 12h PM */
        hr = (uint8_t)(((hr & 0x7fu) % 12u) + 12u);

    uint32_t year = (uint32_t)(cen ? cen * 100u : 2000u) + yr;

    /* days since 1970-01-01 */
    static const uint16_t mdays[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    uint32_t y = year - 1970;
    /* leap years from 1970 to year-1 */
    uint32_t ly = (year - 1) / 4 - (year - 1) / 100 + (year - 1) / 400
                - (1969 / 4   - 1969 / 100   + 1969 / 400);
    int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    uint32_t days = y * 365u + ly + mdays[mon - 1] + (day - 1u);
    if (leap && mon > 2) days++;

    return (uint64_t)days * 86400u + hr * 3600u + min * 60u + sec;
}

void pit_init(void)
{
    outb(PIT_CMD, 0x36);  /* channel 0, lo/hi, mode 2 (rate gen), binary */
    outb(PIT_CHANNEL0, (uint8_t)(PIT_DIVISOR & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)(PIT_DIVISOR >> 8));
    g_epoch_base = rtc_read_unix();
    pic_unmask_irq(0);
}
