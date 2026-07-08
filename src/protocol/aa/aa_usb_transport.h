#ifndef SRC_PROTOCOL_AA_AA_USB_TRANSPORT
#define SRC_PROTOCOL_AA_AA_USB_TRANSPORT

#include <libusb-1.0/libusb.h>

#include <atomic>
#include <thread>
#include <vector>

#include "protocol/usb_buffer.h"
#include "protocol/aa/aa_transport.h"

// USB/AOAP transport: switches the phone into accessory mode, claims the bulk
// endpoints, and streams frame bytes over them. Owns the libusb machinery
// (async IN transfers filling a UsbBuffer, a libusb event-pump thread) that
// used to live in AaConnection.
class AaUsbTransport : public AaTransport
{
public:
    AaUsbTransport();
    ~AaUsbTransport() override;

    bool open(std::atomic<bool> &active) override;
    void close() override;
    bool connected() const override { return _connected.load(); }
    bool read(uint8_t *dst, uint32_t len) override;
    bool write(const uint8_t *src, uint32_t len) override;
    const char *name() const override { return "usb"; }

private:
    struct Context
    {
        AaUsbTransport *owner = nullptr;
        DataSlot *slot = nullptr;
        libusb_transfer *transfer = nullptr;
    };

    static void onTransfer(libusb_transfer *transfer);
    libusb_device_handle *waitForAccessory(std::atomic<bool> &active);
    bool link(libusb_device_handle *handler);
    bool startTransfers();
    void readLoop();

    libusb_context *_context;
    libusb_device_handle *_handle;
    uint8_t _epIn, _epOut;
    UsbBuffer _processQueue;
    std::vector<Context> _transfers;
    std::thread _readThread;
    std::atomic<bool> _connected;
};

#endif /* SRC_PROTOCOL_AA_AA_USB_TRANSPORT */
