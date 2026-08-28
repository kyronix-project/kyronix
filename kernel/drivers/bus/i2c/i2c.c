#include "i2c.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../syscall/syscall.h"

#define EINVAL 22
#define ENODEV 19
#define EBUSY 16
#define EIO 5
#define EFAULT 14
#define EOPNOTSUPP 95
#define ENOMEM 12

#define MAX_DRIVERS 16
#define I2C_RDWR_IOCTL_MAX_MSGS 42
#define I2C_MAX_MSG_LEN 8192

static struct i2c_adapter *g_adapters[I2C_MAX_ADAPTERS];
static struct i2c_driver *g_drivers[MAX_DRIVERS];

static int64_t i2c_dev_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) off;
    struct i2c_adapter *adap = (struct i2c_adapter *) n->data;
    if (!adap || !buf || len == 0 || len > I2C_MAX_MSG_LEN) return -EINVAL;
    if (adap->current_slave_addr == 0) return -EINVAL;

    uint8_t *kbuf = (uint8_t *) kmalloc(len);
    if (!kbuf) return -ENOMEM;

    struct i2c_client client;
    memset(&client, 0, sizeof(client));
    client.addr = adap->current_slave_addr;
    client.adapter = adap;

    int ret = i2c_master_recv(&client, kbuf, (int) len);
    if (ret > 0) {
        memcpy(buf, kbuf, (size_t) ret);
    }
    kfree(kbuf);
    return (int64_t) ret;
}

static int64_t i2c_dev_write(vfs_node_t *n, const char *buf, uint64_t len, uint64_t off) {
    (void) off;
    struct i2c_adapter *adap = (struct i2c_adapter *) n->data;
    if (!adap || !buf || len == 0 || len > I2C_MAX_MSG_LEN) return -EINVAL;
    if (adap->current_slave_addr == 0) return -EINVAL;

    uint8_t *kbuf = (uint8_t *) kmalloc(len);
    if (!kbuf) return -ENOMEM;
    memcpy(kbuf, buf, len);

    struct i2c_client client;
    memset(&client, 0, sizeof(client));
    client.addr = adap->current_slave_addr;
    client.adapter = adap;

    int ret = i2c_master_send(&client, kbuf, (int) len);
    kfree(kbuf);
    return (int64_t) ret;
}

static int64_t i2cdev_ioctl_rdwr(struct i2c_adapter *adap, uint64_t arg) {
    if (!uptr_ok((void *) (uintptr_t) arg, sizeof(struct i2c_rdwr_ioctl_data))) return -EFAULT;
    struct i2c_rdwr_ioctl_data rdwr;
    memcpy(&rdwr, (void *) (uintptr_t) arg, sizeof(rdwr));
    if (rdwr.nmsgs == 0 || rdwr.nmsgs > I2C_RDWR_IOCTL_MAX_MSGS || !rdwr.msgs) return -EINVAL;

    if (!uptr_ok(rdwr.msgs, rdwr.nmsgs * sizeof(struct i2c_msg))) return -EFAULT;

    struct i2c_msg msgs[I2C_RDWR_IOCTL_MAX_MSGS];
    memcpy(msgs, rdwr.msgs, rdwr.nmsgs * sizeof(struct i2c_msg));

    uint8_t *user_bufs[I2C_RDWR_IOCTL_MAX_MSGS];
    memset(user_bufs, 0, sizeof(user_bufs));

    int res = 0;
    for (uint32_t i = 0; i < rdwr.nmsgs; i++) {
        if (msgs[i].len > I2C_MAX_MSG_LEN) {
            res = -EINVAL;
            break;
        }

        user_bufs[i] = msgs[i].buf;
        if (msgs[i].len > 0) {
            if (!user_bufs[i]) {
                res = -EFAULT;
                break;
            }

            if (msgs[i].flags & I2C_M_RD) {
                if (!uptr_ok_w(user_bufs[i], msgs[i].len)) {
                    res = -EFAULT;
                    break;
                }
                msgs[i].buf = (uint8_t *) kmalloc(msgs[i].len);
                if (!msgs[i].buf) {
                    res = -ENOMEM;
                    break;
                }
                memset(msgs[i].buf, 0, msgs[i].len);
            } else {
                if (!uptr_ok(user_bufs[i], msgs[i].len)) {
                    res = -EFAULT;
                    break;
                }
                msgs[i].buf = (uint8_t *) kmalloc(msgs[i].len);
                if (!msgs[i].buf) {
                    res = -ENOMEM;
                    break;
                }
                memcpy(msgs[i].buf, user_bufs[i], msgs[i].len);
            }
        } else {
            msgs[i].buf = NULL;
        }
    }

    if (res == 0) {
        res = i2c_transfer(adap, msgs, (int) rdwr.nmsgs);
        if (res >= 0) {
            for (uint32_t i = 0; i < rdwr.nmsgs; i++) {
                if ((msgs[i].flags & I2C_M_RD) && msgs[i].len > 0 && msgs[i].buf && user_bufs[i]) {
                    memcpy(user_bufs[i], msgs[i].buf, msgs[i].len);
                }
            }
        }
    }

    for (uint32_t i = 0; i < rdwr.nmsgs; i++) {
        if (msgs[i].buf) {
            kfree(msgs[i].buf);
        }
    }

    return (int64_t) res;
}

