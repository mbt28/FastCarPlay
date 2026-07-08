// Smoke-test AaBluetooth against a live BlueZ: power the adapter, install the
// agent, register the AA + HFP profiles, hold for a few seconds. Run as root.
//   make aa_bt_test && sudo ../out/aa_bt_test
#include <chrono>
#include <cstdio>
#include <thread>

#include "protocol/aa/aa_bluetooth.h"
#include "common/logger.h"

int main()
{
    set_log_level(6); // show the bt: info/debug lines
    aa_aaw::Params p{"192.168.53.1", 5277, "FastCarPlay", "carplay1234", "aa:bb:cc:dd:ee:ff", 6};
    AaBluetooth bt;
    if (!bt.start(p))
    {
        fprintf(stderr, "aa_bt_test: start FAILED\n");
        return 1;
    }
    fprintf(stderr, "aa_bt_test: BT bootstrap up, holding 8s (check bluetoothctl show)\n");
    std::this_thread::sleep_for(std::chrono::seconds(8));
    bt.stop();
    fprintf(stderr, "aa_bt_test: done\n");
    return 0;
}
