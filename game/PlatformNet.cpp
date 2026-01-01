#include "game/Platform.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb_cdc.h>
#include "usb_cdc.h"

namespace {
constexpr uint8_t VCP_CH = 0;

#ifndef CDC_DATA_SZ
#define CDC_DATA_SZ 64
#endif

constexpr size_t USB_PKT_LEN = CDC_DATA_SZ;
constexpr size_t RX_STREAM_SIZE = USB_PKT_LEN * 8;

typedef enum {
    NetEvtStop = (1 << 0),
    NetEvtRx = (1 << 1),
} NetEvtFlags;

static FuriThread* net_thread = NULL;
static FuriStreamBuffer* rx_stream = NULL;
static FuriMutex* usb_mutex = NULL;

static uint8_t peek_valid = 0;
static uint8_t peek_byte = 0;
static bool started = false;

static int32_t net_worker(void* ctx);

static void net_on_cdc_tx_complete(void* ctx) {
    UNUSED(ctx);
}

static void net_on_cdc_rx(void* ctx) {
    UNUSED(ctx);
    if(net_thread) {
        furi_thread_flags_set(furi_thread_get_id(net_thread), NetEvtRx);
    }
}

static void net_state_cb(void* ctx, CdcState state) {
    UNUSED(ctx);
    UNUSED(state);
}

static void net_on_ctrl_line(void* ctx, CdcCtrlLine state) {
    UNUSED(ctx);
    UNUSED(state);
}

static void net_on_line_cfg(void* ctx, struct usb_cdc_line_coding* cfg) {
    UNUSED(ctx);
    UNUSED(cfg);
}

static const CdcCallbacks cdc_cb = {
    net_on_cdc_tx_complete,
    net_on_cdc_rx,
    net_state_cb,
    net_on_ctrl_line,
    net_on_line_cfg,
};

static void ensure_started() {
    if(started) return;
    started = true;

    usb_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    rx_stream = furi_stream_buffer_alloc(RX_STREAM_SIZE, 1);

    furi_hal_usb_unlock();
    furi_check(furi_hal_usb_set_config(&usb_cdc_single, NULL) == true);

    furi_hal_cdc_set_callbacks(VCP_CH, (CdcCallbacks*)&cdc_cb, NULL);

    net_thread = furi_thread_alloc_ex("PlatformNet", 1024, net_worker, NULL);
    furi_thread_start(net_thread);

    peek_valid = 0;
}

static int32_t net_worker(void* ctx) {
    UNUSED(ctx);

    uint8_t buf[USB_PKT_LEN];

    while(true) {
        uint32_t ev =
            furi_thread_flags_wait(NetEvtStop | NetEvtRx, FuriFlagWaitAny, FuriWaitForever);
        furi_check(!(ev & FuriFlagError));
        if(ev & NetEvtStop) break;

        if(ev & NetEvtRx) {
            while(true) {
                furi_check(furi_mutex_acquire(usb_mutex, FuriWaitForever) == FuriStatusOk);
                size_t n = furi_hal_cdc_receive(VCP_CH, buf, sizeof(buf));
                furi_check(furi_mutex_release(usb_mutex) == FuriStatusOk);

                if(n == 0) break;

                furi_stream_buffer_send(rx_stream, buf, n, 0);

                if(n < sizeof(buf)) break;
            }
        }
    }

    return 0;
}

static inline size_t rx_available() {
    if(!rx_stream) return 0;
    return furi_stream_buffer_bytes_available(rx_stream);
}
}

bool PlatformNet::IsAvailable() {
    ensure_started();
    if(peek_valid) return true;
    return rx_available() > 0;
}

bool PlatformNet::IsAvailableForWrite() {
    ensure_started();
    return true;
}

uint8_t PlatformNet::Read() {
    ensure_started();

    if(peek_valid) {
        peek_valid = 0;
        return peek_byte;
    }

    uint8_t b = 0;
    size_t n = furi_stream_buffer_receive(rx_stream, &b, 1, 0);
    return (n == 1) ? b : 0;
}

uint8_t PlatformNet::Peek() {
    ensure_started();

    if(peek_valid) return peek_byte;

    uint8_t b = 0;
    size_t n = furi_stream_buffer_receive(rx_stream, &b, 1, 0);
    if(n == 1) {
        peek_byte = b;
        peek_valid = 1;
        return peek_byte;
    }
    return 0;
}

void PlatformNet::Write(uint8_t data) {
    ensure_started();

    furi_check(furi_mutex_acquire(usb_mutex, FuriWaitForever) == FuriStatusOk);
    furi_hal_cdc_send(VCP_CH, &data, 1);
    furi_check(furi_mutex_release(usb_mutex) == FuriStatusOk);
}

char PlatformNet::GenerateRandomNetworkToken() {
    uint32_t r = furi_hal_random_get();
    char t = (char)((r & 0x7F) | 1);
    if(t == 0) t = 1;
    return t;
}
