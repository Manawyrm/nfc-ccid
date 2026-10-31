#include "nfc_ccid_card_i.h"

#include <nfc/protocols/iso14443_3a/iso14443_3a.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a_poller.h>

#define TAG "NfcCcidIso14443"

/* Pause before detecting again when the card cannot be used, saves field cycling */
#define NFC_CCID_ISO14443_UNSUPPORTED_PAUSE_MS (200)

static void nfc_ccid_iso14443_update_info(NfcCcidCard* instance, NfcCcidCardType type) {
    const Iso14443_3aData* data = (const Iso14443_3aData*)nfc_poller_get_data(instance->poller);
    size_t uid_size = 0;
    const uint8_t* uid = iso14443_3a_get_uid(data, &uid_size);

    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    instance->info.type = type;
    instance->info.uid_size = MIN(uid_size, (size_t)NFC_CCID_CARD_UID_MAX);
    memcpy(instance->info.uid, uid, instance->info.uid_size);
    instance->info.sak = iso14443_3a_get_sak(data);
    instance->info.fsc = instance->iso_dep->fsc;
    furi_mutex_release(instance->mutex);
}

/* The card left the field or stopped answering */
static NfcCommand nfc_ccid_iso14443_lost(NfcCcidCard* instance, Iso14443_3aPoller* poller) {
    instance->active = false;
    nfc_ccid_card_set_present(instance, false);
    nfc_ccid_card_fail_requests(instance);

    iso14443_3a_poller_halt(poller);
    return nfc_ccid_card_end_session(instance);
}

static NfcCommand nfc_ccid_iso14443_activate(NfcCcidCard* instance, Iso14443_3aPoller* poller) {
    // Anything still queued belongs to a previous activation
    nfc_ccid_card_fail_requests(instance);

    const Iso14443_3aData* data = (const Iso14443_3aData*)nfc_poller_get_data(instance->poller);
    if(!iso14443_3a_supports_iso14443_4(data)) {
        // MIFARE Classic, Ultralight and friends are not supported yet
        nfc_ccid_iso14443_update_info(instance, NfcCcidCardTypeIso14443_3a);
        nfc_ccid_card_set_present(instance, false);
        iso14443_3a_poller_halt(poller);
        furi_delay_ms(NFC_CCID_ISO14443_UNSUPPORTED_PAUSE_MS);
        return nfc_ccid_card_end_session(instance);
    }

    if(iso_dep_activate(instance->iso_dep, poller) != IsoDepErrorNone) {
        FURI_LOG_W(TAG, "Activation failed");
        return nfc_ccid_iso14443_lost(instance, poller);
    }

    instance->active = true;
    instance->last_activity = furi_get_tick();

    nfc_ccid_iso14443_update_info(instance, NfcCcidCardTypeIso14443_4a);
    nfc_ccid_card_publish_atr(
        instance, instance->iso_dep->historical, instance->iso_dep->historical_size);
    nfc_ccid_card_set_present(instance, true);

    return NfcCommandContinue;
}

/* Pseudo-APDUs for the reader itself */
static size_t nfc_ccid_iso14443_reader_command(
    NfcCcidCard* instance,
    const uint8_t* command,
    size_t command_size) {
    if(command[1] == 0xCA) {
        const Iso14443_3aData* data =
            (const Iso14443_3aData*)nfc_poller_get_data(instance->poller);
        size_t uid_size = 0;
        const uint8_t* uid = iso14443_3a_get_uid(data, &uid_size);

        return nfc_ccid_card_get_data(
            instance,
            command,
            command_size,
            uid,
            uid_size,
            instance->iso_dep->historical,
            instance->iso_dep->historical_size);
    }

    return nfc_ccid_card_status(instance, 0, NFC_CCID_SW_INS_NOT_SUPPORTED);
}

static NfcCommand nfc_ccid_iso14443_serve(NfcCcidCard* instance, Iso14443_3aPoller* poller) {
    NfcCcidCardRequest request;

    if(nfc_ccid_card_get_request(instance, &request)) {
        instance->last_activity = furi_get_tick();

        if(request.command_size >= 4 && request.command[0] == NFC_CCID_CLA_READER) {
            const size_t response_size =
                nfc_ccid_iso14443_reader_command(instance, request.command, request.command_size);
            nfc_ccid_card_complete(instance, response_size, false);
            return NfcCommandContinue;
        }

        size_t response_size = 0;
        const IsoDepError error = iso_dep_transceive(
            instance->iso_dep,
            request.command,
            request.command_size,
            instance->response,
            sizeof(instance->response),
            &response_size);

        if(error != IsoDepErrorNone) {
            // The protocol state is unknown now, report the card as gone and start over
            FURI_LOG_W(TAG, "APDU failed: %d", error);
            nfc_ccid_card_complete(instance, 0, true);
            return nfc_ccid_iso14443_lost(instance, poller);
        }

        nfc_ccid_card_complete(instance, response_size, false);
        return NfcCommandContinue;
    }

    if(furi_get_tick() - instance->last_activity >=
       furi_ms_to_ticks(NFC_CCID_CARD_PRESENCE_CHECK_MS)) {
        instance->last_activity = furi_get_tick();
        if(!iso_dep_check_presence(instance->iso_dep)) {
            return nfc_ccid_iso14443_lost(instance, poller);
        }
    }

    return NfcCommandContinue;
}

NfcCommand nfc_ccid_card_iso14443_callback(NfcGenericEvent event, void* context) {
    NfcCcidCard* instance = context;
    const Iso14443_3aPollerEvent* poller_event = event.event_data;
    Iso14443_3aPoller* poller = event.instance;

    if(instance->session_ended) {
        return NfcCommandStop;
    }

    if(poller_event->type != Iso14443_3aPollerEventTypeReady) {
        instance->active = false;
        nfc_ccid_card_set_present(instance, false);
        return nfc_ccid_card_end_session(instance);
    }

    if(nfc_ccid_card_take_deactivate(instance)) {
        // PowerOff: deselect and cycle the field, the card comes back with a fresh state
        if(instance->active) {
            iso_dep_deselect(instance->iso_dep);
        }
        instance->active = false;
        iso14443_3a_poller_halt(poller);
        return NfcCommandReset;
    }

    if(!instance->active) {
        return nfc_ccid_iso14443_activate(instance, poller);
    }

    return nfc_ccid_iso14443_serve(instance, poller);
}
