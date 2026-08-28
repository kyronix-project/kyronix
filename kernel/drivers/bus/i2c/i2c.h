#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif

#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#define I2C_NAME_SIZE 20
#define I2C_MAX_ADAPTERS 8

#define I2C_M_RD 0x0001
#define I2C_M_TEN 0x0010
#define I2C_M_DMA_SAFE 0x0200
#define I2C_M_NOSTART 0x4000
#define I2C_M_REV_DIR_ADDR 0x2000
#define I2C_M_NO_RD_ACK 0x0800
#define I2C_M_RECV_LEN 0x0400
#define I2C_M_STOP 0x8000

#define I2C_SLAVE 0x0703
#define I2C_SLAVE_FORCE 0x0706
#define I2C_TENBIT 0x0704
#define I2C_FUNCS 0x0705
#define I2C_RDWR 0x0707
#define I2C_PEC 0x0708
#define I2C_SMBUS 0x0720

#define I2C_FUNC_I2C 0x00000001
#define I2C_FUNC_10BIT_ADDR 0x00000002
#define I2C_FUNC_PROTOCOL_MANGLING 0x00000004
#define I2C_FUNC_SMBUS_PEC 0x00000008
#define I2C_FUNC_NOSTART 0x00000010
#define I2C_FUNC_SLAVE 0x00000020
#define I2C_FUNC_SMBUS_BLOCK_PROC_CALL 0x00008000
#define I2C_FUNC_SMBUS_QUICK 0x00010000
#define I2C_FUNC_SMBUS_READ_BYTE 0x00020000
#define I2C_FUNC_SMBUS_WRITE_BYTE 0x00040000
#define I2C_FUNC_SMBUS_READ_BYTE_DATA 0x00080000
#define I2C_FUNC_SMBUS_WRITE_BYTE_DATA 0x00100000
#define I2C_FUNC_SMBUS_READ_WORD_DATA 0x00200000
#define I2C_FUNC_SMBUS_WRITE_WORD_DATA 0x00400000
#define I2C_FUNC_SMBUS_PROC_CALL 0x00800000
#define I2C_FUNC_SMBUS_READ_BLOCK_DATA 0x01000000
#define I2C_FUNC_SMBUS_WRITE_BLOCK_DATA 0x02000000
#define I2C_FUNC_SMBUS_READ_I2C_BLOCK 0x04000000
#define I2C_FUNC_SMBUS_WRITE_I2C_BLOCK 0x08000000
#define I2C_FUNC_SMBUS_HOST_NOTIFY 0x10000000

#define I2C_FUNC_SMBUS_EMUL (I2C_FUNC_SMBUS_QUICK | \
                             I2C_FUNC_SMBUS_READ_BYTE | \
                             I2C_FUNC_SMBUS_WRITE_BYTE | \
                             I2C_FUNC_SMBUS_READ_BYTE_DATA | \
                             I2C_FUNC_SMBUS_WRITE_BYTE_DATA | \
                             I2C_FUNC_SMBUS_READ_WORD_DATA | \
                             I2C_FUNC_SMBUS_WRITE_WORD_DATA | \
                             I2C_FUNC_SMBUS_PROC_CALL | \
                             I2C_FUNC_SMBUS_WRITE_BLOCK_DATA | \
                             I2C_FUNC_SMBUS_READ_I2C_BLOCK | \
                             I2C_FUNC_SMBUS_WRITE_I2C_BLOCK)

#define I2C_SMBUS_READ 1
#define I2C_SMBUS_WRITE 0

#define I2C_SMBUS_QUICK 0
#define I2C_SMBUS_BYTE 1
#define I2C_SMBUS_BYTE_DATA 2
#define I2C_SMBUS_WORD_DATA 3
#define I2C_SMBUS_PROC_CALL 4
#define I2C_SMBUS_BLOCK_DATA 5
#define I2C_SMBUS_I2C_BLOCK_BROKEN 6
#define I2C_SMBUS_BLOCK_PROC_CALL 7
#define I2C_SMBUS_I2C_BLOCK_DATA 8

