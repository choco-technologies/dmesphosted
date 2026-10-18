#include "priv.h"
#include <string.h>

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool priv_parse_init(const uint8_t *payload, size_t len, priv_init_t *init)
{
    memset(init, 0, sizeof(*init));
    init->chip_id = 0xFF;
    if (len < 2 || payload[0] != PRIV_EVENT_INIT || (size_t)payload[1] + 2u > len)
        return false;

    const uint8_t *p   = payload + 2;
    size_t         left = payload[1];
    while (left >= 2)
    {
        uint8_t tag = p[0], tlen = p[1];
        if ((size_t)tlen + 2u > left)
            return false;
        const uint8_t *v = p + 2;
        switch (tag)
        {
            case PRIV_TLV_CAPABILITY:
                if (tlen >= 1) init->capabilities = v[0];
                break;
            case PRIV_TLV_CHIP_ID:
                if (tlen >= 1) init->chip_id = v[0];
                break;
            case PRIV_TLV_CAP_EXT:
                if (tlen >= 4) init->ext_capabilities = get_le32(v);
                break;
            case PRIV_TLV_FIRMWARE_VERSION:
                if (tlen >= 4) init->firmware_version = get_le32(v);
                break;
            case PRIV_TLV_RPC_VERSION:
                if (tlen >= 1)
                {
                    init->has_rpc_version = true;
                    init->rpc_version = v[0];
                }
                break;
            default:
                break;      /* Queue sizes, feature caps, ...: not needed */
        }
        p    += 2u + tlen;
        left -= 2u + tlen;
    }
    return true;
}

static uint8_t *put_tlv(uint8_t *p, uint8_t tag, uint8_t value)
{
    p[0] = tag;
    p[1] = 1;
    p[2] = value;
    return p + 3;
}

size_t priv_build_host_config(uint8_t *out, const priv_init_t *init)
{
    uint8_t *p = out + 2;
    p = put_tlv(p, PRIV_TLV_HOST_CAPABILITIES, 0);
    p = put_tlv(p, PRIV_TLV_RCVD_CHIP_ID, init->chip_id);
    p = put_tlv(p, PRIV_TLV_TEST_RAW_TP, 0);
    p = put_tlv(p, PRIV_TLV_THROTTLE_HIGH, 0);
    p = put_tlv(p, PRIV_TLV_THROTTLE_LOW, 0);
    if (init->has_rpc_version)
        p = put_tlv(p, PRIV_TLV_RPC_VERSION, PRIV_RPC_VERSION);

    out[0] = PRIV_EVENT_INIT;
    out[1] = (uint8_t)(p - out - 2);
    return (size_t)(p - out);
}
