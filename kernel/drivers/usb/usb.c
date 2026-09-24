#include "usb_glue.h"
#include "usb.h"
#include "xhci.h"
#include "ehci.h"
#include "uhci.h"

void usb_init(void) {

    xhci_init();
    ehci_init();
    uhci_init();

    xhci_start_worker();
    ehci_start_worker();
    uhci_start_worker();
}
