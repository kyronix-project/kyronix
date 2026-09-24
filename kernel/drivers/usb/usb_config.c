#include "usb_glue.h"
#include "usb_config.h"
#include "../lib/string.h"

void usb_parse_config(const uint8_t *buf, uint16_t total, usb_ifaces_t *out) {
    memset(out, 0, sizeof(*out));
    if (total < 9) return;

    out->cfg_value = buf[5];

    bool in_msc = false, in_kbd = false, in_mouse = false, in_hub = false;
    uint8_t cur_intf = 0;
    bool first_intf = true;
    int cur_hid = -1;

    uint16_t pos = 9;
    while (pos + 2 <= total) {
        uint8_t blen  = buf[pos];
        uint8_t btype = buf[pos + 1];
        if (blen < 2 || pos + blen > total) break;

        if (btype == 0x04 && blen >= 9) {
            uint8_t inum  = buf[pos + 2];
            uint8_t alt   = buf[pos + 3];
            uint8_t cls   = buf[pos + 5];
            uint8_t sub   = buf[pos + 6];
            uint8_t proto = buf[pos + 7];
            cur_intf = inum;
            cur_hid  = -1;

            in_msc   = (cls == 0x08 && sub == 0x06 && proto == 0x50 && alt == 0);
            in_kbd   = (cls == 0x03 && sub == 0x01 && proto == 0x01 && alt == 0);
            in_mouse = (cls == 0x03 && sub == 0x01 && proto == 0x02 && alt == 0);
            in_hub   = (cls == 0x09 && alt == 0);

            if (in_msc && !out->msc.present) {
                out->msc.present = true;
                out->msc.intf    = inum;
            }
            if ((in_kbd || in_mouse) && out->hid_count < USB_MAX_HID_IFACES) {
                cur_hid = out->hid_count++;
                usb_hid_iface_t *h = &out->hid_list[cur_hid];
                h->present  = true;
                h->is_mouse = in_mouse;
                h->intf     = inum;
                if (!out->hid.present) {
                    out->hid.present  = true;
                    out->hid.is_mouse = in_mouse;
                    out->hid.intf     = inum;
                }
            }
            if (in_hub && !out->hub.present) {
                out->hub.present = true;
                out->hub.intf    = inum;
            }
            if (first_intf && alt == 0) {
                out->first_class = cls;
                out->first_sub   = sub;
                out->first_proto = proto;
                first_intf = false;
            }
        } else if (btype == 0x05 && blen >= 7) {
            uint8_t addr = buf[pos + 2];
            uint8_t attr = buf[pos + 3];
            uint16_t mps = (uint16_t)buf[pos + 4] | ((uint16_t)buf[pos + 5] << 8);
            uint8_t  ivl = buf[pos + 6];
            uint8_t  num = addr & 0x0F;
            bool     dir_in = (addr & 0x80) != 0;
            uint8_t  type = attr & 0x3;

            if (in_msc && out->msc.present && out->msc.intf == cur_intf && type == 2) {
                if (dir_in && out->msc.in_ep == 0) {
                    out->msc.in_ep  = num;
                    out->msc.in_mps = mps & 0x7FF;
                } else if (!dir_in && out->msc.out_ep == 0) {
                    out->msc.out_ep  = num;
                    out->msc.out_mps = mps & 0x7FF;
                }
            }
            if (cur_hid >= 0 && type == 3 && dir_in &&
                out->hid_list[cur_hid].in_ep == 0) {
                usb_hid_iface_t *h = &out->hid_list[cur_hid];
                h->in_ep    = num;
                h->in_mps   = mps & 0x7FF;
                h->interval = ivl;
                if (out->hid.intf == cur_intf && out->hid.in_ep == 0) {
                    out->hid.in_ep    = num;
                    out->hid.in_mps   = mps & 0x7FF;
                    out->hid.interval = ivl;
                }
            }
            if (in_hub && out->hub.present && out->hub.intf == cur_intf &&
                type == 3 && dir_in && out->hub.in_ep == 0) {
                out->hub.in_ep    = num;
                out->hub.in_mps   = mps & 0x7FF;
                out->hub.interval = ivl;
            }
        }
        pos += blen;
    }
}