static int64_t i2c_dev_ioctl(vfs_node_t *n, uint64_t req, uint64_t arg) {
    struct i2c_adapter *adap = (struct i2c_adapter *) n->data;
    if (!adap) return -EINVAL;

    switch (req) {
    case I2C_SLAVE:
    case I2C_SLAVE_FORCE: {
        if (arg > 0x3FF) return -EINVAL;
        adap->current_slave_addr = (uint16_t) arg;
        return 0;
    }
    case I2C_FUNCS: {
        if (!uptr_ok_w((void *) (uintptr_t) arg, sizeof(uint32_t))) return -EFAULT;
        uint32_t funcs = I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
        if (adap->algo && adap->algo->functionality) {
            funcs = adap->algo->functionality(adap);
        }
        *(uint32_t *) (uintptr_t) arg = funcs;
        return 0;
    }
    case I2C_RDWR: {
        return i2cdev_ioctl_rdwr(adap, arg);
    }
    case I2C_SMBUS: {
        if (!uptr_ok((void *) (uintptr_t) arg, sizeof(struct i2c_smbus_ioctl_data))) return -EFAULT;
        struct i2c_smbus_ioctl_data smbus;
        memcpy(&smbus, (void *) (uintptr_t) arg, sizeof(smbus));

        struct i2c_client client;
        memset(&client, 0, sizeof(client));
        client.addr = adap->current_slave_addr;
        client.adapter = adap;
        if (client.addr == 0) return -EINVAL;

        switch (smbus.size) {
        case I2C_SMBUS_BYTE_DATA: {
            if (!smbus.data) return -EINVAL;
            if (smbus.read_write == I2C_SMBUS_READ) {
                if (!uptr_ok_w(smbus.data, sizeof(union i2c_smbus_data))) return -EFAULT;
                uint8_t val = 0;
                int ret = i2c_smbus_read_byte_data(&client, smbus.command, &val);
                if (ret < 0) return ret;
                union i2c_smbus_data kdata;
                memset(&kdata, 0, sizeof(kdata));
                kdata.byte = val;
                memcpy(smbus.data, &kdata, sizeof(kdata));
                return 0;
            } else {
                if (!uptr_ok(smbus.data, sizeof(union i2c_smbus_data))) return -EFAULT;
                union i2c_smbus_data kdata;
                memcpy(&kdata, smbus.data, sizeof(kdata));
                return i2c_smbus_write_byte_data(&client, smbus.command, kdata.byte);
            }
        }
        case I2C_SMBUS_WORD_DATA: {
            if (!smbus.data) return -EINVAL;
            if (smbus.read_write == I2C_SMBUS_READ) {
                if (!uptr_ok_w(smbus.data, sizeof(union i2c_smbus_data))) return -EFAULT;
                uint16_t val = 0;
                int ret = i2c_smbus_read_word_data(&client, smbus.command, &val);
                if (ret < 0) return ret;
                union i2c_smbus_data kdata;
                memset(&kdata, 0, sizeof(kdata));
                kdata.word = val;
                memcpy(smbus.data, &kdata, sizeof(kdata));
                return 0;
            } else {
                if (!uptr_ok(smbus.data, sizeof(union i2c_smbus_data))) return -EFAULT;
                union i2c_smbus_data kdata;
                memcpy(&kdata, smbus.data, sizeof(kdata));
                return i2c_smbus_write_word_data(&client, smbus.command, kdata.word);
            }
        }
        default:
            return -EOPNOTSUPP;
        }
    }
    default:
        return -EINVAL;
    }
}

