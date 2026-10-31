#include "ccid_usb.h"

#include <furi.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_cdc.h>
#include <usb_ccid.h>

#define TAG "CcidUsb"

/* Flipper uses 0483:5740 for CDC and 0483:5741 for U2F */
#define CCID_USB_PID (0x5742)

#define CCID_INTERFACE (CDC_EXT_FIRST_INTERFACE)

#define CCID_EP_OUT         (0x04)
#define CCID_EP_IN          (0x85)
#define CCID_EP_NOTIFY      (0x86)
#define CCID_EP_SIZE        (64U)
#define CCID_EP_NOTIFY_SIZE (8)

/* Interrupt endpoint for slot change notifications. It is optional in CCID, without it
 * the host polls PC_to_RDR_GetSlotStatus instead. Off by default: everything was tested
 * without it, it was switched off while chasing the lost bulk OUT events. */
#ifndef CCID_USE_NOTIFY_EP
#define CCID_USE_NOTIFY_EP (0)
#endif

#define CCID_HEADER_SIZE    (10U)
/* Payload per CCID message, longer APDUs are split into several messages */
#define CCID_PAYLOAD_MAX    (1024U)
#define CCID_MESSAGE_MAX    (CCID_HEADER_SIZE + CCID_PAYLOAD_MAX)
#define CCID_T1_PARAMS_SIZE (7)

#define CCID_TX_TIMEOUT_MS              (1000)
#define CCID_IDLE_POLL_MS               (20)
#define CCID_TIME_EXTENSION_INTERVAL_MS (1000)

/* Contactless: 13.56 MHz carrier, 106 kbit/s */
#define CCID_CLOCK_KHZ (13560)
#define CCID_DATA_RATE (106000)

/* Values from the CCID 1.1 specification. Some of the usb_ccid.h constants
 * (voltage, protocol bits) do not match it, so they are defined here. */
#define CCID_VOLTAGE_5V   (0x01)
#define CCID_PROTOCOLS_T1 (0x02)

#define CCID_FEATURE_AUTO_PARAMETERS  (0x00000002)
#define CCID_FEATURE_AUTO_ACTIVATION  (0x00000004)
#define CCID_FEATURE_AUTO_VOLTAGE     (0x00000008)
#define CCID_FEATURE_AUTO_CLOCK       (0x00000010)
#define CCID_FEATURE_AUTO_BAUD        (0x00000020)
#define CCID_FEATURE_AUTO_NEGOTIATION (0x00000040)
#define CCID_FEATURE_EXTENDED_APDU    (0x00040000)

#define CCID_ICC_ACTIVE   (0x00)
#define CCID_ICC_INACTIVE (0x01)
#define CCID_ICC_ABSENT   (0x02)

#define CCID_CMD_OK             (0x00)
#define CCID_CMD_FAILED         (0x40)
#define CCID_CMD_TIME_EXTENSION (0x80)

#define CCID_ERR_NONE              (0x00)
#define CCID_ERR_CMD_NOT_SUPPORTED (0x00)
#define CCID_ERR_BAD_LENGTH        (0x01)
#define CCID_ERR_SLOT_NOT_FOUND    (0x05)
#define CCID_ERR_BAD_PROTOCOL      (0x07)
#define CCID_ERR_BAD_LEVEL         (0x08)
#define CCID_ERR_ICC_MUTE          (0xFE)

/* PC_to_RDR_XfrBlock wLevelParameter for extended APDUs */
#define CCID_LEVEL_COMPLETE      (0x0000)
#define CCID_LEVEL_BEGIN         (0x0001)
#define CCID_LEVEL_END           (0x0002)
#define CCID_LEVEL_CONTINUE      (0x0003)
#define CCID_LEVEL_RESPONSE_NEXT (0x0010)

/* RDR_to_PC_DataBlock bChainParameter */
#define CCID_CHAIN_COMPLETE     (0x00)
#define CCID_CHAIN_BEGIN        (0x01)
#define CCID_CHAIN_END          (0x02)
#define CCID_CHAIN_CONTINUE     (0x03)
#define CCID_CHAIN_COMMAND_MORE (0x10)

#define CCID_NOTIFY_PRESENT (0x01)
#define CCID_NOTIFY_CHANGED (0x02)

typedef enum {
    CcidUsbEventStop = (1 << 0),
    CcidUsbEventRx = (1 << 1),
    CcidUsbEventTxDone = (1 << 2),
    CcidUsbEventNotifyDone = (1 << 3),
    CcidUsbEventSlotChange = (1 << 4),
    CcidUsbEventXfrDone = (1 << 5),
    CcidUsbEventConfigured = (1 << 6),
} CcidUsbEvent;

