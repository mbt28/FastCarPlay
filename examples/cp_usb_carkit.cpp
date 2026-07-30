// cp_usb_carkit -- wired-CarPlay bring-up probe (run as root).
//
// Step 1 (this milestone): put a plugged iPhone into CarPlay USB mode the way
// LIVI's muxd.py does -- reveal the hidden configs with a vendor control request
// over usbfs (NOT libusb: libusb_control_transfer returns LIBUSB_ERROR_PIPE on
// iOS 26), then select config 6 via sysfs. Config 6 exposes the usbmux interface
// (-> lockdown/com.apple.carkit.service) + a CDC-NCM interface (-> the :7000 AV
// link over USB). Later milestones add the usbmux mux-TCP shim + the libimobile-
// device carkit open + the NCM bring-up.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include <dirent.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace
{
const char *APPLE_VID = "05ac";
const char *SYS_USB = "/sys/bus/usb/devices";

std::string readSys(const std::string &path)
{
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

// Find the (first) Apple device in sysfs; fills its sysfs dir, bus/dev, serial.
bool findIphone(std::string &sysdir, int &bus, int &dev, std::string &serial)
{
    DIR *d = opendir(SYS_USB);
    if (!d)
        return false;
    bool found = false;
    for (struct dirent *e; (e = readdir(d));)
    {
        std::string base = std::string(SYS_USB) + "/" + e->d_name + "/";
        if (readSys(base + "idVendor") != APPLE_VID)
            continue;
        std::string s = readSys(base + "serial");
        if (s.empty())
            continue;
        sysdir = base;
        serial = s;
        bus = atoi(readSys(base + "busnum").c_str());
        dev = atoi(readSys(base + "devnum").c_str());
        found = true;
        break;
    }
    closedir(d);
    return found;
}

int numConfigs(const std::string &sysdir)
{
    std::string s = readSys(sysdir + "bNumConfigurations");
    return s.empty() ? 0 : atoi(s.c_str());
}

// The reveal: vendor ctrl 0xC0/0x52 wValue=0 wIndex=4 (device->host, 1 byte) via
// the usbfs USBDEVFS_CONTROL ioctl on /dev/bus/usb/BUS/DEV.
bool revealConfigs(int bus, int dev)
{
    char node[64];
    snprintf(node, sizeof(node), "/dev/bus/usb/%03d/%03d", bus, dev);
    int fd = open(node, O_RDWR);
    if (fd < 0)
    {
        printf("open %s: %s (root?)\n", node, strerror(errno));
        return false;
    }
    uint8_t buf[1] = {0};
    struct usbdevfs_ctrltransfer ct;
    memset(&ct, 0, sizeof(ct));
    ct.bRequestType = 0xC0;
    ct.bRequest = 0x52;
    ct.wValue = 0x0000;
    ct.wIndex = 0x0004;
    ct.wLength = 1;
    ct.timeout = 3000;
    ct.data = buf;
    int r = ioctl(fd, USBDEVFS_CONTROL, &ct);
    close(fd);
    printf("0x52 reveal (usbfs) -> %d (%s), reply=0x%02x\n", r, r < 0 ? strerror(errno) : "ok", buf[0]);
    return r >= 0;
}

bool setConfigSysfs(const std::string &sysdir, int cfg)
{
    std::ofstream f(sysdir + "bConfigurationValue");
    if (!f)
    {
        printf("open %sbConfigurationValue for write: %s (root?)\n", sysdir.c_str(), strerror(errno));
        return false;
    }
    f << cfg;
    f.flush();
    return f.good();
}
} // namespace

int main()
{
    std::string sysdir, serial;
    int bus = 0, dev = 0;
    if (!findIphone(sysdir, bus, dev, serial))
    {
        printf("no iPhone on USB (plugged + trusted + unlocked?)\n");
        return 1;
    }
    printf("iPhone %s at bus %d dev %d [%s] numConfigs=%d curCfg=%s\n", serial.c_str(), bus, dev,
           sysdir.c_str(), numConfigs(sysdir), readSys(sysdir + "bConfigurationValue").c_str());

    // 1) Reveal the CarPlay configs if they are still hidden.
    if (numConfigs(sysdir) < 6)
    {
        if (!revealConfigs(bus, dev))
            return 1;
        // The phone re-enumerates; wait for it to reappear with >= 6 configs.
        bool ok = false;
        for (int i = 0; i < 30; i++)
        {
            usleep(200000);
            if (findIphone(sysdir, bus, dev, serial) && numConfigs(sysdir) >= 6)
            {
                ok = true;
                break;
            }
        }
        if (!ok)
        {
            printf("phone did not expose CarPlay configs 5/6 (still %d)\n", numConfigs(sysdir));
            return 1;
        }
        printf("CarPlay configs revealed: numConfigs=%d (bus %d dev %d)\n", numConfigs(sysdir), bus, dev);
    }

    // 2) Select config 6.
    if (readSys(sysdir + "bConfigurationValue") != "6")
    {
        if (!setConfigSysfs(sysdir, 6))
            return 1;
        usleep(500000);
        findIphone(sysdir, bus, dev, serial);
    }

    std::string cur = readSys(sysdir + "bConfigurationValue");
    printf("current config = %s\n", cur.c_str());
    if (cur == "6")
    {
        printf("\n>>> iPhone is in CONFIG-6 CARPLAY USB MODE <<<\n");
        printf("interfaces now on config 6:\n");
        // list the interface classes so the mux (iAP2) + NCM ifaces are visible.
        DIR *d = opendir(sysdir.c_str());
        if (d)
        {
            for (struct dirent *e; (e = readdir(d));)
            {
                std::string cls = readSys(sysdir + e->d_name + "/bInterfaceClass");
                std::string sub = readSys(sysdir + e->d_name + "/bInterfaceSubClass");
                std::string num = readSys(sysdir + e->d_name + "/bInterfaceNumber");
                if (!cls.empty())
                    printf("  iface %s: class=%s subclass=%s\n", num.c_str(), cls.c_str(), sub.c_str());
            }
            closedir(d);
        }
        return 0;
    }
    printf("failed to reach config 6\n");
    return 1;
}
