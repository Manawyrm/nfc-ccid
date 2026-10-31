#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ccid_usb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NFC_CCID_CARD_UID_MAX (10)

typedef struct NfcCcidCard NfcCcidCard;

typedef enum {
    NfcCcidCardTypeNone,
    NfcCcidCardTypeIso14443_4a, /**< Smart card, APDUs go to the card */
    NfcCcidCardTypeIso14443_3a, /**< ISO 14443-3A without ISO 14443-4, not supported */
    NfcCcidCardTypeIso15693, /**< Storage card, accessed through PC/SC pseudo-APDUs */
} NfcCcidCardType;

/** Card state for display */
typedef struct {
    bool present; /**< Card activated and reported to the host */
    NfcCcidCardType type;
    uint8_t uid[NFC_CCID_CARD_UID_MAX]; /**< UID, most significant byte first */
    size_t uid_size;
    uint8_t sak; /**< ISO 14443-3A */
    uint16_t fsc; /**< ISO 14443-4 */
    uint16_t block_count; /**< ISO 15693 */
    uint8_t block_size; /**< ISO 15693 */
    uint32_t apdu_count;
    uint32_t error_count;
    uint32_t retry_count; /**< ISO-DEP frames repeated after transmission errors */
} NfcCcidCardInfo;

/** CCID card backend callbacks, context: NfcCcidCard* */
extern const CcidUsbCallbacks nfc_ccid_card_ccid_callbacks;

NfcCcidCard* nfc_ccid_card_alloc(void);

/** Free the instance, nfc_ccid_card_stop() and ccid_usb_free() must have been called */
void nfc_ccid_card_free(NfcCcidCard* instance);

/** Start polling for ISO 14443-A and ISO 15693 cards and report them to the CCID layer */
void nfc_ccid_card_start(NfcCcidCard* instance, CcidUsb* ccid);

/** Stop polling, the NFC field is turned off */
void nfc_ccid_card_stop(NfcCcidCard* instance);

void nfc_ccid_card_get_info(NfcCcidCard* instance, NfcCcidCardInfo* info);

#ifdef __cplusplus
}
#endif
