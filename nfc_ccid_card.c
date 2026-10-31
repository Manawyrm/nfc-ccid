#include "nfc_ccid_card_i.h"

#define TAG "NfcCcidCard"

#define NFC_CCID_CARD_ACTIVATE_WAIT_MS (2000)
#define NFC_CCID_CARD_DETECT_PAUSE_MS  (100)
#define NFC_CCID_CARD_REQUEST_QUEUE    (4)
#define NFC_CCID_CARD_FRAME_BUFFER     (512)

typedef enum {
    NfcCcidCardFlagStop = (1 << 0),
    NfcCcidCardFlagSessionEnd = (1 << 1),
} NfcCcidCardFlag;

typedef struct {
    NfcProtocol protocol;
    NfcGenericCallback callback;
} NfcCcidCardProtocol;

/* Detection order */
static const NfcCcidCardProtocol nfc_ccid_card_protocols[] = {
    {NfcProtocolIso14443_3a, nfc_ccid_card_iso14443_callback},
    {NfcProtocolIso15693_3, nfc_ccid_card_iso15693_callback},
};

void nfc_ccid_card_set_present(NfcCcidCard* instance, bool present) {
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    const bool changed = (instance->info.present != present);
    instance->info.present = present;
    if(!present) {
        instance->atr_valid = false;
    }
    furi_mutex_release(instance->mutex);

    if(changed) {
        FURI_LOG_I(TAG, "Card %s", present ? "present" : "gone");
        ccid_usb_set_card_present(instance->ccid, present);
    }
}

void nfc_ccid_card_fail_requests(NfcCcidCard* instance) {
    NfcCcidCardRequest request;

    while(furi_message_queue_get(instance->requests, &request, 0) == FuriStatusOk) {
        if(request.command != NULL) {
            ccid_usb_xfr_complete(instance->ccid, NULL, 0);
        }
    }
}

NfcCommand nfc_ccid_card_end_session(NfcCcidCard* instance) {
    instance->session_ended = true;
    furi_thread_flags_set(furi_thread_get_id(instance->thread), NfcCcidCardFlagSessionEnd);
    return NfcCommandStop;
}

void nfc_ccid_card_publish_atr(NfcCcidCard* instance, const uint8_t* historical, size_t size) {
    furi_check(size <= ISO_DEP_HISTORICAL_MAX);

    furi_mutex_acquire(instance->mutex, FuriWaitForever);

    uint8_t* atr = instance->atr;
    atr[0] = 0x3B;
    atr[1] = 0x80 | size;
    atr[2] = 0x80;
    atr[3] = 0x01;
    memcpy(&atr[4], historical, size);

    uint8_t tck = 0;
    for(size_t i = 1; i < 4 + size; i++) {
        tck ^= atr[i];
    }
    atr[4 + size] = tck;

    instance->atr_size = 5 + size;
    instance->atr_valid = true;

    furi_mutex_release(instance->mutex);
}

bool nfc_ccid_card_take_deactivate(NfcCcidCard* instance) {
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    const bool deactivate = instance->deactivate_requested;
    instance->deactivate_requested = false;
    furi_mutex_release(instance->mutex);

    return deactivate;
}

bool nfc_ccid_card_get_request(NfcCcidCard* instance, NfcCcidCardRequest* request) {
    const FuriStatus status =
        furi_message_queue_get(instance->requests, request, NFC_CCID_CARD_REQUEST_WAIT_MS);

    return (status == FuriStatusOk) && (request->command != NULL);
}

void nfc_ccid_card_complete(NfcCcidCard* instance, size_t response_size, bool failed) {
    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    instance->info.apdu_count++;
    if(failed) {
        instance->info.error_count++;
    }
    instance->info.retry_count = instance->iso_dep->retry_count;
    furi_mutex_release(instance->mutex);

    if(failed) {
        ccid_usb_xfr_complete(instance->ccid, NULL, 0);
    } else {
        ccid_usb_xfr_complete(instance->ccid, instance->response, response_size);
    }
}

