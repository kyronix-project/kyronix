#include "tmp117.h"
#include "../lib/log.h"
#include "../lib/printf.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../syscall/syscall.h"

#define EINVAL 22
#define EIO 5
#define ENODEV 19
#define EFAULT 14
#define ERANGE 34
#define ENOMEM 12

static const struct i2c_device_id tmp117_id[] = {
    { "tmp117", 0 },
    { "", 0 }
};

static const uint16_t g_conv_times_ms[] = { 16, 125, 250, 500, 1000, 4000, 8000, 16000 };

static tmp117_data_t *g_default_data;

int32_t tmp117_raw_to_mc(int16_t raw) {
    int64_t r = (int64_t) raw;
    int64_t round = (raw >= 0) ? TMP117_ROUND_OFFSET : -TMP117_ROUND_OFFSET;
    return (int32_t) ((r * TMP117_RESOLUTION_NUM + round) / TMP117_RESOLUTION_DENOM);
}

int16_t tmp117_mc_to_raw(int32_t temp_mc) {
    if (temp_mc < TMP117_TEMP_MIN_MC) temp_mc = TMP117_TEMP_MIN_MC;
    if (temp_mc > TMP117_TEMP_MAX_MC) temp_mc = TMP117_TEMP_MAX_MC;
    int64_t mc = (int64_t) temp_mc;
    int64_t round = (temp_mc >= 0) ? TMP117_REVERSE_ROUND : -TMP117_REVERSE_ROUND;
    return (int16_t) ((mc * TMP117_RESOLUTION_DENOM + round) / TMP117_RESOLUTION_NUM);
}

int64_t tmp117_raw_to_uc(int16_t raw) {
    return ((int64_t) raw * TMP117_RESOLUTION_NUM) / 10LL;
}

int16_t tmp117_uc_to_raw(int64_t temp_uc) {
    int64_t round = (temp_uc >= 0) ? TMP117_REVERSE_ROUND : -TMP117_REVERSE_ROUND;
    return (int16_t) ((temp_uc * 10LL + round) / TMP117_RESOLUTION_NUM);
}

int tmp117_read_raw(tmp117_data_t *data, int16_t *raw) {
    if (!data || !data->client || !raw) return -EINVAL;
    uint16_t val = 0;
    int ret = i2c_smbus_read_word_swapped(data->client, TMP117_REG_TEMP_RESULT, &val);
    if (ret < 0) return ret;
    *raw = (int16_t) val;
    return 0;
}

int tmp117_read_temp_mc(tmp117_data_t *data, int32_t *temp_mc) {
    if (!data || !temp_mc) return -EINVAL;
    int16_t raw = 0;
    int ret = tmp117_read_raw(data, &raw);
    if (ret < 0) return ret;
    *temp_mc = tmp117_raw_to_mc(raw);
    data->last_temp_mc = *temp_mc;
    return 0;
}

int tmp117_read_temp_uc(tmp117_data_t *data, int64_t *temp_uc) {
    if (!data || !temp_uc) return -EINVAL;
    int16_t raw = 0;
    int ret = tmp117_read_raw(data, &raw);
    if (ret < 0) return ret;
    *temp_uc = tmp117_raw_to_uc(raw);
    return 0;
}

int tmp117_set_offset_mc(tmp117_data_t *data, int32_t offset_mc) {
    if (!data || !data->client) return -EINVAL;
    int16_t raw = tmp117_mc_to_raw(offset_mc);
    return i2c_smbus_write_word_swapped(data->client, TMP117_REG_TEMP_OFFSET, (uint16_t) raw);
}

int tmp117_get_offset_mc(tmp117_data_t *data, int32_t *offset_mc) {
    if (!data || !data->client || !offset_mc) return -EINVAL;
    uint16_t val = 0;
    int ret = i2c_smbus_read_word_swapped(data->client, TMP117_REG_TEMP_OFFSET, &val);
    if (ret < 0) return ret;
    *offset_mc = tmp117_raw_to_mc((int16_t) val);
    return 0;
}

