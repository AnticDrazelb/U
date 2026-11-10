/*
 * QEMU USB bus (hw/usb/bus.c, GPL-2.0-or-later), the parts xbox-core uses: ports, attach and
 * detach, a device's realize, and the calls into its class. Taken from QEMU; the monitor, snapshot
 * and automatic-hub parts left out, the bus made without QOM (xb_usb_bus_new).
 */
#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/usb.h"
#include "qapi/error.h"
#include "trace.h"

static void usb_qdev_realize(DeviceState *qdev, Error **errp);
static void usb_qdev_unrealize(DeviceState *qdev);
static int next_usb_bus = 0;
static QTAILQ_HEAD(, USBBus) busses = QTAILQ_HEAD_INITIALIZER(busses);

void usb_bus_new(USBBus *bus, size_t bus_size, USBBusOps *ops, DeviceState *host)
{
    (void)bus_size;
    bus->qbus.parent = host;
    bus->ops = ops;
    bus->busnr = next_usb_bus++;
    bus->qbus.name = g_strdup_printf("usb-bus.%d", bus->busnr);
    QTAILQ_INIT(&bus->free);
    QTAILQ_INIT(&bus->used);
    QTAILQ_INSERT_TAIL(&busses, bus, next);
}

/* xbox-core: a device realized at a port of a bus (QEMU's qdev_realize with its "port" property) */
void xb_usb_realize(USBDevice *dev, USBBus *bus, const char *port_path, Error **errp)
{
    dev->qdev.parent_bus = &bus->qbus;
    dev->port_path = g_strdup(port_path);
    usb_qdev_realize(&dev->qdev, errp);
}

static void usb_device_realize(USBDevice *dev, Error **errp)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);

    if (klass->realize) {
        klass->realize(dev, errp);
    }
}

USBDevice *usb_device_find_device(USBDevice *dev, uint8_t addr)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    if (klass->find_device) {
        return klass->find_device(dev, addr);
    }
    return NULL;
}

static void usb_device_unrealize(USBDevice *dev)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);

    if (klass->unrealize) {
        klass->unrealize(dev);
    }
}

void usb_device_cancel_packet(USBDevice *dev, USBPacket *p)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    if (klass->cancel_packet) {
        klass->cancel_packet(dev, p);
    }
}

void usb_device_handle_attach(USBDevice *dev)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    if (klass->handle_attach) {
        klass->handle_attach(dev);
    }
}

void usb_device_handle_reset(USBDevice *dev)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    if (klass->handle_reset) {
        klass->handle_reset(dev);
    }
}

void usb_device_handle_control(USBDevice *dev, USBPacket *p, int request,
                               int value, int index, int length, uint8_t *data)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    if (klass->handle_control) {
        klass->handle_control(dev, p, request, value, index, length, data);
    }
}

void usb_device_handle_data(USBDevice *dev, USBPacket *p)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    if (klass->handle_data) {
        klass->handle_data(dev, p);
    }
}

const char *usb_device_get_product_desc(USBDevice *dev)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    return klass->product_desc;
}

const USBDesc *usb_device_get_usb_desc(USBDevice *dev)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    if (dev->usb_desc) {
        return dev->usb_desc;
    }
    return klass->usb_desc;
}

void usb_device_set_interface(USBDevice *dev, int interface,
                              int alt_old, int alt_new)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    if (klass->set_interface) {
        klass->set_interface(dev, interface, alt_old, alt_new);
    }
}

void usb_device_flush_ep_queue(USBDevice *dev, USBEndpoint *ep)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    if (klass->flush_ep_queue) {
        klass->flush_ep_queue(dev, ep);
    }
}

void usb_device_ep_stopped(USBDevice *dev, USBEndpoint *ep)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    if (klass->ep_stopped) {
        klass->ep_stopped(dev, ep);
    }
}

