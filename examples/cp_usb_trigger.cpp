// CarPlay wired trigger: send the Apple vendor control request that makes an
// iPhone reveal its hidden CarPlay USB configurations (5 and 6), then report
// how many configs it exposes. After this, switching to config 6 brings up the
// usbmux + NCM interfaces that carry CarPlay over IP. Mirrors LIVI muxd.py
// (_config_carplay: ctrl 0xC0/0x52, then bConfigurationValue=6).
//
//   make cp_usb_trigger && sudo ../out/cp_usb_trigger
//
// Reversible: set the config back to 1 (or replug) to restore the phone.

#include <cstdio>
#include <unistd.h>

#include <libusb-1.0/libusb.h>

static int numConfigs(libusb_context *ctx)
{
    libusb_device **list = nullptr;
    ssize_t n = libusb_get_device_list(ctx, &list);
    int configs = -1;
    for (ssize_t i = 0; i < n; i++)
    {
        libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) == 0 && d.idVendor == 0x05ac)
        {
            configs = d.bNumConfigurations;
            break;
        }
    }
    libusb_free_device_list(list, 1);
    return configs;
}

int main()
{
    libusb_context *ctx = nullptr;
    libusb_init(&ctx);

    printf("iPhone exposes %d configs before the trigger\n", numConfigs(ctx));

    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, 0x05ac, 0);
    // vid+any product: fall back to the first Apple device.
    if (!h)
    {
        libusb_device **list = nullptr;
        ssize_t n = libusb_get_device_list(ctx, &list);
        for (ssize_t i = 0; i < n; i++)
        {
            libusb_device_descriptor d;
            if (libusb_get_device_descriptor(list[i], &d) == 0 && d.idVendor == 0x05ac)
            {
                libusb_open(list[i], &h);
                break;
            }
        }
        libusb_free_device_list(list, 1);
    }
    if (!h)
    {
        printf("could not open the iPhone (need sudo / it may be gone)\n");
        libusb_exit(ctx);
        return 1;
    }

    // The magic: vendor request 0x52 (device->host, vendor, device recipient),
    // wIndex 0x0004 -- "expose the CarPlay configurations".
    unsigned char buf[8] = {0};
    int r = libusb_control_transfer(h, 0xC0, 0x52, 0x0000, 0x0004, buf, sizeof(buf), 2000);
    printf("vendor 0x52 request -> %s (%d bytes)\n", r >= 0 ? "ok" : libusb_error_name(r), r);
    libusb_close(h);

    // The phone re-enumerates; wait for the extra configs to appear.
    int configs = -1;
    for (int i = 0; i < 25; i++)
    {
        usleep(200 * 1000);
        configs = numConfigs(ctx);
        if (configs >= 6)
            break;
    }
    printf("iPhone exposes %d configs after the trigger\n", configs);

    if (configs >= 6)
        printf("\nCarPlay configs revealed. Next: set bConfigurationValue=6 and watch\n"
               "for a cdc_ncm network interface.\n");
    else
        printf("\nno extra configs -- this phone/iOS may use a different trigger.\n");

    libusb_exit(ctx);
    return configs >= 6 ? 0 : 1;
}