int tmp117_set_high_limit_mc(tmp117_data_t *data, int32_t limit_mc) {
    if (!data || !data->client) return -EINVAL;
    int16_t raw = tmp117_mc_to_raw(limit_mc);
    return i2c_smbus_write_word_swapped(data->client, TMP117_REG_THIGH_LIMIT, (uint16_t) raw);
}

int tmp117_get_high_limit_mc(tmp117_data_t *data, int32_t *limit_mc) {
    if (!data || !data->client || !limit_mc) return -EINVAL;
    uint16_t val = 0;
    int ret = i2c_smbus_read_word_swapped(data->client, TMP117_REG_THIGH_LIMIT, &val);
    if (ret < 0) return ret;
    *limit_mc = tmp117_raw_to_mc((int16_t) val);
    return 0;
}

int tmp117_set_low_limit_mc(tmp117_data_t *data, int32_t limit_mc) {
    if (!data || !data->client) return -EINVAL;
    int16_t raw = tmp117_mc_to_raw(limit_mc);
    return i2c_smbus_write_word_swapped(data->client, TMP117_REG_TLOW_LIMIT, (uint16_t) raw);
}

int tmp117_get_low_limit_mc(tmp117_data_t *data, int32_t *limit_mc) {
    if (!data || !data->client || !limit_mc) return -EINVAL;
    uint16_t val = 0;
    int ret = i2c_smbus_read_word_swapped(data->client, TMP117_REG_TLOW_LIMIT, &val);
    if (ret < 0) return ret;
    *limit_mc = tmp117_raw_to_mc((int16_t) val);
    return 0;
}

int tmp117_set_conversion_mode(tmp117_data_t *data, uint16_t mode) {
    if (!data || !data->client) return -EINVAL;
    uint16_t cfg = 0;
    int ret = i2c_smbus_read_word_swapped(data->client, TMP117_REG_CONFIGURATION, &cfg);
    if (ret < 0) return ret;
    cfg = (cfg & ~TMP117_CONFIG_MOD_MASK) | (mode & TMP117_CONFIG_MOD_MASK);
    ret = i2c_smbus_write_word_swapped(data->client, TMP117_REG_CONFIGURATION, cfg);
    if (ret == 0) data->cached_config = cfg;
    return ret;
}

int tmp117_set_conversion_cycle(tmp117_data_t *data, uint16_t conv) {
    if (!data || !data->client) return -EINVAL;
    uint16_t cfg = 0;
    int ret = i2c_smbus_read_word_swapped(data->client, TMP117_REG_CONFIGURATION, &cfg);
    if (ret < 0) return ret;
    cfg = (cfg & ~TMP117_CONFIG_CONV_MASK) | (conv & TMP117_CONFIG_CONV_MASK);
    ret = i2c_smbus_write_word_swapped(data->client, TMP117_REG_CONFIGURATION, cfg);
    if (ret == 0) data->cached_config = cfg;
    return ret;
}

int tmp117_set_averaging(tmp117_data_t *data, uint16_t avg) {
    if (!data || !data->client) return -EINVAL;
    uint16_t cfg = 0;
    int ret = i2c_smbus_read_word_swapped(data->client, TMP117_REG_CONFIGURATION, &cfg);
    if (ret < 0) return ret;
    cfg = (cfg & ~TMP117_CONFIG_AVG_MASK) | (avg & TMP117_CONFIG_AVG_MASK);
    ret = i2c_smbus_write_word_swapped(data->client, TMP117_REG_CONFIGURATION, cfg);
    if (ret == 0) data->cached_config = cfg;
    return ret;
}

