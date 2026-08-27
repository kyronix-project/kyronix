#include "test_harness.h"

#define TMP117_IOC_GET_TEMP_MC 0x5480
#define TMP117_IOC_GET_TEMP_RAW 0x5481
#define TMP117_IOC_SET_OFFSET_MC 0x5482
#define TMP117_IOC_GET_OFFSET_MC 0x5483
#define TMP117_IOC_SET_HIGH_LIMIT_MC 0x5484
#define TMP117_IOC_GET_HIGH_LIMIT_MC 0x5485
#define TMP117_IOC_SET_LOW_LIMIT_MC 0x5486
#define TMP117_IOC_GET_LOW_LIMIT_MC 0x5487
#define TMP117_IOC_SET_MODE 0x5488
#define TMP117_IOC_GET_STATUS 0x5489
#define TMP117_IOC_RESET 0x548A
#define TMP117_IOC_INJECT_RAW 0x548B
#define TMP117_IOC_INJECT_TEMP_MC 0x548C

#define TMP117_CONFIG_HIGH_ALERT (1u << 15)
#define TMP117_CONFIG_LOW_ALERT (1u << 14)

#define TMP117_RESOLUTION_NUM 78125LL
#define TMP117_RESOLUTION_DENOM 10000LL
#define TMP117_ROUND_OFFSET 5000LL
#define TMP117_REVERSE_ROUND 39062LL

#define I2C_M_RD 0x0001

#define I2C_SLAVE 0x0703
#define I2C_FUNCS 0x0705
#define I2C_RDWR 0x0707
#define I2C_SMBUS 0x0720
#define I2C_SMBUS_READ 1
#define I2C_SMBUS_WRITE 0
#define I2C_SMBUS_WORD_DATA 3

struct i2c_msg {
    uint16_t addr;
    uint16_t flags;
    uint16_t len;
    uint8_t *buf;
};

struct i2c_rdwr_ioctl_data {
    struct i2c_msg *msgs;
    uint32_t nmsgs;
};

union i2c_smbus_data {
    uint8_t byte;
    uint16_t word;
    uint8_t block[34];
};

struct i2c_smbus_ioctl_data {
    uint8_t read_write;
    uint8_t command;
    uint32_t size;
    union i2c_smbus_data *data;
};

static inline int32_t test_raw_to_mc(int16_t raw) {
    int64_t r = (int64_t) raw;
    int64_t round = (raw >= 0) ? TMP117_ROUND_OFFSET : -TMP117_ROUND_OFFSET;
    return (int32_t) ((r * TMP117_RESOLUTION_NUM + round) / TMP117_RESOLUTION_DENOM);
}

static inline int16_t test_mc_to_raw(int32_t temp_mc) {
    int64_t mc = (int64_t) temp_mc;
    int64_t round = (temp_mc >= 0) ? TMP117_REVERSE_ROUND : -TMP117_REVERSE_ROUND;
    return (int16_t) ((mc * TMP117_RESOLUTION_DENOM + round) / TMP117_RESOLUTION_NUM);
}

int test_tmp117_math(void) {
    ASSERT_EQ(test_raw_to_mc(0), 0);
    ASSERT_EQ(test_mc_to_raw(0), 0);

    ASSERT_EQ(test_raw_to_mc(3200), 25000);
    ASSERT_EQ(test_mc_to_raw(25000), 3200);

    ASSERT_EQ(test_raw_to_mc(-5120), -40000);
    ASSERT_EQ(test_mc_to_raw(-40000), -5120);

    ASSERT_EQ(test_raw_to_mc(16000), 125000);
    ASSERT_EQ(test_mc_to_raw(125000), 16000);

    ASSERT_EQ(test_raw_to_mc(1), 8);
    ASSERT_EQ(test_raw_to_mc(-1), -8);

    return TEST_PASS;
}
REGISTER_TEST(tmp117_math, "Drivers");

int test_tmp117_dev_nodes(void) {
    int fd = open("/dev/tmp117", O_RDWR);
    ASSERT_GE(fd, 0);

    ioctl(fd, TMP117_IOC_INJECT_TEMP_MC, (void *) (intptr_t) 25000);
    ioctl(fd, TMP117_IOC_SET_OFFSET_MC, (void *) 0);

    char buf[64];
    memset(buf, 0, sizeof(buf));
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    ASSERT_GT(n, 0);

    int32_t val = atoi(buf);
    ASSERT_EQ(val, 25000);

    close(fd);

    int fd2 = open("/dev/temp0", O_RDONLY);
    ASSERT_GE(fd2, 0);
    memset(buf, 0, sizeof(buf));
    n = read(fd2, buf, sizeof(buf) - 1);
    ASSERT_GT(n, 0);
    int32_t val2 = atoi(buf);
    ASSERT_EQ(val2, 25000);
    close(fd2);

    return TEST_PASS;
}
REGISTER_TEST(tmp117_dev_nodes, "Drivers");

