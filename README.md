# NFC CCID Reader

Turns the Flipper Zero into a USB smartcard reader (CCID class) for contactless cards.
The host talks to cards through its normal PC/SC stack (pcsc-lite, Windows, macOS).

The serial CLI keeps working while the app runs: the device is a composite of the
regular CDC port and the CCID reader (`0483:5742`). This needs the `usb_cdc_ext` USB mode
from this branch (firmware API 88.5).

## Supported cards

| Card | How the host sees it |
|---|---|
| ISO 14443-4A (bank cards, eIDs, DESFire, ...) | Smartcard, APDUs go to the card. ATR `3B 8n 80 01 <historical bytes> TCK` |
| ISO 15693 (ICODE, ...) | PC/SC storage card, ATR `3B 8F 80 01 80 4F 0C A0 00 00 03 06 0B 00 00 00 00 00 00 63` |

ISO 14443-3A cards without ISO 14443-4 (MIFARE Classic, Ultralight) are shown on screen
but not reported to the host. ISO 14443-B is not supported yet.

APDUs up to 4 KB (commands) and 16 KB (responses) are supported, using the CCID extended
APDU level. Long frames are chained on the ISO-DEP side.

## Reader commands (CLA FF)

| APDU | Card | Function |
|---|---|---|
| `FF CA 00 00 00` | all | UID. ISO 15693: transmission order, least significant byte first |
| `FF CA 01 00 00` | ISO 14443-4A | Historical bytes from the ATS |
| `FF B0 00 <block> 00` | ISO 15693 | Read single block |
| `FF D6 00 <block> <Lc> <data>` | ISO 15693 | Write single block |
| `FF 00 00 00 <Lc> <frame>` | ISO 15693 | Send a raw request (flags, command, parameters, no CRC), returns the raw response |

`FF 00 00 00` is specific to this reader. Example, addressed Get System Information:
`FF 00 00 00 0A 22 2B <UID in transmission order>`.

## Host setup

macOS and Windows bind CCID readers by USB class, nothing to do.

pcsc-lite on Linux only loads libccid for VID/PID pairs listed in
`/usr/lib/pcsc/drivers/ifd-ccid.bundle/Contents/Info.plist`. Until the reader is listed
upstream, add an entry at the start of the `ifdVendorID`, `ifdProductID` and
`ifdFriendlyName` arrays:

```xml
<string>0x0483</string>              <!-- ifdVendorID -->
<string>0x5742</string>              <!-- ifdProductID -->
<string>Flipper Zero NFC CCID</string> <!-- ifdFriendlyName -->
```

A libccid package update overwrites the file.

## Tools

`tools/ntag5_temperature.py` reads NTAG 5 link tags with a TMP117 temperature sensor behind
the tag's I2C master, using the command sequence of the BeUno app. The header of the script
documents the commands.

## Build and run

Stock firmware does not have the `usb_cdc_ext` USB mode yet. `firmware/usb_cdc_ext.patch`
adds it (plus an atomic `furi_thread_flags_clear`) on top of upstream commit `9981d9d4`.
The app only builds against, and runs on, firmware with the patch.

Build and flash the patched firmware, then build the app with uFBT against its SDK:

```sh
git clone https://github.com/flipperdevices/flipperzero-firmware.git
cd flipperzero-firmware
git checkout 9981d9d4
git apply ../nfc_ccid/firmware/usb_cdc_ext.patch
./fbt COMPACT=1 DEBUG=0 flash_usb_full

cd ../nfc_ccid
ufbt update --local=../flipperzero-firmware/dist/f7-C/flipper-z-f7-sdk-local.zip --hw-target=7
ufbt launch
```

CI does the same (without flashing) and publishes the FAP as a build artifact.

The patch is the firmware side of the `nfc-ccid` branch in the firmware tree. After changing
it there, regenerate it with:

```sh
git diff 9981d9d4 HEAD -- furi targets > ../nfc_ccid/firmware/usb_cdc_ext.patch
```

Starting the app switches USB to CDC + CCID, so the host sees the Flipper reconnect.
Back exits and restores the previous USB mode.

## Screen

- Card UID and type, ISO 14443-4 frame size or ISO 15693 memory layout
- `APDU / err / retry`: exchanges, failed exchanges, ISO-DEP frames repeated by error recovery
- `USB lost events / tx err`: bulk OUT packets whose interrupt event got lost and that were
  picked up by the 20 ms idle poll instead, failed bulk IN writes

## Known issues

- About 0.4% of bulk OUT packets lose their wake-up event. The idle poll covers it, so it
  costs up to 20 ms instead of a hang. Root cause unknown.
- The slot change interrupt endpoint is disabled (`CCID_USE_NOTIFY_EP`), hosts poll the
  slot status instead.