int tmp117_get_status_flags(tmp117_data_t *data, uint16_t *flags) {
    if (!data || !data->client || !flags) return -EINVAL;
    uint16_t cfg = 0;
    int ret = i2c_smbus_read_word_swapped(data->client, TMP117_REG_CONFIGURATION, &cfg);
    if (ret < 0) return ret;
    data->cached_config = cfg;
    *flags = cfg & (TMP117_CONFIG_HIGH_ALERT | TMP117_CONFIG_LOW_ALERT | TMP117_CONFIG_DATA_READY | TMP117_CONFIG_EEPROM_BUSY);
    return 0;
}

int tmp117_soft_reset(tmp117_data_t *data) {
    if (!data || !data->client) return -EINVAL;
    return i2c_smbus_write_word_swapped(data->client, TMP117_REG_CONFIGURATION, TMP117_CONFIG_SOFT_RESET);
}

int tmp117_inject_raw(tmp117_data_t *data, int16_t raw) {
    if (!data || !data->client) return -EINVAL;
    return i2c_stub_set_word((uint8_t) data->client->addr, TMP117_REG_TEMP_RESULT, (uint16_t) raw);
}

int tmp117_inject_temp_mc(tmp117_data_t *data, int32_t temp_mc) {
    int16_t raw = tmp117_mc_to_raw(temp_mc);
    return tmp117_inject_raw(data, raw);
}

static int32_t parse_signed_int(const char *buf, uint64_t len) {
    int32_t val = 0;
    bool neg = false;
    uint64_t idx = 0;
    while (idx < len && (buf[idx] == ' ' || buf[idx] == '\t' || buf[idx] == '\n' || buf[idx] == '\r')) idx++;
    if (idx < len && buf[idx] == '-') {
        neg = true;
        idx++;
    } else if (idx < len && buf[idx] == '+') {
        idx++;
    }
    while (idx < len && buf[idx] >= '0' && buf[idx] <= '9') {
        val = val * 10 + (buf[idx] - '0');
        idx++;
    }
    return neg ? -val : val;
}

static int64_t tmp117_chr_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) off;
    tmp117_data_t *data = (tmp117_data_t *) n->data;
    if (!data || !buf || len == 0) return -EINVAL;

    int32_t temp_mc = 0;
    int ret = tmp117_read_temp_mc(data, &temp_mc);
    if (ret < 0) return ret;

    char formatted[64];
    int slen = snprintf(formatted, sizeof(formatted), "%d\n", temp_mc);
    if (slen < 0) return -EIO;

    if (len < (uint64_t) slen) {
        memcpy(buf, formatted, len);
        return (int64_t) len;
    }
    memcpy(buf, formatted, (size_t) slen);
    return (int64_t) slen;
}

static int64_t tmp117_chr_write(vfs_node_t *n, const char *buf, uint64_t len, uint64_t pos) {
    (void) pos;
    tmp117_data_t *data = (tmp117_data_t *) n->data;
    if (!data || !buf || len == 0) return -EINVAL;

    int32_t val = parse_signed_int(buf, len);
    int ret = tmp117_set_offset_mc(data, val);
    if (ret < 0) return ret;
    return (int64_t) len;
}

