#!/usr/bin/env python3
"""Read the temperature of an NTAG 5 link + TMP117 sensor tag through the Flipper CCID reader.

Command sequence from the BeUno app (com.dsruptive2.BeUno), sent as raw ISO 15693 frames
with the reader's FF 00 00 00 passthrough:

  SELECT                22 25 <UID>                  put the tag into the selected state
  WRITE I2C (D4)        12 D4 04 48 02 01 0C 00      TMP117 @ 0x48: config (reg 01) = 0C00,
                                                     one-shot conversion
  (wait 50 ms)
  WRITE I2C (D4)        12 D4 04 48 00 00            register pointer = 00 (temperature)
  READ I2C (D5)         12 D5 04 C8 01               read 2 bytes into the tag's SRAM
  READ SRAM (D2)        12 D2 04 00 00               SRAM block 0: flags, T_hi, T_lo, ...

Flags 0x12: high data rate + select flag. 04 is the NXP manufacturer code. The temperature is
signed big-endian in 1/128 degC, 0x8000 means no conversion has happened (the app treats
256.0 as an error).

The sensor is powered by the tag's energy harvesting output, configured in config block 0x3D
(READ CONFIG: 12 C0 04 3D 00, 1D 00 00 00 = enabled, 3.0 V). This script only reads it.
Nothing here writes NFC tag memory: the TMP117 config register and the tag SRAM are volatile.

Usage: ntag5_temperature.py [readings]   (needs pyscard and the reader running a card session)
"""

import sys
import time

from smartcard.System import readers
from smartcard.util import toHexString

I2C_ADDRESS = 0x48
ONE_SHOT_WAIT_S = 0.05

EH_LEVELS = {1: "1.8 V", 3: "2.4 V", 5: "3.0 V"}


def transparent(connection, name, frame):
    """Send a raw ISO 15693 frame, return the raw response (flags first)."""
    data, sw1, sw2 = connection.transmit([0xFF, 0x00, 0x00, 0x00, len(frame)] + frame)
    if (sw1, sw2) != (0x90, 0x00):
        sys.exit(f"{name}: no answer, SW {sw1:02X}{sw2:02X}")
    if data and data[0] & 0x01:
        sys.exit(f"{name}: tag error {toHexString(data)}")
    return data


def read_temperature(connection):
    transparent(
        connection,
        "start one-shot",
        [0x12, 0xD4, 0x04, I2C_ADDRESS, 0x02, 0x01, 0x0C, 0x00],
    )
    time.sleep(ONE_SHOT_WAIT_S)
    transparent(connection, "set pointer", [0x12, 0xD4, 0x04, I2C_ADDRESS, 0x00, 0x00])
    transparent(connection, "read I2C", [0x12, 0xD5, 0x04, 0x80 | I2C_ADDRESS, 0x01])
    sram = transparent(connection, "read SRAM", [0x12, 0xD2, 0x04, 0x00, 0x00])

    raw = (sram[1] << 8) | sram[2]
    if raw == 0x8000:
        sys.exit("sensor returned 0x8000, no conversion (no power from the field?)")

    value = raw - 0x10000 if raw & 0x8000 else raw
    return value / 128.0, raw


def main():
    readings = int(sys.argv[1]) if len(sys.argv) > 1 else 1

    reader = [r for r in readers() if "Flipper" in str(r)][0]
    connection = reader.createConnection()
    connection.connect()

    uid, sw1, sw2 = connection.transmit([0xFF, 0xCA, 0x00, 0x00, 0x00])
    print("UID:", toHexString(uid[::-1]))

    transparent(connection, "SELECT", [0x22, 0x25] + uid)

    config = transparent(connection, "read EH config", [0x12, 0xC0, 0x04, 0x3D, 0x00])
    print("Energy harvesting:", EH_LEVELS.get(config[1] & 0x07, "disabled"))

    for i in range(readings):
        temperature, raw = read_temperature(connection)
        print(f"{temperature:6.2f} degC  (raw 0x{raw:04X})")
        if i + 1 < readings:
            time.sleep(1)


if __name__ == "__main__":
    main()
