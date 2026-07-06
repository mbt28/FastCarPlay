#include "aoap.h"

#include <cstring>

#include "protocol/aa/aa_const.h"
#include "protocol/protocol_const.h"
#include "common/logger.h"
#include "settings.h"

#define AOAP_CTRL_TIMEOUT 2000

namespace aoap
{

libusb_device_handle *openAccessory(libusb_context *context)
{
    libusb_device_handle *handler =
        libusb_open_device_with_vid_pid(context, AOAP_VID_GOOGLE, AOAP_PID_ACCESSORY);
    if (!handler)
        handler = libusb_open_device_with_vid_pid(context, AOAP_VID_GOOGLE, AOAP_PID_ACCESSORY_ADB);
    return handler;
}

static bool sendString(libusb_device_handle *handler, uint16_t index, const char *value)
{
    int length = strlen(value) + 1; // include the terminating '\0'
    int result = libusb_control_transfer(
        handler, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR, AOAP_REQ_SEND_STRING,
        0, index, (unsigned char *)value, length, AOAP_CTRL_TIMEOUT);
    return result == length;
}

// Ask for AOAP support and switch the device to accessory mode. The device
// is gone (re-enumerating) on success.
static bool trySwitch(libusb_device *device)
{
    libusb_device_handle *handler = nullptr;
    if (libusb_open(device, &handler) != LIBUSB_SUCCESS)
        return false;

    libusb_set_auto_detach_kernel_driver(handler, 1);

    // Control transfers need a claimed interface on Linux; interface 0 may be
    // held by a kernel driver (MTP), so take the first one that claims.
    int iface = -1;
    for (int i = 0; i < 4 && iface < 0; i++)
        if (libusb_claim_interface(handler, i) == LIBUSB_SUCCESS)
            iface = i;

    bool switched = false;
    uint8_t protocol[2] = {0, 0};
    int result = libusb_control_transfer(
        handler, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR, AOAP_REQ_GET_PROTOCOL,
        0, 0, protocol, sizeof(protocol), AOAP_CTRL_TIMEOUT);
    uint16_t version = protocol[0] | (protocol[1] << 8);

    if (result == 2 && version >= 1)
    {
        log_i("AOAP capable device (protocol %d), switching to accessory mode", version);
        switched = sendString(handler, AOAP_STR_MANUFACTURER, AOAP_MANUFACTURER) &&
                   sendString(handler, AOAP_STR_MODEL, AOAP_MODEL) &&
                   sendString(handler, AOAP_STR_DESCRIPTION, AOAP_DESCRIPTION) &&
                   sendString(handler, AOAP_STR_VERSION, AOAP_VERSION) &&
                   sendString(handler, AOAP_STR_URI, AOAP_URI) &&
                   sendString(handler, AOAP_STR_SERIAL, AOAP_SERIAL);
        if (switched)
        {
            result = libusb_control_transfer(
                handler, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR, AOAP_REQ_START,
                0, 0, nullptr, 0, AOAP_CTRL_TIMEOUT);
            switched = result >= 0;
        }
        if (!switched)
            log_w("AOAP switch failed");
    }

    if (iface >= 0)
        libusb_release_interface(handler, iface);
    libusb_close(handler);
    return switched;
}

int switchCandidates(libusb_context *context)
{
    libusb_device **list = nullptr;
    ssize_t count = libusb_get_device_list(context, &list);
    if (count < 0)
        return 0;

    int switched = 0;
    for (ssize_t i = 0; i < count; i++)
    {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != LIBUSB_SUCCESS)
            continue;

        if (desc.bDeviceClass == LIBUSB_CLASS_HUB)
            continue;
        if (desc.idVendor == CARLINKIT_VID)
            continue;
        if (desc.idVendor == AOAP_VID_GOOGLE &&
            (desc.idProduct == AOAP_PID_ACCESSORY || desc.idProduct == AOAP_PID_ACCESSORY_ADB))
            continue; // already switched

        // Optional pinning to one phone
        if (Settings::aaVendorid > 0 && desc.idVendor != Settings::aaVendorid)
            continue;
        if (Settings::aaProductid > 0 && desc.idProduct != Settings::aaProductid)
            continue;

        log_v("AOAP probing %04x:%04x", desc.idVendor, desc.idProduct);
        if (trySwitch(list[i]))
            switched++;
    }

    libusb_free_device_list(list, 1);
    return switched;
}

} // namespace aoap
