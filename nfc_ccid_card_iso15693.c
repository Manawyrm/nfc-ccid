#include "nfc_ccid_card_i.h"

#include <nfc/protocols/iso15693_3/iso15693_3.h>
#include <nfc/protocols/iso15693_3/iso15693_3_poller.h>

#define TAG "NfcCcidIso15693"

/* Request flags: high data rate, addressed */
#define ISO15693_FLAGS_ADDRESSED (0x22)
#define ISO15693_FLAG_ERROR      (0x01)

#define ISO15693_CMD_READ_SINGLE_BLOCK  (0x20)
#define ISO15693_CMD_WRITE_SINGLE_BLOCK (0x21)

/* Writes and custom commands may answer late, allow up to 20 ms */
#define ISO15693_FWT_FC      (ISO15693_3_FDT_POLL_FC)
#define ISO15693_FWT_LONG_FC (271200U)

/* PC/SC part 3 storage card ATR historical bytes: RID A0 00 00 03 06, standard 0B
 * (ISO 15693 part 3), card name 00 00 (not specified) */
static const uint8_t iso15693_atr_historical[] = {
    0x80,
    0x4F,
    0x0C,
    0xA0,
    0x00,
    0x00,
    0x03,
    0x06,
    0x0B,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
};

static uint16_t nfc_ccid_iso15693_error_sw(uint8_t error) {
    switch(error) {
    case 0x01: // command not supported
    case 0x02: // command not recognized
    case 0x03: // option not supported
        return NFC_CCID_SW_NOT_SUPPORTED;
    case 0x10: // block not available
        return NFC_CCID_SW_NOT_FOUND;
    case 0x11: // block already locked
    case 0x12: // block locked, content cannot change
        return NFC_CCID_SW_SECURITY;
    case 0x13: // programming failed
    case 0x14: // locking failed
        return NFC_CCID_SW_MEMORY_FAILURE;
    default:
        return NFC_CCID_SW_NO_DIAGNOSIS;
    }
}

/* Send instance->tx, returns false if the card did not answer */
static bool
    nfc_ccid_iso15693_exchange(NfcCcidCard* instance, Iso15693_3Poller* poller, uint32_t fwt) {
    const Iso15693_3Error error =
        iso15693_3_poller_send_frame(poller, instance->tx, instance->rx, fwt);
    if(error != Iso15693_3ErrorNone) {
        FURI_LOG_D(TAG, "Exchange failed: %d", error);
        return false;
    }

    return bit_buffer_get_size_bytes(instance->rx) > 0;
}

static void nfc_ccid_iso15693_begin_addressed(NfcCcidCard* instance, uint8_t command) {
    bit_buffer_reset(instance->tx);
    bit_buffer_append_byte(instance->tx, ISO15693_FLAGS_ADDRESSED);
    bit_buffer_append_byte(instance->tx, command);
    bit_buffer_append_bytes(instance->tx, instance->iso15693_uid, sizeof(instance->iso15693_uid));
}

/* FF B0 00 <block> [Le]: READ SINGLE BLOCK */
static size_t nfc_ccid_iso15693_read_binary(
    NfcCcidCard* instance,
    Iso15693_3Poller* poller,
    const uint8_t* command,
    size_t command_size) {
    if(command_size != 4 && command_size != 5) {
        return nfc_ccid_card_status(instance, 0, NFC_CCID_SW_WRONG_LENGTH);
    }
    if(command[2] != 0x00) {
        return nfc_ccid_card_status(instance, 0, NFC_CCID_SW_WRONG_P1P2);
    }

    nfc_ccid_iso15693_begin_addressed(instance, ISO15693_CMD_READ_SINGLE_BLOCK);
    bit_buffer_append_byte(instance->tx, command[3]);

    if(!nfc_ccid_iso15693_exchange(instance, poller, ISO15693_FWT_FC)) {
        return nfc_ccid_card_status(instance, 0, NFC_CCID_SW_NO_DIAGNOSIS);
    }

    const uint8_t flags = bit_buffer_get_byte(instance->rx, 0);
    if(flags & ISO15693_FLAG_ERROR) {
        const uint8_t error = (bit_buffer_get_size_bytes(instance->rx) > 1) ?
                                  bit_buffer_get_byte(instance->rx, 1) :
                                  0;
        return nfc_ccid_card_status(instance, 0, nfc_ccid_iso15693_error_sw(error));
    }

    const size_t data_size = bit_buffer_get_size_bytes(instance->rx) - 1;
    const size_t le = (command_size == 5) ? command[4] : 0;
    if(le != 0 && le < data_size) {
        return nfc_ccid_card_status(instance, 0, NFC_CCID_SW_WRONG_LE | data_size);
    }

    memcpy(instance->response, bit_buffer_get_data(instance->rx) + 1, data_size);
    return nfc_ccid_card_status(instance, data_size, NFC_CCID_SW_OK);
}