#define CCID_WORKER_EVENTS                                                                 \
    (CcidUsbEventStop | CcidUsbEventRx | CcidUsbEventNotifyDone | CcidUsbEventSlotChange | \
     CcidUsbEventConfigured)

struct CcidFunctionDescriptor {
    struct usb_interface_descriptor interface;
    struct usb_ccid_descriptor ccid;
    struct usb_endpoint_descriptor ep_out;
    struct usb_endpoint_descriptor ep_in;
    struct usb_endpoint_descriptor ep_notify;
} FURI_PACKED;

static const struct CcidFunctionDescriptor ccid_function_descriptor = {
    .interface =
        {
            .bLength = sizeof(struct usb_interface_descriptor),
            .bDescriptorType = USB_DTYPE_INTERFACE,
            .bInterfaceNumber = CCID_INTERFACE,
            .bAlternateSetting = 0,
            .bNumEndpoints = CCID_USE_NOTIFY_EP ? 3 : 2,
            .bInterfaceClass = USB_CLASS_CCID,
            .bInterfaceSubClass = USB_CCID_SUBCLASS,
            .bInterfaceProtocol = USB_CCID_PROTO_CCID,
            .iInterface = NO_DESCRIPTOR,
        },
    .ccid =
        {
            .bLength = sizeof(struct usb_ccid_descriptor),
            .bDescriptorType = USB_DTYPE_CCID_FUNCTIONAL,
            .bcdCCID = CCID_CURRENT_SPEC_RELEASE_NUMBER,
            .bMaxSlotIndex = 0,
            .bVoltageSupport = CCID_VOLTAGE_5V,
            .dwProtocols = CCID_PROTOCOLS_T1,
            .dwDefaultClock = CCID_CLOCK_KHZ,
            .dwMaximumClock = CCID_CLOCK_KHZ,
            .bNumClockSupported = 0,
            .dwDataRate = CCID_DATA_RATE,
            .dwMaxDataRate = CCID_DATA_RATE,
            .bNumDataRatesSupported = 0,
            .dwMaxIFSD = 254,
            .dwSynchProtocols = 0,
            .dwMechanical = 0,
            .dwFeatures = CCID_FEATURE_AUTO_PARAMETERS | CCID_FEATURE_AUTO_ACTIVATION |
                          CCID_FEATURE_AUTO_VOLTAGE | CCID_FEATURE_AUTO_CLOCK |
                          CCID_FEATURE_AUTO_BAUD | CCID_FEATURE_AUTO_NEGOTIATION |
                          CCID_FEATURE_EXTENDED_APDU,
            .dwMaxCCIDMessageLength = CCID_MESSAGE_MAX,
            .bClassGetResponse = 0xFF,
            .bClassEnvelope = 0xFF,
            .wLcdLayout = 0,
            .bPINSupport = 0,
            .bMaxCCIDBusySlots = 1,
        },
    .ep_out =
        {
            .bLength = sizeof(struct usb_endpoint_descriptor),
            .bDescriptorType = USB_DTYPE_ENDPOINT,
            .bEndpointAddress = CCID_EP_OUT,
            .bmAttributes = USB_EPTYPE_BULK,
            .wMaxPacketSize = CCID_EP_SIZE,
            .bInterval = 0,
        },
    .ep_in =
        {
            .bLength = sizeof(struct usb_endpoint_descriptor),
            .bDescriptorType = USB_DTYPE_ENDPOINT,
            .bEndpointAddress = CCID_EP_IN,
            .bmAttributes = USB_EPTYPE_BULK,
            .wMaxPacketSize = CCID_EP_SIZE,
            .bInterval = 0,
        },
    .ep_notify =
        {
            .bLength = sizeof(struct usb_endpoint_descriptor),
            .bDescriptorType = USB_DTYPE_ENDPOINT,
            .bEndpointAddress = CCID_EP_NOTIFY,
            .bmAttributes = USB_EPTYPE_INTERRUPT,
            .wMaxPacketSize = CCID_EP_NOTIFY_SIZE,
            .bInterval = 16,
        },
};

