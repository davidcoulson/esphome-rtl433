// Entry points the ESPHome component uses
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

int rtl_433_main(int argc, char **argv);
void rtl433_port_set_main_task(void);
extern volatile int rtl433_port_exit_code;

typedef void (*rtl433_port_log_sink_t)(int level, char const *src, char const *msg);
void rtl433_port_set_log_sink(rtl433_port_log_sink_t sink);
void rtl433_port_set_band_decoders(int band, char const *list);

#include <stddef.h>
#include <stdint.h>
extern size_t rtl433_port_usb_ring_bytes;
extern uint8_t rtl433_port_usb_task_priority;
extern uint8_t rtl433_port_usb_task_core;
extern volatile uint32_t rtl433_port_events;
extern volatile int rtl433_port_http_read_only;  // refuse every /cmd method except get_*
void *rtl433_port_acquire_task_handle(void);     // FreeRTOS handle of the USB acquire thread, or NULL

// Snapshot of the USB stream counters; false while no dongle is open
struct rtl433_usb_stats {
    uint32_t effective_sps;
    uint32_t sample_rate;
    uint32_t frequency;
    uint32_t overruns;        // USB side couldn't keep the ring fed
    uint32_t consumer_drops;  // rtl_433 didn't read fast enough: samples lost
    uint64_t bytes_total;
};
int rtl433_port_usb_stats(struct rtl433_usb_stats *out);

#ifdef __cplusplus
}
#endif
