#include "spi.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/spinlock.h"
#include "fs/vfs.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "syscall/syscall.h"

#define EINVAL 22
#define ENODEV 19
#define EBUSY 16
#define EIO 5
#define ENOMEM 12
#define EFAULT 14

static spinlock_t g_spi_core_lock = SPINLOCK_INIT;
static struct spi_controller *g_controllers[SPI_MAX_CONTROLLERS];
static struct spi_device *g_devices[SPI_MAX_CONTROLLERS][SPI_MAX_DEVICES_PER_CONTROLLER];
static struct spi_driver *g_drivers[32];
static struct spi_controller g_stub_spi_controllers[SPI_MAX_CONTROLLERS];

static void spi_delay_us(uint32_t us) {
    if (us == 0) return;
    for (uint32_t i = 0; i < us; i++) {
        io_wait();
    }
}

static uint8_t reverse_bits8(uint8_t b) {
    b = (uint8_t) (((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t) (((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t) (((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

static uint16_t reverse_bits16(uint16_t w) {
    uint8_t hi = reverse_bits8((uint8_t) ((w >> 8) & 0xFF));
    uint8_t lo = reverse_bits8((uint8_t) (w & 0xFF));
    return (uint16_t) (((uint16_t) lo << 8) | hi);
}

static uint32_t reverse_bits32(uint32_t d) {
    uint16_t hi = reverse_bits16((uint16_t) ((d >> 16) & 0xFFFF));
    uint16_t lo = reverse_bits16((uint16_t) (d & 0xFFFF));
    return ((uint32_t) lo << 16) | hi;
}

static void spi_set_cs_level(struct spi_device *spi, bool active) {
    if (!spi || !spi->controller) return;
    struct spi_controller *ctlr = spi->controller;
    if (!ctlr->set_cs) return;

    if (spi->mode & SPI_NO_CS) return;

    bool level = active;
    if (spi->mode & SPI_CS_HIGH) {
        level = active;
    } else {
        level = !active;
    }

    ctlr->set_cs(spi, level);
}

static int spi_transfer_one_message_default(struct spi_controller *ctlr, struct spi_message *mesg) {
    struct spi_device *spi = mesg->spi;
    if (!spi || !ctlr) return -EINVAL;

    spi_set_cs_level(spi, true);

    unsigned total_len = 0;
    int status = 0;

    for (unsigned i = 0; i < mesg->ntransfers; i++) {
        struct spi_transfer *t = &mesg->transfers[i];

        if (ctlr->transfer_one) {
            status = ctlr->transfer_one(ctlr, spi, t);
            if (status < 0) break;
        } else {
            if (t->rx_buf && t->tx_buf) {
                memcpy(t->rx_buf, t->tx_buf, t->len);
            } else if (t->rx_buf) {
                memset(t->rx_buf, 0, t->len);
            }
        }
        total_len += t->len;

        if (t->delay_usecs > 0) {
            spi_delay_us(t->delay_usecs);
        }

        if (t->cs_change) {
            spi_set_cs_level(spi, false);
            spi_delay_us(1);
            if (i + 1 < mesg->ntransfers) {
                spi_set_cs_level(spi, true);
            }
        }
    }

    spi_set_cs_level(spi, false);

    mesg->actual_length = total_len;
    mesg->status = status;
    mesg->completed = true;
    if (mesg->complete) mesg->complete(mesg->context);
    return status;
}

static void spi_pump_messages(struct spi_controller *ctlr) {
    for (;;) {
        struct spi_message *msg = NULL;
        spin_lock(&ctlr->lock);
        if (ctlr->queue_count > 0) {
            msg = ctlr->queue[ctlr->queue_head];
            ctlr->queue[ctlr->queue_head] = NULL;
            ctlr->queue_head = (ctlr->queue_head + 1) % SPI_QUEUE_MAX_MESSAGES;
            ctlr->queue_count--;
        } else {
            ctlr->worker_running = false;
            spin_unlock(&ctlr->lock);
            break;
        }
        spin_unlock(&ctlr->lock);

        if (msg) {
            if (ctlr->transfer_one_message) {
                ctlr->transfer_one_message(ctlr, msg);
            } else {
                spi_transfer_one_message_default(ctlr, msg);
            }
        }
    }
}

int spi_async(struct spi_device *spi, struct spi_message *message) {
    if (!spi || !message || !spi->controller) return -EINVAL;
    struct spi_controller *ctlr = spi->controller;

    message->spi = spi;
    message->status = 0;
    message->actual_length = 0;
    message->completed = false;

    bool start_worker = false;
    spin_lock(&ctlr->lock);
    if (ctlr->queue_count >= SPI_QUEUE_MAX_MESSAGES) {
        spin_unlock(&ctlr->lock);
        return -EBUSY;
    }

    ctlr->queue[ctlr->queue_tail] = message;
    ctlr->queue_tail = (ctlr->queue_tail + 1) % SPI_QUEUE_MAX_MESSAGES;
    ctlr->queue_count++;

    if (!ctlr->worker_running) {
        ctlr->worker_running = true;
        start_worker = true;
    }
    spin_unlock(&ctlr->lock);

    if (start_worker) {
        spi_pump_messages(ctlr);
    }

    return 0;
}

int spi_register_controller(struct spi_controller *ctlr) {
    if (!ctlr) return -EINVAL;
    if (ctlr->bus_num < 0 || ctlr->bus_num >= SPI_MAX_CONTROLLERS) return -EINVAL;

    spin_lock(&g_spi_core_lock);
    if (g_controllers[ctlr->bus_num]) {
        spin_unlock(&g_spi_core_lock);
        return -EBUSY;
    }

    if (!ctlr->transfer_one_message && !ctlr->transfer) {
        ctlr->transfer_one_message = spi_transfer_one_message_default;
    }

    ctlr->lock.lock = 0;
    ctlr->queue_head = 0;
    ctlr->queue_tail = 0;
    ctlr->queue_count = 0;
    ctlr->worker_running = false;
    ctlr->active = true;
    g_controllers[ctlr->bus_num] = ctlr;
    spin_unlock(&g_spi_core_lock);
    return 0;
}

void spi_unregister_controller(struct spi_controller *ctlr) {
    if (!ctlr) return;
    if (ctlr->bus_num < 0 || ctlr->bus_num >= SPI_MAX_CONTROLLERS) return;

    struct spi_device *to_free[SPI_MAX_DEVICES_PER_CONTROLLER];
    memset(to_free, 0, sizeof(to_free));

    spin_lock(&g_spi_core_lock);
    for (int cs = 0; cs < SPI_MAX_DEVICES_PER_CONTROLLER; cs++) {
        to_free[cs] = g_devices[ctlr->bus_num][cs];
    }
    ctlr->active = false;
    g_controllers[ctlr->bus_num] = NULL;
    spin_unlock(&g_spi_core_lock);

    for (int cs = 0; cs < SPI_MAX_DEVICES_PER_CONTROLLER; cs++) {
        if (to_free[cs]) {
            spi_unregister_device(to_free[cs]);
        }
    }
}

struct spi_controller *spi_get_controller(int bus_num) {
    if (bus_num < 0 || bus_num >= SPI_MAX_CONTROLLERS) return NULL;
    spin_lock(&g_spi_core_lock);
    struct spi_controller *ctlr = g_controllers[bus_num];
    spin_unlock(&g_spi_core_lock);
    return ctlr;
}

static struct spi_driver *spi_find_matching_driver(struct spi_device *spi) {
    if (!spi) return NULL;
    for (int i = 0; i < 32; i++) {
        struct spi_driver *drv = g_drivers[i];
        if (!drv) continue;

        if (drv->id_table) {
            const struct spi_device_id *id = drv->id_table;
            while (id->name[0]) {
                if (strncmp(id->name, spi->modalias, SPI_NAME_SIZE) == 0) {
                    return drv;
                }
                id++;
            }
        } else if (strncmp(drv->name, spi->modalias, SPI_NAME_SIZE) == 0) {
            return drv;
        }
    }
    return NULL;
}

int spi_register_driver(struct spi_driver *sdrv) {
    if (!sdrv) return -EINVAL;
    struct spi_device *match_list[SPI_MAX_CONTROLLERS * SPI_MAX_DEVICES_PER_CONTROLLER];
    int match_count = 0;

    spin_lock(&g_spi_core_lock);
    int slot = -1;
    for (int i = 0; i < 32; i++) {
        if (!g_drivers[i]) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        spin_unlock(&g_spi_core_lock);
        return -EBUSY;
    }

    g_drivers[slot] = sdrv;

    for (int b = 0; b < SPI_MAX_CONTROLLERS; b++) {
        for (int cs = 0; cs < SPI_MAX_DEVICES_PER_CONTROLLER; cs++) {
            struct spi_device *dev = g_devices[b][cs];
            if (dev && !dev->driver) {
                bool match = false;
                if (sdrv->id_table) {
                    const struct spi_device_id *id = sdrv->id_table;
                    while (id->name[0]) {
                        if (strncmp(id->name, dev->modalias, SPI_NAME_SIZE) == 0) {
                            match = true;
                            break;
                        }
                        id++;
                    }
                } else if (strncmp(sdrv->name, dev->modalias, SPI_NAME_SIZE) == 0) {
                    match = true;
                }
                if (match) {
                    dev->driver = sdrv;
                    match_list[match_count++] = dev;
                }
            }
        }
    }
    spin_unlock(&g_spi_core_lock);

    for (int i = 0; i < match_count; i++) {
        if (sdrv->probe) {
            int ret = sdrv->probe(match_list[i]);
            if (ret < 0) {
                spin_lock(&g_spi_core_lock);
                match_list[i]->driver = NULL;
                match_list[i]->driver_data = NULL;
                spin_unlock(&g_spi_core_lock);
            }
        }
    }

    return 0;
}

void spi_unregister_driver(struct spi_driver *sdrv) {
    if (!sdrv) return;
    struct spi_device *matched[SPI_MAX_CONTROLLERS * SPI_MAX_DEVICES_PER_CONTROLLER];
    int matched_count = 0;

    spin_lock(&g_spi_core_lock);
    for (int i = 0; i < 32; i++) {
        if (g_drivers[i] == sdrv) {
            for (int b = 0; b < SPI_MAX_CONTROLLERS; b++) {
                for (int cs = 0; cs < SPI_MAX_DEVICES_PER_CONTROLLER; cs++) {
                    struct spi_device *dev = g_devices[b][cs];
                    if (dev && dev->driver == sdrv) {
                        matched[matched_count++] = dev;
                    }
                }
            }
            g_drivers[i] = NULL;
            break;
        }
    }
    spin_unlock(&g_spi_core_lock);

    for (int i = 0; i < matched_count; i++) {
        if (sdrv->remove) sdrv->remove(matched[i]);
        matched[i]->driver = NULL;
        matched[i]->driver_data = NULL;
    }
}

struct spi_device *spi_new_device(struct spi_controller *ctlr, struct spi_board_info *chip) {
    if (!ctlr || !chip) return NULL;
    if (chip->chip_select >= SPI_MAX_DEVICES_PER_CONTROLLER) return NULL;

    spin_lock(&g_spi_core_lock);
    if (g_devices[ctlr->bus_num][chip->chip_select]) {
        spin_unlock(&g_spi_core_lock);
        return NULL;
    }

    struct spi_device *spi = (struct spi_device *) kmalloc(sizeof(struct spi_device));
    if (!spi) {
        spin_unlock(&g_spi_core_lock);
        return NULL;
    }
    memset(spi, 0, sizeof(*spi));

    spi->controller = ctlr;
    spi->chip_select = (uint8_t) chip->chip_select;
    spi->max_speed_hz = chip->max_speed_hz;
    spi->mode = chip->mode;
    spi->irq = chip->irq;
    spi->bits_per_word = 8;
    spi->active = true;
    strncpy(spi->modalias, chip->modalias, SPI_NAME_SIZE - 1);

    g_devices[ctlr->bus_num][chip->chip_select] = spi;

    struct spi_driver *matched_drv = spi_find_matching_driver(spi);
    if (matched_drv) {
        spi->driver = matched_drv;
    }
    spin_unlock(&g_spi_core_lock);

    if (ctlr->setup) {
        ctlr->setup(spi);
    }

    if (matched_drv && matched_drv->probe) {
        int ret = matched_drv->probe(spi);
        if (ret < 0) {
            spin_lock(&g_spi_core_lock);
            spi->driver = NULL;
            spi->driver_data = NULL;
            spin_unlock(&g_spi_core_lock);
        }
    }

    return spi;
}

void spi_unregister_device(struct spi_device *spi) {
    if (!spi || !spi->controller) return;
    int b = spi->controller->bus_num;
    int cs = spi->chip_select;

    if (spi->driver && spi->driver->remove) {
        spi->driver->remove(spi);
        spi->driver = NULL;
        spi->driver_data = NULL;
    }

    spin_lock(&g_spi_core_lock);
    if (b >= 0 && b < SPI_MAX_CONTROLLERS && cs >= 0 && cs < SPI_MAX_DEVICES_PER_CONTROLLER) {
        g_devices[b][cs] = NULL;
    }
    spin_unlock(&g_spi_core_lock);

    kfree(spi);
}

int spi_setup(struct spi_device *spi) {
    if (!spi || !spi->controller) return -EINVAL;
    if (spi->bits_per_word != 8 && spi->bits_per_word != 16 && spi->bits_per_word != 32) {
        return -EINVAL;
    }
    if (spi->controller->setup) {
        return spi->controller->setup(spi);
    }
    return 0;
}

int spi_sync(struct spi_device *spi, struct spi_message *message) {
    if (!spi || !message || !spi->controller) return -EINVAL;
    if (spi->controller->transfer) {
        message->spi = spi;
        return spi->controller->transfer(spi, message);
    }

    int ret = spi_async(spi, message);
    if (ret < 0) return ret;

    while (!message->completed) {
        __asm__ volatile("sti; pause" ::: "memory");
    }
    return message->status;
}

int spi_sync_transfer(struct spi_device *spi, struct spi_transfer *xfers, unsigned int num_xfers) {
    if (!spi || !xfers || num_xfers == 0) return -EINVAL;
    struct spi_message msg;
    spi_message_init(&msg);
    msg.transfers = xfers;
    msg.ntransfers = num_xfers;
    return spi_sync(spi, &msg);
}

int spi_write(struct spi_device *spi, const void *buf, size_t len) {
    struct spi_transfer t;
    memset(&t, 0, sizeof(t));
    t.tx_buf = buf;
    t.len = (unsigned) len;
    return spi_sync_transfer(spi, &t, 1);
}

int spi_read(struct spi_device *spi, void *buf, size_t len) {
    struct spi_transfer t;
    memset(&t, 0, sizeof(t));
    t.rx_buf = buf;
    t.len = (unsigned) len;
    return spi_sync_transfer(spi, &t, 1);
}

int spi_write_then_read(struct spi_device *spi, const void *txbuf, unsigned n_tx, void *rxbuf, unsigned n_rx) {
    struct spi_transfer t[2];
    memset(t, 0, sizeof(t));
    t[0].tx_buf = txbuf;
    t[0].len = n_tx;
    t[1].rx_buf = rxbuf;
    t[1].len = n_rx;
    return spi_sync_transfer(spi, t, 2);
}

static struct spi_device *spidev_get_or_create_device(int bus, int cs) {
    if (bus < 0 || bus >= SPI_MAX_CONTROLLERS || cs < 0 || cs >= SPI_MAX_DEVICES_PER_CONTROLLER) {
        return NULL;
    }

    spin_lock(&g_spi_core_lock);
    struct spi_controller *ctlr = g_controllers[bus];
    if (!ctlr) {
        spin_unlock(&g_spi_core_lock);
        return NULL;
    }

    struct spi_device *spi = g_devices[bus][cs];
    spin_unlock(&g_spi_core_lock);

    if (!spi) {
        struct spi_board_info chip;
        memset(&chip, 0, sizeof(chip));
        chip.bus_num = (uint16_t) bus;
        chip.chip_select = (uint16_t) cs;
        strncpy(chip.modalias, "spidev", sizeof(chip.modalias) - 1);
        spi = spi_new_device(ctlr, &chip);
    }
    return spi;
}

static int64_t spidev_read(vfs_node_t *node, char *buf, uint64_t len, uint64_t off) {
    (void) off;
    if (!buf || len == 0 || len > 65536) return -EINVAL;
    if (!uptr_ok_w(buf, len)) return -EFAULT;

    int bus = (int) (node->rdev >> 8) & 0xFF;
    int cs = (int) (node->rdev & 0xFF);

    struct spi_device *spi = spidev_get_or_create_device(bus, cs);
    if (!spi) return -ENODEV;

    void *kbuf = kmalloc(len);
    if (!kbuf) return -ENOMEM;

    int ret = spi_read(spi, kbuf, (size_t) len);
    if (ret >= 0) {
        memcpy(buf, kbuf, len);
        ret = (int) len;
    }

    kfree(kbuf);
    return ret;
}

static int64_t spidev_write(vfs_node_t *node, const char *buf, uint64_t len, uint64_t off) {
    (void) off;
    if (!buf || len == 0 || len > 65536) return -EINVAL;
    if (!uptr_ok(buf, len)) return -EFAULT;

    int bus = (int) (node->rdev >> 8) & 0xFF;
    int cs = (int) (node->rdev & 0xFF);

    struct spi_device *spi = spidev_get_or_create_device(bus, cs);
    if (!spi) return -ENODEV;

    void *kbuf = kmalloc(len);
    if (!kbuf) return -ENOMEM;

    memcpy(kbuf, buf, len);
    int ret = spi_write(spi, kbuf, (size_t) len);
    if (ret >= 0) {
        ret = (int) len;
    }

    kfree(kbuf);
    return ret;
}

static int64_t spidev_ioctl(vfs_node_t *node, uint64_t cmd, uint64_t arg) {
    int bus = (int) (node->rdev >> 8) & 0xFF;
    int cs = (int) (node->rdev & 0xFF);

    struct spi_device *spi = spidev_get_or_create_device(bus, cs);
    if (!spi) return -ENODEV;

    switch ((uint32_t) cmd) {
    case SPI_IOC_RD_MODE:
        if (!uptr_ok_w((void *) (uintptr_t) arg, sizeof(uint8_t))) return -EFAULT;
        *(uint8_t *) (uintptr_t) arg = (uint8_t) spi->mode;
        return 0;
    case SPI_IOC_WR_MODE:
        if (!uptr_ok((const void *) (uintptr_t) arg, sizeof(uint8_t))) return -EFAULT;
        spi->mode = *(uint8_t *) (uintptr_t) arg;
        return spi_setup(spi);
    case SPI_IOC_RD_LSB_FIRST:
        if (!uptr_ok_w((void *) (uintptr_t) arg, sizeof(uint8_t))) return -EFAULT;
        *(uint8_t *) (uintptr_t) arg = (spi->mode & SPI_LSB_FIRST) ? 1 : 0;
        return 0;
    case SPI_IOC_WR_LSB_FIRST:
        if (!uptr_ok((const void *) (uintptr_t) arg, sizeof(uint8_t))) return -EFAULT;
        if (*(uint8_t *) (uintptr_t) arg) spi->mode |= SPI_LSB_FIRST;
        else spi->mode &= ~SPI_LSB_FIRST;
        return spi_setup(spi);
    case SPI_IOC_RD_BITS_PER_WORD:
        if (!uptr_ok_w((void *) (uintptr_t) arg, sizeof(uint8_t))) return -EFAULT;
        *(uint8_t *) (uintptr_t) arg = spi->bits_per_word;
        return 0;
    case SPI_IOC_WR_BITS_PER_WORD:
        if (!uptr_ok((const void *) (uintptr_t) arg, sizeof(uint8_t))) return -EFAULT;
        spi->bits_per_word = *(uint8_t *) (uintptr_t) arg;
        return spi_setup(spi);
    case SPI_IOC_RD_MAX_SPEED_HZ:
        if (!uptr_ok_w((void *) (uintptr_t) arg, sizeof(uint32_t))) return -EFAULT;
        *(uint32_t *) (uintptr_t) arg = spi->max_speed_hz;
        return 0;
    case SPI_IOC_WR_MAX_SPEED_HZ:
        if (!uptr_ok((const void *) (uintptr_t) arg, sizeof(uint32_t))) return -EFAULT;
        spi->max_speed_hz = *(uint32_t *) (uintptr_t) arg;
        return spi_setup(spi);
    default:
        break;
    }

    if (((uint32_t) cmd & 0xFFFF) == 0x6B00 || (((uint32_t) cmd >> 8) & 0xFF) == SPI_IOC_MAGIC) {
        uint32_t size = ((uint32_t) cmd >> 16) & 0x3FFF;
        uint32_t nxfers = size / sizeof(struct spi_ioc_transfer);
        if (nxfers == 0 || nxfers > 16) return -EINVAL;

        if (!uptr_ok((const void *) (uintptr_t) arg, size)) return -EFAULT;

        struct spi_ioc_transfer kxfers[16];
        memcpy(kxfers, (const void *) (uintptr_t) arg, size);

        struct spi_transfer transfers[16];
        uint8_t *kbufs_tx[16];
        uint8_t *kbufs_rx[16];
        memset(transfers, 0, sizeof(transfers));
        memset(kbufs_tx, 0, sizeof(kbufs_tx));
        memset(kbufs_rx, 0, sizeof(kbufs_rx));

        int ret = 0;

        for (uint32_t i = 0; i < nxfers; i++) {
            uint32_t len = kxfers[i].len;
            if (len > 4096) {
                ret = -EINVAL;
                break;
            }

            uint8_t bpw = kxfers[i].bits_per_word ? kxfers[i].bits_per_word : spi->bits_per_word;
            if (bpw == 16 && (len % 2 != 0)) {
                ret = -EINVAL;
                break;
            }
            if (bpw == 32 && (len % 4 != 0)) {
                ret = -EINVAL;
                break;
            }

            if (kxfers[i].tx_buf) {
                if (!uptr_ok((const void *) (uintptr_t) kxfers[i].tx_buf, len)) {
                    ret = -EFAULT;
                    break;
                }
                kbufs_tx[i] = (uint8_t *) kmalloc(len);
                if (!kbufs_tx[i]) {
                    ret = -ENOMEM;
                    break;
                }
                memcpy(kbufs_tx[i], (const void *) (uintptr_t) kxfers[i].tx_buf, len);
                transfers[i].tx_buf = kbufs_tx[i];
            }

            if (kxfers[i].rx_buf) {
                if (!uptr_ok_w((void *) (uintptr_t) kxfers[i].rx_buf, len)) {
                    ret = -EFAULT;
                    break;
                }
                kbufs_rx[i] = (uint8_t *) kmalloc(len);
                if (!kbufs_rx[i]) {
                    ret = -ENOMEM;
                    break;
                }
                memset(kbufs_rx[i], 0, len);
                transfers[i].rx_buf = kbufs_rx[i];
            }

            transfers[i].len = len;
            transfers[i].speed_hz = kxfers[i].speed_hz;
            transfers[i].delay_usecs = kxfers[i].delay_usecs;
            transfers[i].bits_per_word = bpw;
            transfers[i].cs_change = kxfers[i].cs_change;
            transfers[i].word_delay_usecs = kxfers[i].word_delay_usecs;
        }

        if (ret == 0) {
            ret = spi_sync_transfer(spi, transfers, nxfers);
            if (ret >= 0) {
                for (uint32_t i = 0; i < nxfers; i++) {
                    if (kxfers[i].rx_buf && kbufs_rx[i]) {
                        memcpy((void *) (uintptr_t) kxfers[i].rx_buf, kbufs_rx[i], kxfers[i].len);
                    }
                }
                ret = (int) size;
            }
        }

        for (uint32_t i = 0; i < nxfers; i++) {
            if (kbufs_tx[i]) kfree(kbufs_tx[i]);
            if (kbufs_rx[i]) kfree(kbufs_rx[i]);
        }

        return ret;
    }

    return -EINVAL;
}

static int stub_spi_transfer_one(struct spi_controller *ctlr, struct spi_device *spi, struct spi_transfer *t) {
    (void) ctlr;
    uint32_t mode = spi ? spi->mode : 0;
    bool lsb = (mode & SPI_LSB_FIRST) != 0;
    uint8_t bpw = t->bits_per_word ? t->bits_per_word : (spi ? spi->bits_per_word : 8);

    if (bpw == 16 && (t->len % 2 != 0)) return -EINVAL;
    if (bpw == 32 && (t->len % 4 != 0)) return -EINVAL;

    if (t->rx_buf && t->tx_buf) {
        if (bpw == 16) {
            unsigned words = t->len / 2;
            const uint8_t *src8 = (const uint8_t *) t->tx_buf;
            uint8_t *dst8 = (uint8_t *) t->rx_buf;
            for (unsigned i = 0; i < words; i++) {
                uint16_t val;
                memcpy(&val, src8 + i * 2, 2);
                if (lsb) val = reverse_bits16(val);
                memcpy(dst8 + i * 2, &val, 2);
                if (t->word_delay_usecs > 0) spi_delay_us(t->word_delay_usecs);
            }
        } else if (bpw == 32) {
            unsigned dwords = t->len / 4;
            const uint8_t *src8 = (const uint8_t *) t->tx_buf;
            uint8_t *dst8 = (uint8_t *) t->rx_buf;
            for (unsigned i = 0; i < dwords; i++) {
                uint32_t val;
                memcpy(&val, src8 + i * 4, 4);
                if (lsb) val = reverse_bits32(val);
                memcpy(dst8 + i * 4, &val, 4);
                if (t->word_delay_usecs > 0) spi_delay_us(t->word_delay_usecs);
            }
        } else {
            const uint8_t *src = (const uint8_t *) t->tx_buf;
            uint8_t *dst = (uint8_t *) t->rx_buf;
            for (unsigned i = 0; i < t->len; i++) {
                uint8_t val = src[i];
                if (lsb) val = reverse_bits8(val);
                dst[i] = val;
                if (t->word_delay_usecs > 0) spi_delay_us(t->word_delay_usecs);
            }
        }
    } else if (t->rx_buf) {
        memset(t->rx_buf, 0xFF, t->len);
    }
    return 0;
}

int spi_stub_create_controller(int bus_num) {
    if (bus_num < 0 || bus_num >= SPI_MAX_CONTROLLERS) return -EINVAL;

    struct spi_controller *ctlr = &g_stub_spi_controllers[bus_num];
    memset(ctlr, 0, sizeof(*ctlr));
    ctlr->bus_num = bus_num;
    ctlr->num_chipselect = 4;
    ctlr->mode_bits = SPI_MODE_3 | SPI_CS_HIGH | SPI_LSB_FIRST | SPI_LOOP;
    ctlr->max_speed_hz = 50000000;
    ctlr->min_speed_hz = 1000;
    ctlr->lock.lock = 0;
    snprintf(ctlr->name, sizeof(ctlr->name), "spi-stub-%d", bus_num);
    ctlr->transfer_one = stub_spi_transfer_one;

    int ret = spi_register_controller(ctlr);
    if (ret < 0) return ret;

    for (int cs = 0; cs < 2; cs++) {
        char devpath[32];
        snprintf(devpath, sizeof(devpath), "/dev/spidev%d.%d", bus_num, cs);
        vfs_node_t *spidev = vfs_create_chr(devpath, spidev_read, spidev_write);
        if (spidev) {
            spidev->chr_ioctl = spidev_ioctl;
            spidev->rdev = ((uint32_t) bus_num << 8) | (uint32_t) cs;
        }
    }

    return 0;
}
