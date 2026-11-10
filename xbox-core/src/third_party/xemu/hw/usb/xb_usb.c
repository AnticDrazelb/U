/*
 * xbox-core: the MCPX's two OHCI controllers as xemu's machine has them (QEMU's hcd-ohci, 4 ports
 * each), and player 1's controller as xemu plugs one in: a 3-port hub (the controller's own) in
 * root port 3 of the first controller, the gamepad (xemu's XID) in that hub's port 1.
 */
#include "qemu/osdep.h"
#include "hw/usb.h"
#include "migration/vmstate.h"
#include "hcd-ohci.h"

USBDevice *xb_usb_hub_new(uint32_t ports);
USBDevice *xb_usb_xbox_gamepad_new(int index);
void xb_usb_realize(USBDevice *dev, USBBus *bus, const char *port_path, Error **errp);
void xb_bql_lock(void);
void xb_bql_unlock(void);
void xb_xemu_log(const char *fmt, ...);

static OHCIState g_ohci[2];
static DeviceState g_host[2];

static void ohci_die(OHCIState *ohci)
{
    xb_xemu_log("usb: %s stopped on an error (its DMA)", ohci->name ? ohci->name : "ohci");
}

void xb_usb_init(void)
{
    static const int kIrq[2] = {1, 9};
    xb_bql_lock();
    for (int i = 0; i < 2; i++) {
        Error *err = NULL;
        usb_ohci_init(&g_ohci[i], &g_host[i], 4, 0, NULL, 0, &address_space_memory, ohci_die, &err);
        if (err) {
            xb_xemu_log("usb: controller %d: %s", i, error_get_pretty(err));
            free(err);
        }
        g_ohci[i].irq = (qemu_irq)(intptr_t)kIrq[i];
    }
    Error *err = NULL;
    USBDevice *hub = xb_usb_hub_new(3);
    xb_usb_realize(hub, &g_ohci[0].bus, "3", &err);
    if (!err) {
        USBDevice *pad = xb_usb_xbox_gamepad_new(0);
        xb_usb_realize(pad, &g_ohci[0].bus, "3.1", &err);
    }
    if (err) {
        xb_xemu_log("usb: the controller: %s", error_get_pretty(err));
        free(err);
    }
    xb_bql_unlock();
}

uint64_t xb_ohci_read(int which, uint64_t addr, unsigned int size)
{
    xb_bql_lock();
    uint64_t r = g_ohci[which].mem.ops->read(&g_ohci[which], addr, size);
    xb_bql_unlock();
    return r;
}

void xb_ohci_write(int which, uint64_t addr, uint64_t val, unsigned int size)
{
    xb_bql_lock();
    g_ohci[which].mem.ops->write(&g_ohci[which], addr, val, size);
    xb_bql_unlock();
}
