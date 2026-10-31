#include "iso_dep.h"

#include <furi.h>

#define TAG "IsoDep"

#define ISO_DEP_BUFFER_SIZE (512)

/* RATS: FSDI 8 (256 bytes), CID 0 */
#define ISO_DEP_RATS         (0xE0)
#define ISO_DEP_RATS_PARAM   (0x80)
#define ISO_DEP_ATS_FWT_FC   (71680)
#define ISO_DEP_FWT_DELTA_FC (49152)
#define ISO_DEP_FWI_DEFAULT  (4)
#define ISO_DEP_FWI_MAX      (14)
#define ISO_DEP_FSCI_DEFAULT (2)
#define ISO_DEP_WTXM_MAX     (59)
#define ISO_DEP_RETRIES      (2)

#define ISO_DEP_PCB_I          (0x02)
#define ISO_DEP_PCB_I_CHAIN    (0x10)
#define ISO_DEP_PCB_R_ACK      (0xA2)
#define ISO_DEP_PCB_R_NAK      (0xB2)
#define ISO_DEP_PCB_S_DESELECT (0xC2)
#define ISO_DEP_PCB_S_WTX      (0xF2)
#define ISO_DEP_PCB_BLOCK_NUM  (0x01)
#define ISO_DEP_PCB_NAK        (0x10)

#define ISO_DEP_IS_I_BLOCK(pcb)   (((pcb) & 0xE2) == 0x02)
#define ISO_DEP_IS_R_BLOCK(pcb)   (((pcb) & 0xE6) == 0xA2)
#define ISO_DEP_IS_S_WTX(pcb)     (((pcb) & 0xF7) == ISO_DEP_PCB_S_WTX)
#define ISO_DEP_IS_CHAINING(pcb)  (((pcb) & ISO_DEP_PCB_I_CHAIN) != 0)
#define ISO_DEP_BLOCK_NUMBER(pcb) ((pcb) & ISO_DEP_PCB_BLOCK_NUM)

/* PCB + CRC */
#define ISO_DEP_FRAME_OVERHEAD (3)

/* Carrier cycles per 10 microseconds (13.56 MHz) */
#define ISO_DEP_FC_PER_10US (136)

static const uint16_t iso_dep_fsc_table[] = {16, 24, 32, 40, 48, 64, 96, 128, 256};

IsoDep* iso_dep_alloc(void) {
    IsoDep* instance = malloc(sizeof(IsoDep));
    instance->tx = bit_buffer_alloc(ISO_DEP_BUFFER_SIZE);
    instance->rx = bit_buffer_alloc(ISO_DEP_BUFFER_SIZE);
    return instance;
}

void iso_dep_free(IsoDep* instance) {
    furi_check(instance);

    bit_buffer_free(instance->tx);
    bit_buffer_free(instance->rx);
    free(instance);
}

static uint32_t iso_dep_fwt_from_fwi(uint8_t fwi) {
    return ((256U * 16U) << fwi) + ISO_DEP_FWT_DELTA_FC;
}

/* Send tx, receive rx and answer waiting time extension requests */
static IsoDepError iso_dep_exchange(IsoDep* instance) {
    Iso14443_3aError error = iso14443_3a_poller_send_standard_frame(
        instance->poller, instance->tx, instance->rx, instance->fwt_fc);
    if(error != Iso14443_3aErrorNone) {
        FURI_LOG_D(TAG, "Exchange failed: %d", error);
        return IsoDepErrorTransmission;
    }

    while(bit_buffer_get_size_bytes(instance->rx) >= 2 &&
          ISO_DEP_IS_S_WTX(bit_buffer_get_byte(instance->rx, 0))) {
        const uint8_t wtxm = bit_buffer_get_byte(instance->rx, 1) & 0x3F;
        if(wtxm == 0 || wtxm > ISO_DEP_WTXM_MAX) {
            return IsoDepErrorProtocol;
        }

        bit_buffer_reset(instance->tx);
        bit_buffer_append_byte(instance->tx, ISO_DEP_PCB_S_WTX);
        bit_buffer_append_byte(instance->tx, wtxm);

        const uint32_t fwt = MIN(instance->fwt_fc * wtxm, iso_dep_fwt_from_fwi(ISO_DEP_FWI_MAX));
        error = iso14443_3a_poller_send_standard_frame(
            instance->poller, instance->tx, instance->rx, fwt);
        if(error != Iso14443_3aErrorNone) {
            FURI_LOG_D(TAG, "WTX exchange failed: %d", error);
            return IsoDepErrorTransmission;
        }
    }

    if(bit_buffer_get_size_bytes(instance->rx) == 0) {
        return IsoDepErrorProtocol;
    }

    return IsoDepErrorNone;
}