/* FF D6 00 <block> Lc <data>: WRITE SINGLE BLOCK */
static size_t nfc_ccid_iso15693_update_binary(
    NfcCcidCard* instance,
    Iso15693_3Poller* poller,
    const uint8_t* command,
    size_t command_size) {
    if(command_size < 6 || command_size != 5U + command[4]) {
        return nfc_ccid_card_status(instance, 0, NFC_CCID_SW_WRONG_LENGTH);
    }
    if(command[2] != 0x00) {
        return nfc_ccid_card_status(instance, 0, NFC_CCID_SW_WRONG_P1P2);
    }

    nfc_ccid_iso15693_begin_addressed(instance, ISO15693_CMD_WRITE_SINGLE_BLOCK);
    bit_buffer_append_byte(instance->tx, command[3]);
    bit_buffer_append_bytes(instance->tx, &command[5], command[4]);

    if(!nfc_ccid_iso15693_exchange(instance, poller, ISO15693_FWT_LONG_FC)) {
        return nfc_ccid_card_status(instance, 0, NFC_CCID_SW_NO_DIAGNOSIS);
    }

    const uint8_t flags = bit_buffer_get_byte(instance->rx, 0);
    if(flags & ISO15693_FLAG_ERROR) {
        const uint8_t error = (bit_buffer_get_size_bytes(instance->rx) > 1) ?
                                  bit_buffer_get_byte(instance->rx, 1) :
                                  0;
        return nfc_ccid_card_status(instance, 0, nfc_ccid_iso15693_error_sw(error));
    }

    return nfc_ccid_card_status(instance, 0, NFC_CCID_SW_OK);
}

/* FF 00 00 00 Lc <frame> [Le]: send a raw ISO 15693 request (flags, command, parameters,
 * without CRC) and return the raw response (flags, data) */
static size_t nfc_ccid_iso15693_transparent(
    NfcCcidCard* instance,
    Iso15693_3Poller* poller,
    const uint8_t* command,
    size_t command_size) {
    if(command_size < 6) {
        return nfc_ccid_card_status(instance, 0, NFC_CCID_SW_WRONG_LENGTH);
    }

    const size_t frame_size = command[4];
    if(command_size != 5 + frame_size && command_size != 6 + frame_size) {
        return nfc_ccid_card_status(instance, 0, NFC_CCID_SW_WRONG_LENGTH);
    }
    if(command[2] != 0x00 || command[3] != 0x00) {
        return nfc_ccid_card_status(instance, 0, NFC_CCID_SW_WRONG_P1P2);
    }

    bit_buffer_reset(instance->tx);
    bit_buffer_append_bytes(instance->tx, &command[5], frame_size);

    if(!nfc_ccid_iso15693_exchange(instance, poller, ISO15693_FWT_LONG_FC)) {
        return nfc_ccid_card_status(instance, 0, NFC_CCID_SW_NO_DIAGNOSIS);
    }

    const size_t response_size = bit_buffer_get_size_bytes(instance->rx);
    memcpy(instance->response, bit_buffer_get_data(instance->rx), response_size);
    return nfc_ccid_card_status(instance, response_size, NFC_CCID_SW_OK);
}

static size_t nfc_ccid_iso15693_command(
    NfcCcidCard* instance,
    Iso15693_3Poller* poller,
    const uint8_t* command,
    size_t command_size) {
    if(command_size < 4) {
        return nfc_ccid_card_status(instance, 0, NFC_CCID_SW_WRONG_LENGTH);
    }
    if(command[0] != NFC_CCID_CLA_READER) {
        // A storage card does not understand APDUs
        return nfc_ccid_card_status(instance, 0, NFC_CCID_SW_CLA_NOT_SUPPORTED);
    }

    switch(command[1]) {
    case 0xCA:
        return nfc_ccid_card_get_data(
            instance,
            command,
            command_size,
            instance->iso15693_uid,
            sizeof(instance->iso15693_uid),
            NULL,
            0);
    case 0xB0:
        return nfc_ccid_iso15693_read_binary(instance, poller, command, command_size);
    case 0xD6:
        return nfc_ccid_iso15693_update_binary(instance, poller, command, command_size);
    case 0x00:
        return nfc_ccid_iso15693_transparent(instance, poller, command, command_size);
    default:
        return nfc_ccid_card_status(instance, 0, NFC_CCID_SW_INS_NOT_SUPPORTED);
    }
}