size_t nfc_ccid_card_status(NfcCcidCard* instance, size_t offset, uint16_t sw) {
    instance->response[offset] = sw >> 8;
    instance->response[offset + 1] = sw & 0xFF;
    return offset + 2;
}

size_t nfc_ccid_card_get_data(
    NfcCcidCard* instance,
    const uint8_t* command,
    size_t command_size,
    const uint8_t* uid,
    size_t uid_size,
    const uint8_t* historical,
    size_t historical_size) {
    // FF CA P1 P2 [Le]
    if(command_size != 4 && command_size != 5) {
        return nfc_ccid_card_status(instance, 0, NFC_CCID_SW_WRONG_LENGTH);
    }
    if(command[3] != 0x00) {
        return nfc_ccid_card_status(instance, 0, NFC_CCID_SW_WRONG_P1P2);
    }

    const uint8_t* data = NULL;
    size_t data_size = 0;

    if(command[2] == 0x00) {
        data = uid;
        data_size = uid_size;
    } else if(command[2] == 0x01 && historical != NULL) {
        data = historical;
        data_size = historical_size;
    } else {
        return nfc_ccid_card_status(instance, 0, NFC_CCID_SW_NOT_SUPPORTED);
    }

    // Le 00 means "everything", a smaller Le gets the correct length back
    const size_t le = (command_size == 5) ? command[4] : 0;
    if(le != 0 && le < data_size) {
        return nfc_ccid_card_status(instance, 0, NFC_CCID_SW_WRONG_LE | data_size);
    }

    memcpy(instance->response, data, data_size);
    return nfc_ccid_card_status(instance, data_size, NFC_CCID_SW_OK);
}

static void nfc_ccid_card_gone(NfcCcidCard* instance) {
    nfc_ccid_card_set_present(instance, false);
    nfc_ccid_card_fail_requests(instance);

    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    instance->info.type = NfcCcidCardTypeNone;
    furi_mutex_release(instance->mutex);
}

/* Look for a card with one of the supported protocols, returns its index or COUNT_OF on none */
static size_t nfc_ccid_card_detect(NfcCcidCard* instance) {
    for(size_t i = 0; i < COUNT_OF(nfc_ccid_card_protocols); i++) {
        NfcPoller* poller = nfc_poller_alloc(instance->nfc, nfc_ccid_card_protocols[i].protocol);
        const bool detected = nfc_poller_detect(poller);

        // A poller cannot be started after nfc_poller_detect(), its session state stays active
        nfc_poller_free(poller);

        if(detected) {
            return i;
        }
    }

    return COUNT_OF(nfc_ccid_card_protocols);
}

static int32_t nfc_ccid_card_thread(void* context) {
    NfcCcidCard* instance = context;

    while(!instance->stop) {
        const size_t index = nfc_ccid_card_detect(instance);

        if(index == COUNT_OF(nfc_ccid_card_protocols)) {
            nfc_ccid_card_gone(instance);
            furi_thread_flags_wait(
                NfcCcidCardFlagStop, FuriFlagWaitAny, NFC_CCID_CARD_DETECT_PAUSE_MS);
            continue;
        }

        // Session: the protocol callback serves the card until it is gone
        instance->session_ended = false;
        instance->active = false;
        instance->poller =
            nfc_poller_alloc(instance->nfc, nfc_ccid_card_protocols[index].protocol);
        nfc_poller_start(instance->poller, nfc_ccid_card_protocols[index].callback, instance);

        while(!instance->session_ended && !instance->stop) {
            furi_thread_flags_wait(
                NfcCcidCardFlagStop | NfcCcidCardFlagSessionEnd,
                FuriFlagWaitAny,
                NFC_CCID_CARD_DETECT_PAUSE_MS);
        }

        nfc_poller_stop(instance->poller);
        nfc_poller_free(instance->poller);
        instance->poller = NULL;
    }

    nfc_ccid_card_gone(instance);

    return 0;
}