int test_tmp117_sysfs_hwmon(void) {
    char name_buf[32];
    memset(name_buf, 0, sizeof(name_buf));
    ssize_t n = read_file("/sys/class/hwmon/hwmon0/name", name_buf, sizeof(name_buf));
    ASSERT_GT(n, 0);
    ASSERT_EQ(strncmp(name_buf, "tmp117", 6), 0);

    int fd = open("/dev/tmp117", O_RDWR);
    ASSERT_GE(fd, 0);
    ioctl(fd, TMP117_IOC_INJECT_TEMP_MC, (void *) (intptr_t) 25000);
    ioctl(fd, TMP117_IOC_SET_OFFSET_MC, (void *) 0);
    close(fd);

    char temp_buf[32];
    memset(temp_buf, 0, sizeof(temp_buf));
    n = read_file("/sys/class/hwmon/hwmon0/temp1_input", temp_buf, sizeof(temp_buf));
    ASSERT_GT(n, 0);
    int32_t temp_mc = atoi(temp_buf);
    ASSERT_EQ(temp_mc, 25000);

    char intv_buf[32];
    memset(intv_buf, 0, sizeof(intv_buf));
    n = read_file("/sys/class/hwmon/hwmon0/update_interval", intv_buf, sizeof(intv_buf));
    ASSERT_GT(n, 0);
    ASSERT_GT(atoi(intv_buf), 0);

    return TEST_PASS;
}
REGISTER_TEST(tmp117_sysfs_hwmon, "Drivers");

int test_tmp117_ioctls(void) {
    int fd = open("/dev/tmp117", O_RDWR);
    ASSERT_GE(fd, 0);

    ioctl(fd, TMP117_IOC_INJECT_TEMP_MC, (void *) (intptr_t) 25000);
    ioctl(fd, TMP117_IOC_SET_OFFSET_MC, (void *) 0);

    int32_t temp_mc = 0;
    int ret = ioctl(fd, TMP117_IOC_GET_TEMP_MC, &temp_mc);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(temp_mc, 25000);

    int16_t raw_temp = 0;
    ret = ioctl(fd, TMP117_IOC_GET_TEMP_RAW, &raw_temp);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(raw_temp, 0x0C80);

    int32_t offset = 12500;
    ret = ioctl(fd, TMP117_IOC_SET_OFFSET_MC, (void *) (intptr_t) offset);
    ASSERT_EQ(ret, 0);

    int32_t read_offset = 0;
    ret = ioctl(fd, TMP117_IOC_GET_OFFSET_MC, &read_offset);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(read_offset, 12500);

    int32_t high_limit = 85000;
    ret = ioctl(fd, TMP117_IOC_SET_HIGH_LIMIT_MC, (void *) (intptr_t) high_limit);
    ASSERT_EQ(ret, 0);

    int32_t read_high = 0;
    ret = ioctl(fd, TMP117_IOC_GET_HIGH_LIMIT_MC, &read_high);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(read_high, 85000);

    int32_t low_limit = -10000;
    ret = ioctl(fd, TMP117_IOC_SET_LOW_LIMIT_MC, (void *) (intptr_t) low_limit);
    ASSERT_EQ(ret, 0);

    int32_t read_low = 0;
    ret = ioctl(fd, TMP117_IOC_GET_LOW_LIMIT_MC, &read_low);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(read_low, -10000);

    uint16_t status = 0;
    ret = ioctl(fd, TMP117_IOC_GET_STATUS, &status);
    ASSERT_EQ(ret, 0);
    ASSERT_TRUE((status & 0x2000) != 0);

    ret = ioctl(fd, TMP117_IOC_RESET, NULL);
    ASSERT_EQ(ret, 0);

    read_offset = 999;
    ret = ioctl(fd, TMP117_IOC_GET_OFFSET_MC, &read_offset);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(read_offset, 0);

    close(fd);
    return TEST_PASS;
}
REGISTER_TEST(tmp117_ioctls, "Drivers");

