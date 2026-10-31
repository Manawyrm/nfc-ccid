#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>

#include "ccid_usb.h"
#include "nfc_ccid_card.h"

#define NFC_CCID_REFRESH_MS (250)

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;

    NfcCcidCard* card;
    CcidUsb* ccid;
} NfcCcidApp;

static void nfc_ccid_app_draw_callback(Canvas* canvas, void* context) {
    NfcCcidApp* app = context;
    char line[48];

    NfcCcidCardInfo info;
    nfc_ccid_card_get_info(app->card, &info);

    CcidUsbStats stats;
    ccid_usb_get_stats(app->ccid, &stats);

    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "NFC CCID Reader");

    canvas_set_font(canvas, FontSecondary);

    if(ccid_usb_is_connected(app->ccid)) {
        canvas_draw_str(canvas, 0, 21, "USB: configured");
    } else {
        canvas_draw_str(canvas, 0, 21, "USB: waiting for host");
    }

    if(info.type != NfcCcidCardTypeNone) {
        size_t length = snprintf(line, sizeof(line), "UID");
        for(size_t i = 0; i < info.uid_size && length < sizeof(line) - 3; i++) {
            length += snprintf(&line[length], sizeof(line) - length, " %02X", info.uid[i]);
        }
        canvas_draw_str(canvas, 0, 31, line);

        if(info.type == NfcCcidCardTypeIso14443_4a) {
            snprintf(line, sizeof(line), "ISO 14443-4  SAK %02X  FSC %u", info.sak, info.fsc);
        } else if(info.type == NfcCcidCardTypeIso15693) {
            snprintf(
                line,
                sizeof(line),
                "ISO 15693  %u blocks x %u B",
                info.block_count,
                info.block_size);
        } else {
            snprintf(line, sizeof(line), "Not ISO 14443-4 (SAK %02X)", info.sak);
        }
        canvas_draw_str(canvas, 0, 41, line);
    } else {
        canvas_draw_str(canvas, 0, 31, "Place a card on the back");
    }

    snprintf(
        line,
        sizeof(line),
        "APDU %lu  err %lu  retry %lu",
        info.apdu_count,
        info.error_count,
        info.retry_count);
    canvas_draw_str(canvas, 0, 52, line);

    snprintf(
        line, sizeof(line), "USB lost events %lu  tx err %lu", stats.recovered, stats.tx_errors);
    canvas_draw_str(canvas, 0, 62, line);
}

static void nfc_ccid_app_input_callback(InputEvent* input_event, void* context) {
    NfcCcidApp* app = context;
    furi_message_queue_put(app->input_queue, input_event, FuriWaitForever);
}

static NfcCcidApp* nfc_ccid_app_alloc(void) {
    NfcCcidApp* app = malloc(sizeof(NfcCcidApp));

    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    // Before the view port goes live: the draw callback queries both
    app->card = nfc_ccid_card_alloc();
    app->ccid = ccid_usb_alloc(&nfc_ccid_card_ccid_callbacks, app->card);
    nfc_ccid_card_start(app->card, app->ccid);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, nfc_ccid_app_draw_callback, app);
    view_port_input_callback_set(app->view_port, nfc_ccid_app_input_callback, app);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    return app;
}

static void nfc_ccid_app_free(NfcCcidApp* app) {
    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(app->view_port);

    // NFC first, so nothing completes transfers on a freed CCID instance
    nfc_ccid_card_stop(app->card);
    ccid_usb_free(app->ccid);
    nfc_ccid_card_free(app->card);

    furi_message_queue_free(app->input_queue);

    free(app);
}

int32_t nfc_ccid_app(void* p) {
    UNUSED(p);

    NfcCcidApp* app = nfc_ccid_app_alloc();

    bool running = true;
    while(running) {
        InputEvent event;
        const FuriStatus status =
            furi_message_queue_get(app->input_queue, &event, NFC_CCID_REFRESH_MS);

        if(status == FuriStatusOk && event.type == InputTypeShort && event.key == InputKeyBack) {
            running = false;
        }

        view_port_update(app->view_port);
    }

    nfc_ccid_app_free(app);

    return 0;
}
