#include "aa_usb_transport.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

#include "protocol/aa/aoap.h"
#include "protocol/aa/aa_const.h"
#include "common/functions.h"
#include "common/logger.h"
#include "common/threading.h"
#include "settings.h"


static void interruptibleSleep(std::atomic<bool> &active, int ms)
{
    const int step = 20;
    for (int elapsed = 0; elapsed < ms && active.load(); elapsed += step)
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
}

AaUsbTransport::AaUsbTransport()
    : _context(nullptr),
      _handle(nullptr),
      _epIn(0),
      _epOut(0),
      _processQueue(Settings::usbBuffer, Settings::usbTransferSize),
      _transfers(Settings::usbQueue),
      _connected(false)
{
    int result = libusb_init(&_context);
    if (result < 0)
        throw std::runtime_error(std::string("Can't initialise USB: ") + libusb_error_name(result));

    for (Context &context : _transfers)
    {
        context.owner = this;
        context.transfer = libusb_alloc_transfer(0);
        context.slot = nullptr;
    }
}

AaUsbTransport::~AaUsbTransport()
{
    close();
    for (Context &context : _transfers)
    {
        if (context.transfer)
        {
            libusb_free_transfer(context.transfer);
            context.transfer = nullptr;
        }
    }
    if (_context)
    {
        libusb_exit(_context);
        _context = nullptr;
    }
}

void AaUsbTransport::onTransfer(libusb_transfer *transfer)
{
    if (!transfer || !transfer->user_data)
        return;

    Context *c = static_cast<Context *>(transfer->user_data);
    if (!c->owner->_connected)
        return;

    log_p("Transfer %d [%d] > %s", transfer->actual_length, transfer->status,
          bytes(transfer->buffer, transfer->actual_length, 40).c_str());

    if (transfer->status == LIBUSB_TRANSFER_CANCELLED)
        return;

    if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE)
    {
        c->owner->_connected = false;
        c->owner->_processQueue.notify();
        return;
    }

    if (transfer->status == LIBUSB_TRANSFER_COMPLETED)
    {
        c->slot->commit(transfer->actual_length);
        c->slot = c->owner->_processQueue.get();
        if (!c->slot)
        {
            log_e("Can't allocate data slot for next usb transfer, increase usb buffer slots");
            c->owner->_connected = false;
            c->owner->_processQueue.notify();
            return;
        }
        c->transfer->buffer = c->slot->data;
    }
    int status = libusb_submit_transfer(c->transfer);
    if (status != LIBUSB_SUCCESS)
    {
        log_w("USB transfer re-submit failed with status %d", status);
        c->owner->_connected = false;
        c->owner->_processQueue.notify();
    }
}

// Wait for a phone to show up in accessory mode; kick the AOAP switch on
// anything that looks like a candidate while waiting.
libusb_device_handle *AaUsbTransport::waitForAccessory(std::atomic<bool> &active)
{
    libusb_device_handle *handler = aoap::openAccessory(_context);
    if (handler)
        return handler;

    if (aoap::switchCandidates(_context) == 0)
        return nullptr;

    // Devices re-enumerate within a few hundred ms; the first connection can
    // take longer because the phone shows a consent dialog.
    int waited = 0;
    while (active.load() && waited < Settings::aaAccessoryTimeout)
    {
        interruptibleSleep(active, AA_RECONNECT_TIMEOUT);
        waited += AA_RECONNECT_TIMEOUT;
        handler = aoap::openAccessory(_context);
        if (handler)
            return handler;
    }
    return nullptr;
}

bool AaUsbTransport::link(libusb_device_handle *handler)
{
    // Neither libusb_reset_device nor an unconditional set_configuration here:
    // both drop the phone out of accessory mode. set_configuration on a device
    // already in the target configuration acts as a lightweight *device reset*,
    // which makes an accessory-mode phone re-enumerate in a loop. Only set it
    // if it isn't already active.
    libusb_set_auto_detach_kernel_driver(handler, 1);
    int currentConfig = 0;
    if (libusb_get_configuration(handler, &currentConfig) == LIBUSB_SUCCESS && currentConfig != 1)
    {
        int result = libusb_set_configuration(handler, 1);
        if (result != LIBUSB_SUCCESS)
            log_v("set_configuration: %s (continuing)", libusb_error_name(result));
    }

    if (libusb_claim_interface(handler, 0) != LIBUSB_SUCCESS)
    {
        log_w("Can't claim interface");
        return false;
    }

    libusb_device *device = libusb_get_device(handler);
    struct libusb_config_descriptor *config = nullptr;
    if (libusb_get_active_config_descriptor(device, &config) != LIBUSB_SUCCESS)
    {
        log_w("Can't get config descriptor");
        return false;
    }

    _epIn = 0;
    _epOut = 0;
    for (int i = 0; i < config->interface[0].altsetting[0].bNumEndpoints; i++)
    {
        const struct libusb_endpoint_descriptor *ep = &config->interface[0].altsetting[0].endpoint[i];
        if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
            continue;
        if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN)
            _epIn = ep->bEndpointAddress;
        else
            _epOut = ep->bEndpointAddress;
    }
    libusb_free_config_descriptor(config);

    if (!_epIn || !_epOut)
    {
        log_w("Accessory device has no bulk endpoint pair");
        return false;
    }

    log_i("Accessory linked %d:%d speed: %d", libusb_get_bus_number(device),
          libusb_get_device_address(device), libusb_get_device_speed(device));
    return true;
}

