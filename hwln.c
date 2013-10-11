#include <linux/module.h>
#include <linux/init.h>

#include <linux/usb.h>
#include <linux/input.h>
#include <linux/usb/input.h>

#include <linux/kernel.h>
#include <linux/slab.h>

//#define VENDOR_ID 0x0951
//#define PRODUCT_ID 0x1624
//mouse
#define VENDOR_ID 0x0461
#define PRODUCT_ID 0x4d03

struct hwln_dev {
    //for usb dev
    struct usb_device *udev;
    struct usb_interface *intf;
    unsigned char *buf;
    dma_addr_t buf_dma;
    size_t buf_size;
    struct urb *irq;

    //for input dev
    struct input_dev *idev;
    char name[128];
    char phys[64];
};

static void hwln_packet(struct hwln_dev *dev)
{
    int i;
    int code;

    printk("recv hwln data=");
    for (i = 0; i < dev->buf_size; i++) {
        printk(" %u", dev->buf[i]);
    }
    printk("\n");

    code = (int)dev->buf[0];
    switch (code) {
        case 0: //key up
            input_report_key(dev->idev, KEY_A, 0);
            input_report_key(dev->idev, KEY_B, 0);
            break;
        case 1: //left
            input_report_key(dev->idev, KEY_A, 1);
            break;
        case 2: //right
            input_report_key(dev->idev, KEY_B, 1);
            break;
        default:
            break;
    }
    input_sync(dev->idev);
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
            dev_info(&dev->udev->dev, "urb shutdown\n");
            return;
        default:
            dev_err(&dev->udev->dev, "nonzero urb status: %d\n", urb->status);
            break;
    }
    retval = usb_submit_urb(dev->irq, GFP_ATOMIC);
    if (retval)
        pr_err("usb_submit_urb failed in irq\n");
}

static int hwln_open(struct input_dev *idev)
{
    int retval;
    struct hwln_dev *dev;

    printk("hwln_open\n");

    //enable urb
    dev = input_get_drvdata(idev);
    retval = usb_submit_urb(dev->irq, GFP_KERNEL);
    if (retval) {
        pr_err("usb_submit_urb failed\n");
        return -EIO;
    }

    return 0;
}

static void hwln_close(struct input_dev *idev)
{
    struct hwln_dev *dev;

    printk("hwln_close\n");

    dev = input_get_drvdata(idev);
    usb_kill_urb(dev->irq);
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

static struct usb_endpoint_descriptor *check_endpoint(
        struct usb_host_interface *iface_desc)
{
    struct usb_endpoint_descriptor *endpoint;

    //check endpoint type to be DIR_IN and TYPE_INT
    if (iface_desc->desc.bNumEndpoints < 1) {
        return NULL;
    }
    endpoint = &iface_desc->endpoint[0].desc;
    if (!(endpoint->bEndpointAddress & USB_DIR_IN) ||
            (endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) !=
            USB_ENDPOINT_XFER_INT) {
        return NULL;
    }

    return endpoint;
}

static int setup_usb(struct hwln_dev *dev)
{
    int retval = 0;
    struct usb_endpoint_descriptor *endpoint;

    endpoint = check_endpoint(dev->intf->cur_altsetting);
    if (!endpoint) {
        pr_err("error endpoint\n");
        retval = -1;
        goto error;
    }
    dev->buf_size = endpoint->wMaxPacketSize;

    //alloc dma buffer for int input
    dev->buf = usb_alloc_coherent(dev->udev, dev->buf_size, GFP_KERNEL,
            &dev->buf_dma);
    if (!dev->buf) {
        pr_err("alloc for buf failed\n");
        retval = -ENOMEM;
        goto error;
    }

    //alloc urb
    dev->irq = usb_alloc_urb(0, GFP_KERNEL);
    if (!dev->irq) {
        pr_err("alloc for urb failed\n");
        retval = -ENOMEM;
        goto err_devbuf;
    }
    usb_fill_int_urb(dev->irq, dev->udev,
            usb_rcvintpipe(dev->udev, endpoint->bEndpointAddress),
            dev->buf, dev->buf_size, hwln_irq, dev, endpoint->bInterval);
    dev->irq->transfer_dma = dev->buf_dma;
    dev->irq->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

    usb_set_intfdata(dev->intf, dev);

    return 0;

err_devbuf:
    usb_free_coherent(dev->udev, dev->buf_size, dev->buf, dev->buf_dma);
error:
    return retval;
}

//call after setup_usb
static int setup_input(struct hwln_dev *dev)
{
    int retval = 0;
    struct usb_device *udev = dev->udev;
    struct input_dev *idev;

    //alloc input dev
    idev = input_allocate_device();
    if (!idev) {
        retval = -ENOMEM;
        goto error;
    }
    dev->idev = idev;

    //setup name
    if (udev->manufacturer)
        strlcpy(dev->name, udev->manufacturer, sizeof (dev->name));
    if (udev->product) {
        if (udev->manufacturer)
            strlcat(dev->name, " ", sizeof (dev->name));
        strlcat(dev->name, udev->product, sizeof (dev->name));
    }
    usb_make_path(udev, dev->phys, sizeof (dev->phys));
    strlcat(dev->phys, "/input0", sizeof (dev->phys));

    idev->name = dev->name;
    idev->phys = dev->phys;
    usb_to_input_id(udev, &idev->id);
    idev->dev.parent = &dev->intf->dev;
    input_set_drvdata(idev, dev);

    idev->open = hwln_open;
    idev->close = hwln_close;

    //idev->evbit[0] = BIT_MASK(EV_KEY);
    __set_bit(EV_KEY, idev->evbit);
    __set_bit(KEY_A, idev->keybit);
    __set_bit(KEY_B, idev->keybit);

    retval = input_register_device(idev);
    if (retval) {
        pr_err("register input dev failed\n");
        goto err_idev;
    }

    return 0;

err_idev:
    input_free_device(idev);
error:
    return retval;
}

static int hwln_probe(struct usb_interface *intf,
        const struct usb_device_id *id)
{
    int retval = 0;
    struct hwln_dev *dev;

    printk("hwln_probe\n");
    detect_endpoints(intf);

    //alloc struct dev
    dev = kzalloc(sizeof (struct hwln_dev), GFP_KERNEL);
    if (dev == NULL) {
        pr_err("alloc for dev failed\n");
        retval = -ENOMEM;
        goto error;
    }
    dev->udev = usb_get_dev(interface_to_usbdev(intf));
    dev->intf = intf;

    retval = setup_usb(dev);
    if (retval) {
        goto err_getdev;
    }

    retval = setup_input(dev);
    if (retval) {
        //teardown usb
        usb_set_intfdata(intf, NULL);
        usb_free_urb(dev->irq);
        usb_free_coherent(dev->udev, dev->buf_size, dev->buf, dev->buf_dma);
        goto err_getdev;
    }

    return 0;

err_getdev:
    usb_put_dev(dev->udev);
    kfree(dev);
error:
    return retval;
}

static void hwln_disconnect(struct usb_interface *intf)
{
    struct hwln_dev *dev;

    printk("hwln_disconnect\n");

    dev = usb_get_intfdata(intf);
    usb_kill_urb(dev->irq);

    //teardown input
    input_unregister_device(dev->idev);
    input_free_device(dev->idev);

    //teardown usb
    usb_set_intfdata(intf, NULL);
    usb_free_urb(dev->irq);
    usb_free_coherent(dev->udev, dev->buf_size, dev->buf, dev->buf_dma);

    //free dev
    usb_put_dev(dev->udev);
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
        pr_err("hwln register driver failed: %d\n", result);
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

