# EFR32FG28 port (TODO)

This directory will hold the Simplicity Studio / SLC project for the lock
mainboard. It implements `hal/hal.h` and `slp/crypto_port.h` and wires the
hardware events into the portable code under `firmware/lock/`.

| Piece | SDK component | Notes |
| --- | --- | --- |
| Sub-GHz radio | RAIL (433 MHz, 2-GFSK 50 kbps) | duty-cycled RX at the lock's slot, preamble detect, auto-ACK |
| BLE | Bluetooth stack + DMP | near-field unlock and maintenance only |
| Crypto | PSA Crypto, keys in Secure Vault | AES-CCM*, HKDF; keys never leave the chip |
| Storage | NVM3 | credentials, tx frame counter blocks, config |
| Power | EMU, sleeptimer | EM2 between events, FreeRTOS tickless idle |
| Bootloader | Gecko bootloader, signed GBL | dual-slot OTA |

Tasks: radio (highest priority, slot timer driven), lock control, user
interaction (card / keypad / BLE), background (log, OTA, storage).

The host build (`cmake -S . -B build`) compiles the same logic against
`port/host` so it can be unit-tested without a board.
