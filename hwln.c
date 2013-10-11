#include <linux/module.h>
#include <linux/init.h>

#include <linux/usb.h>

#include <linux/kernel.h>
#include <linux/slab.h>

//#define VENDOR_ID 0x0951
//#define PRODUCT_ID 0x1624
//mouse
#define VENDOR_ID 0x0461
#define PRODUCT_ID 0x4d03

struct hwln_dev {
    struct usb_device *udev;
    struct usb_interface *intf;
    unsigned char *buf;
    dma_addr_t buf_dma;
    size_t buf_size;
    struct urb *irq;
};

static void hwln_packet(struct hwln_dev *dev)
{
    int i;
    printk("recv hwln data=");
    for (i = 0; i < dev->buf_size; i++) {
        printk(" %u", dev->buf[i]);
    }
    printk("\n");
}

static void hwln_irq(struct urb *urb)
{
    int retval;
    struct hwln_dev *dev = urb->context;

    switch (urb->status) {
        case 0:
            hwln_packet(dev);
            break;
        case -ECONNRESET:
        case -ENOENT:
        case -ESHUTDOWN:
            dev_err(&dev->udev->dev, "urb shutdown");
            return;
        default:
            dev_err(&dev->udev->dev, "nonzero urb status: %d", urb->status);
            break;
    }
    retval = usb_submit_urb(dev->irq, GFP_ATOMIC);
    if (retval)
        pr_err("usb_submit_urb failed in irq");
}

//list all endpoints
static void detect_endpoints(struct usb_interface *intf)
{
    struct usb_host_interface *iface_desc;
    int i;

    iface_desc = intf->cur_altsetting;
    for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
        struct usb_endpoint_descriptor *endpoint
            = &iface_desc->endpoint[i].desc;

        int in = (endpoint->bEndpointAddress & USB_DIR_IN);
        int type = (endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK);
        size_t buffer_size = endpoint->wMaxPacketSize;
        __u8 addr = endpoint->bEndpointAddress;
        __u8 interval = endpoint->bInterval;
        const char *type_str;

        switch (type) {
            case USB_ENDPOINT_XFER_CONTROL:
                type_str = "control";
                break;
            case USB_ENDPOINT_XFER_ISOC:
                type_str = "isoc";
                break;
            case USB_ENDPOINT_XFER_BULK:
                type_str = "bulk";
                break;
            case USB_ENDPOINT_XFER_INT:
                type_str = "int";
                break;
            default:
                type_str = "other";
                break;
        }

        printk("[%d] %s %s BufSize:%lu Addr:%u Interval:%u\n", i,
                (in ? "IN" : "OUT"), type_str, buffer_size, addr, interval);
    }
}

static int hwln_probe(struct usb_interface *intf,
        const struct usb_device_id *id)
{
    int retval = 0;
    struct hwln_dev *dev;
    struct usb_host_interface *iface_desc;
    struct usb_endpoint_descriptor *endpoint;

    printk("hwln_probe\n");

    detect_endpoints(intf);

    //check endpoint type to be DIR_IN and TYPE_INT
    iface_desc = intf->cur_altsetting;
    if (iface_desc->desc.bNumEndpoints < 1) {
        retval = -1;
        goto error;
    }
    endpoint = &iface_desc->endpoint[0].desc;
    if (!(endpoint->bEndpointAddress & USB_DIR_IN) ||
            (endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) !=
            USB_ENDPOINT_XFER_INT) {
        retval = -1;
        goto error;
    }

    //malloc struct dev
    dev = kzalloc(sizeof (struct hwln_dev), GFP_KERNEL);
    if (dev == NULL) {
        pr_err("alloc for dev failed");
        retval = -ENOMEM;
        goto error;
    }

    dev->udev = usb_get_dev(interface_to_usbdev(intf));
    dev->intf = intf;
    dev->buf_size = endpoint->wMaxPacketSize;

    //malloc dma buffer for int input
    dev->buf = usb_alloc_coherent(dev->udev, dev->buf_size, GFP_KERNEL,
            &dev->buf_dma);
    if (!dev->buf) {
        pr_err("alloc for buf failed");
        retval = -ENOMEM;
        goto err_dev;
    }

    //malloc urb
    dev->irq = usb_alloc_urb(0, GFP_KERNEL);
    if (!dev->irq) {
        pr_err("alloc for urb failed");
        retval = -ENOMEM;
        goto err_devbuf;
    }
    usb_fill_int_urb(dev->irq, dev->udev,
            usb_rcvintpipe(dev->udev, endpoint->bEndpointAddress),
            dev->buf, dev->buf_size, hwln_irq, dev, endpoint->bInterval);
    dev->irq->transfer_dma = dev->buf_dma;
    dev->irq->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

    usb_set_intfdata(intf, dev);

    //enable urb
    retval = usb_submit_urb(dev->irq, GFP_KERNEL);
    if (retval) {
        pr_err("usb_submit_urb failed");
        goto err_devbuf;
    }

    return 0;

err_devbuf:
    usb_free_coherent(dev->udev, dev->buf_size, dev->buf, dev->buf_dma);
err_dev:
    kfree(dev);
error:
    return retval;
}

static void hwln_disconnect(struct usb_interface *intf)
{
    struct hwln_dev *dev;

    printk("hwln_disconnect\n");

    dev = usb_get_intfdata(intf);
    usb_set_intfdata(intf, NULL);
    usb_kill_urb(dev->irq);
    usb_free_urb(dev->irq);
    usb_free_coherent(interface_to_usbdev(intf), dev->buf_size, dev->buf,
            dev->buf_dma);
    kfree(dev);
}

static const struct usb_device_id hwln_ids[] = {
    { USB_DEVICE(VENDOR_ID, PRODUCT_ID) },
    { }
};
MODULE_DEVICE_TABLE(usb, hwln_ids);

static struct usb_driver hwln_driver = {
    .name = "hwln",
    .id_table = hwln_ids,
    .probe = hwln_probe,
    .disconnect = hwln_disconnect,
};

static int __init hwln_init(void)
{
    int result;
    printk("hwln_init\n");
    result = usb_register(&hwln_driver);
    if (result)
        pr_err("hwln register driver failed: %d", result);
    return result;
}

static void __exit hwln_exit(void)
{
    printk("hwln_exit\n");
    usb_deregister(&hwln_driver);
}

module_init(hwln_init);
module_exit(hwln_exit);

MODULE_LICENSE("GPL");

