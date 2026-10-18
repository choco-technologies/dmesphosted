#include "dmod.h"
#include "dmosi.h"
#include "dmesphosted.h"
#include <string.h>

/**
 * @brief The Wi-Fi of a dmesphosted device, from the shell.
 *
 * Opens the device node (configured by dmdevfs from the board's INI) and
 * uses its dmesphosted_ioctl_cmd_* commands - the network itself (address,
 * routes) is ifconfig's, as for any interface.
 */

#define DEFAULT_DEVICE      "/dev/wlan0"
#define SCAN_MAX            16
#define WAIT_STEP_MS        250
#define CONNECT_WAIT_MS     15000

static void print_usage(const char *name)
{
    Dmod_Printf("Usage: %s [-d DEVICE] COMMAND [ARGS]\n", name);
    Dmod_Printf("  status                   the connection (default)\n");
    Dmod_Printf("  info                     the co-processor: chip, firmware, MAC, counters\n");
    Dmod_Printf("  scan                     the networks in range\n");
    Dmod_Printf("  connect SSID [PASSWORD]  connect (and again whenever lost), wait for it\n");
    Dmod_Printf("  disconnect               disconnect and forget the network\n");
    Dmod_Printf("DEVICE defaults to %s\n", DEFAULT_DEVICE);
}

/* A switch, not a table of name pointers: the dmod loader does not relocate
 * pointers stored in initialized data. */
static const char *auth_name(uint8_t authmode)
{
    switch (authmode)
    {
        case 0:  return "open";
        case 1:  return "WEP";
        case 2:  return "WPA";
        case 3:  return "WPA2";
        case 4:  return "WPA/WPA2";
        case 5:  return "WPA2-Enterprise";
        case 6:  return "WPA3";
        case 7:  return "WPA2/WPA3";
        default: return "other";
    }
}