static int64_t tmp117_chr_ioctl(vfs_node_t *n, uint64_t req, uint64_t arg) {
    tmp117_data_t *data = (tmp117_data_t *) n->data;
    if (!data) return -EINVAL;

    switch (req) {
    case TMP117_IOC_GET_TEMP_MC: {
        if (!uptr_ok_w((void *) (uintptr_t) arg, sizeof(int32_t))) return -EFAULT;
        int32_t mc = 0;
        int ret = tmp117_read_temp_mc(data, &mc);
        if (ret < 0) return ret;
        *(int32_t *) (uintptr_t) arg = mc;
        return 0;
    }
    case TMP117_IOC_GET_TEMP_RAW: {
        if (!uptr_ok_w((void *) (uintptr_t) arg, sizeof(int16_t))) return -EFAULT;
        int16_t raw = 0;
        int ret = tmp117_read_raw(data, &raw);
        if (ret < 0) return ret;
        *(int16_t *) (uintptr_t) arg = raw;
        return 0;
    }
    case TMP117_IOC_SET_OFFSET_MC: {
        int32_t offset = (int32_t) arg;
        return tmp117_set_offset_mc(data, offset);
    }
    case TMP117_IOC_GET_OFFSET_MC: {
        if (!uptr_ok_w((void *) (uintptr_t) arg, sizeof(int32_t))) return -EFAULT;
        int32_t offset = 0;
        int ret = tmp117_get_offset_mc(data, &offset);
        if (ret < 0) return ret;
        *(int32_t *) (uintptr_t) arg = offset;
        return 0;
    }
    case TMP117_IOC_SET_HIGH_LIMIT_MC: {
        int32_t limit = (int32_t) arg;
        return tmp117_set_high_limit_mc(data, limit);
    }
    case TMP117_IOC_GET_HIGH_LIMIT_MC: {
        if (!uptr_ok_w((void *) (uintptr_t) arg, sizeof(int32_t))) return -EFAULT;
        int32_t limit = 0;
        int ret = tmp117_get_high_limit_mc(data, &limit);
        if (ret < 0) return ret;
        *(int32_t *) (uintptr_t) arg = limit;
        return 0;
    }
    case TMP117_IOC_SET_LOW_LIMIT_MC: {
        int32_t limit = (int32_t) arg;
        return tmp117_set_low_limit_mc(data, limit);
    }
    case TMP117_IOC_GET_LOW_LIMIT_MC: {
        if (!uptr_ok_w((void *) (uintptr_t) arg, sizeof(int32_t))) return -EFAULT;
        int32_t limit = 0;
        int ret = tmp117_get_low_limit_mc(data, &limit);
        if (ret < 0) return ret;
        *(int32_t *) (uintptr_t) arg = limit;
        return 0;
    }
    case TMP117_IOC_SET_MODE: {
        return tmp117_set_conversion_mode(data, (uint16_t) arg);
    }
    case TMP117_IOC_GET_STATUS: {
        if (!uptr_ok_w((void *) (uintptr_t) arg, sizeof(uint16_t))) return -EFAULT;
        uint16_t st = 0;
        int ret = tmp117_get_status_flags(data, &st);
        if (ret < 0) return ret;
        *(uint16_t *) (uintptr_t) arg = st;
        return 0;
    }
    case TMP117_IOC_RESET: {
        return tmp117_soft_reset(data);
    }
    case TMP117_IOC_INJECT_RAW: {
        return tmp117_inject_raw(data, (int16_t) arg);
    }
    case TMP117_IOC_INJECT_TEMP_MC: {
        return tmp117_inject_temp_mc(data, (int32_t) arg);
    }
    default:
        return -EINVAL;
    }
}

static int64_t sysfs_name_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    (void) off;
    const char *name = "tmp117\n";
    size_t slen = strlen(name);
    if (len < slen) return -EINVAL;
    memcpy(buf, name, slen);
    return (int64_t) slen;
}

static int64_t sysfs_temp_input_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) off;
    tmp117_data_t *data = (tmp117_data_t *) n->data;
    if (!data || !buf) return -EINVAL;
    int32_t mc = 0;
    int ret = tmp117_read_temp_mc(data, &mc);
    if (ret < 0) return ret;
    char tmp[32];
    int slen = snprintf(tmp, sizeof(tmp), "%d\n", mc);
    if (slen < 0 || len < (size_t) slen) return -EINVAL;
    memcpy(buf, tmp, (size_t) slen);
    return (int64_t) slen;
}

static int64_t sysfs_temp_max_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) off;
    tmp117_data_t *data = (tmp117_data_t *) n->data;
    if (!data || !buf) return -EINVAL;
    int32_t mc = 0;
    int ret = tmp117_get_high_limit_mc(data, &mc);
    if (ret < 0) return ret;
    char tmp[32];
    int slen = snprintf(tmp, sizeof(tmp), "%d\n", mc);
    if (slen < 0 || len < (size_t) slen) return -EINVAL;
    memcpy(buf, tmp, (size_t) slen);
    return (int64_t) slen;
}