#define I2C_SMBUS_BLOCK_MAX 32

struct list_head {
    struct list_head *next, *prev;
};

static inline void INIT_LIST_HEAD(struct list_head *list) {
    list->next = list;
    list->prev = list;
}

static inline void list_add_tail(struct list_head *new_entry, struct list_head *head) {
    struct list_head *prev = head->prev;
    head->prev = new_entry;
    new_entry->next = head;
    new_entry->prev = prev;
    prev->next = new_entry;
}

static inline void list_del(struct list_head *entry) {
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
    entry->next = NULL;
    entry->prev = NULL;
}

struct mutex {
    uint32_t locked;
    uint32_t count;
    void *owner;
    struct list_head wait_list;
};

struct completion {
    unsigned int done;
    void *wait;
};

struct device_type;
struct class;
struct bus_type;
struct device_driver;

struct device {
    struct device *parent;
    void *p;
    void *driver_data;
    struct bus_type *bus;
    struct device_driver *driver;
    void *platform_data;
    struct list_head devres_head;
    char init_name[64];
    const struct device_type *type;
    struct class *class;
    void (*release)(struct device *dev);
};

struct device_driver {
    const char *name;
    struct bus_type *bus;
    void *owner;
    const struct of_device_id *of_match_table;
    const struct acpi_device_id *acpi_match_table;
    int (*probe)(struct device *dev);
    void (*remove)(struct device *dev);
    void (*shutdown)(struct device *dev);
};

struct i2c_msg {
    uint16_t addr;
    uint16_t flags;
    uint16_t len;
    uint8_t *buf;
};

union i2c_smbus_data {
    uint8_t byte;
    uint16_t word;
    uint8_t block[I2C_SMBUS_BLOCK_MAX + 2];
};

struct i2c_smbus_ioctl_data {
    uint8_t read_write;
    uint8_t command;
    uint32_t size;
    union i2c_smbus_data *data;
};

struct i2c_rdwr_ioctl_data {
    struct i2c_msg *msgs;
    uint32_t nmsgs;
};

struct i2c_device_id {
    char name[I2C_NAME_SIZE];
    uint64_t driver_data;
};

struct i2c_board_info {
    char type[I2C_NAME_SIZE];
    unsigned short flags;
    unsigned short addr;
    const char *dev_name;
    void *platform_data;
    int irq;
};

struct i2c_adapter;
struct i2c_client;

struct i2c_bus_recovery_info {
    int (*recover_bus)(struct i2c_adapter *);
    int (*get_scl)(struct i2c_adapter *);
    void (*set_scl)(struct i2c_adapter *, int);
    int (*get_sda)(struct i2c_adapter *);
    void (*set_sda)(struct i2c_adapter *, int);
    int (*get_bus_free)(struct i2c_adapter *);
    void (*prepare_recovery)(struct i2c_adapter *);
    void (*unprepare_recovery)(struct i2c_adapter *);
    int scl_gpiod;
    int sda_gpiod;
};

struct i2c_timings {
    uint32_t bus_freq_hz;
    uint32_t scl_fall_ns;
    uint32_t scl_rise_ns;
    uint32_t sda_fall_ns;
    uint32_t sda_rise_ns;
    uint32_t scl_int_delay_ns;
    uint32_t sda_hold_ns;
};

struct i2c_lock_operations {
    void (*lock_bus)(struct i2c_adapter *, unsigned int);
    int (*trylock_bus)(struct i2c_adapter *, unsigned int);
    void (*unlock_bus)(struct i2c_adapter *, unsigned int);
};