/* Answer to PC_to_RDR_GetParameters for T=1 (CCID 1.1, 6.2.3) */
static const uint8_t ccid_t1_parameters[CCID_T1_PARAMS_SIZE] = {
    0x11, // bmFindexDindex: Fi=372, Di=1
    0x10, // bmTCCKST1: LRC, direct convention
    0x00, // bGuardTimeT1
    0x4D, // bmWaitingIntegersT1: BWI=4, CWI=13
    0x00, // bClockStop: not allowed
    0xFE, // bIFSC
    0x00, // bNadValue
};

struct CcidUsb {
    const CcidUsbCallbacks* callbacks;
    void* context;

    FuriHalUsbInterface* usb_mode_prev;
    FuriHalUsbCdcExtension extension;
    usbd_device* dev;
    volatile bool connected;
    volatile bool tx_busy;

    FuriThread* thread;

    /* Requested by the backend, applied by the worker */
    volatile bool card_present_request;

    /* Worker state */
    bool card_present;
    bool card_active;
    bool notify_changed;
    bool notify_busy;

    /* xfr_pending: waiting for the backend, xfr_done: response copied and ready */
    volatile bool xfr_pending;
    volatile bool xfr_done;

    /* Extended APDUs: command reassembly and response streaming */
    uint8_t* command;
    size_t command_size;
    const uint8_t* response;
    size_t response_size;
    size_t response_offset;

    size_t rx_received;
    uint8_t rx_buffer[CCID_MESSAGE_MAX];
    uint8_t tx_buffer[CCID_MESSAGE_MAX];
    uint8_t notify_buffer[2];

    CcidUsbStats stats;
};

/* usbd callbacks carry no context */
static CcidUsb* ccid_usb = NULL;