static void print_mac(const char *label, const uint8_t mac[DMESPHOSTED_MAC_LEN])
{
    Dmod_Printf("%s%02x:%02x:%02x:%02x:%02x:%02x\n", label, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int show_status(void *dev)
{
    dmesphosted_status_t st;
    int ret = Dmod_Ioctl(dev, dmesphosted_ioctl_cmd_get_status, &st);
    if (ret != 0)
        return ret;

    Dmod_Printf("state:   %s\n", dmesphosted_state_name(st.state));
    if (st.ssid[0] != '\0')
        Dmod_Printf("network: %s\n", st.ssid);
    if (st.state == dmesphosted_state_connected)
    {
        print_mac("access point: ", st.bssid);
        Dmod_Printf("channel: %u, signal %d dBm\n", st.channel, st.rssi);
    }
    if (st.reason != 0 && st.state != dmesphosted_state_connected)
        Dmod_Printf("last disconnection: reason %u\n", st.reason);
    return 0;
}

static int show_info(void *dev)
{
    dmesphosted_info_t info;
    int ret = Dmod_Ioctl(dev, dmesphosted_ioctl_cmd_get_info, &info);
    if (ret != 0)
        return ret;

    uint32_t fw = info.firmware_version;
    Dmod_Printf("chip:      %s (0x%02x)\n", dmesphosted_chip_name(info.chip_id), info.chip_id);
    if (info.chip_id != DMESPHOSTED_CHIP_UNKNOWN)
    {
        Dmod_Printf("firmware:  ESP-Hosted %u.%u.%u\n",
                    (unsigned)((fw >> 16) & 0xFF), (unsigned)((fw >> 8) & 0xFF), (unsigned)(fw & 0xFF));
        Dmod_Printf("caps:      0x%02x / 0x%08x\n", info.capabilities, (unsigned)info.ext_capabilities);
        print_mac("MAC:       ", info.mac);
    }
    Dmod_Printf("frames:    rx %u (dropped %u), tx %u (dropped %u), bad %u\n",
                (unsigned)info.rx_frames, (unsigned)info.rx_dropped,
                (unsigned)info.tx_frames, (unsigned)info.tx_dropped, (unsigned)info.bad_frames);
    Dmod_Printf("restarts:  %u\n", (unsigned)info.resets);
    return 0;
}

static int do_scan(void *dev)
{
    dmesphosted_ap_t   aps[SCAN_MAX];
    dmesphosted_scan_t request = { aps, SCAN_MAX, 0 };

    Dmod_Printf("scanning...\n");
    int ret = Dmod_Ioctl(dev, dmesphosted_ioctl_cmd_scan, &request);
    if (ret != 0)
        return ret;

    Dmod_Printf("%-32s  %4s  %2s  %-10s  %s\n", "SSID", "dBm", "ch", "security", "access point");
    for (uint16_t i = 0; i < request.count; i++)
    {
        const dmesphosted_ap_t *ap = &aps[i];
        Dmod_Printf("%-32s  %4d  %2u  %-10s  %02x:%02x:%02x:%02x:%02x:%02x\n",
                    ap->ssid[0] != '\0' ? ap->ssid : "(hidden)", ap->rssi, ap->channel, auth_name(ap->authmode),
                    ap->bssid[0], ap->bssid[1], ap->bssid[2], ap->bssid[3], ap->bssid[4], ap->bssid[5]);
    }
    Dmod_Printf("%u network(s)\n", request.count);
    return 0;
}

static int do_connect(void *dev, const char *ssid, const char *password)
{
    dmesphosted_connect_t network;
    memset(&network, 0, sizeof(network));
    if (strlen(ssid) > DMESPHOSTED_SSID_MAX || (password != NULL && strlen(password) > DMESPHOSTED_PASSWORD_MAX))
    {
        Dmod_Printf("wifi: SSID at most %d, password at most %d characters\n",
                    DMESPHOSTED_SSID_MAX, DMESPHOSTED_PASSWORD_MAX);
        return -1;
    }
    strcpy(network.ssid, ssid);
    if (password != NULL)
        strcpy(network.password, password);

    int ret = Dmod_Ioctl(dev, dmesphosted_ioctl_cmd_connect, &network);
    if (ret != 0)
    {
        Dmod_Printf("wifi: not taken (%d) - a password has 8..64 characters\n", ret);
        return ret;
    }

    Dmod_Printf("connecting to %s...\n", ssid);
    for (uint32_t waited = 0; waited < CONNECT_WAIT_MS; waited += WAIT_STEP_MS)
    {
        dmesphosted_status_t st;
        if (Dmod_Ioctl(dev, dmesphosted_ioctl_cmd_get_status, &st) == 0 && st.state == dmesphosted_state_connected)
            return show_status(dev);
        dmosi_thread_sleep(WAIT_STEP_MS);
    }
    Dmod_Printf("not connected yet - it keeps trying (wifi status)\n");
    return show_status(dev);
}

int main(int argc, char **argv)
{
    const char *device = DEFAULT_DEVICE;
    int i = 1;

    if (i + 1 < argc && strcmp(argv[i], "-d") == 0)
    {
        device = argv[i + 1];
        i += 2;
    }
    const char *command = (i < argc) ? argv[i] : "status";
    if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0 || strcmp(command, "help") == 0)
    {
        print_usage(argv[0]);
        return 0;
    }

    void *dev = Dmod_FileOpen(device, "r+");
    if (dev == NULL)
    {
        Dmod_Printf("wifi: cannot open %s\n", device);
        return -1;
    }

    int ret;
    if (strcmp(command, "status") == 0)
        ret = show_status(dev);
    else if (strcmp(command, "info") == 0)
        ret = show_info(dev);
    else if (strcmp(command, "scan") == 0)
        ret = do_scan(dev);
    else if (strcmp(command, "connect") == 0 && i + 1 < argc)
        ret = do_connect(dev, argv[i + 1], (i + 2 < argc) ? argv[i + 2] : NULL);
    else if (strcmp(command, "disconnect") == 0)
        ret = Dmod_Ioctl(dev, dmesphosted_ioctl_cmd_disconnect, NULL);
    else
    {
        print_usage(argv[0]);
        ret = -1;
    }

    if (ret != 0 && ret != -1)
        Dmod_Printf("wifi: %s failed (%d)\n", command, ret);
    Dmod_FileClose(dev);
    return ret;
}
