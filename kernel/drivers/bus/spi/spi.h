#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "arch/x86_64/spinlock.h"

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif

#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#define SPI_CPHA 0x01
#define SPI_CPOL 0x02
#define SPI_MODE_0 (0 | 0)
#define SPI_MODE_1 (0 | SPI_CPHA)
#define SPI_MODE_2 (SPI_CPOL | 0)
#define SPI_MODE_3 (SPI_CPOL | SPI_CPHA)

#define SPI_CS_HIGH 0x04
#define SPI_LSB_FIRST 0x08
#define SPI_3WIRE 0x10
#define SPI_LOOP 0x20
#define SPI_NO_CS 0x40
#define SPI_READY 0x80
#define SPI_TX_DUAL 0x100
#define SPI_TX_QUAD 0x200
#define SPI_RX_DUAL 0x400
#define SPI_RX_QUAD 0x800

#define SPI_IOC_MAGIC 'k'
#define SPI_IOC_RD_MODE 0x80016B01
#define SPI_IOC_WR_MODE 0x40016B01
#define SPI_IOC_RD_LSB_FIRST 0x80016B02
#define SPI_IOC_WR_LSB_FIRST 0x40016B02
#define SPI_IOC_RD_BITS_PER_WORD 0x80016B03
#define SPI_IOC_WR_BITS_PER_WORD 0x40016B03
#define SPI_IOC_RD_MAX_SPEED_HZ 0x80046B04
#define SPI_IOC_WR_MAX_SPEED_HZ 0x40046B04
#define SPI_IOC_MESSAGE(N) (0x40006B00 | ((uint32_t)(N * sizeof(struct spi_ioc_transfer)) << 16))

#define SPI_NAME_SIZE 32
#define SPI_MAX_CONTROLLERS 8
#define SPI_MAX_DEVICES_PER_CONTROLLER 8
#define SPI_QUEUE_MAX_MESSAGES 64

struct spi_controller;
struct spi_device;
struct spi_driver;

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

struct spi_transfer {
    const void *tx_buf;
    void *rx_buf;
    unsigned len;
    uint32_t speed_hz;
    uint16_t delay_usecs;
    uint8_t bits_per_word;
    uint8_t cs_change;
    uint8_t tx_nbits;
    uint8_t rx_nbits;
    uint8_t word_delay_usecs;
};

struct spi_message {
    struct spi_transfer *transfers;
    unsigned ntransfers;
    struct spi_device *spi;
    unsigned actual_length;
    int status;
    void (*complete)(void *context);
    void *context;
    volatile bool completed;
};

struct spi_board_info {
    char modalias[SPI_NAME_SIZE];
    const void *platform_data;
    int controller_data;
    int irq;
    uint32_t max_speed_hz;
    uint16_t bus_num;
    uint16_t chip_select;
    uint32_t mode;
};

struct spi_device_id {
    char name[SPI_NAME_SIZE];
    uint64_t driver_data;
};

struct spi_driver {
    const struct spi_device_id *id_table;
    int (*probe)(struct spi_device *spi);
    void (*remove)(struct spi_device *spi);
    void (*shutdown)(struct spi_device *spi);
    char name[SPI_NAME_SIZE];
};

struct spi_device {
    struct spi_controller *controller;
    struct spi_driver *driver;
    char modalias[SPI_NAME_SIZE];
    uint32_t max_speed_hz;
    uint8_t chip_select;
    uint8_t bits_per_word;
    uint32_t mode;
    int irq;
    void *driver_data;
    bool active;
};

struct spi_controller {
    int bus_num;
    uint16_t num_chipselect;
    uint32_t mode_bits;
    uint32_t min_speed_hz;
    uint32_t max_speed_hz;
    uint32_t flags;
    char name[SPI_NAME_SIZE];
    int (*setup)(struct spi_device *spi);
    int (*transfer)(struct spi_device *spi, struct spi_message *mesg);
    int (*transfer_one)(struct spi_controller *ctlr, struct spi_device *spi, struct spi_transfer *t);
    int (*transfer_one_message)(struct spi_controller *ctlr, struct spi_message *mesg);
    void (*set_cs)(struct spi_device *spi, bool enable);
    void *priv;
    bool active;

    spinlock_t lock;
    struct spi_message *queue[SPI_QUEUE_MAX_MESSAGES];
    int queue_head;
    int queue_tail;
    int queue_count;
    bool worker_running;
};

static inline void spi_message_init(struct spi_message *m) {
    m->transfers = NULL;
    m->ntransfers = 0;
    m->spi = NULL;
    m->actual_length = 0;
    m->status = 0;
    m->complete = NULL;
    m->context = NULL;
    m->completed = false;
}

static inline void *spi_get_drvdata(const struct spi_device *spi) {
    return spi ? spi->driver_data : NULL;
}

static inline void spi_set_drvdata(struct spi_device *spi, void *data) {
    if (spi) spi->driver_data = data;
}

static inline void *spi_controller_get_devdata(const struct spi_controller *ctlr) {
    return ctlr ? ctlr->priv : NULL;
}

static inline void spi_controller_set_devdata(struct spi_controller *ctlr, void *data) {
    if (ctlr) ctlr->priv = data;
}

int spi_register_controller(struct spi_controller *ctlr);
void spi_unregister_controller(struct spi_controller *ctlr);
struct spi_controller *spi_get_controller(int bus_num);

int spi_register_driver(struct spi_driver *sdrv);
void spi_unregister_driver(struct spi_driver *sdrv);

struct spi_device *spi_new_device(struct spi_controller *ctlr, struct spi_board_info *chip);
void spi_unregister_device(struct spi_device *spi);

int spi_setup(struct spi_device *spi);
int spi_async(struct spi_device *spi, struct spi_message *message);
int spi_sync(struct spi_device *spi, struct spi_message *message);
int spi_sync_transfer(struct spi_device *spi, struct spi_transfer *xfers, unsigned int num_xfers);

int spi_write(struct spi_device *spi, const void *buf, size_t len);
int spi_read(struct spi_device *spi, void *buf, size_t len);
int spi_write_then_read(struct spi_device *spi, const void *txbuf, unsigned n_tx, void *rxbuf, unsigned n_rx);

int spi_stub_create_controller(int bus_num);
