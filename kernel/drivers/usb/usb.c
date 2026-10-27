#include "usb.h"
#include "ehci.h"
#include "uhci.h"
#include "usb_glue.h"
#include "xhci.h"

void usb_init(void) {

    xhci_init();
    ehci_init();
    uhci_init();

    xhci_start_worker();
    ehci_start_worker();
    uhci_start_worker();
}