static uint32_t ccid_get_u32(const uint8_t* data) {
    return data[0] | (data[1] << 8) | (data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void ccid_put_u32(uint8_t* data, uint32_t value) {
    data[0] = value & 0xFF;
    data[1] = (value >> 8) & 0xFF;
    data[2] = (value >> 16) & 0xFF;
    data[3] = (value >> 24) & 0xFF;
}

static void ccid_usb_set_event(CcidUsb* instance, CcidUsbEvent event) {
    furi_thread_flags_set(furi_thread_get_id(instance->thread), event);
}

/* USB interrupt context */
static void ccid_usb_ep_callback(usbd_device* dev, uint8_t event, uint8_t ep) {
    UNUSED(dev);

    if(ccid_usb == NULL) {
        return;
    }

    if(event == usbd_evt_eprx) {
        ccid_usb_set_event(ccid_usb, CcidUsbEventRx);
    } else if(event == usbd_evt_eptx) {
        if(ep == CCID_EP_IN) {
            ccid_usb->tx_busy = false;
            ccid_usb_set_event(ccid_usb, CcidUsbEventTxDone);
        } else if(ep == CCID_EP_NOTIFY) {
            ccid_usb_set_event(ccid_usb, CcidUsbEventNotifyDone);
        }
    }
}

/* USB interrupt or USB driver thread context */
static usbd_respond ccid_usb_ep_config(usbd_device* dev, uint8_t cfg) {
    furi_check(ccid_usb);

    switch(cfg) {
    case 0:
        ccid_usb->connected = false;

        usbd_ep_deconfig(dev, CCID_EP_OUT);
        usbd_ep_deconfig(dev, CCID_EP_IN);
        if(CCID_USE_NOTIFY_EP) {
            usbd_ep_deconfig(dev, CCID_EP_NOTIFY);
        }

        usbd_reg_endpoint(dev, CCID_EP_OUT, NULL);
        usbd_reg_endpoint(dev, CCID_EP_IN, NULL);
        if(CCID_USE_NOTIFY_EP) {
            usbd_reg_endpoint(dev, CCID_EP_NOTIFY, NULL);
        }
        return usbd_ack;
    case 1:
        ccid_usb->dev = dev;

        usbd_ep_config(dev, CCID_EP_OUT, USB_EPTYPE_BULK, CCID_EP_SIZE);
        usbd_ep_config(dev, CCID_EP_IN, USB_EPTYPE_BULK, CCID_EP_SIZE);
        if(CCID_USE_NOTIFY_EP) {
            usbd_ep_config(dev, CCID_EP_NOTIFY, USB_EPTYPE_INTERRUPT, CCID_EP_NOTIFY_SIZE);
        }

        usbd_reg_endpoint(dev, CCID_EP_OUT, ccid_usb_ep_callback);
        usbd_reg_endpoint(dev, CCID_EP_IN, ccid_usb_ep_callback);
        if(CCID_USE_NOTIFY_EP) {
            usbd_reg_endpoint(dev, CCID_EP_NOTIFY, ccid_usb_ep_callback);
        }

        ccid_usb->connected = true;
        ccid_usb_set_event(ccid_usb, CcidUsbEventConfigured);
        return usbd_ack;
    default:
        return usbd_fail;
    }
}

/* USB interrupt context */
static usbd_respond
    ccid_usb_control(usbd_device* dev, usbd_ctlreq* req, usbd_rqc_callback* callback) {
    UNUSED(dev);
    UNUSED(callback);

    const uint8_t type = req->bmRequestType & (USB_REQ_RECIPIENT | USB_REQ_TYPE);
    if(type != (USB_REQ_INTERFACE | USB_REQ_CLASS) || req->wIndex != CCID_INTERFACE) {
        return usbd_fail;
    }

    switch(req->bRequest) {
    case CCID_ABORT:
        // The bulk PC_to_RDR_Abort that follows is answered by the worker
        return usbd_ack;
    default:
        // No selectable clock frequencies or data rates
        return usbd_fail;
    }
}

static uint8_t ccid_usb_icc_status(CcidUsb* instance) {
    if(!instance->card_present) {
        return CCID_ICC_ABSENT;
    } else if(instance->card_active) {
        return CCID_ICC_ACTIVE;
    } else {
        return CCID_ICC_INACTIVE;
    }
}

static bool ccid_usb_write(CcidUsb* instance, const uint8_t* data, size_t size) {
    size_t offset = 0;

    while(true) {
        const size_t chunk = MIN(size - offset, (size_t)CCID_EP_SIZE);

        if(!instance->connected) {
            return false;
        }

        // tx_busy is cleared by the interrupt. Waiting on it instead of clearing the event
        // first means a stale TxDone event costs one loop pass and nothing gets lost.
        instance->tx_busy = true;
        if(usbd_ep_write(instance->dev, CCID_EP_IN, &data[offset], chunk) < 0) {
            instance->tx_busy = false;
            FURI_LOG_W(TAG, "Bulk IN busy");
            instance->stats.tx_errors++;
            return false;
        }

        const uint32_t deadline = furi_get_tick() + furi_ms_to_ticks(CCID_TX_TIMEOUT_MS);
        while(instance->tx_busy) {
            const int32_t remaining = (int32_t)(deadline - furi_get_tick());
            if(remaining <= 0) {
                instance->tx_busy = false;
                FURI_LOG_W(TAG, "Bulk IN timeout");
                instance->stats.tx_errors++;
                return false;
            }
            furi_thread_flags_wait(CcidUsbEventTxDone, FuriFlagWaitAny, remaining);
        }

        offset += chunk;

        // A short packet ends the transfer. A message that is a multiple of the packet
        // size needs a zero length packet, unless it fills the host buffer completely.
        if(chunk < CCID_EP_SIZE) {
            break;
        }
        if(offset == size && size == CCID_MESSAGE_MAX) {
            break;
        }
    }

    return true;
}

/* Send a RDR_to_PC message, payload must already be at tx_buffer + CCID_HEADER_SIZE */
static void ccid_usb_respond(
    CcidUsb* instance,
    uint8_t type,
    uint8_t seq,
    uint8_t command_status,
    uint8_t error,
    uint8_t parameter,
    size_t payload_size) {
    uint8_t* message = instance->tx_buffer;

    message[0] = type;
    ccid_put_u32(&message[1], payload_size);
    message[5] = 0; // bSlot
    message[6] = seq;
    message[7] = command_status | ccid_usb_icc_status(instance);
    message[8] = error;
    message[9] = parameter;

    ccid_usb_write(instance, message, CCID_HEADER_SIZE + payload_size);
}

static void ccid_usb_send_time_extension(CcidUsb* instance, uint8_t seq) {
    uint8_t message[CCID_HEADER_SIZE] = {0};

    message[0] = RDR_TO_PC_DATABLOCK;
    message[6] = seq;
    message[7] = CCID_CMD_TIME_EXTENSION | ccid_usb_icc_status(instance);
    message[8] = 1; // BWT multiplier

    ccid_usb_write(instance, message, sizeof(message));
}

static void ccid_usb_respond_parameters(CcidUsb* instance, uint8_t seq, uint8_t error) {
    const uint8_t command_status = (error == CCID_ERR_NONE) ? CCID_CMD_OK : CCID_CMD_FAILED;

    memcpy(&instance->tx_buffer[CCID_HEADER_SIZE], ccid_t1_parameters, CCID_T1_PARAMS_SIZE);
    ccid_usb_respond(
        instance, RDR_TO_PC_PARAMETERS, seq, command_status, error, 1, CCID_T1_PARAMS_SIZE);
}

static void ccid_usb_handle_power_on(CcidUsb* instance, uint8_t seq) {
    if(!instance->card_present) {
        ccid_usb_respond(
            instance, RDR_TO_PC_DATABLOCK, seq, CCID_CMD_FAILED, CCID_ERR_ICC_MUTE, 0, 0);
        return;
    }

    size_t atr_size = 0;
    uint8_t* atr = &instance->tx_buffer[CCID_HEADER_SIZE];
    if(instance->callbacks->power_on(instance->context, atr, &atr_size)) {
        furi_check(atr_size <= CCID_USB_ATR_MAX);
        instance->card_active = true;
        ccid_usb_respond(
            instance, RDR_TO_PC_DATABLOCK, seq, CCID_CMD_OK, CCID_ERR_NONE, 0, atr_size);
    } else {
        instance->card_active = false;
        ccid_usb_respond(
            instance, RDR_TO_PC_DATABLOCK, seq, CCID_CMD_FAILED, CCID_ERR_ICC_MUTE, 0, 0);
    }
}

static void ccid_usb_handle_power_off(CcidUsb* instance, uint8_t seq) {
    instance->command_size = 0;
    instance->response = NULL;

    if(instance->card_active) {
        instance->callbacks->power_off(instance->context);
        instance->card_active = false;
    }

    ccid_usb_respond(instance, RDR_TO_PC_SLOTSTATUS, seq, CCID_CMD_OK, CCID_ERR_NONE, 0, 0);
}

/* Send the next part of the response, split into messages of CCID_PAYLOAD_MAX bytes */
static void ccid_usb_send_response_chunk(CcidUsb* instance, uint8_t seq) {
    if(instance->response == NULL || instance->response_offset >= instance->response_size) {
        ccid_usb_respond(
            instance, RDR_TO_PC_DATABLOCK, seq, CCID_CMD_FAILED, CCID_ERR_BAD_LEVEL, 0, 0);
        return;
    }

    const size_t remaining = instance->response_size - instance->response_offset;
    const size_t chunk = MIN(remaining, CCID_PAYLOAD_MAX);
    const bool first = (instance->response_offset == 0);
    const bool last = (chunk == remaining);

    uint8_t chain = CCID_CHAIN_CONTINUE;
    if(first && last) {
        chain = CCID_CHAIN_COMPLETE;
    } else if(first) {
        chain = CCID_CHAIN_BEGIN;
    } else if(last) {
        chain = CCID_CHAIN_END;
    }

    memcpy(
        &instance->tx_buffer[CCID_HEADER_SIZE],
        &instance->response[instance->response_offset],
        chunk);
    instance->response_offset += chunk;
    if(last) {
        instance->response = NULL;
    }

    ccid_usb_respond(instance, RDR_TO_PC_DATABLOCK, seq, CCID_CMD_OK, CCID_ERR_NONE, chain, chunk);
}

/* Hand the reassembled command APDU to the backend and wait for its response */
static void ccid_usb_execute(CcidUsb* instance, uint8_t seq) {
    FURI_CRITICAL_ENTER();
    instance->xfr_pending = true;
    instance->xfr_done = false;
    instance->response = NULL;
    instance->response_size = 0;
    FURI_CRITICAL_EXIT();

    instance->callbacks->xfr_start(instance->context, instance->command, instance->command_size);

    // xfr_done is set by ccid_usb_xfr_complete(), the event only wakes us up
    while(!instance->xfr_done) {
        const uint32_t flags = furi_thread_flags_wait(
            CcidUsbEventXfrDone | CcidUsbEventStop,
            FuriFlagWaitAny,
            CCID_TIME_EXTENSION_INTERVAL_MS);

        if(!(flags & FuriFlagError) && (flags & CcidUsbEventStop)) {
            // Leave the flag set for the main loop
            FURI_CRITICAL_ENTER();
            instance->xfr_pending = false;
            FURI_CRITICAL_EXIT();
            ccid_usb_set_event(instance, CcidUsbEventStop);
            return;
        }

        if((flags & FuriFlagError) && !instance->xfr_done) {
            ccid_usb_send_time_extension(instance, seq);
        }
    }

    if(instance->response_size == 0) {
        ccid_usb_respond(
            instance, RDR_TO_PC_DATABLOCK, seq, CCID_CMD_FAILED, CCID_ERR_ICC_MUTE, 0, 0);
        return;
    }

    instance->response_offset = 0;
    ccid_usb_send_response_chunk(instance, seq);
}

static void ccid_usb_handle_xfr_block(
    CcidUsb* instance,
    uint8_t seq,
    uint16_t level,
    const uint8_t* data,
    size_t data_size) {
    if(!instance->card_active) {
        instance->command_size = 0;
        instance->response = NULL;
        ccid_usb_respond(
            instance, RDR_TO_PC_DATABLOCK, seq, CCID_CMD_FAILED, CCID_ERR_ICC_MUTE, 0, 0);
        return;
    }

    switch(level) {
    case CCID_LEVEL_COMPLETE:
    case CCID_LEVEL_BEGIN:
        // A new command, drop anything left over from the previous one
        instance->command_size = 0;
        instance->response = NULL;
        break;
    case CCID_LEVEL_CONTINUE:
    case CCID_LEVEL_END:
        break;
    case CCID_LEVEL_RESPONSE_NEXT:
        ccid_usb_send_response_chunk(instance, seq);
        return;
    default:
        ccid_usb_respond(
            instance, RDR_TO_PC_DATABLOCK, seq, CCID_CMD_FAILED, CCID_ERR_BAD_LEVEL, 0, 0);
        return;
    }

    if(instance->command_size + data_size > CCID_USB_COMMAND_MAX) {
        instance->command_size = 0;
        ccid_usb_respond(
            instance, RDR_TO_PC_DATABLOCK, seq, CCID_CMD_FAILED, CCID_ERR_BAD_LENGTH, 0, 0);
        return;
    }

    memcpy(&instance->command[instance->command_size], data, data_size);
    instance->command_size += data_size;

    if(level == CCID_LEVEL_BEGIN || level == CCID_LEVEL_CONTINUE) {
        ccid_usb_respond(
            instance,
            RDR_TO_PC_DATABLOCK,
            seq,
            CCID_CMD_OK,
            CCID_ERR_NONE,
            CCID_CHAIN_COMMAND_MORE,
            0);
        return;
    }

    if(instance->command_size == 0) {
        ccid_usb_respond(
            instance, RDR_TO_PC_DATABLOCK, seq, CCID_CMD_FAILED, CCID_ERR_BAD_LENGTH, 0, 0);
        return;
    }

    ccid_usb_execute(instance, seq);
}

static void ccid_usb_handle_set_parameters(CcidUsb* instance, uint8_t seq, uint8_t protocol) {
    if(protocol == 1) {
        ccid_usb_respond_parameters(instance, seq, CCID_ERR_NONE);
    } else {
        ccid_usb_respond_parameters(instance, seq, CCID_ERR_BAD_PROTOCOL);
    }
}

static void ccid_usb_handle_unsupported(CcidUsb* instance, uint8_t type, uint8_t seq) {
    uint8_t response_type = RDR_TO_PC_SLOTSTATUS;
    size_t payload_size = 0;

    if(type == PC_TO_RDR_SECURE) {
        response_type = RDR_TO_PC_DATABLOCK;
    } else if(type == PC_TO_RDR_ESCAPE) {
        response_type = RDR_TO_PC_ESCAPE;
    } else if(type == PC_TO_RDR_SETDATARATEANDCLOCKFREQUENCY) {
        response_type = RDR_TO_PC_DATARATEANDCLOCKFREQUENCY;
        ccid_put_u32(&instance->tx_buffer[CCID_HEADER_SIZE], CCID_CLOCK_KHZ);
        ccid_put_u32(&instance->tx_buffer[CCID_HEADER_SIZE + 4], CCID_DATA_RATE);
        payload_size = 8;
    }

    FURI_LOG_D(TAG, "Unsupported message 0x%02X", type);
    ccid_usb_respond(
        instance, response_type, seq, CCID_CMD_FAILED, CCID_ERR_CMD_NOT_SUPPORTED, 0, payload_size);
}

static void ccid_usb_handle_message(CcidUsb* instance, size_t payload_size) {
    const uint8_t* message = instance->rx_buffer;
    const uint8_t type = message[0];
    const uint8_t slot = message[5];
    const uint8_t seq = message[6];
    const uint8_t* payload = &message[CCID_HEADER_SIZE];

    FURI_LOG_T(TAG, "Message 0x%02X seq %u size %u", type, seq, payload_size);

    if(slot != 0) {
        uint8_t response_type = RDR_TO_PC_SLOTSTATUS;
        if(type == PC_TO_RDR_ICCPOWERON || type == PC_TO_RDR_XFRBLOCK ||
           type == PC_TO_RDR_SECURE) {
            response_type = RDR_TO_PC_DATABLOCK;
        } else if(
            type == PC_TO_RDR_GETPARAMETERS || type == PC_TO_RDR_RESETPARAMETERS ||
            type == PC_TO_RDR_SETPARAMETERS) {
            response_type = RDR_TO_PC_PARAMETERS;
        }
        ccid_usb_respond(
            instance, response_type, seq, CCID_CMD_FAILED, CCID_ERR_SLOT_NOT_FOUND, 0, 0);
        return;
    }

    switch(type) {
    case PC_TO_RDR_ICCPOWERON:
        ccid_usb_handle_power_on(instance, seq);
        break;
    case PC_TO_RDR_ICCPOWEROFF:
        ccid_usb_handle_power_off(instance, seq);
        break;
    case PC_TO_RDR_GETSLOTSTATUS:
    case PC_TO_RDR_ABORT:
        ccid_usb_respond(instance, RDR_TO_PC_SLOTSTATUS, seq, CCID_CMD_OK, CCID_ERR_NONE, 0, 0);
        break;
    case PC_TO_RDR_XFRBLOCK:
        ccid_usb_handle_xfr_block(
            instance, seq, message[8] | (message[9] << 8), payload, payload_size);
        break;
    case PC_TO_RDR_GETPARAMETERS:
    case PC_TO_RDR_RESETPARAMETERS:
        ccid_usb_respond_parameters(instance, seq, CCID_ERR_NONE);
        break;
    case PC_TO_RDR_SETPARAMETERS:
        ccid_usb_handle_set_parameters(instance, seq, message[7]);
        break;
    default:
        ccid_usb_handle_unsupported(instance, type, seq);
        break;
    }
}

/* Read one bulk OUT packet if there is one, returns true if a packet was read */
static bool ccid_usb_receive(CcidUsb* instance) {
    uint8_t packet[CCID_EP_SIZE];

    if(!instance->connected) {
        return false;
    }

    const int32_t packet_size = usbd_ep_read(instance->dev, CCID_EP_OUT, packet, sizeof(packet));
    if(packet_size < 0) {
        return false;
    }

    if(packet_size == 0) {
        return true;
    }

    // Store what fits, count the rest so oversized messages are still consumed completely
    if(instance->rx_received < sizeof(instance->rx_buffer)) {
        const size_t space = sizeof(instance->rx_buffer) - instance->rx_received;
        memcpy(
            &instance->rx_buffer[instance->rx_received], packet, MIN((size_t)packet_size, space));
    }
    instance->rx_received += packet_size;

    if(instance->rx_received < CCID_HEADER_SIZE) {
        return true;
    }

    const size_t payload_size = ccid_get_u32(&instance->rx_buffer[1]);
    if(instance->rx_received < CCID_HEADER_SIZE + payload_size) {
        return true;
    }

    instance->rx_received = 0;

    if(payload_size > CCID_PAYLOAD_MAX) {
        FURI_LOG_W(TAG, "Message too long: %u", payload_size);
        ccid_usb_respond(
            instance,
            RDR_TO_PC_SLOTSTATUS,
            instance->rx_buffer[6],
            CCID_CMD_FAILED,
            CCID_ERR_BAD_LENGTH,
            0,
            0);
        return true;
    }

    ccid_usb_handle_message(instance, payload_size);
    return true;
}

static void ccid_usb_update_slot(CcidUsb* instance) {
    const bool present = instance->card_present_request;

    if(present != instance->card_present) {
        instance->card_present = present;
        instance->card_active = false;
        instance->notify_changed = true;
        FURI_LOG_I(TAG, "Card %s", present ? "inserted" : "removed");
    }

    if(!CCID_USE_NOTIFY_EP) {
        return;
    }

    if(!instance->connected || !instance->notify_changed || instance->notify_busy) {
        return;
    }

    instance->notify_buffer[0] = RDR_TO_PC_NOTIFYSLOTCHANGE;
    instance->notify_buffer[1] = CCID_NOTIFY_CHANGED;
    if(present) {
        instance->notify_buffer[1] |= CCID_NOTIFY_PRESENT;
    }

    if(usbd_ep_write(
           instance->dev,
           CCID_EP_NOTIFY,
           instance->notify_buffer,
           sizeof(instance->notify_buffer)) >= 0) {
        instance->notify_changed = false;
        instance->notify_busy = true;
    }
}

static int32_t ccid_usb_worker(void* context) {
    CcidUsb* instance = context;

    while(true) {
        const uint32_t flags =
            furi_thread_flags_wait(CCID_WORKER_EVENTS, FuriFlagWaitAny, CCID_IDLE_POLL_MS);

        if(flags & FuriFlagError) {
            // Idle: make sure no bulk OUT packet is waiting without its event
            if(ccid_usb_receive(instance)) {
                instance->stats.recovered++;
                FURI_LOG_D(TAG, "Recovered bulk OUT packet without event");
            }
            continue;
        }

        if(flags & CcidUsbEventStop) {
            break;
        }

        if(flags & CcidUsbEventConfigured) {
            // Fresh configuration: drop partial messages and announce the current slot state
            instance->rx_received = 0;
            instance->notify_busy = false;
            instance->notify_changed = instance->card_present;
        }

        if(flags & CcidUsbEventNotifyDone) {
            instance->notify_busy = false;
        }

        if(flags & (CcidUsbEventSlotChange | CcidUsbEventNotifyDone | CcidUsbEventConfigured)) {
            ccid_usb_update_slot(instance);
        }

        if(flags & CcidUsbEventRx) {
            ccid_usb_receive(instance);
        }
    }

    return 0;
}

CcidUsb* ccid_usb_alloc(const CcidUsbCallbacks* callbacks, void* context) {
    furi_check(callbacks);
    furi_check(ccid_usb == NULL);

    CcidUsb* instance = malloc(sizeof(CcidUsb));
    instance->command = malloc(CCID_USB_COMMAND_MAX);
    instance->callbacks = callbacks;
    instance->context = context;

    instance->extension.pid = CCID_USB_PID;
    instance->extension.descriptor = &ccid_function_descriptor;
    // The notify endpoint descriptor is the last one, leave it out when unused
    instance->extension.descriptor_size = sizeof(ccid_function_descriptor);
    if(!CCID_USE_NOTIFY_EP) {
        instance->extension.descriptor_size -= sizeof(struct usb_endpoint_descriptor);
    }
    instance->extension.interface_count = 1;
    instance->extension.ep_config = ccid_usb_ep_config;
    instance->extension.control = ccid_usb_control;

    instance->thread = furi_thread_alloc_ex("CcidUsbWorker", 2048, ccid_usb_worker, instance);
    furi_thread_start(instance->thread);

    ccid_usb = instance;

    instance->usb_mode_prev = furi_hal_usb_get_config();
    furi_hal_usb_unlock();
    furi_check(furi_hal_usb_set_config(&usb_cdc_ext, &instance->extension));

    return instance;
}

void ccid_usb_free(CcidUsb* instance) {
    furi_check(instance);

    // Detaches the extension, no USB callbacks after this returns
    furi_hal_usb_set_config(instance->usb_mode_prev, NULL);

    ccid_usb_set_event(instance, CcidUsbEventStop);
    furi_thread_join(instance->thread);
    furi_thread_free(instance->thread);

    ccid_usb = NULL;
    free(instance->command);
    free(instance);
}

void ccid_usb_set_card_present(CcidUsb* instance, bool present) {
    furi_check(instance);

    instance->card_present_request = present;
    ccid_usb_set_event(instance, CcidUsbEventSlotChange);
}

void ccid_usb_xfr_complete(CcidUsb* instance, const uint8_t* response, size_t response_size) {
    furi_check(instance);

    FURI_CRITICAL_ENTER();
    const bool pending = instance->xfr_pending;
    instance->xfr_pending = false;
    FURI_CRITICAL_EXIT();

    if(!pending) {
        return;
    }

    if(response_size > CCID_USB_RESPONSE_MAX) {
        FURI_LOG_E(TAG, "Response too long: %u", response_size);
        response_size = 0;
    }

    if(response_size > 0) {
        furi_check(response);
    }
    instance->response = response;
    instance->response_size = response_size;
    instance->xfr_done = true;

    ccid_usb_set_event(instance, CcidUsbEventXfrDone);
}

void ccid_usb_get_stats(CcidUsb* instance, CcidUsbStats* stats) {
    furi_check(instance);
    furi_check(stats);

    *stats = instance->stats;
}

bool ccid_usb_is_connected(CcidUsb* instance) {
    furi_check(instance);
    return instance->connected;
}
