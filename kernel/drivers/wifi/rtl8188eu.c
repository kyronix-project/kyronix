#include "ieee80211.h"
#include "../../fs/vfs.h"
#include "../../fs/vfs_internal.h"
#include "../../lib/log.h"
#include "../../lib/printf.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../usb/usb.h"

#define RTL8188EU_REQT_READ 0xC0
#define RTL8188EU_REQT_WRITE 0x40
#define RTL8188EU_REQ_GET_REG 0x05
#define RTL8188EU_REQ_SET_REG 0x05

#define RTL8188EU_REG_SYS_FUNC_EN 0x0002
#define RTL8188EU_REG_APS_FSMCO 0x0004
#define RTL8188EU_REG_SYS_CLKR 0x0008
#define RTL8188EU_REG_9346CR 0x000A
#define RTL8188EU_REG_EE_VPD 0x000C
#define RTL8188EU_REG_AFE_MISC 0x0010
#define RTL8188EU_REG_SPS0_CTRL 0x0011
#define RTL8188EU_REG_SPS0_CTRL_6 0x0016
#define RTL8188EU_REG_SPS_OCP_CFG 0x0018
#define RTL8188EU_REG_RSV_CTRL 0x001C
#define RTL8188EU_REG_RF_CTRL 0x001F
#define RTL8188EU_REG_LDOA15_CTRL 0x0020
#define RTL8188EU_REG_LDOV12D_CTRL 0x0021
#define RTL8188EU_REG_LDOHCI12_CTRL 0x0022
#define RTL8188EU_REG_LPLDO_CTRL 0x0023
#define RTL8188EU_REG_AFE_XTAL_CTRL 0x0024
#define RTL8188EU_REG_AFE_LDO_CTRL 0x0027
#define RTL8188EU_REG_AFE_XTAL_CTRL_EXT 0x0028
#define RTL8188EU_REG_AFE_PLL_CTRL 0x002C
#define RTL8188EU_REG_MAC_PHY_CTRL 0x003C
#define RTL8188EU_REG_EFUSE_CTRL 0x0030
#define RTL8188EU_REG_EFUSE_TEST 0x0034
#define RTL8188EU_REG_PWR_DATA 0x0038
#define RTL8188EU_REG_CAL_TIMER 0x003C
#define RTL8188EU_REG_ACLK_MON 0x003E
#define RTL8188EU_REG_GPIO_MUXCFG 0x0040
#define RTL8188EU_REG_GPIO_IO_SEL 0x0042
#define RTL8188EU_REG_MAC_PINMUX_CFG 0x0043
#define RTL8188EU_REG_GPIO_PIN_CTRL 0x0044
#define RTL8188EU_REG_GPIO_INTM 0x0048
#define RTL8188EU_REG_LEDCFG0 0x004C
#define RTL8188EU_REG_LEDCFG1 0x004D
#define RTL8188EU_REG_LEDCFG2 0x004E
#define RTL8188EU_REG_LEDCFG3 0x004F
#define RTL8188EU_REG_FSIMR 0x0050
#define RTL8188EU_REG_FSISR 0x0054
#define RTL8188EU_REG_HSIMR 0x0058
#define RTL8188EU_REG_HSISR 0x005C
#define RTL8188EU_REG_GPIO_EXT_CTRL 0x0060
#define RTL8188EU_REG_PAD_CTRL1 0x0064
#define RTL8188EU_REG_MULTI_FUNC_CTRL 0x0068
#define RTL8188EU_REG_GPIO_STATUS 0x006C
#define RTL8188EU_REG_SDIO_CTRL 0x0070
#define RTL8188EU_REG_OPT_CTRL 0x0074
#define RTL8188EU_REG_AFE_XTAL_CTRL_EXT 0x0078
#define RTL8188EU_REG_MCUFWDL 0x0080
#define RTL8188EU_REG_HMIMR 0x0084
#define RTL8188EU_REG_HMISR 0x0088
#define RTL8188EU_REG_HIMRE 0x008C
#define RTL8188EU_REG_HIMR 0x0090
#define RTL8188EU_REG_HISR 0x0094
#define RTL8188EU_REG_HIMR2 0x0098
#define RTL8188EU_REG_HISR2 0x009C
#define RTL8188EU_REG_HIMR3 0x00A0
#define RTL8188EU_REG_HISR3 0x00A4
#define RTL8188EU_REG_EFUSE_BURNER 0x00A8
#define RTL8188EU_REG_EFUSE_TEST2 0x00AC
#define RTL8188EU_REG_EFUSE_TEST3 0x00B0
#define RTL8188EU_REG_EFUSE_TEST4 0x00B4
#define RTL8188EU_REG_EFUSE_TEST5 0x00B8
#define RTL8188EU_REG_EFUSE_TEST6 0x00BC
#define RTL8188EU_REG_EFUSE_TEST7 0x00C0
#define RTL8188EU_REG_EFUSE_TEST8 0x00C4
#define RTL8188EU_REG_EFUSE_TEST9 0x00C8
#define RTL8188EU_REG_EFUSE_TEST10 0x00CC
#define RTL8188EU_REG_EFUSE_TEST11 0x00D0
#define RTL8188EU_REG_EFUSE_TEST12 0x00D4
#define RTL8188EU_REG_EFUSE_TEST13 0x00D8
#define RTL8188EU_REG_EFUSE_TEST14 0x00DC
#define RTL8188EU_REG_EFUSE_TEST15 0x00E0
#define RTL8188EU_REG_EFUSE_TEST16 0x00E4
#define RTL8188EU_REG_EFUSE_TEST17 0x00E8
#define RTL8188EU_REG_EFUSE_TEST18 0x00EC
#define RTL8188EU_REG_EFUSE_TEST19 0x00F0
#define RTL8188EU_REG_EFUSE_TEST20 0x00F4
#define RTL8188EU_REG_EFUSE_TEST21 0x00F8
#define RTL8188EU_REG_EFUSE_TEST22 0x00FC