int i2c_add_adapter(struct i2c_adapter *adap) {
    if (!adap || !adap->algo || !adap->algo->master_xfer) {
        return -EINVAL;
    }
    for (int i = 0; i < I2C_MAX_ADAPTERS; i++) {
        if (!g_adapters[i]) {
            adap->nr = i;
            adap->active = true;
            INIT_LIST_HEAD(&adap->userspace_clients);
            g_adapters[i] = adap;

            char dev_path[32];
            dev_path[0] = '/'; dev_path[1] = 'd'; dev_path[2] = 'e'; dev_path[3] = 'v';
            dev_path[4] = '/'; dev_path[5] = 'i'; dev_path[6] = '2'; dev_path[7] = 'c';
            dev_path[8] = '-'; dev_path[9] = (char) ('0' + i); dev_path[10] = '\0';

            vfs_node_t *node = vfs_create_chr(dev_path, i2c_dev_read, i2c_dev_write);
            if (node) {
                node->chr_ioctl = i2c_dev_ioctl;
                node->data = (uint8_t *) adap;
                node->mode = S_IFCHR | 0666;
                node->rdev = VFS_MKDEV(89, (uint32_t) i);
            }

            return 0;
        }
    }
    return -EBUSY;
}

int i2c_del_adapter(struct i2c_adapter *adap) {
    if (!adap) return -EINVAL;
    for (int i = 0; i < I2C_MAX_ADAPTERS; i++) {
        if (g_adapters[i] == adap) {
            adap->active = false;
            g_adapters[i] = NULL;
            return 0;
        }
    }
    return -ENODEV;
}

struct i2c_adapter *i2c_get_adapter(int id) {
    if (id < 0 || id >= I2C_MAX_ADAPTERS) return NULL;
    return g_adapters[id];
}

int i2c_register_driver(void *owner, struct i2c_driver *driver) {
    (void) owner;
    if (!driver) return -EINVAL;
    INIT_LIST_HEAD(&driver->clients);

    for (int i = 0; i < MAX_DRIVERS; i++) {
        if (!g_drivers[i]) {
            g_drivers[i] = driver;
            return 0;
        }
    }
    return -EBUSY;
}

void i2c_del_driver(struct i2c_driver *driver) {
    if (!driver) return;
    for (int i = 0; i < MAX_DRIVERS; i++) {
        if (g_drivers[i] == driver) {
            g_drivers[i] = NULL;
            break;
        }
    }
}

static const struct i2c_device_id *i2c_match_id(const struct i2c_device_id *id, const struct i2c_client *client) {
    if (!id || !client) return NULL;
    while (id->name[0]) {
        if (strcmp(client->name, id->name) == 0) return id;
        id++;
    }
    return NULL;
}

struct i2c_client *i2c_new_client_device(struct i2c_adapter *adap, struct i2c_board_info const *info) {
    if (!adap || !info) return NULL;

    struct i2c_client *client = (struct i2c_client *) kmalloc(sizeof(struct i2c_client));
    if (!client) return NULL;

