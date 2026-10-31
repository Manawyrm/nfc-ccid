#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <nfc/protocols/iso14443_3a/iso14443_3a_poller.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Largest number of historical bytes that fit into a PC/SC contactless ATR */
#define ISO_DEP_HISTORICAL_MAX (15)

/** ISO 14443-4 (ISO-DEP) half-duplex block protocol, PCD side, type A
 *
 * Runs on top of an ISO 14443-3A poller. All functions must be called from inside
 * the poller callback.
 */
typedef struct {
    Iso14443_3aPoller* poller;
    BitBuffer* tx;
    BitBuffer* rx;

    uint16_t fsc; /**< Largest frame the card accepts, including PCB and CRC */
    uint32_t fwt_fc; /**< Frame waiting time in carrier cycles */
    uint8_t block_number; /**< Next I-block number */
    uint32_t retry_count; /**< Frames repeated by error recovery */

    uint8_t ats[256];
    size_t ats_size;
    uint8_t historical[ISO_DEP_HISTORICAL_MAX];
    size_t historical_size;
} IsoDep;

typedef enum {
    IsoDepErrorNone,
    IsoDepErrorTransmission, /**< Timeout, CRC or collision, the card is probably gone */
    IsoDepErrorProtocol, /**< Unexpected block from the card */
    IsoDepErrorOverflow, /**< Response does not fit into the caller's buffer */
} IsoDepError;

IsoDep* iso_dep_alloc(void);

void iso_dep_free(IsoDep* instance);

/** Send RATS, parse the ATS and wait the start-up frame guard time
 *
 * @param      instance  IsoDep instance
 * @param      poller    activated ISO 14443-3A poller
 */
IsoDepError iso_dep_activate(IsoDep* instance, Iso14443_3aPoller* poller);

/** Exchange one APDU, chaining in both directions as needed */
IsoDepError iso_dep_transceive(
    IsoDep* instance,
    const uint8_t* command,
    size_t command_size,
    uint8_t* response,
    size_t response_max,
    size_t* response_size);

/** Check that the card is still there (R(NAK) presence check) */
bool iso_dep_check_presence(IsoDep* instance);

/** Send S(DESELECT), errors are ignored */
void iso_dep_deselect(IsoDep* instance);

#ifdef __cplusplus
}
#endif
