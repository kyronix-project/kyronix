#pragma once

#include "i2c.h"
#include "../fs/vfs.h"
#include <stdbool.h>
#include <stdint.h>

#define TMP117_REG_TEMP_RESULT 0x00
#define TMP117_REG_CONFIGURATION 0x01
#define TMP117_REG_THIGH_LIMIT 0x02
#define TMP117_REG_TLOW_LIMIT 0x03
#define TMP117_REG_EEPROM_UL 0x04
#define TMP117_REG_EEPROM1 0x05
#define TMP117_REG_EEPROM2 0x06
#define TMP117_REG_TEMP_OFFSET 0x07
#define TMP117_REG_EEPROM3 0x08
#define TMP117_REG_DEVICE_ID 0x0F

#define TMP117_DEVICE_ID_VALUE 0x0117
#define TMP117_DEVICE_ID_MASK 0x0FFF

#define TMP117_I2C_ADDR_GND 0x48
#define TMP117_I2C_ADDR_VCC 0x49
#define TMP117_I2C_ADDR_SDA 0x4A
#define TMP117_I2C_ADDR_SCL 0x4B

#define TMP117_CONFIG_HIGH_ALERT (1u << 15)
#define TMP117_CONFIG_LOW_ALERT (1u << 14)
#define TMP117_CONFIG_DATA_READY (1u << 13)
#define TMP117_CONFIG_EEPROM_BUSY (1u << 12)
#define TMP117_CONFIG_MOD_MASK (3u << 10)
#define TMP117_CONFIG_MOD_CC (0u << 10)
#define TMP117_CONFIG_MOD_SD (1u << 10)
#define TMP117_CONFIG_MOD_OS (3u << 10)
#define TMP117_CONFIG_CONV_MASK (7u << 7)
#define TMP117_CONFIG_CONV_15_5MS (0u << 7)
#define TMP117_CONFIG_CONV_125MS (1u << 7)
#define TMP117_CONFIG_CONV_250MS (2u << 7)
#define TMP117_CONFIG_CONV_500MS (3u << 7)
#define TMP117_CONFIG_CONV_1S (4u << 7)
#define TMP117_CONFIG_CONV_4S (5u << 7)
#define TMP117_CONFIG_CONV_8S (6u << 7)
#define TMP117_CONFIG_CONV_16S (7u << 7)
#define TMP117_CONFIG_AVG_MASK (3u << 5)
#define TMP117_CONFIG_AVG_NONE (0u << 5)
#define TMP117_CONFIG_AVG_8 (1u << 5)
#define TMP117_CONFIG_AVG_32 (2u << 5)
#define TMP117_CONFIG_AVG_64 (3u << 5)
#define TMP117_CONFIG_T_NA (1u << 4)
#define TMP117_CONFIG_POL (1u << 3)
#define TMP117_CONFIG_DR_ALERT (1u << 2)
#define TMP117_CONFIG_SOFT_RESET (1u << 1)

#define TMP117_RESOLUTION_NUM 78125LL
#define TMP117_RESOLUTION_DENOM 10000LL
#define TMP117_ROUND_OFFSET 5000LL
#define TMP117_REVERSE_ROUND 39062LL

#define TMP117_TEMP_MIN_MC -256000
#define TMP117_TEMP_MAX_MC 255992

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

typedef struct {
    struct i2c_client *client;
    struct mutex lock;
    uint16_t cached_config;
    uint16_t device_id;
    int32_t last_temp_mc;
    vfs_node_t *dev_node;
    vfs_node_t *sysfs_name;
    vfs_node_t *sysfs_temp_input;
    vfs_node_t *sysfs_temp_max;
    vfs_node_t *sysfs_temp_min;
    vfs_node_t *sysfs_temp_max_alarm;
    vfs_node_t *sysfs_temp_min_alarm;
    vfs_node_t *sysfs_temp_offset;
    vfs_node_t *sysfs_interval;
    bool probed;
} tmp117_data_t;

typedef tmp117_data_t tmp117_dev_t;

int32_t tmp117_raw_to_mc(int16_t raw);
int16_t tmp117_mc_to_raw(int32_t temp_mc);
int64_t tmp117_raw_to_uc(int16_t raw);
int16_t tmp117_uc_to_raw(int64_t temp_uc);

int tmp117_init(tmp117_dev_t *dev, struct i2c_adapter *adapter, uint8_t addr);
int tmp117_probe(tmp117_dev_t *dev);
int tmp117_read_raw(tmp117_dev_t *dev, int16_t *raw);
int tmp117_read_temp_mc(tmp117_dev_t *dev, int32_t *temp_mc);
int tmp117_read_temp_uc(tmp117_dev_t *dev, int64_t *temp_uc);

int tmp117_set_offset_mc(tmp117_dev_t *dev, int32_t offset_mc);
int tmp117_get_offset_mc(tmp117_dev_t *dev, int32_t *offset_mc);

int tmp117_set_high_limit_mc(tmp117_dev_t *dev, int32_t limit_mc);
int tmp117_get_high_limit_mc(tmp117_dev_t *dev, int32_t *limit_mc);
int tmp117_set_low_limit_mc(tmp117_dev_t *dev, int32_t limit_mc);
int tmp117_get_low_limit_mc(tmp117_dev_t *dev, int32_t *limit_mc);

int tmp117_set_conversion_mode(tmp117_dev_t *dev, uint16_t mode);
int tmp117_set_conversion_cycle(tmp117_dev_t *dev, uint16_t conv);
int tmp117_set_averaging(tmp117_dev_t *dev, uint16_t avg);
int tmp117_get_status_flags(tmp117_dev_t *dev, uint16_t *flags);
int tmp117_soft_reset(tmp117_dev_t *dev);
int tmp117_inject_raw(tmp117_dev_t *dev, int16_t raw);
int tmp117_inject_temp_mc(tmp117_dev_t *dev, int32_t temp_mc);

void tmp117_subsys_init(void);
tmp117_dev_t *tmp117_get_default_device(void);