    memset(client, 0, sizeof(*client));
    client->adapter = adap;
    client->addr = info->addr;
    client->flags = info->flags;
    client->irq = info->irq;
    strncpy(client->name, info->type, I2C_NAME_SIZE - 1);
    client->dev.parent = &adap->dev;
    client->dev.platform_data = info->platform_data;
    INIT_LIST_HEAD(&client->detected);
    INIT_LIST_HEAD(&client->slave);

    list_add_tail(&client->detected, &adap->userspace_clients);

    for (int i = 0; i < MAX_DRIVERS; i++) {
        struct i2c_driver *drv = g_drivers[i];
        if (!drv) continue;

        const struct i2c_device_id *match = i2c_match_id(drv->id_table, client);
        if (match && drv->probe) {
            client->dev.driver = &drv->driver;
            int ret = drv->probe(client, match);
            if (ret == 0) {
                list_add_tail(&client->slave, &drv->clients);
                break;
            }
            client->dev.driver = NULL;
        }
    }

    return client;
}

void i2c_unregister_device(struct i2c_client *client) {
    if (!client) return;

    if (client->dev.driver) {
        struct i2c_driver *drv = to_i2c_driver(client->dev.driver);
        if (drv && drv->remove) {
            drv->remove(client);
        }
        client->dev.driver = NULL;
    }

    if (client->detected.next) list_del(&client->detected);
    if (client->slave.next) list_del(&client->slave);

    kfree(client);
}

int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num) {
    if (!adap || !adap->active || !adap->algo || !adap->algo->master_xfer) {
        return -ENODEV;
    }
    if (!msgs || num <= 0) {
        return -EINVAL;
    }
    return adap->algo->master_xfer(adap, msgs, num);
}

int i2c_master_send(const struct i2c_client *client, const uint8_t *buf, int count) {
    if (!client || !client->adapter || !buf || count <= 0) return -EINVAL;
    struct i2c_msg msg = {
        .addr = client->addr,
        .flags = 0,
        .len = (uint16_t) count,
        .buf = (uint8_t *) buf,
    };
    int ret = i2c_transfer(client->adapter, &msg, 1);
    return (ret == 1) ? count : ret;
}

int i2c_master_recv(const struct i2c_client *client, uint8_t *buf, int count) {
    if (!client || !client->adapter || !buf || count <= 0) return -EINVAL;
    struct i2c_msg msg = {
        .addr = client->addr,
        .flags = I2C_M_RD,
        .len = (uint16_t) count,
        .buf = buf,
    };
    int ret = i2c_transfer(client->adapter, &msg, 1);
    return (ret == 1) ? count : ret;
}

int i2c_smbus_read_byte_data(const struct i2c_client *client, uint8_t command, uint8_t *val) {
    if (!client || !client->adapter || !val) return -EINVAL;
    struct i2c_msg msgs[2] = {
        { .addr = client->addr, .flags = 0, .len = 1, .buf = &command },
        { .addr = client->addr, .flags = I2C_M_RD, .len = 1, .buf = val },
    };
    int ret = i2c_transfer(client->adapter, msgs, 2);
    return (ret == 2) ? 0 : (ret < 0 ? ret : -EIO);
}

int i2c_smbus_write_byte_data(const struct i2c_client *client, uint8_t command, uint8_t val) {
    if (!client || !client->adapter) return -EINVAL;
    uint8_t buf[2] = { command, val };
    struct i2c_msg msg = {
        .addr = client->addr,
        .flags = 0,
        .len = 2,
        .buf = buf,
    };
    int ret = i2c_transfer(client->adapter, &msg, 1);
    return (ret == 1) ? 0 : (ret < 0 ? ret : -EIO);
}

int i2c_smbus_read_word_data(const struct i2c_client *client, uint8_t command, uint16_t *val) {
    if (!client || !client->adapter || !val) return -EINVAL;
    uint8_t buf[2] = { 0, 0 };
    struct i2c_msg msgs[2] = {
        { .addr = client->addr, .flags = 0, .len = 1, .buf = &command },
        { .addr = client->addr, .flags = I2C_M_RD, .len = 2, .buf = buf },
    };
    int ret = i2c_transfer(client->adapter, msgs, 2);
    if (ret != 2) return (ret < 0 ? ret : -EIO);
    *val = ((uint16_t) buf[1] << 8) | buf[0];
    return 0;
}

