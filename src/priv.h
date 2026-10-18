#ifndef DMESPHOSTED_PRIV_H
#define DMESPHOSTED_PRIV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The bring-up on FRAME_IF_PRIV (priv_pkt_type FRAME_PRIV_EVENT). After its
 * start the co-processor sends an init event, [0x22][len][TLVs...] with
 * [tag][len][value] TLVs of what it is; the host answers with the same
 * event of its own TLVs - and the co-processor opens its data path. What
 * the host answers is what Espressif's MCU host does: no capabilities, the
 * chip seen, no throughput test, no flow control, and its RPC version back
 * when the co-processor gave one (the co-processor stops on a mismatch).
 * Both stay on the 12-byte header.
 */

#define PRIV_EVENT_INIT                 0x22u

/* Co-processor -> host */
#define PRIV_TLV_CAPABILITY             0x11u   /* u8 */
#define PRIV_TLV_CHIP_ID                0x12u   /* u8 */
#define PRIV_TLV_CAP_EXT                0x16u   /* u32 LE */
#define PRIV_TLV_FIRMWARE_VERSION       0x17u   /* u32 LE */
#define PRIV_TLV_RPC_VERSION            0x1Au   /* u8 - both directions */

/* Host -> co-processor */
#define PRIV_TLV_HOST_CAPABILITIES      0x44u
#define PRIV_TLV_RCVD_CHIP_ID           0x45u
#define PRIV_TLV_TEST_RAW_TP            0x46u
#define PRIV_TLV_THROTTLE_HIGH          0x47u
#define PRIV_TLV_THROTTLE_LOW           0x48u

/* Capability bits */
#define PRIV_CAP_WLAN_SPI               0x20u
#define PRIV_CAP_CHECKSUM               0x80u

/* The RPC this driver speaks (ESP_HOSTED_RPC_VERSION_V2: the MCU one) */
#define PRIV_RPC_VERSION                0x02u

#define PRIV_HOST_CONFIG_MAX            20u

typedef struct
{
    uint8_t     capabilities;
    uint32_t    ext_capabilities;
    uint8_t     chip_id;
    uint32_t    firmware_version;
    bool        has_rpc_version;
    uint8_t     rpc_version;
} priv_init_t;

/**
 * Reads an init event - the payload of a FRAME_IF_PRIV frame. False when it
 * is not one (or a TLV runs past its end).
 */
bool priv_parse_init(const uint8_t *payload, size_t len, priv_init_t *init);

/**
 * The host's answer to @p init into @p out (PRIV_HOST_CONFIG_MAX bytes);
 * returns its bytes.
 */
size_t priv_build_host_config(uint8_t *out, const priv_init_t *init);

#endif /* DMESPHOSTED_PRIV_H */
