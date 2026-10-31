#pragma once

#include "nfc_ccid_card.h"
#include "iso_dep.h"

#include <furi.h>
#include <nfc/nfc.h>
#include <nfc/nfc_poller.h>

#define NFC_CCID_CARD_REQUEST_WAIT_MS   (50)
#define NFC_CCID_CARD_PRESENCE_CHECK_MS (250)

#define NFC_CCID_CARD_ISO15693_UID_SIZE (8)

/* Status words */
#define NFC_CCID_SW_OK                (0x9000)
#define NFC_CCID_SW_WRONG_LENGTH      (0x6700)
#define NFC_CCID_SW_SECURITY          (0x6982)
#define NFC_CCID_SW_MEMORY_FAILURE    (0x6581)
#define NFC_CCID_SW_NOT_SUPPORTED     (0x6A81)
#define NFC_CCID_SW_NOT_FOUND         (0x6A82)
#define NFC_CCID_SW_WRONG_P1P2        (0x6B00)
#define NFC_CCID_SW_WRONG_LE          (0x6C00)
#define NFC_CCID_SW_INS_NOT_SUPPORTED (0x6D00)
#define NFC_CCID_SW_CLA_NOT_SUPPORTED (0x6E00)
#define NFC_CCID_SW_NO_DIAGNOSIS      (0x6F00)

/* CLA of the PC/SC part 3 pseudo-APDUs, handled by the reader itself */
#define NFC_CCID_CLA_READER (0xFF)

/* A request from the CCID worker, a NULL command only wakes the NFC thread up */
typedef struct {
    const uint8_t* command;
    size_t command_size;
} NfcCcidCardRequest;

struct NfcCcidCard {
    Nfc* nfc;
    NfcPoller* poller;
    CcidUsb* ccid;
    IsoDep* iso_dep;
    BitBuffer* tx;
    BitBuffer* rx;
    FuriMutex* mutex;
    FuriMessageQueue* requests;
    FuriThread* thread;
    volatile bool stop;

    /* Session state, owned by the NFC worker thread */
    volatile bool session_ended;
    bool active;
    uint32_t last_activity;
    uint8_t iso15693_uid[NFC_CCID_CARD_ISO15693_UID_SIZE]; /**< Transmission order */

    /* Shared with the CCID worker and the GUI, protected by mutex */
    bool atr_valid;
    bool deactivate_requested;
    uint8_t atr[CCID_USB_ATR_MAX];
    size_t atr_size;
    NfcCcidCardInfo info;

    uint8_t response[CCID_USB_RESPONSE_MAX];
};

/* Session callbacks, run in the NFC worker thread */
NfcCommand nfc_ccid_card_iso14443_callback(NfcGenericEvent event, void* context);
NfcCommand nfc_ccid_card_iso15693_callback(NfcGenericEvent event, void* context);

/* Helpers shared by the sessions */
void nfc_ccid_card_set_present(NfcCcidCard* instance, bool present);
void nfc_ccid_card_fail_requests(NfcCcidCard* instance);

/** Mark the session as finished, the manager thread starts detecting again */
NfcCommand nfc_ccid_card_end_session(NfcCcidCard* instance);

/** Build the ATR "3B 8n 80 01 <historical bytes> TCK" and make it available to PowerOn */
void nfc_ccid_card_publish_atr(NfcCcidCard* instance, const uint8_t* historical, size_t size);

/** Fetch and clear a PowerOff request */
bool nfc_ccid_card_take_deactivate(NfcCcidCard* instance);

/** Wait shortly for an APDU, returns false on timeout or wake-up */
bool nfc_ccid_card_get_request(NfcCcidCard* instance, NfcCcidCardRequest* request);

/** Count the APDU and hand the response in instance->response to the CCID layer */
void nfc_ccid_card_complete(NfcCcidCard* instance, size_t response_size, bool failed);

/** Write a status word to instance->response at offset, returns the response size */
size_t nfc_ccid_card_status(NfcCcidCard* instance, size_t offset, uint16_t sw);

/** PC/SC GET DATA (FF CA): P1 00 returns the UID, P1 01 the historical bytes if given */
size_t nfc_ccid_card_get_data(
    NfcCcidCard* instance,
    const uint8_t* command,
    size_t command_size,
    const uint8_t* uid,
    size_t uid_size,
    const uint8_t* historical,
    size_t historical_size);
