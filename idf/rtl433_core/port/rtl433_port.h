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