int usb_device_alloc_streams(USBDevice *dev, USBEndpoint **eps, int nr_eps,
                             int streams)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    if (klass->alloc_streams) {
        return klass->alloc_streams(dev, eps, nr_eps, streams);
    }
    return 0;
}

void usb_device_free_streams(USBDevice *dev, USBEndpoint **eps, int nr_eps)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    if (klass->free_streams) {
        klass->free_streams(dev, eps, nr_eps);
    }
}

static void usb_qdev_realize(DeviceState *qdev, Error **errp)
{
    USBDevice *dev = USB_DEVICE(qdev);
    Error *local_err = NULL;

    pstrcpy(dev->product_desc, sizeof(dev->product_desc),
            usb_device_get_product_desc(dev));
    dev->auto_attach = 1;
    QLIST_INIT(&dev->strings);
    usb_ep_init(dev);

    usb_claim_port(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    usb_device_realize(dev, &local_err);
    if (local_err) {
        usb_release_port(dev);
        error_propagate(errp, local_err);
        return;
    }

    if (dev->auto_attach) {
        usb_device_attach(dev, &local_err);
        if (local_err) {
            usb_qdev_unrealize(qdev);
            error_propagate(errp, local_err);
            return;
        }
    }

}

static void usb_qdev_unrealize(DeviceState *qdev)
{
    USBDevice *dev = USB_DEVICE(qdev);
    USBDescString *s, *next;

    QLIST_FOREACH_SAFE(s, &dev->strings, next, next) {
        QLIST_REMOVE(s, next);
        g_free(s->str);
        g_free(s);
    }

    if (dev->pcap) {
        fclose(dev->pcap);
    }

    if (dev->attached) {
        usb_device_detach(dev);
    }
    usb_device_unrealize(dev);
    if (dev->port) {
        usb_release_port(dev);
    }
}

static void usb_fill_port(USBPort *port, void *opaque, int index,
                          USBPortOps *ops, int speedmask)
{
    port->opaque = opaque;
    port->index = index;
    port->ops = ops;
    port->speedmask = speedmask;
    usb_port_location(port, NULL, index + 1);
}

void usb_register_port(USBBus *bus, USBPort *port, void *opaque, int index,
                       USBPortOps *ops, int speedmask)
{
    usb_fill_port(port, opaque, index, ops, speedmask);
    QTAILQ_INSERT_TAIL(&bus->free, port, next);
    bus->nfree++;
}

void usb_register_companion(const char *masterbus, USBPort *ports[],
                            uint32_t portcount, uint32_t firstport,
                            void *opaque, USBPortOps *ops, int speedmask,
                            Error **errp)
{
    USBBus *bus;
    int i;

    QTAILQ_FOREACH(bus, &busses, next) {
        if (strcmp(bus->qbus.name, masterbus) == 0) {
            break;
        }
    }

    if (!bus) {
        error_setg(errp, "USB bus '%s' not found", masterbus);
        return;
    }
    if (!bus->ops->register_companion) {
        error_setg(errp, "Can't use USB bus '%s' as masterbus,"
                   " it doesn't support companion controllers",
                   masterbus);
        return;
    }

    for (i = 0; i < portcount; i++) {
        usb_fill_port(ports[i], opaque, i, ops, speedmask);
    }

    bus->ops->register_companion(bus, ports, portcount, firstport, errp);
}

void usb_port_location(USBPort *downstream, USBPort *upstream, int portnr)
{
    if (upstream) {
        int l = snprintf(downstream->path, sizeof(downstream->path), "%s.%d",
                         upstream->path, portnr);
        /* Max string is nn.nn.nn.nn.nn, which fits in 16 bytes */
        assert(l < sizeof(downstream->path));
        downstream->hubcount = upstream->hubcount + 1;
    } else {
        snprintf(downstream->path, sizeof(downstream->path), "%d", portnr);
        downstream->hubcount = 0;
    }
}

void usb_unregister_port(USBBus *bus, USBPort *port)
{
    if (port->dev) {
        object_unparent(OBJECT(port->dev));
    }
    QTAILQ_REMOVE(&bus->free, port, next);
    bus->nfree--;
}

void usb_claim_port(USBDevice *dev, Error **errp)
{
    USBBus *bus = usb_bus_from_device(dev);
    USBPort *port;
    USBDevice *hub;

    assert(dev->port == NULL);

    if (dev->port_path) {
        QTAILQ_FOREACH(port, &bus->free, next) {
            if (strcmp(port->path, dev->port_path) == 0) {
                break;
            }
        }
        if (port == NULL) {
            error_setg(errp, "usb port %s (bus %s) not found (in use?)",
                       dev->port_path, bus->qbus.name);
            return;
        }
    } else {
        if (bus->nfree == 0) {
            error_setg(errp, "tried to attach usb device %s to a bus "
                       "with no free ports", dev->product_desc);
            return;
        }
        port = QTAILQ_FIRST(&bus->free);
    }
    trace_usb_port_claim(bus->busnr, port->path);

    QTAILQ_REMOVE(&bus->free, port, next);
    bus->nfree--;

    dev->port = port;
    port->dev = dev;

    QTAILQ_INSERT_TAIL(&bus->used, port, next);
    bus->nused++;
}

void usb_release_port(USBDevice *dev)
{
    USBBus *bus = usb_bus_from_device(dev);
    USBPort *port = dev->port;

    assert(port != NULL);
    trace_usb_port_release(bus->busnr, port->path);

    QTAILQ_REMOVE(&bus->used, port, next);
    bus->nused--;

    dev->port = NULL;
    port->dev = NULL;

    QTAILQ_INSERT_TAIL(&bus->free, port, next);
    bus->nfree++;
}

static void usb_mask_to_str(char *dest, size_t size,
                            unsigned int speedmask)
{
    static const struct {
        unsigned int mask;
        const char *name;
    } speeds[] = {
        { .mask = USB_SPEED_MASK_FULL,  .name = "full"  },
        { .mask = USB_SPEED_MASK_HIGH,  .name = "high"  },
        { .mask = USB_SPEED_MASK_SUPER, .name = "super" },
    };
    int i, pos = 0;

    for (i = 0; i < ARRAY_SIZE(speeds); i++) {
        if (speeds[i].mask & speedmask) {
            pos += snprintf(dest + pos, size - pos, "%s%s",
                            pos ? "+" : "",
                            speeds[i].name);
        }
    }

    if (pos == 0) {
        snprintf(dest, size, "unknown");
    }
}

void usb_check_attach(USBDevice *dev, Error **errp)
{
    USBBus *bus = usb_bus_from_device(dev);
    USBPort *port = dev->port;
    char devspeed[32], portspeed[32];

    assert(port != NULL);
    assert(!dev->attached);
    usb_mask_to_str(devspeed, sizeof(devspeed), dev->speedmask);
    usb_mask_to_str(portspeed, sizeof(portspeed), port->speedmask);
    trace_usb_port_attach(bus->busnr, port->path,
                          devspeed, portspeed);

    if (!(port->speedmask & dev->speedmask)) {
        error_setg(errp, "Warning: speed mismatch trying to attach"
                   " usb device \"%s\" (%s speed)"
                   " to bus \"%s\", port \"%s\" (%s speed)",
                   dev->product_desc, devspeed,
                   bus->qbus.name, port->path, portspeed);
        return;
    }
}

void usb_device_attach(USBDevice *dev, Error **errp)
{
    USBPort *port = dev->port;
    Error *local_err = NULL;

    usb_check_attach(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    dev->attached = true;
    usb_attach(port);
}

int usb_device_detach(USBDevice *dev)
{
    USBBus *bus = usb_bus_from_device(dev);
    USBPort *port = dev->port;

    assert(port != NULL);
    assert(dev->attached);
    trace_usb_port_detach(bus->busnr, port->path);

    usb_detach(port);
    dev->attached = false;
    return 0;
}