static int64_t sysfs_temp_max_write(vfs_node_t *n, const char *buf, uint64_t len, uint64_t pos) {
    (void) pos;
    tmp117_data_t *data = (tmp117_data_t *) n->data;
    if (!data || !buf || len == 0) return -EINVAL;
    int32_t val = parse_signed_int(buf, len);
    int ret = tmp117_set_high_limit_mc(data, val);
    if (ret < 0) return ret;
    return (int64_t) len;
}

static int64_t sysfs_temp_min_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) off;
    tmp117_data_t *data = (tmp117_data_t *) n->data;
    if (!data || !buf) return -EINVAL;
    int32_t mc = 0;
    int ret = tmp117_get_low_limit_mc(data, &mc);
    if (ret < 0) return ret;
    char tmp[32];
    int slen = snprintf(tmp, sizeof(tmp), "%d\n", mc);
    if (slen < 0 || len < (size_t) slen) return -EINVAL;
    memcpy(buf, tmp, (size_t) slen);
    return (int64_t) slen;
}

static int64_t sysfs_temp_min_write(vfs_node_t *n, const char *buf, uint64_t len, uint64_t pos) {
    (void) pos;
    tmp117_data_t *data = (tmp117_data_t *) n->data;
    if (!data || !buf || len == 0) return -EINVAL;
    int32_t val = parse_signed_int(buf, len);
    int ret = tmp117_set_low_limit_mc(data, val);
    if (ret < 0) return ret;
    return (int64_t) len;
}

static int64_t sysfs_max_alarm_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) off;
    tmp117_data_t *data = (tmp117_data_t *) n->data;
    if (!data || !buf) return -EINVAL;
    uint16_t st = 0;
    int ret = tmp117_get_status_flags(data, &st);
    if (ret < 0) return ret;
    char tmp[8];
    int slen = snprintf(tmp, sizeof(tmp), "%d\n", (st & TMP117_CONFIG_HIGH_ALERT) ? 1 : 0);
    if (slen < 0 || len < (size_t) slen) return -EINVAL;
    memcpy(buf, tmp, (size_t) slen);
    return (int64_t) slen;
}

static int64_t sysfs_min_alarm_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) off;
    tmp117_data_t *data = (tmp117_data_t *) n->data;
    if (!data || !buf) return -EINVAL;
    uint16_t st = 0;
    int ret = tmp117_get_status_flags(data, &st);
    if (ret < 0) return ret;
    char tmp[8];
    int slen = snprintf(tmp, sizeof(tmp), "%d\n", (st & TMP117_CONFIG_LOW_ALERT) ? 1 : 0);
    if (slen < 0 || len < (size_t) slen) return -EINVAL;
    memcpy(buf, tmp, (size_t) slen);
    return (int64_t) slen;
}

static int64_t sysfs_temp_offset_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) off;
    tmp117_data_t *data = (tmp117_data_t *) n->data;
    if (!data || !buf) return -EINVAL;
    int32_t mc = 0;
    int ret = tmp117_get_offset_mc(data, &mc);
    if (ret < 0) return ret;
    char tmp[32];
    int slen = snprintf(tmp, sizeof(tmp), "%d\n", mc);
    if (slen < 0 || len < (size_t) slen) return -EINVAL;
    memcpy(buf, tmp, (size_t) slen);
    return (int64_t) slen;
}

static int64_t sysfs_temp_offset_write(vfs_node_t *n, const char *buf, uint64_t len, uint64_t pos) {
    (void) pos;
    tmp117_data_t *data = (tmp117_data_t *) n->data;
    if (!data || !buf || len == 0) return -EINVAL;
    int32_t val = parse_signed_int(buf, len);
    int ret = tmp117_set_offset_mc(data, val);
    if (ret < 0) return ret;
    return (int64_t) len;
}

