# dmesphosted - configuration

A `dmesphosted` device is a section of a dmdevfs `.ini` with
`driver_name=dmesphosted`; its section name is the node's name (`[wlan0]` →
`/dev/wlan0`). Integers are decimal.

| Key | Default | |
|-----|---------|---|
| `interface` | `wlan0` | dmnetif's name of the interface |
| `friends_group` | - | The group of its bus and pins (below) |
| `handshake_interrupt` | - | The dmhaman name the Handshake pin's dmgpio `interrupt_handler` has. Without it: polled |
| `data_ready_interrupt` | - | ... the Data Ready pin's |
| `poll_interval_ms` | `5` | How often the pins are looked at without their interrupts (1..1000) |
| `thread_priority` | `1` | Of the driver's two threads |
| `power_save` | `off` | esp_wifi's power save: `off`, `min`, `max`. In it the access point holds the frames until a beacon: 100 ms and more for every packet |
| `tx_power` | `0` | The co-processor's highest transmit power, dBm (1..20; 0: its own) |
| `ssid`, `password` | - | A network to connect to from the start - only in a configuration of one's own, never in a published one |

## Its friends

In the same `friends_group`, found by their `friend_role` (dmdevfs reports
their nodes):

| `friend_role` | What | |
|---|---|---|
| `spi_bus` | A dmspi master, mode 3 (its chip select: dmspi's own `chip_select` friend, `nss_mode=soft`) | needed |
| `handshake` | A dmgpio input, rising-edge interrupt | needed |
| `data_ready` | A dmgpio input, rising-edge interrupt | without it the bus is polled |
| `reset` | A dmgpio output (open drain): the co-processor's reset (LOW ≥ 2 ms restarts it) | without it the co-processor must start after the host |

A complete board configuration: [`configs/board/stm32f746g-disco/wifi.ini`](../configs/board/stm32f746g-disco/wifi.ini).