#define RTL8188EU_REG_RCR 0x0608
#define RTL8188EU_REG_MBID_NUM 0x0600
#define RTL8188EU_REG_MBID_BCN_SPACE 0x0604
#define RTL8188EU_REG_FWHW_TXQ_CTRL 0x0606
#define RTL8188EU_REG_HWSEQ_CTRL 0x0607
#define RTL8188EU_REG_TXPAUSE 0x060A
#define RTL8188EU_REG_BCN_CTRL 0x0610
#define RTL8188EU_REG_BCN_CTRL_1 0x0611
#define RTL8188EU_REG_TBTT_PROHIBIT 0x0612
#define RTL8188EU_REG_BCN_ITV 0x0614
#define RTL8188EU_REG_DTIM_COUNT 0x0616
#define RTL8188EU_REG_DRVERLYINT 0x0618
#define RTL8188EU_REG_BCNDMATIM 0x0619
#define RTL8188EU_REG_BCN_ERR_ITV 0x061A
#define RTL8188EU_REG_ATIMWND 0x061C
#define RTL8188EU_REG_BCN_MAX_ERR 0x061D
#define RTL8188EU_REG_MLT 0x061E
#define RTL8188EU_REG_TSFTIMER_HCI 0x0620
#define RTL8188EU_REG_HG_TIMER 0x0624
#define RTL8188EU_REG_PS_TIMER 0x0628
#define RTL8188EU_REG_PS_TIMER2 0x062C
#define RTL8188EU_REG_RXERR_RPT 0x0630
#define RTL8188EU_REG_BSSID 0x0634
#define RTL8188EU_REG_RESP_SIFS_OFDM 0x063C
#define RTL8188EU_REG_RESP_SIFS_CCK 0x063E
#define RTL8188EU_REG_SIFS_CCK 0x0640
#define RTL8188EU_REG_SIFS_OFDM 0x0642
#define RTL8188EU_REG_ACK_TIMEOUT 0x0644
#define RTL8188EU_REG_EIFS 0x0646
#define RTL8188EU_REG_NAV_MAX 0x0648
#define RTL8188EU_REG_SLOT 0x064A
#define RTL8188EU_REG_TXOP_STALL_CTRL 0x064C
#define RTL8188EU_REG_TBTT_PROHIBIT_EXT 0x064E
#define RTL8188EU_REG_USTIME_TSF 0x0650
#define RTL8188EU_REG_USTIME_EDCA 0x0652
#define RTL8188EU_REG_EDCA_VO_PARAM 0x0654
#define RTL8188EU_REG_EDCA_VI_PARAM 0x0658
#define RTL8188EU_REG_EDCA_BE_PARAM 0x065C
#define RTL8188EU_REG_EDCA_BK_PARAM 0x0660
#define RTL8188EU_REG_PIFS 0x0664
#define RTL8188EU_REG_AGGLEN_LMT 0x0668
#define RTL8188EU_REG_AMPDU_MIN_SPACE 0x066C
#define RTL8188EU_REG_TXOP_CTRL 0x0670
#define RTL8188EU_REG_TXOP_RTY_CTRL 0x0674
#define RTL8188EU_REG_FAST_EDCA_CTRL 0x0678
#define RTL8188EU_REG_RD_RESP_PKT_TH 0x067C
#define RTL8188EU_REG_INIRTS_RATE_SEL 0x0680
#define RTL8188EU_REG_DARFRC 0x0684
#define RTL8188EU_REG_DARFRCH 0x0688
#define RTL8188EU_REG_RARPT 0x068C
#define RTL8188EU_REG_RARPTH 0x0690
#define RTL8188EU_REG_POWER_STAGE1 0x0694
#define RTL8188EU_REG_POWER_STAGE2 0x0698
#define RTL8188EU_REG_PKT_VO_VI_LIFE_TIME 0x06C0
#define RTL8188EU_REG_PKT_BE_BK_LIFE_TIME 0x06C2
#define RTL8188EU_REG_SECURITY_CFG 0x0681
#define RTL8188EU_REG_CAMCMD 0x0684
#define RTL8188EU_REG_CAMWRITE 0x0688
#define RTL8188EU_REG_CAMREAD 0x068C
#define RTL8188EU_REG_CAMDBG 0x0690
#define RTL8188EU_REG_SECCFG 0x0681

