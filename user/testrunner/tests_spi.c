#include "test_harness.h"

#define SPI_CPHA 0x01
#define SPI_CPOL 0x02
#define SPI_MODE_0 (0 | 0)
#define SPI_MODE_1 (0 | SPI_CPHA)
#define SPI_MODE_2 (SPI_CPOL | 0)
#define SPI_MODE_3 (SPI_CPOL | SPI_CPHA)
#define SPI_CS_HIGH 0x04
#define SPI_LSB_FIRST 0x08
#define SPI_LOOP 0x20

#define SPI_IOC_RD_MODE 0x80016B01
#define SPI_IOC_WR_MODE 0x40016B01
#define SPI_IOC_RD_LSB_FIRST 0x80016B02
#define SPI_IOC_WR_LSB_FIRST 0x40016B02
#define SPI_IOC_RD_BITS_PER_WORD 0x80016B03
#define SPI_IOC_WR_BITS_PER_WORD 0x40016B03
#define SPI_IOC_RD_MAX_SPEED_HZ 0x80046B04
#define SPI_IOC_WR_MAX_SPEED_HZ 0x40046B04
#define SPI_IOC_MESSAGE(N) (0x40006B00 | ((uint32_t)(N * sizeof(struct spi_ioc_transfer)) << 16))

struct spi_ioc_transfer {
    uint64_t tx_buf;
    uint64_t rx_buf;
    uint32_t len;
    uint32_t speed_hz;
    uint16_t delay_usecs;
    uint8_t bits_per_word;
    uint8_t cs_change;
    uint8_t tx_nbits;
    uint8_t rx_nbits;
    uint8_t word_delay_usecs;
    uint8_t pad;
};

int test_spi_dev_nodes(void) {
    int fd0 = open("/dev/spidev0.0", O_RDWR);
    ASSERT_GE(fd0, 0);
    close(fd0);

    int fd1 = open("/dev/spidev0.1", O_RDWR);
    ASSERT_GE(fd1, 0);
    close(fd1);

    return TEST_PASS;
}
REGISTER_TEST(spi_dev_nodes, "Drivers");

int test_spi_vfs_read_write(void) {
    int fd = open("/dev/spidev0.0", O_RDWR);
    ASSERT_GE(fd, 0);

    uint8_t tx[4] = { 0x11, 0x22, 0x33, 0x44 };
    ssize_t w = write(fd, tx, sizeof(tx));
    ASSERT_EQ(w, (ssize_t) sizeof(tx));

    uint8_t rx[4] = { 0 };
    ssize_t r = read(fd, rx, sizeof(rx));
    ASSERT_EQ(r, (ssize_t) sizeof(rx));

    close(fd);
    return TEST_PASS;
}
REGISTER_TEST(spi_vfs_read_write, "Drivers");

int test_spi_ioc_mode_and_speed(void) {
    int fd = open("/dev/spidev0.0", O_RDWR);
    ASSERT_GE(fd, 0);

    uint8_t mode = SPI_MODE_3;
    int ret = ioctl(fd, SPI_IOC_WR_MODE, &mode);
    ASSERT_EQ(ret, 0);

    uint8_t read_mode = 0;
    ret = ioctl(fd, SPI_IOC_RD_MODE, &read_mode);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(read_mode, SPI_MODE_3);

    uint8_t lsb = 1;
    ret = ioctl(fd, SPI_IOC_WR_LSB_FIRST, &lsb);
    ASSERT_EQ(ret, 0);

    uint8_t read_lsb = 0;
    ret = ioctl(fd, SPI_IOC_RD_LSB_FIRST, &read_lsb);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(read_lsb, 1);

    uint8_t bpw = 16;
    ret = ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bpw);
    ASSERT_EQ(ret, 0);

    uint8_t read_bpw = 0;
    ret = ioctl(fd, SPI_IOC_RD_BITS_PER_WORD, &read_bpw);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(read_bpw, 16);

    uint32_t speed = 10000000;
    ret = ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
    ASSERT_EQ(ret, 0);

    uint32_t read_speed = 0;
    ret = ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &read_speed);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(read_speed, 10000000);

    close(fd);
    return TEST_PASS;
}
REGISTER_TEST(spi_ioc_mode_and_speed, "Drivers");

int test_spi_transfer_loopback(void) {
    int fd = open("/dev/spidev0.0", O_RDWR);
    ASSERT_GE(fd, 0);

    uint8_t tx[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    uint8_t rx[4] = { 0 };

    struct spi_ioc_transfer xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.tx_buf = (uint64_t)(uintptr_t) tx;
    xfer.rx_buf = (uint64_t)(uintptr_t) rx;
    xfer.len = sizeof(tx);
    xfer.speed_hz = 1000000;
    xfer.bits_per_word = 8;
    xfer.delay_usecs = 5;
    xfer.word_delay_usecs = 2;

    int ret = ioctl(fd, SPI_IOC_MESSAGE(1), &xfer);
    ASSERT_EQ(ret, (int) sizeof(xfer));
    ASSERT_EQ(rx[0], 0xDE);
    ASSERT_EQ(rx[1], 0xAD);
    ASSERT_EQ(rx[2], 0xBE);
    ASSERT_EQ(rx[3], 0xEF);

    close(fd);
    return TEST_PASS;
}
REGISTER_TEST(spi_transfer_loopback, "Drivers");