static int64_t sysfs_interval_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) off;
    tmp117_data_t *data = (tmp117_data_t *) n->data;
    if (!data || !buf) return -EINVAL;
    uint16_t cfg = 0;
    int ret = i2c_smbus_read_word_swapped(data->client, TMP117_REG_CONFIGURATION, &cfg);
    if (ret < 0) return ret;
    uint8_t conv_idx = (uint8_t) ((cfg & TMP117_CONFIG_CONV_MASK) >> 7);
    if (conv_idx > 7) conv_idx = 7;
    char tmp[16];
    int slen = snprintf(tmp, sizeof(tmp), "%u\n", (unsigned) g_conv_times_ms[conv_idx]);
    if (slen < 0 || len < (size_t) slen) return -EINVAL;
    memcpy(buf, tmp, (size_t) slen);
    return (int64_t) slen;
}

static int tmp117_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id) {
    (void) id;
    if (!client || !client->adapter) return -EINVAL;

    uint16_t dev_id = 0;
    int ret = i2c_smbus_read_word_swapped(client, TMP117_REG_DEVICE_ID, &dev_id);
    if (ret < 0) return ret;

    if ((dev_id & TMP117_DEVICE_ID_MASK) != TMP117_DEVICE_ID_VALUE) return -ENODEV;

    tmp117_data_t *data = (tmp117_data_t *) kmalloc(sizeof(tmp117_data_t));
    if (!data) return -ENOMEM;
    memset(data, 0, sizeof(*data));

    data->client = client;
    data->device_id = dev_id;
    i2c_set_clientdata(client, data);

    i2c_smbus_read_word_swapped(client, TMP117_REG_CONFIGURATION, &data->cached_config);

    vfs_node_t *n_dev = vfs_create_chr("/dev/tmp117", tmp117_chr_read, tmp117_chr_write);
    if (n_dev) {
        n_dev->chr_ioctl = tmp117_chr_ioctl;
        n_dev->data = (uint8_t *) data;
        n_dev->mode = S_IFCHR | 0666;
        n_dev->rdev = VFS_MKDEV(10, 240);
        data->dev_node = n_dev;
    }

    vfs_node_t *n_temp = vfs_create_chr("/dev/temp0", tmp117_chr_read, tmp117_chr_write);
    if (n_temp) {
        n_temp->chr_ioctl = tmp117_chr_ioctl;
        n_temp->data = (uint8_t *) data;
        n_temp->mode = S_IFCHR | 0666;
        n_temp->rdev = VFS_MKDEV(10, 241);
    }

    vfs_mkdir_p("/sys/class/hwmon/hwmon0", 0755);

    vfs_node_t *n_name = vfs_create_chr("/sys/class/hwmon/hwmon0/name", sysfs_name_read, NULL);
    if (n_name) {
        n_name->data = (uint8_t *) data;
        n_name->mode = S_IFCHR | 0444;
        data->sysfs_name = n_name;
    }

    vfs_node_t *n_input = vfs_create_chr("/sys/class/hwmon/hwmon0/temp1_input", sysfs_temp_input_read, NULL);
    if (n_input) {
        n_input->data = (uint8_t *) data;
        n_input->mode = S_IFCHR | 0444;
        data->sysfs_temp_input = n_input;
    }

    vfs_node_t *n_max = vfs_create_chr("/sys/class/hwmon/hwmon0/temp1_max", sysfs_temp_max_read, sysfs_temp_max_write);
    if (n_max) {
        n_max->data = (uint8_t *) data;
        n_max->mode = S_IFCHR | 0666;
        data->sysfs_temp_max = n_max;
    }

    vfs_node_t *n_min = vfs_create_chr("/sys/class/hwmon/hwmon0/temp1_min", sysfs_temp_min_read, sysfs_temp_min_write);
    if (n_min) {
        n_min->data = (uint8_t *) data;
        n_min->mode = S_IFCHR | 0666;
        data->sysfs_temp_min = n_min;
    }

    vfs_node_t *n_max_alarm = vfs_create_chr("/sys/class/hwmon/hwmon0/temp1_max_alarm", sysfs_max_alarm_read, NULL);
    if (n_max_alarm) {
        n_max_alarm->data = (uint8_t *) data;
        n_max_alarm->mode = S_IFCHR | 0444;
        data->sysfs_temp_max_alarm = n_max_alarm;
    }

    vfs_node_t *n_min_alarm = vfs_create_chr("/sys/class/hwmon/hwmon0/temp1_min_alarm", sysfs_min_alarm_read, NULL);
    if (n_min_alarm) {
        n_min_alarm->data = (uint8_t *) data;
        n_min_alarm->mode = S_IFCHR | 0444;
        data->sysfs_temp_min_alarm = n_min_alarm;
    }

    vfs_node_t *n_offset = vfs_create_chr("/sys/class/hwmon/hwmon0/temp1_offset", sysfs_temp_offset_read, sysfs_temp_offset_write);
    if (n_offset) {
        n_offset->data = (uint8_t *) data;
        n_offset->mode = S_IFCHR | 0666;
        data->sysfs_temp_offset = n_offset;
    }

    vfs_node_t *n_intv = vfs_create_chr("/sys/class/hwmon/hwmon0/update_interval", sysfs_interval_read, NULL);
    if (n_intv) {
        n_intv->data = (uint8_t *) data;
        n_intv->mode = S_IFCHR | 0444;
        data->sysfs_interval = n_intv;
    }

    data->probed = true;
    g_default_data = data;
    log_info("tmp117: probed TI TMP117 at I2C addr 0x%02x (device id 0x%04x)", client->addr, dev_id);
    return 0;
}