#define RTL8188EU_RCR_AAP (1u << 0)
#define RTL8188EU_RCR_APM (1u << 1)
#define RTL8188EU_RCR_AM (1u << 2)
#define RTL8188EU_RCR_AB (1u << 3)
#define RTL8188EU_RCR_ACRC32 (1u << 4)
#define RTL8188EU_RCR_CBSSID_DATA (1u << 5)
#define RTL8188EU_RCR_CBSSID_BCN (1u << 6)
#define RTL8188EU_RCR_AICV (1u << 7)
#define RTL8188EU_RCR_ADD3 (1u << 8)
#define RTL8188EU_RCR_APWRMGT (1u << 9)
#define RTL8188EU_RCR_APP_BCN (1u << 10)
#define RTL8188EU_RCR_APP_ICV (1u << 11)
#define RTL8188EU_RCR_APP_MIC (1u << 12)
#define RTL8188EU_RCR_APP_FCS (1u << 13)
#define RTL8188EU_RCR_APP_PHYSTS (1u << 14)
#define RTL8188EU_RCR_APP_PHYST_RXFF (1u << 15)
#define RTL8188EU_RCR_APPBAID (1u << 16)
#define RTL8188EU_RCR_HTC_LOC_CTRL (1u << 22)
#define RTL8188EU_RCR_AMF (1u << 23)
#define RTL8188EU_RCR_ACF (1u << 24)
#define RTL8188EU_RCR_ADF (1u << 25)
#define RTL8188EU_RCR_APP_KEYSEARCH (1u << 26)
#define RTL8188EU_RCR_CHECK_BSSID (1u << 27)
#define RTL8188EU_RCR_TSPEC (1u << 28)
#define RTL8188EU_RCR_MBIEN (1u << 29)
#define RTL8188EU_RCR_FORCE_ACK (1u << 30)
#define RTL8188EU_RCR_FORCE_BSSID_MATCH (1u << 31)

#define RTL8188EU_MAX_XFER 2048
#define RTL8188EU_FW_CHUNK 512
#define RTL8188EU_FW_MAX 32768

typedef struct {
    usb_device_t *dev;
    usb_interface_t *iface;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t *toggle_in;
    uint8_t *toggle_out;
    uint8_t *xfer;
    uint64_t xfer_phys;
    uint8_t *fw;
    uint32_t fw_size;
    wifi_t wifi;
    int cur_channel;
} rtl8188eu_t;

static rtl8188eu_t g_r8e[2];
static int g_nr8e;

static int r8e_read(usb_device_t *dev, uint16_t addr, void *data, uint16_t len) {
    return usb_control_msg(dev, RTL8188EU_REQT_READ, RTL8188EU_REQ_GET_REG, addr, 0, data, len,
                           NULL);
}