static NfcCommand nfc_ccid_iso15693_activate(NfcCcidCard* instance) {
    nfc_ccid_card_fail_requests(instance);

    const Iso15693_3Data* data = (const Iso15693_3Data*)nfc_poller_get_data(instance->poller);
    size_t uid_size = 0;
    const uint8_t* uid = iso15693_3_get_uid(data, &uid_size);

    // Flipper keeps the UID most significant byte first, the air interface sends it reversed
    for(size_t i = 0; i < NFC_CCID_CARD_ISO15693_UID_SIZE; i++) {
        instance->iso15693_uid[i] = uid[NFC_CCID_CARD_ISO15693_UID_SIZE - 1 - i];
    }

    furi_mutex_acquire(instance->mutex, FuriWaitForever);
    instance->info.type = NfcCcidCardTypeIso15693;
    instance->info.uid_size = NFC_CCID_CARD_ISO15693_UID_SIZE;
    memcpy(instance->info.uid, uid, NFC_CCID_CARD_ISO15693_UID_SIZE);
    instance->info.block_count = iso15693_3_get_block_count(data);
    instance->info.block_size = iso15693_3_get_block_size(data);
    furi_mutex_release(instance->mutex);

    instance->active = true;
    instance->last_activity = furi_get_tick();

    nfc_ccid_card_publish_atr(instance, iso15693_atr_historical, sizeof(iso15693_atr_historical));
    nfc_ccid_card_set_present(instance, true);

    return NfcCommandContinue;
}

static bool nfc_ccid_iso15693_check_presence(NfcCcidCard* instance, Iso15693_3Poller* poller) {
    const Iso15693_3Data* data = (const Iso15693_3Data*)nfc_poller_get_data(instance->poller);
    uint8_t uid[ISO15693_3_UID_SIZE];

    if(iso15693_3_poller_inventory(poller, uid) != Iso15693_3ErrorNone) {
        return false;
    }

    return memcmp(uid, iso15693_3_get_uid(data, NULL), sizeof(uid)) == 0;
}

NfcCommand nfc_ccid_card_iso15693_callback(NfcGenericEvent event, void* context) {
    NfcCcidCard* instance = context;
    const Iso15693_3PollerEvent* poller_event = event.event_data;
    Iso15693_3Poller* poller = event.instance;

    if(instance->session_ended) {
        return NfcCommandStop;
    }

    if(poller_event->type != Iso15693_3PollerEventTypeReady) {
        instance->active = false;
        nfc_ccid_card_set_present(instance, false);
        return nfc_ccid_card_end_session(instance);
    }

    if(!instance->active) {
        return nfc_ccid_iso15693_activate(instance);
    }

    if(nfc_ccid_card_take_deactivate(instance)) {
        // A storage card keeps no session state, it is ready again right away
        nfc_ccid_card_publish_atr(
            instance, iso15693_atr_historical, sizeof(iso15693_atr_historical));
    }

    NfcCcidCardRequest request;
    if(nfc_ccid_card_get_request(instance, &request)) {
        instance->last_activity = furi_get_tick();
        const size_t response_size =
            nfc_ccid_iso15693_command(instance, poller, request.command, request.command_size);
        nfc_ccid_card_complete(instance, response_size, false);
        return NfcCommandContinue;
    }

    if(furi_get_tick() - instance->last_activity >=
       furi_ms_to_ticks(NFC_CCID_CARD_PRESENCE_CHECK_MS)) {
        instance->last_activity = furi_get_tick();
        if(!nfc_ccid_iso15693_check_presence(instance, poller)) {
            instance->active = false;
            nfc_ccid_card_set_present(instance, false);
            nfc_ccid_card_fail_requests(instance);
            return nfc_ccid_card_end_session(instance);
        }
    }

    return NfcCommandContinue;
}