/* CCID worker thread */
static bool nfc_ccid_card_power_on(void* context, uint8_t* atr, size_t* atr_size) {
    NfcCcidCard* instance = context;
    const uint32_t start = furi_get_tick();

    // After a PowerOff the card is being reactivated, wait for it
    while(true) {
        furi_mutex_acquire(instance->mutex, FuriWaitForever);
        const bool present = instance->info.present;
        const bool atr_valid = instance->atr_valid;
        if(atr_valid) {
            memcpy(atr, instance->atr, instance->atr_size);
            *atr_size = instance->atr_size;
        }
        furi_mutex_release(instance->mutex);

        if(atr_valid) {
            return true;
        }
        if(!present) {
            return false;
        }
        if(furi_get_tick() - start >= furi_ms_to_ticks(NFC_CCID_CARD_ACTIVATE_WAIT_MS)) {
            return false;
        }

        furi_delay_ms(10);
    }
}

/* CCID worker thread */
static void nfc_ccid_card_power_off(void* context) {
    NfcCcidCard* instance = context;

    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    instance->atr_valid = false;
    instance->deactivate_requested = true;
    furi_mutex_release(instance->mutex);

    const NfcCcidCardRequest wake_up = {.command = NULL, .command_size = 0};
    furi_message_queue_put(instance->requests, &wake_up, 0);
}

/* CCID worker thread */
static void nfc_ccid_card_xfr_start(void* context, const uint8_t* command, size_t command_size) {
    NfcCcidCard* instance = context;

    const NfcCcidCardRequest request = {.command = command, .command_size = command_size};
    if(furi_message_queue_put(instance->requests, &request, 0) != FuriStatusOk) {
        ccid_usb_xfr_complete(instance->ccid, NULL, 0);
    }
}

const CcidUsbCallbacks nfc_ccid_card_ccid_callbacks = {
    .power_on = nfc_ccid_card_power_on,
    .power_off = nfc_ccid_card_power_off,
    .xfr_start = nfc_ccid_card_xfr_start,
};

NfcCcidCard* nfc_ccid_card_alloc(void) {
    NfcCcidCard* instance = malloc(sizeof(NfcCcidCard));

    instance->nfc = nfc_alloc();
    instance->iso_dep = iso_dep_alloc();
    instance->tx = bit_buffer_alloc(NFC_CCID_CARD_FRAME_BUFFER);
    instance->rx = bit_buffer_alloc(NFC_CCID_CARD_FRAME_BUFFER);
    instance->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    instance->requests =
        furi_message_queue_alloc(NFC_CCID_CARD_REQUEST_QUEUE, sizeof(NfcCcidCardRequest));
    instance->thread = furi_thread_alloc_ex("NfcCcidCard", 2048, nfc_ccid_card_thread, instance);

    return instance;
}

void nfc_ccid_card_free(NfcCcidCard* instance) {
    furi_check(instance);
    furi_check(instance->poller == NULL);

    furi_thread_free(instance->thread);
    furi_message_queue_free(instance->requests);
    furi_mutex_free(instance->mutex);
    bit_buffer_free(instance->tx);
    bit_buffer_free(instance->rx);
    iso_dep_free(instance->iso_dep);
    nfc_free(instance->nfc);

    free(instance);
}

void nfc_ccid_card_start(NfcCcidCard* instance, CcidUsb* ccid) {
    furi_check(instance);
    furi_check(ccid);

    instance->ccid = ccid;
    instance->stop = false;
    furi_thread_start(instance->thread);
}

void nfc_ccid_card_stop(NfcCcidCard* instance) {
    furi_check(instance);

    instance->stop = true;
    furi_thread_flags_set(furi_thread_get_id(instance->thread), NfcCcidCardFlagStop);
    furi_thread_join(instance->thread);
}

void nfc_ccid_card_get_info(NfcCcidCard* instance, NfcCcidCardInfo* info) {
    furi_check(instance);
    furi_check(info);

    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    *info = instance->info;
    furi_mutex_release(instance->mutex);
}