static int r8e_write(usb_device_t *dev, uint16_t addr, const void *data, uint16_t len) {
    return usb_control_msg(dev, RTL8188EU_REQT_WRITE, RTL8188EU_REQ_SET_REG, addr, 0,
                           (void *) data, len, NULL);
}

static int r8e_write8(usb_device_t *dev, uint16_t addr, uint8_t v) {
    return r8e_write(dev, addr, &v, 1);
}

static int r8e_write16(usb_device_t *dev, uint16_t addr, uint16_t v) {
    return r8e_write(dev, addr, &v, 2);
}

static int r8e_write32(usb_device_t *dev, uint16_t addr, uint32_t v) {
    return r8e_write(dev, addr, &v, 4);
}

static int r8e_read8(usb_device_t *dev, uint16_t addr, uint8_t *v) {
    return r8e_read(dev, addr, v, 1);
}

static int r8e_read32(usb_device_t *dev, uint16_t addr, uint32_t *v) {
    return r8e_read(dev, addr, v, 4);
}

static int rtl8188eu_load_fw(rtl8188eu_t *r) {
    int fd = fd_open_host("/lib/firmware/rtl8188eufw.bin", 0, 0);
    if (fd < 0) {
        log_warn("rtl8188eu: firmware /lib/firmware/rtl8188eufw.bin not found");
        return -1;
    }
    r->fw = kmalloc(RTL8188EU_FW_MAX);
    if (!r->fw) {
        fd_close(fd);
        return -1;
    }
    int64_t got = fd_read(fd, r->fw, RTL8188EU_FW_MAX);
    fd_close(fd);
    if (got <= 0) {
        kfree(r->fw);
        return -1;
    }
    r->fw_size = (uint32_t) got;
    log_info("rtl8188eu: firmware %lu bytes", r->fw_size);

    uint8_t val8;
    r8e_read8(r->dev, RTL8188EU_REG_MCUFWDL, &val8);
    r8e_write8(r->dev, RTL8188EU_REG_MCUFWDL, (uint8_t) (val8 | 0x02));
    usb_msleep(10);

    uint32_t offset = 0;
    while (offset < r->fw_size) {
        uint32_t chunk = r->fw_size - offset;
        if (chunk > RTL8188EU_FW_CHUNK) chunk = RTL8188EU_FW_CHUNK;
        int actual = 0;
        int ret = usb_bulk_transfer(r->dev, r->ep_out, r->toggle_out, r->fw + offset,
                                    (int) chunk, &actual);
        if (ret) {
            log_warn("rtl8188eu: fw chunk @%lu failed", offset);
            return -1;
        }
        offset += chunk;
        usb_msleep(1);
    }

    r8e_read8(r->dev, RTL8188EU_REG_MCUFWDL, &val8);
    r8e_write8(r->dev, RTL8188EU_REG_MCUFWDL, (uint8_t) (val8 | 0x04));
    usb_msleep(100);

    uint32_t to = 500;
    uint32_t fw_rdy;
    while (to--) {
        r8e_read32(r->dev, RTL8188EU_REG_MCUFWDL, &fw_rdy);
        if (fw_rdy & 0x00010000u) break;
        usb_msleep(10);
    }
    if (!to) {
        log_warn("rtl8188eu: fw not ready");
        return -1;
    }
    log_info("rtl8188eu: firmware running");
    return 0;
}

static int rtl8188eu_hw_init(rtl8188eu_t *r) {
    uint8_t val8;
    uint32_t val32;

    r8e_write8(r->dev, RTL8188EU_REG_SYS_FUNC_EN, 0x02);
    usb_msleep(5);
    r8e_write8(r->dev, RTL8188EU_REG_SYS_FUNC_EN, 0x03);
    usb_msleep(5);

    r8e_read8(r->dev, RTL8188EU_REG_APS_FSMCO, &val8);
    val8 |= 0x04;
    r8e_write8(r->dev, RTL8188EU_REG_APS_FSMCO, val8);
    usb_msleep(5);

    r8e_write32(r->dev, RTL8188EU_REG_RCR, RTL8188EU_RCR_AM | RTL8188EU_RCR_AB |
                                             RTL8188EU_RCR_APM | RTL8188EU_RCR_ACRC32 |
                                             RTL8188EU_RCR_APP_FCS | RTL8188EU_RCR_APP_ICV |
                                             RTL8188EU_RCR_APP_MIC | RTL8188EU_RCR_ADF |
                                             RTL8188EU_RCR_ACF);
    r8e_read32(r->dev, RTL8188EU_REG_RCR, &val32);
    log_info("rtl8188eu: RCR=0x%08x", val32);
    return 0;
}