bool AaUsbTransport::startTransfers()
{
    _processQueue.reset();
    for (Context &context : _transfers)
    {
        context.owner = this;
        context.slot = _processQueue.get();
        if (context.slot == nullptr)
        {
            log_e("Can't allocate data slot for usb transfer, increase usb buffer slots");
            return false;
        }
        libusb_fill_bulk_transfer(context.transfer, _handle, _epIn, context.slot->data,
                                  context.slot->size, AaUsbTransport::onTransfer, &context, 0);
        if (libusb_submit_transfer(context.transfer) != LIBUSB_SUCCESS)
        {
            log_w("USB transfer submit failed");
            return false;
        }
    }
    return true;
}

void AaUsbTransport::readLoop()
{
    setThreadName("aa-read");
    setThreadPriority(ThreadPriority::Realtime);
    timeval timeout{0, 1000};

    log_d("AA reading thread started");
    while (_connected)
        libusb_handle_events_timeout_completed(_context, &timeout, nullptr);

    log_v("Canceling transfer requests");
    for (Context &context : _transfers)
    {
        if (context.transfer)
            libusb_cancel_transfer(context.transfer);
        libusb_handle_events_timeout_completed(_context, &timeout, nullptr);
    }
    log_v("AA reading thread stopped");
}

bool AaUsbTransport::open(std::atomic<bool> &active)
{
    libusb_device_handle *handler = waitForAccessory(active);
    if (!handler)
        return false;

    _handle = handler;
    if (!link(handler))
    {
        close();
        return false;
    }

    _connected = true;
    if (!startTransfers())
    {
        _connected = false;
        close();
        return false;
    }

    _readThread = std::thread(&AaUsbTransport::readLoop, this);
    return true;
}

void AaUsbTransport::close()
{
    _connected = false;
    _processQueue.notify();

    if (_readThread.joinable())
        _readThread.join();

    if (_handle)
    {
        libusb_release_interface(_handle, 0);
        // Reset before dropping: kicks the phone out of accessory mode so the
        // next open() does a fresh AOAP switch instead of reconnecting to a
        // wedged/half-open pipe (which NAKs every write -> ERROR_TIMEOUT).
        libusb_reset_device(_handle);
        libusb_close(_handle);
        _handle = nullptr;
    }
}

bool AaUsbTransport::read(uint8_t *dst, uint32_t len)
{
    return _processQueue.read(dst, len, _connected);
}

bool AaUsbTransport::write(const uint8_t *src, uint32_t len)
{
    if (!_handle)
        return false;

    const int maxRetries = Settings::usbWriteRetries;
    const int timeoutMs = Settings::usbWriteTimeoutMs;
    int status = LIBUSB_ERROR_TIMEOUT;
    int transferred = 0;

    for (int attempt = 0;; attempt++)
    {
        transferred = 0;
        status = libusb_bulk_transfer(_handle, _epOut, const_cast<uint8_t *>(src), len,
                                      &transferred, timeoutMs);
        if (status == LIBUSB_SUCCESS && transferred == (int)len)
            return true;

        // Retry ONLY the clean transient timeout where nothing was sent: on the
        // F1C200s the video RX-DMA holds the shared USB FIFO (BUS_SEL=1), so a
        // PIO control write in that window times out with 0 bytes -- re-issuing
        // is safe and the retry lands in a DMA gap. Any other error (real
        // disconnect) or a partial write (desync) is NOT retried.
        bool cleanTimeout = (status == LIBUSB_ERROR_TIMEOUT && transferred == 0);
        if (!cleanTimeout || attempt >= maxRetries)
            break;

        log_v("Bulk write timeout (%u bytes), retry %d/%d", len, attempt + 1, maxRetries);
        if (Settings::usbWriteRetryDelayMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(Settings::usbWriteRetryDelayMs));
    }

    log_w("Bulk write failed (%u bytes) > %s", len, libusb_error_name(status));
    _connected = false;
    _processQueue.notify();
    return false;
}
