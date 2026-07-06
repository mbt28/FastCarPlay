#ifndef SRC_PROTOCOL_AA_AOAP
#define SRC_PROTOCOL_AA_AOAP

#include <libusb-1.0/libusb.h>

// Android Open Accessory switch: puts an attached phone into accessory mode
// so it re-enumerates as 18D1:2D00/2D01 with two bulk endpoints and starts
// the Android Auto stack. https://source.android.com/docs/core/interaction/accessories/aoa
namespace aoap
{
// Open an already accessory-mode device, or nullptr.
libusb_device_handle *openAccessory(libusb_context *context);

// Scan the bus and try the AOAP switch on every candidate device (skips
// hubs, the Carlinkit dongle and accessory-mode devices). Returns the number
// of devices that accepted the switch; they re-enumerate shortly after.
int switchCandidates(libusb_context *context);
} // namespace aoap

#endif /* SRC_PROTOCOL_AA_AOAP */