static int rtl8188eu_set_channel(void *priv, int channel) {
    rtl8188eu_t *r = (rtl8188eu_t *) priv;
    r->cur_channel = channel;
    r8e_write32(r->dev, 0xF018, (uint32_t) (channel & 0x0F));
    usb_msleep(10);
    return 0;
}

static int rtl8188eu_tx(void *priv, const uint8_t *frame, uint16_t len) {
    rtl8188eu_t *r = (rtl8188eu_t *) priv;
    uint8_t *p = r->xfer;
    memset(p, 0, 32);
    p[0] = 0x01;
    p[1] = 0x00;
    p[2] = (uint8_t) len;
    p[3] = (uint8_t) (len >> 8);
    p[4] = (uint8_t) r->cur_channel;
    p[5] = 0x00;
    p[6] = 0x00;
    p[7] = 0x00;
    memcpy(p + 32, frame, len);
    int actual = 0;
    int ret = usb_bulk_transfer(r->dev, r->ep_out, r->toggle_out, p, (int) (32 + len), &actual);
    if (ret == USB_STALL) {
        usb_clear_halt(r->dev, r->ep_out);
        ret = usb_bulk_transfer(r->dev, r->ep_out, r->toggle_out, p, (int) (32 + len), &actual);
    }
    return ret;
}

static void rtl8188eu_poll_rx(rtl8188eu_t *r) {
    for (int i = 0; i < 4; i++) {
        int actual = 0;
        int ret = usb_bulk_transfer(r->dev, r->ep_in, r->toggle_in, r->xfer,
                                    RTL8188EU_MAX_XFER, &actual);
        if (ret == USB_STALL) {
            usb_clear_halt(r->dev, r->ep_in);
            continue;
        }
        if (ret || actual < 32) break;
        uint32_t pkt_len = r->xfer[2] | (r->xfer[3] << 8);
        if (pkt_len < 24 || pkt_len > (uint32_t) actual - 32) continue;
        wifi_rx_frame(&r->wifi, r->xfer + 32, (uint16_t) pkt_len);
    }
}

void rtl8188eu_probe(usb_device_t *dev, usb_interface_t *iface) {
    if (g_nr8e >= 2) return;

    usb_endpoint_t *ep_in = NULL, *ep_out = NULL;
    for (int i = 0; i < iface->num_eps; i++) {
        usb_endpoint_t *e = &iface->eps[i];
        if ((e->attributes & 3) != 2) continue;
        if (e->addr & 0x80) ep_in = e;
        else ep_out = e;
    }
    if (!ep_in || !ep_out) return;

    rtl8188eu_t *r = &g_r8e[g_nr8e];
    memset(r, 0, sizeof(*r));
    r->dev = dev;
    r->iface = iface;
    r->ep_in = ep_in->addr;
    r->ep_out = ep_out->addr;
    r->toggle_in = &ep_in->toggle;
    r->toggle_out = &ep_out->toggle;

    r->xfer_phys = (uint64_t) pmm_alloc_contiguous(
        (RTL8188EU_MAX_XFER + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!r->xfer_phys) return;
    r->xfer = (uint8_t *) phys_to_virt(r->xfer_phys);

    if (rtl8188eu_load_fw(r) || rtl8188eu_hw_init(r)) {
        log_warn("rtl8188eu: init failed");
        return;
    }

    uint8_t mac[6];
    r8e_read(dev, 0x1000, mac, 6);

    static wifi_hw_ops_t hw_ops[2];
    hw_ops[g_nr8e].tx = rtl8188eu_tx;
    hw_ops[g_nr8e].set_channel = rtl8188eu_set_channel;
    hw_ops[g_nr8e].hw_priv = r;

    char name[NETDEV_NAME_MAX];
    snprintf(name, NETDEV_NAME_MAX, "uwlan%d", g_nr8e);
    wifi_init_dev(&r->wifi, name, mac, &hw_ops[g_nr8e]);
    r->wifi.nd.priv = r;
    netdev_register(&r->wifi.nd);

    r->cur_channel = 6;
    log_info("rtl8188eu: ready, MAC %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
    g_nr8e++;
}
