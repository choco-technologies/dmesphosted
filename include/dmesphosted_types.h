#ifndef DMESPHOSTED_TYPES_H
#define DMESPHOSTED_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmdrvi_ioctl.h"

/** The longest SSID and passphrase of a station (esp_wifi's) */
#define DMESPHOSTED_SSID_MAX            32
#define DMESPHOSTED_PASSWORD_MAX        64
#define DMESPHOSTED_MAC_LEN             6

/** Chip IDs the co-processor reports (EH_PRIV_FIRMWARE_CHIP_ID) */
#define DMESPHOSTED_CHIP_ESP32          0x00
#define DMESPHOSTED_CHIP_ESP32S2        0x02
#define DMESPHOSTED_CHIP_ESP32C3        0x05
#define DMESPHOSTED_CHIP_ESP32S3        0x09
#define DMESPHOSTED_CHIP_ESP32C2        0x0C
#define DMESPHOSTED_CHIP_ESP32C6        0x0D
#define DMESPHOSTED_CHIP_ESP32C5        0x17
#define DMESPHOSTED_CHIP_UNKNOWN        0xFF    /**< Not reached (yet) */

/**
 * @brief Where the Wi-Fi is
 */
typedef enum
{
    dmesphosted_state_off = 0,          /**< No co-processor: not reached (yet), or lost */
    dmesphosted_state_starting,         /**< Reached, its Wi-Fi is being started */
    dmesphosted_state_idle,             /**< Wi-Fi started, not connected to a network */
    dmesphosted_state_connecting,       /**< Connecting (again) to the network set */
    dmesphosted_state_connected,        /**< Connected: the link is up */
} dmesphosted_state_t;

/**
 * @brief The co-processor - dmesphosted_ioctl_cmd_get_info
 */
typedef struct
{
    uint8_t     chip_id;                /**< DMESPHOSTED_CHIP_* */
    uint8_t     capabilities;           /**< ESP-Hosted's capability bits (EH_PRIV_CAPABILITY) */
    uint32_t    ext_capabilities;       /**< ... and the extended ones (EH_PRIV_CAP_EXT) */
    uint32_t    firmware_version;       /**< As it reports it (EH_PRIV_FIRMWARE_VERSION) */
    uint8_t     mac[DMESPHOSTED_MAC_LEN]; /**< Its station's MAC address */
    uint32_t    rx_frames;              /**< 802.3 frames from / to the network */
    uint32_t    tx_frames;
    uint32_t    rx_dropped;             /**< ... received with no room for them */
    uint32_t    tx_dropped;             /**< ... not sent: the link down, no room */
    uint32_t    bad_frames;             /**< Bus frames with a bad header or checksum */
    uint32_t    resets;                 /**< Times the co-processor was (re)started */
} dmesphosted_info_t;

/**
 * @brief The connection - dmesphosted_ioctl_cmd_get_status
 */
typedef struct
{
    dmesphosted_state_t state;
    char        ssid[DMESPHOSTED_SSID_MAX + 1];     /**< The network set ("" none) */
    uint8_t     bssid[DMESPHOSTED_MAC_LEN];         /**< Its access point, when connected */
    uint8_t     channel;
    int8_t      rssi;                   /**< dBm when connected, 0: not known */
    uint16_t    reason;                 /**< Of the last disconnection (esp_wifi's wifi_err_reason_t), 0: none */
} dmesphosted_status_t;

/**
 * @brief The network to connect to - dmesphosted_ioctl_cmd_connect
 */
typedef struct
{
    char        ssid[DMESPHOSTED_SSID_MAX + 1];
    char        password[DMESPHOSTED_PASSWORD_MAX + 1];  /**< "" for an open network */
} dmesphosted_connect_t;

/**
 * @brief A network found - dmesphosted_ioctl_cmd_scan
 */
typedef struct
{
    char        ssid[DMESPHOSTED_SSID_MAX + 1];
    uint8_t     bssid[DMESPHOSTED_MAC_LEN];
    uint8_t     channel;
    int8_t      rssi;                   /**< dBm */
    uint8_t     authmode;               /**< esp_wifi's wifi_auth_mode_t: 0 open, 3 WPA2-PSK, 6 WPA3-PSK... */
} dmesphosted_ap_t;

typedef struct
{
    dmesphosted_ap_t   *aps;            /**< In: room for max of them */
    uint16_t            max;
    uint16_t            count;          /**< Out: how many were found (at most max) */
} dmesphosted_scan_t;

/**
 * @brief IOCTL commands of a dmesphosted device
 *
 * The node is a network device like dmeth's: one read() is one received
 * 802.3 frame, one write() one sent, and DMDRVI_IOCTL_NET_* work as for any
 * network driver (the link is up while connected). These add the Wi-Fi.
 */
typedef enum
{
    /* Private commands start at DMDRVI_IOCTL_CUSTOM_BASE - generic code must
     * know the node is a dmesphosted one before sending them. */
    dmesphosted_ioctl_cmd_get_info = DMDRVI_IOCTL_CUSTOM_BASE, /**< arg: dmesphosted_info_t* */
    dmesphosted_ioctl_cmd_get_status,   /**< arg: dmesphosted_status_t* */
    dmesphosted_ioctl_cmd_connect,      /**< arg: const dmesphosted_connect_t* - kept: connects again whenever lost */
    dmesphosted_ioctl_cmd_disconnect,   /**< arg: NULL - and forgets the network */
    dmesphosted_ioctl_cmd_scan,         /**< arg: dmesphosted_scan_t* - waits for the scan (a few seconds) */
    dmesphosted_ioctl_cmd_max
} dmesphosted_ioctl_cmd_t;

/**
 * @brief Opaque driver context type (forward declaration)
 *
 * The concrete definition is private to the dmesphosted driver.
 */
struct dmdrvi_context;
typedef struct dmdrvi_context *dmdrvi_context_t;

#endif /* DMESPHOSTED_TYPES_H */