static void iso_dep_parse_ats(IsoDep* instance) {
    const uint8_t* ats = instance->ats;
    const size_t length = MIN((size_t)ats[0], instance->ats_size);

    uint8_t fsci = ISO_DEP_FSCI_DEFAULT;
    uint8_t fwi = ISO_DEP_FWI_DEFAULT;
    uint8_t sfgi = 0;
    size_t index = 1;

    if(length > 1) {
        const uint8_t t0 = ats[index++];
        fsci = t0 & 0x0F;

        if((t0 & 0x10) && index < length) {
            index++; // TA: bit rates, only 106 kbit/s is used
        }
        if((t0 & 0x20) && index < length) {
            const uint8_t tb = ats[index++];
            fwi = tb >> 4;
            sfgi = tb & 0x0F;
        }
        if((t0 & 0x40) && index < length) {
            index++; // TC: CID and NAD are not used
        }
    }

    if(fsci >= COUNT_OF(iso_dep_fsc_table)) {
        fsci = COUNT_OF(iso_dep_fsc_table) - 1;
    }
    if(fwi > ISO_DEP_FWI_MAX) {
        fwi = ISO_DEP_FWI_DEFAULT;
    }
    if(sfgi > ISO_DEP_FWI_MAX) {
        sfgi = 0;
    }

    instance->fsc = iso_dep_fsc_table[fsci];
    instance->fwt_fc = iso_dep_fwt_from_fwi(fwi);

    instance->historical_size = 0;
    if(index < length) {
        instance->historical_size = MIN(length - index, (size_t)ISO_DEP_HISTORICAL_MAX);
        memcpy(instance->historical, &ats[index], instance->historical_size);
    }

    if(sfgi > 0) {
        // Start-up frame guard time before the first block
        const uint32_t sfgt_fc = iso_dep_fwt_from_fwi(sfgi);
        furi_delay_us(sfgt_fc * 10 / ISO_DEP_FC_PER_10US);
    }

    FURI_LOG_I(
        TAG,
        "ATS: FSC %u, FWI %u, SFGI %u, %u historical bytes",
        instance->fsc,
        fwi,
        sfgi,
        instance->historical_size);
}

IsoDepError iso_dep_activate(IsoDep* instance, Iso14443_3aPoller* poller) {
    furi_check(instance);
    furi_check(poller);

    instance->poller = poller;
    instance->block_number = 0;
    instance->fwt_fc = ISO_DEP_ATS_FWT_FC;

    bit_buffer_reset(instance->tx);
    bit_buffer_append_byte(instance->tx, ISO_DEP_RATS);
    bit_buffer_append_byte(instance->tx, ISO_DEP_RATS_PARAM);

    const Iso14443_3aError error = iso14443_3a_poller_send_standard_frame(
        instance->poller, instance->tx, instance->rx, ISO_DEP_ATS_FWT_FC);
    if(error != Iso14443_3aErrorNone) {
        FURI_LOG_D(TAG, "RATS failed: %d", error);
        return IsoDepErrorTransmission;
    }

    instance->ats_size = bit_buffer_get_size_bytes(instance->rx);
    if(instance->ats_size == 0 || instance->ats_size > sizeof(instance->ats)) {
        return IsoDepErrorProtocol;
    }
    bit_buffer_write_bytes(instance->rx, instance->ats, instance->ats_size);

    iso_dep_parse_ats(instance);

    return IsoDepErrorNone;
}

static void
    iso_dep_build_i_block(IsoDep* instance, const uint8_t* data, size_t size, bool chaining) {
    uint8_t pcb = ISO_DEP_PCB_I | instance->block_number;
    if(chaining) {
        pcb |= ISO_DEP_PCB_I_CHAIN;
    }

    bit_buffer_reset(instance->tx);
    bit_buffer_append_byte(instance->tx, pcb);
    bit_buffer_append_bytes(instance->tx, data, size);
}

/* The answer to our I-block: R(ACK) while chaining, else the card's I-block, both with our number */
static bool iso_dep_is_i_block_answer(IsoDep* instance, bool chaining) {
    const uint8_t pcb = bit_buffer_get_byte(instance->rx, 0);

    if(chaining) {
        return ISO_DEP_IS_R_BLOCK(pcb) && !(pcb & ISO_DEP_PCB_NAK) &&
               ISO_DEP_BLOCK_NUMBER(pcb) == instance->block_number;
    }
    return ISO_DEP_IS_I_BLOCK(pcb) && ISO_DEP_BLOCK_NUMBER(pcb) == instance->block_number;
}

/* Send an I-block and receive its answer, recovering lost or broken frames:
 * R(NAK) makes the card repeat its answer (ISO 14443-4 rule 4), an R(ACK) with the other
 * block number means the card never got our block, so it is sent again (rule 6). */
