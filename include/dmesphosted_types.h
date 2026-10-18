#ifndef DMESPHOSTED_TYPES_H
#define DMESPHOSTED_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmdrvi_ioctl.h"       /* DMDRVI_WIFI_*: the Wi-Fi contract (dmdrvi 2.9) */

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
 * @brief The co-processor, as the driver knows it (DMDRVI_IOCTL_WIFI_GET_INFO reports it in dmdrvi's generic form)
 */
typedef struct
{
    uint8_t     chip_id;                /**< DMESPHOSTED_CHIP_* */
    uint8_t     capabilities;           /**< ESP-Hosted's capability bits (EH_PRIV_CAPABILITY) */
    uint32_t    ext_capabilities;       /**< ... and the extended ones (EH_PRIV_CAP_EXT) */
    uint32_t    firmware_version;       /**< As it reports it (EH_PRIV_FIRMWARE_VERSION) */
    uint8_t     mac[DMDRVI_WIFI_MAC_LEN]; /**< Its station's MAC address */
    uint32_t    rx_frames;              /**< 802.3 frames from / to the network */
    uint32_t    tx_frames;
    uint32_t    rx_dropped;             /**< ... received with no room for them */
    uint32_t    tx_dropped;             /**< ... not sent: the link down, no room */
    uint32_t    bad_frames;             /**< Bus frames with a bad header or checksum */
    uint32_t    resets;                 /**< Times the co-processor was (re)started */
} dmesphosted_info_t;

/**
 * @brief Opaque driver context type (forward declaration)
 *
 * The concrete definition is private to the dmesphosted driver.
 */
struct dmdrvi_context;
typedef struct dmdrvi_context *dmdrvi_context_t;

#endif /* DMESPHOSTED_TYPES_H */
