// Forced into every rtl_433 translation unit. Keeps the upstream sources close to unmodified.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// rtl_433 calls exit() on fatal errors. On a microcontroller that would reboot the chip, so it ends
// the rtl_433 task instead; the ESPHome component notices and reports the failure.
void rtl433_port_exit(int code) __attribute__((noreturn));

// Every rtl_433 log line (see src/logger.c); the ESPHome component installs the sink
typedef void (*rtl433_port_log_sink_t)(int level, char const *src, char const *msg);
void rtl433_port_set_log_sink(rtl433_port_log_sink_t sink);
void rtl433_port_log(int level, char const *src, char const *msg);

// USB driver tuning, set by the ESPHome component before rtl_433 opens the dongle (0 = driver default)
extern size_t rtl433_port_usb_ring_bytes;
extern uint8_t rtl433_port_usb_task_priority;
extern uint8_t rtl433_port_usb_task_core;  // 0xFF = no affinity

// True on the task that runs rtl_433_main() (recorded when it starts)
int rtl433_port_on_main_task(void);

// upstream rtl_433's sdr.c guards its setters with pthread_equal(dev->thread, pthread_self()) to
// refuse a call made from inside the acquire callback. On desktop every thread is a pthread, so
// pthread_self() is always safe; on ESP-IDF our own main/decode task is a plain FreeRTOS task (never
// pthread_create()'d), and esp-idf's pthread_self() asserts outright when called from one. Track the
// acquire thread's FreeRTOS handle ourselves instead of going through POSIX identity at all.
void rtl433_port_note_acquire_task(void);
int rtl433_port_on_acquire_task(void);

/* Task watchdog: the decoding task subscribes itself (and takes its core's idle task off the
   watch, since a saturated decoder starves idle without being hung) and feeds the watchdog from
   rtl_433's main loop, which returns at least every 500 ms even with no samples. */
/* The acquire thread's own stack high-water mark (bytes), refreshed by the thread itself about once
   a second. Read this instead of calling uxTaskGetStackHighWaterMark() on its handle: the thread is
   recreated on every input restart (dongle replug) and a stale handle is a load fault. */
extern volatile unsigned rtl433_port_acquire_stack_free;
void rtl433_port_wdt_subscribe(void);
void rtl433_port_wdt_feed(void);
void rtl433_port_wdt_unsubscribe(void); /* a subscribed task must call this before it exits */

// Counts decoded events (src/r_api.c)
void rtl433_port_count_event(void);

// Per-band decoder sets (port/rtl433_bands.c)
struct r_cfg;
void rtl433_port_set_band_decoders(int band, char const *list);
void rtl433_port_apply_band(struct r_cfg *cfg);
void rtl433_port_manual_tune(struct r_cfg *cfg);

// lwIP's netdb has getaddrinfo()/getnameinfo() but not these
const char *gai_strerror(int ecode);

#ifdef __cplusplus
}
#endif

#define exit(code) rtl433_port_exit(code)

#ifndef NI_NUMERICHOST
#define NI_NUMERICHOST 1
#endif
#ifndef NI_NUMERICSERV
#define NI_NUMERICSERV 2
#endif
#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 46  // lwIP only defines it when IPv6 is compiled in
#endif