int i2c_smbus_write_word_data(const struct i2c_client *client, uint8_t command, uint16_t val) {
    if (!client || !client->adapter) return -EINVAL;
    uint8_t buf[3] = { command, (uint8_t) (val & 0xFF), (uint8_t) ((val >> 8) & 0xFF) };
    struct i2c_msg msg = {
        .addr = client->addr,
        .flags = 0,
        .len = 3,
        .buf = buf,
    };
    int ret = i2c_transfer(client->adapter, &msg, 1);
    return (ret == 1) ? 0 : (ret < 0 ? ret : -EIO);
}

int i2c_smbus_read_word_swapped(const struct i2c_client *client, uint8_t command, uint16_t *val) {
    if (!client || !client->adapter || !val) return -EINVAL;
    uint8_t buf[2] = { 0, 0 };
    struct i2c_msg msgs[2] = {
        { .addr = client->addr, .flags = 0, .len = 1, .buf = &command },
        { .addr = client->addr, .flags = I2C_M_RD, .len = 2, .buf = buf },
    };
    int ret = i2c_transfer(client->adapter, msgs, 2);
    if (ret != 2) return (ret < 0 ? ret : -EIO);
    *val = ((uint16_t) buf[0] << 8) | buf[1];
    return 0;
}

int i2c_smbus_write_word_swapped(const struct i2c_client *client, uint8_t command, uint16_t val) {
    if (!client || !client->adapter) return -EINVAL;
    uint8_t buf[3] = { command, (uint8_t) ((val >> 8) & 0xFF), (uint8_t) (val & 0xFF) };
    struct i2c_msg msg = {
        .addr = client->addr,
        .flags = 0,
        .len = 3,
        .buf = buf,
    };
    int ret = i2c_transfer(client->adapter, &msg, 1);
    return (ret == 1) ? 0 : (ret < 0 ? ret : -EIO);
}

int i2c_smbus_read_i2c_block_data(const struct i2c_client *client, uint8_t command, uint8_t length, uint8_t *values) {
    if (!client || !client->adapter || !values || length == 0) return -EINVAL;
    struct i2c_msg msgs[2] = {
        { .addr = client->addr, .flags = 0, .len = 1, .buf = &command },
        { .addr = client->addr, .flags = I2C_M_RD, .len = length, .buf = values },
    };
    int ret = i2c_transfer(client->adapter, msgs, 2);
    return (ret == 2) ? (int) length : (ret < 0 ? ret : -EIO);
}

int i2c_smbus_write_i2c_block_data(const struct i2c_client *client, uint8_t command, uint8_t length, const uint8_t *values) {
    if (!client || !client->adapter || !values || length == 0 || length > 32) return -EINVAL;
    uint8_t buf[33];
    buf[0] = command;
    memcpy(&buf[1], values, length);
    struct i2c_msg msg = {
        .addr = client->addr,
        .flags = 0,
        .len = (uint16_t) (length + 1),
        .buf = buf,
    };
    int ret = i2c_transfer(client->adapter, &msg, 1);
    return (ret == 1) ? 0 : (ret < 0 ? ret : -EIO);
}

#define STUB_MAX_ADDR 128
#define STUB_MAX_REGS 256

static uint16_t g_stub_regs[STUB_MAX_ADDR][STUB_MAX_REGS];
static bool g_stub_reg_valid[STUB_MAX_ADDR][STUB_MAX_REGS];

static void stub_update_state(uint8_t addr) {
    if (addr >= 0x48 && addr <= 0x4B) {
        int16_t temp = (int16_t) g_stub_regs[addr][0x00];
        int16_t offset = (int16_t) g_stub_regs[addr][0x07];
        int16_t high = (int16_t) g_stub_regs[addr][0x02];
        int16_t low = (int16_t) g_stub_regs[addr][0x03];
        int16_t effective = temp + offset;

        g_stub_regs[addr][0x01] &= ~( (1u << 15) | (1u << 14) );
        if (effective > high) {
            g_stub_regs[addr][0x01] |= (1u << 15);
        }
        if (effective < low) {
            g_stub_regs[addr][0x01] |= (1u << 14);
        }
    }
}