int test_tmp117_write_offset(void) {
    int fd = open("/dev/tmp117", O_RDWR);
    ASSERT_GE(fd, 0);

    const char *offset_str = "-5000";
    ssize_t written = write(fd, offset_str, strlen(offset_str));
    ASSERT_EQ(written, (ssize_t) strlen(offset_str));

    int32_t offset = 0;
    int ret = ioctl(fd, TMP117_IOC_GET_OFFSET_MC, &offset);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(offset, -5000);

    ioctl(fd, TMP117_IOC_SET_OFFSET_MC, (void *) 0);
    close(fd);
    return TEST_PASS;
}
REGISTER_TEST(tmp117_write_offset, "Drivers");

int test_tmp117_emulation_dynamic(void) {
    int fd = open("/dev/tmp117", O_RDWR);
    ASSERT_GE(fd, 0);

    ioctl(fd, TMP117_IOC_RESET, NULL);

    int32_t test_temps[] = {
        -55000, -40000, -20000, -5000, 0, 1000, 23500, 37000, 50000, 85000, 125000, 150000
    };
    size_t count = sizeof(test_temps) / sizeof(test_temps[0]);

    for (size_t i = 0; i < count; i++) {
        int32_t t = test_temps[i];
        int ret = ioctl(fd, TMP117_IOC_INJECT_TEMP_MC, (void *) (intptr_t) t);
        ASSERT_EQ(ret, 0);

        char buf[64];
        memset(buf, 0, sizeof(buf));
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        ASSERT_GT(n, 0);
        int32_t read_mc = atoi(buf);

        int32_t diff = read_mc - t;
        if (diff < 0) diff = -diff;
        ASSERT_LE(diff, 8);

        char hwmon_buf[32];
        memset(hwmon_buf, 0, sizeof(hwmon_buf));
        ssize_t hn = read_file("/sys/class/hwmon/hwmon0/temp1_input", hwmon_buf, sizeof(hwmon_buf));
        ASSERT_GT(hn, 0);
        int32_t hwmon_mc = atoi(hwmon_buf);
        ASSERT_EQ(hwmon_mc, read_mc);
    }

    close(fd);
    return TEST_PASS;
}
REGISTER_TEST(tmp117_emulation_dynamic, "Drivers");

int test_tmp117_emulation_alerts(void) {
    int fd = open("/dev/tmp117", O_RDWR);
    ASSERT_GE(fd, 0);

    ioctl(fd, TMP117_IOC_RESET, NULL);

    ioctl(fd, TMP117_IOC_SET_LOW_LIMIT_MC, (void *) (intptr_t) 5000);
    ioctl(fd, TMP117_IOC_SET_HIGH_LIMIT_MC, (void *) (intptr_t) 60000);

    ioctl(fd, TMP117_IOC_INJECT_TEMP_MC, (void *) (intptr_t) 25000);
    uint16_t status = 0;
    ioctl(fd, TMP117_IOC_GET_STATUS, &status);
    ASSERT_EQ((status & (TMP117_CONFIG_HIGH_ALERT | TMP117_CONFIG_LOW_ALERT)), 0);

    char alarm_buf[8];
    memset(alarm_buf, 0, sizeof(alarm_buf));
    read_file("/sys/class/hwmon/hwmon0/temp1_max_alarm", alarm_buf, sizeof(alarm_buf));
    ASSERT_EQ(atoi(alarm_buf), 0);
    read_file("/sys/class/hwmon/hwmon0/temp1_min_alarm", alarm_buf, sizeof(alarm_buf));
    ASSERT_EQ(atoi(alarm_buf), 0);

    ioctl(fd, TMP117_IOC_INJECT_TEMP_MC, (void *) (intptr_t) 75000);
    ioctl(fd, TMP117_IOC_GET_STATUS, &status);
    ASSERT_TRUE((status & TMP117_CONFIG_HIGH_ALERT) != 0);
    ASSERT_EQ((status & TMP117_CONFIG_LOW_ALERT), 0);

    memset(alarm_buf, 0, sizeof(alarm_buf));
    read_file("/sys/class/hwmon/hwmon0/temp1_max_alarm", alarm_buf, sizeof(alarm_buf));
    ASSERT_EQ(atoi(alarm_buf), 1);

    ioctl(fd, TMP117_IOC_INJECT_TEMP_MC, (void *) (intptr_t) -10000);
    ioctl(fd, TMP117_IOC_GET_STATUS, &status);
    ASSERT_TRUE((status & TMP117_CONFIG_LOW_ALERT) != 0);
    ASSERT_EQ((status & TMP117_CONFIG_HIGH_ALERT), 0);

    memset(alarm_buf, 0, sizeof(alarm_buf));
    read_file("/sys/class/hwmon/hwmon0/temp1_min_alarm", alarm_buf, sizeof(alarm_buf));
    ASSERT_EQ(atoi(alarm_buf), 1);

    ioctl(fd, TMP117_IOC_INJECT_TEMP_MC, (void *) (intptr_t) 30000);
    ioctl(fd, TMP117_IOC_GET_STATUS, &status);
    ASSERT_EQ((status & (TMP117_CONFIG_HIGH_ALERT | TMP117_CONFIG_LOW_ALERT)), 0);

    ioctl(fd, TMP117_IOC_RESET, NULL);
    close(fd);
    return TEST_PASS;
}
REGISTER_TEST(tmp117_emulation_alerts, "Drivers");