static void tmp117_i2c_remove(struct i2c_client *client) {
    if (!client) return;
    tmp117_data_t *data = (tmp117_data_t *) i2c_get_clientdata(client);
    if (!data) return;

    if (data->dev_node) {
        data->dev_node->data = NULL;
        data->dev_node->chr_read = NULL;
        data->dev_node->chr_write = NULL;
        data->dev_node->chr_ioctl = NULL;
    }
    vfs_unlink("/dev/tmp117");
    vfs_unlink("/dev/temp0");

    vfs_unlink("/sys/class/hwmon/hwmon0/name");
    vfs_unlink("/sys/class/hwmon/hwmon0/temp1_input");
    vfs_unlink("/sys/class/hwmon/hwmon0/temp1_max");
    vfs_unlink("/sys/class/hwmon/hwmon0/temp1_min");
    vfs_unlink("/sys/class/hwmon/hwmon0/temp1_max_alarm");
    vfs_unlink("/sys/class/hwmon/hwmon0/temp1_min_alarm");
    vfs_unlink("/sys/class/hwmon/hwmon0/temp1_offset");
    vfs_unlink("/sys/class/hwmon/hwmon0/update_interval");
    vfs_rmdir("/sys/class/hwmon/hwmon0");

    if (g_default_data == data) g_default_data = NULL;
    i2c_set_clientdata(client, NULL);
    kfree(data);
}

static struct i2c_driver tmp117_driver = {
    .driver = {
        .name = "tmp117",
    },
    .probe = tmp117_i2c_probe,
    .remove = tmp117_i2c_remove,
    .id_table = tmp117_id,
};

tmp117_data_t *tmp117_get_default_device(void) {
    return g_default_data;
}

void tmp117_subsys_init(void) {
    i2c_register_driver(NULL, &tmp117_driver);

    i2c_stub_create_adapter(0);
    struct i2c_adapter *adap = i2c_get_adapter(0);
    if (adap) {
        struct i2c_board_info info = {
            .type = "tmp117",
            .addr = TMP117_I2C_ADDR_GND,
            .flags = 0,
            .irq = 0,
            .platform_data = NULL,
        };
        i2c_new_client_device(adap, &info);
    }
}