static int stub_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num) {
    (void) adap;
    if (!msgs || num <= 0) return -EINVAL;

    for (int i = 0; i < num; i++) {
        if (msgs[i].addr >= STUB_MAX_ADDR) return -ENODEV;
    }

    if (num == 2 && !(msgs[0].flags & I2C_M_RD) && (msgs[1].flags & I2C_M_RD)) {
        if (msgs[0].len < 1 || msgs[1].len < 2) return -EINVAL;
        uint8_t addr = (uint8_t) msgs[0].addr;
        uint8_t reg = msgs[0].buf[0];
        uint16_t val = g_stub_regs[addr][reg];
        if (reg == 0x00 && addr >= 0x48 && addr <= 0x4B) {
            val = (uint16_t) ((int16_t) val + (int16_t) g_stub_regs[addr][0x07]);
        }
        msgs[1].buf[0] = (uint8_t) ((val >> 8) & 0xFF);
        msgs[1].buf[1] = (uint8_t) (val & 0xFF);
        return 2;
    }

    if (num == 1 && !(msgs[0].flags & I2C_M_RD)) {
        if (msgs[0].len == 3) {
            uint8_t addr = (uint8_t) msgs[0].addr;
            uint8_t reg = msgs[0].buf[0];
            uint16_t val = ((uint16_t) msgs[0].buf[1] << 8) | msgs[0].buf[2];
            if (reg == 0x01 && (val & (1u << 1))) {
                g_stub_regs[addr][0x01] = 0x2000 | (0u << 10) | (4u << 7) | (1u << 5);
                g_stub_regs[addr][0x07] = 0x0000;
            } else {
                g_stub_regs[addr][reg] = val;
                g_stub_reg_valid[addr][reg] = true;
            }
            stub_update_state(addr);
            return 1;
        }
    }

    return -EIO;
}

static uint32_t stub_functionality(struct i2c_adapter *adap) {
    (void) adap;
    return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm g_stub_algo = {
    .master_xfer = stub_xfer,
    .functionality = stub_functionality,
};

static struct i2c_adapter g_stub_adapter = {
    .owner = NULL,
    .class = 0,
    .algo = &g_stub_algo,
    .algo_data = NULL,
    .timeout = 100,
    .retries = 3,
    .nr = 0,
    .name = "i2c-stub-0",
    .current_slave_addr = 0,
    .active = false,
};

int i2c_stub_create_adapter(int id) {
    (void) id;
    for (uint8_t a = 0x48; a <= 0x4B; a++) {
        g_stub_regs[a][0x00] = 0x0C80;
        g_stub_regs[a][0x01] = 0x2000 | (0u << 10) | (4u << 7) | (1u << 5);
        g_stub_regs[a][0x02] = 0x1900;
        g_stub_regs[a][0x03] = 0x0000;
        g_stub_regs[a][0x04] = 0x0000;
        g_stub_regs[a][0x05] = 0x0000;
        g_stub_regs[a][0x06] = 0x0000;
        g_stub_regs[a][0x07] = 0x0000;
        g_stub_regs[a][0x08] = 0x0000;
        g_stub_regs[a][0x0F] = 0x0117;
    }
    return i2c_add_adapter(&g_stub_adapter);
}

int i2c_stub_set_word(uint8_t addr, uint8_t reg, uint16_t val) {
    if (addr >= STUB_MAX_ADDR) return -EINVAL;
    g_stub_regs[addr][reg] = val;
    g_stub_reg_valid[addr][reg] = true;
    stub_update_state(addr);
    return 0;
}

int i2c_stub_get_word(uint8_t addr, uint8_t reg, uint16_t *val) {
    if (addr >= STUB_MAX_ADDR || !val) return -EINVAL;
    *val = g_stub_regs[addr][reg];
    return 0;
}