static IsoDepError
    iso_dep_send_i_block(IsoDep* instance, const uint8_t* data, size_t size, bool chaining) {
    iso_dep_build_i_block(instance, data, size, chaining);
    IsoDepError error = iso_dep_exchange(instance);

    for(uint8_t retry = 0;; retry++) {
        if(error == IsoDepErrorNone) {
            if(iso_dep_is_i_block_answer(instance, chaining)) {
                return IsoDepErrorNone;
            }

            const uint8_t pcb = bit_buffer_get_byte(instance->rx, 0);
            const bool resend = ISO_DEP_IS_R_BLOCK(pcb) && !(pcb & ISO_DEP_PCB_NAK) &&
                                ISO_DEP_BLOCK_NUMBER(pcb) != instance->block_number;

            if(retry >= ISO_DEP_RETRIES) {
                FURI_LOG_D(TAG, "Unexpected block 0x%02X", pcb);
                return IsoDepErrorProtocol;
            }

            instance->retry_count++;
            if(resend) {
                iso_dep_build_i_block(instance, data, size, chaining);
                error = iso_dep_exchange(instance);
                continue;
            }
        } else if(retry >= ISO_DEP_RETRIES) {
            return error;
        } else {
            instance->retry_count++;
        }

        bit_buffer_reset(instance->tx);
        bit_buffer_append_byte(instance->tx, ISO_DEP_PCB_R_NAK | instance->block_number);
        error = iso_dep_exchange(instance);
    }
}

/* Ask for the next block of a chained response, R(ACK) is repeated on errors (rule 5) */
static IsoDepError iso_dep_request_next_block(IsoDep* instance) {
    IsoDepError error = IsoDepErrorProtocol;

    for(uint8_t attempt = 0; attempt <= ISO_DEP_RETRIES; attempt++) {
        if(attempt > 0) {
            instance->retry_count++;
        }

        bit_buffer_reset(instance->tx);
        bit_buffer_append_byte(instance->tx, ISO_DEP_PCB_R_ACK | instance->block_number);
        error = iso_dep_exchange(instance);

        if(error == IsoDepErrorNone) {
            const uint8_t pcb = bit_buffer_get_byte(instance->rx, 0);
            if(ISO_DEP_IS_I_BLOCK(pcb) && ISO_DEP_BLOCK_NUMBER(pcb) == instance->block_number) {
                return IsoDepErrorNone;
            }
            error = IsoDepErrorProtocol;
        }
    }

    return error;
}

IsoDepError iso_dep_transceive(
    IsoDep* instance,
    const uint8_t* command,
    size_t command_size,
    uint8_t* response,
    size_t response_max,
    size_t* response_size) {
    furi_check(instance);
    furi_check(instance->poller);
    furi_check(command);
    furi_check(response);
    furi_check(response_size);

    const size_t chunk_max = instance->fsc - ISO_DEP_FRAME_OVERHEAD;
    size_t offset = 0;
    IsoDepError error = IsoDepErrorNone;

    *response_size = 0;

    // Command, chained when longer than the card's frame size
    while(true) {
        const size_t chunk = MIN(command_size - offset, chunk_max);
        const bool more = (offset + chunk) < command_size;

        error = iso_dep_send_i_block(instance, &command[offset], chunk, more);
        if(error != IsoDepErrorNone) {
            return error;
        }

        if(!more) {
            break;
        }

        instance->block_number ^= 1;
        offset += chunk;
    }

    // Response, possibly chained by the card. rx holds a validated I-block here.
    while(true) {
        const uint8_t pcb = bit_buffer_get_byte(instance->rx, 0);

        instance->block_number ^= 1;

        const size_t inf_size = bit_buffer_get_size_bytes(instance->rx) - 1;
        if(*response_size + inf_size > response_max) {
            return IsoDepErrorOverflow;
        }
        memcpy(&response[*response_size], bit_buffer_get_data(instance->rx) + 1, inf_size);
        *response_size += inf_size;

        if(!ISO_DEP_IS_CHAINING(pcb)) {
            break;
        }

        error = iso_dep_request_next_block(instance);
        if(error != IsoDepErrorNone) {
            return error;
        }
    }

    return IsoDepErrorNone;
}

bool iso_dep_check_presence(IsoDep* instance) {
    furi_check(instance);
    furi_check(instance->poller);

    bit_buffer_reset(instance->tx);
    bit_buffer_append_byte(instance->tx, ISO_DEP_PCB_R_NAK | instance->block_number);

    if(iso_dep_exchange(instance) != IsoDepErrorNone) {
        return false;
    }

    // The card answers R(ACK), the block number stays unchanged
    const uint8_t pcb = bit_buffer_get_byte(instance->rx, 0);
    return ISO_DEP_IS_R_BLOCK(pcb) && !(pcb & ISO_DEP_PCB_NAK);
}

void iso_dep_deselect(IsoDep* instance) {
    furi_check(instance);
    furi_check(instance->poller);

    bit_buffer_reset(instance->tx);
    bit_buffer_append_byte(instance->tx, ISO_DEP_PCB_S_DESELECT);

    iso_dep_exchange(instance);
}