struct i2c_algorithm {
    int (*master_xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
    int (*smbus_xfer)(struct i2c_adapter *adap, uint16_t addr, unsigned short flags,
                      char read_write, uint8_t command, int size, union i2c_smbus_data *data);
    uint32_t (*functionality)(struct i2c_adapter *adap);
    int (*reg_slave)(struct i2c_client *client);
    int (*unreg_slave)(struct i2c_client *client);
};

struct i2c_adapter {
    void *owner;
    unsigned int class;
    const struct i2c_algorithm *algo;
    void *algo_data;
    const struct i2c_lock_operations *lock_ops;
    struct mutex bus_lock;
    struct mutex mux_lock;
    int timeout;
    int retries;
    struct device dev;
    int nr;
    char name[48];
    struct completion dev_released;
    struct mutex userspace_clients_lock;
    struct list_head userspace_clients;
    struct i2c_bus_recovery_info *bus_recovery_info;
    uint16_t current_slave_addr;
    bool active;
};

struct i2c_driver {
    unsigned int class;
    int (*attach_adapter)(struct i2c_adapter *);
    int (*probe)(struct i2c_client *, const struct i2c_device_id *);
    void (*remove)(struct i2c_client *);
    void (*shutdown)(struct i2c_client *);
    int (*command)(struct i2c_client *, unsigned int, void *);
    struct device_driver driver;
    const struct i2c_device_id *id_table;
    int (*detect)(struct i2c_client *, struct i2c_board_info *);
    const unsigned short *address_list;
    struct list_head clients;
};

#define to_i2c_driver(d) container_of(d, struct i2c_driver, driver)
#define to_i2c_client(d) container_of(d, struct i2c_client, dev)
#define to_i2c_adapter(d) container_of(d, struct i2c_adapter, dev)

struct i2c_client {
    unsigned short flags;
    unsigned short addr;
    char name[I2C_NAME_SIZE];
    struct i2c_adapter *adapter;
    struct device dev;
    int init_irq;
    int irq;
    struct list_head detected;
    struct list_head slave;
    void *driver_data;
};

int i2c_add_adapter(struct i2c_adapter *adap);
int i2c_del_adapter(struct i2c_adapter *adap);
struct i2c_adapter *i2c_get_adapter(int id);

int i2c_register_driver(void *owner, struct i2c_driver *driver);
void i2c_del_driver(struct i2c_driver *driver);

struct i2c_client *i2c_new_client_device(struct i2c_adapter *adap, struct i2c_board_info const *info);
void i2c_unregister_device(struct i2c_client *client);

int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
int i2c_master_send(const struct i2c_client *client, const uint8_t *buf, int count);
int i2c_master_recv(const struct i2c_client *client, uint8_t *buf, int count);

int i2c_smbus_read_byte_data(const struct i2c_client *client, uint8_t command, uint8_t *val);
int i2c_smbus_write_byte_data(const struct i2c_client *client, uint8_t command, uint8_t val);
int i2c_smbus_read_word_data(const struct i2c_client *client, uint8_t command, uint16_t *val);
int i2c_smbus_write_word_data(const struct i2c_client *client, uint8_t command, uint16_t val);
int i2c_smbus_read_word_swapped(const struct i2c_client *client, uint8_t command, uint16_t *val);
int i2c_smbus_write_word_swapped(const struct i2c_client *client, uint8_t command, uint16_t val);
int i2c_smbus_read_i2c_block_data(const struct i2c_client *client, uint8_t command, uint8_t length, uint8_t *values);
int i2c_smbus_write_i2c_block_data(const struct i2c_client *client, uint8_t command, uint8_t length, const uint8_t *values);

static inline void *i2c_get_clientdata(const struct i2c_client *dev) {
    return dev->driver_data;
}

static inline void i2c_set_clientdata(struct i2c_client *dev, void *data) {
    dev->driver_data = data;
}

static inline void *i2c_get_adapdata(const struct i2c_adapter *dev) {
    return dev->algo_data;
}

static inline void i2c_set_adapdata(struct i2c_adapter *dev, void *data) {
    dev->algo_data = data;
}

int i2c_stub_create_adapter(int id);
int i2c_stub_set_word(uint8_t addr, uint8_t reg, uint16_t val);
int i2c_stub_get_word(uint8_t addr, uint8_t reg, uint16_t *val);
