#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Largest command APDU accepted from the host: header, extended Lc, 4096 bytes data, extended Le */
#define CCID_USB_COMMAND_MAX (4 + 3 + 4096 + 2)

/** Largest response APDU sent to the host: 16384 bytes data + SW1 SW2 */
#define CCID_USB_RESPONSE_MAX (16384 + 2)

/** Largest ATR (ISO 7816-3) */
#define CCID_USB_ATR_MAX (33)

typedef struct CcidUsb CcidUsb;

/** Diagnostic counters */
typedef struct {
    uint32_t isr_rx; /**< Bulk OUT packets signalled by the USB interrupt */
    uint32_t reads; /**< Bulk OUT packets read by the worker */
    uint32_t recovered; /**< Packets only found by the idle poll, their event got lost */
    uint32_t messages; /**< Complete CCID messages handled */
    uint32_t tx_errors; /**< Failed bulk IN writes */
    uint8_t last_type; /**< bMessageType of the last message */
    uint8_t last_seq; /**< bSeq of the last message */
} CcidUsbStats;

/** Card backend callbacks, all of them are called from the CCID worker thread */
typedef struct {
    /** Activate the card and return its ATR
     *
     * @param      context   callback context
     * @param[out] atr       ATR buffer, CCID_USB_ATR_MAX bytes
     * @param[out] atr_size  ATR size
     * @return     true if the card was activated
     */
    bool (*power_on)(void* context, uint8_t* atr, size_t* atr_size);

    /** Deactivate the card */
    void (*power_off)(void* context);

    /** Start an APDU exchange
     *
     * The backend must answer with ccid_usb_xfr_complete(), either from inside this callback
     * or later from any thread. The command buffer is valid until then.
     */
    void (*xfr_start)(void* context, const uint8_t* command, size_t command_size);
} CcidUsbCallbacks;

/** Switch USB to CDC + CCID mode and start the CCID worker
 *
 * @param      callbacks  card backend callbacks, must stay valid until ccid_usb_free()
 * @param      context    callback context
 * @return     CcidUsb instance
 */
CcidUsb* ccid_usb_alloc(const CcidUsbCallbacks* callbacks, void* context);

/** Stop the CCID worker and restore the previous USB mode */
void ccid_usb_free(CcidUsb* instance);

/** Report card insertion or removal to the host */
void ccid_usb_set_card_present(CcidUsb* instance, bool present);

/** Finish the APDU exchange started with xfr_start
 *
 * The response is not copied, it must stay valid until the next xfr_start or power_off.
 *
 * @param      instance       CcidUsb instance
 * @param      response       response APDU including SW1 SW2, ignored if response_size is 0
 * @param      response_size  response size, 0 reports that the card did not answer
 */
void ccid_usb_xfr_complete(CcidUsb* instance, const uint8_t* response, size_t response_size);

/** Read the diagnostic counters */
void ccid_usb_get_stats(CcidUsb* instance, CcidUsbStats* stats);

/** Check whether the host has configured the device */
bool ccid_usb_is_connected(CcidUsb* instance);

#ifdef __cplusplus
}
#endif
