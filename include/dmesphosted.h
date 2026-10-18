#ifndef DMESPHOSTED_H
#define DMESPHOSTED_H

/*
 * dmesphosted - a Wi-Fi network device on an Espressif chip running
 * ESP-Hosted's co-processor firmware, over SPI.
 *
 * The node is a network device like dmeth's: read() / write() are 802.3
 * frames, DMDRVI_IOCTL_NET_* work as for any network driver, and the driver
 * registers the interface with dmnetif itself. The Wi-Fi is the
 * dmesphosted_ioctl_cmd_* of dmesphosted_types.h.
 */

#include "dmesphosted_defs.h"
#include "dmesphosted_types.h"

/**
 * @brief The name of a state ("connected", ...), for messages.
 */
dmod_dmesphosted_api(1.0, const char*, _state_name, ( dmesphosted_state_t state ));

/**
 * @brief The name of a chip a co-processor reports ("ESP32-C3", ...).
 */
dmod_dmesphosted_api(1.0, const char*, _chip_name, ( uint8_t chip_id ));

#endif /* DMESPHOSTED_H */
