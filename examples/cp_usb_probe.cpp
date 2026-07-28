// CarPlay USB probe: enumerate a plugged iPhone's configs/interfaces/endpoints
// and identify the iAP2 interface (vendor-specific 0xFF) used for the wired
// CarPlay trigger. Read-only -- it opens the device and reads descriptors, no
// config change. The foundation for the iAP2-over-USB transport.
//
//   make cp_usb_probe && sudo ../out/cp_usb_probe

#include <cstdio>

#include <libusb-1.0/libusb.h>

static const char *classDesc(uint8_t c)
{
    switch (c)
    {
    case 0x01: return "Audio";
    case 0x02: return "CDC";
    case 0x03: return "HID";
    case 0x06: return "Imaging/PTP";
    case 0x08: return "Mass Storage";
    case 0x0a: return "CDC-Data";
    case 0xe0: return "Wireless";
    case 0xff: return "Vendor-Specific";
    default: return "?";
    }
}

int main()
{
    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) != 0)
    {
        printf("libusb_init failed\n");
        return 1;
    }

    libusb_device **list = nullptr;
    ssize_t n = libusb_get_device_list(ctx, &list);
    libusb_device *iphone = nullptr;
    for (ssize_t i = 0; i < n; i++)
    {
        libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) == 0 && d.idVendor == 0x05ac)
        {
            iphone = list[i];
            printf("Apple device %04x:%04x, %d configuration(s)\n\n", d.idVendor, d.idProduct,
                   d.bNumConfigurations);
            break;
        }
    }
    if (!iphone)
    {
        printf("no Apple device found (is the iPhone plugged in?)\n");
        libusb_free_device_list(list, 1);
        libusb_exit(ctx);
        return 1;
    }

    libusb_device_descriptor dd;
    libusb_get_device_descriptor(iphone, &dd);

    int iap2Config = -1, iap2Iface = -1, iap2In = -1, iap2Out = -1;
    for (uint8_t c = 0; c < dd.bNumConfigurations; c++)
    {
        libusb_config_descriptor *cfg = nullptr;
        if (libusb_get_config_descriptor(iphone, c, &cfg) != 0)
            continue;
        printf("config %d (%d interfaces):\n", cfg->bConfigurationValue, cfg->bNumInterfaces);
        for (int ii = 0; ii < cfg->bNumInterfaces; ii++)
        {
            const libusb_interface &intf = cfg->interface[ii];
            for (int a = 0; a < intf.num_altsetting; a++)
            {
                const libusb_interface_descriptor &id = intf.altsetting[a];
                printf("  if%d.%d  class=0x%02x %-16s sub=0x%02x proto=0x%02x  (%d ep)\n",
                       id.bInterfaceNumber, id.bAlternateSetting, id.bInterfaceClass,
                       classDesc(id.bInterfaceClass), id.bInterfaceSubClass,
                       id.bInterfaceProtocol, id.bNumEndpoints);

                int epIn = -1, epOut = -1;
                for (int e = 0; e < id.bNumEndpoints; e++)
                {
                    const libusb_endpoint_descriptor &ep = id.endpoint[e];
                    const bool bulk = (ep.bmAttributes & 0x03) == LIBUSB_TRANSFER_TYPE_BULK;
                    printf("      ep 0x%02x  %-4s %s\n", ep.bEndpointAddress,
                           (ep.bEndpointAddress & 0x80) ? "IN" : "OUT",
                           bulk ? "bulk" : (ep.bmAttributes & 3) == 3 ? "interrupt" : "other");
                    if (bulk)
                    {
                        if (ep.bEndpointAddress & 0x80) epIn = ep.bEndpointAddress;
                        else epOut = ep.bEndpointAddress;
                    }
                }
                // iAP2 rides a vendor-specific interface with bulk endpoints.
                if (id.bInterfaceClass == 0xff && epIn >= 0 && epOut >= 0 && iap2Config < 0)
                {
                    iap2Config = cfg->bConfigurationValue;
                    iap2Iface = id.bInterfaceNumber;
                    iap2In = epIn;
                    iap2Out = epOut;
                }
            }
        }
        libusb_free_config_descriptor(cfg);
    }

    printf("\n");
    if (iap2Config >= 0)
        printf("iAP2 candidate: config %d, interface %d, bulk IN 0x%02x / OUT 0x%02x\n",
               iap2Config, iap2Iface, iap2In, iap2Out);
    else
        printf("no vendor-specific bulk interface found (phone may need a config switch)\n");

    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return 0;
}