int test_tmp117_emulation_offset(void) {
    int fd = open("/dev/tmp117", O_RDWR);
    ASSERT_GE(fd, 0);

    ioctl(fd, TMP117_IOC_RESET, NULL);

    ioctl(fd, TMP117_IOC_INJECT_TEMP_MC, (void *) (intptr_t) 20000);

    char buf[64];
    memset(buf, 0, sizeof(buf));
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    ASSERT_GT(n, 0);
    ASSERT_EQ(atoi(buf), 20000);

    ioctl(fd, TMP117_IOC_SET_OFFSET_MC, (void *) (intptr_t) 3000);
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf) - 1);
    ASSERT_GT(n, 0);
    int32_t val = atoi(buf);
    int32_t diff = val - 23000;
    if (diff < 0) diff = -diff;
    ASSERT_LE(diff, 8);

    ioctl(fd, TMP117_IOC_SET_OFFSET_MC, (void *) (intptr_t) -5000);
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf) - 1);
    ASSERT_GT(n, 0);
    val = atoi(buf);
    diff = val - 15000;
    if (diff < 0) diff = -diff;
    ASSERT_LE(diff, 8);

    ioctl(fd, TMP117_IOC_RESET, NULL);
    close(fd);
    return TEST_PASS;
}
REGISTER_TEST(tmp117_emulation_offset, "Drivers");

int test_i2c_dev_linux_compat(void) {
    int fd = open("/dev/i2c-0", O_RDWR);
    ASSERT_GE(fd, 0);

    uint32_t funcs = 0;
    int ret = ioctl(fd, I2C_FUNCS, &funcs);
    ASSERT_EQ(ret, 0);
    ASSERT_TRUE((funcs & 0x00000001) != 0);

    ret = ioctl(fd, I2C_SLAVE, (void *) 0x48);
    ASSERT_EQ(ret, 0);

    union i2c_smbus_data data;
    memset(&data, 0, sizeof(data));
    struct i2c_smbus_ioctl_data smbus_args = {
        .read_write = I2C_SMBUS_READ,
        .command = 0x0F,
        .size = I2C_SMBUS_WORD_DATA,
        .data = &data,
    };
    ret = ioctl(fd, I2C_SMBUS, &smbus_args);
    ASSERT_EQ(ret, 0);

    uint16_t swapped_id = ((data.word >> 8) & 0xFF) | ((data.word & 0xFF) << 8);
    ASSERT_EQ(swapped_id & 0x0FFF, 0x0117);

    uint8_t reg_cmd = 0x0F;
    uint8_t rx_buf[2] = { 0, 0 };
    struct i2c_msg rdwr_msgs[2] = {
        { .addr = 0x48, .flags = 0, .len = 1, .buf = &reg_cmd },
        { .addr = 0x48, .flags = I2C_M_RD, .len = 2, .buf = rx_buf },
    };
    struct i2c_rdwr_ioctl_data rdwr = {
        .msgs = rdwr_msgs,
        .nmsgs = 2,
    };
    ret = ioctl(fd, I2C_RDWR, &rdwr);
    ASSERT_EQ(ret, 2);

    uint16_t rdwr_id = ((uint16_t) rx_buf[0] << 8) | rx_buf[1];
    ASSERT_EQ(rdwr_id & 0x0FFF, 0x0117);

    close(fd);
    return TEST_PASS;
}
REGISTER_TEST(i2c_dev_linux_compat, "Drivers");

int test_tmp117_kmemleak_check(void) {
    int fd = open("/proc/kmemleak", O_RDONLY);
    if (fd >= 0) {
        char buf[512];
        memset(buf, 0, sizeof(buf));
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            ASSERT_TRUE(strstr(buf, "tmp117") == NULL);
            ASSERT_TRUE(strstr(buf, "i2c") == NULL);
        }
    }
    return TEST_PASS;
}
REGISTER_TEST(tmp117_kmemleak_check, "Drivers");
