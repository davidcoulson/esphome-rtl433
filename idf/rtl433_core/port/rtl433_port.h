// Forced into every rtl_433 translation unit. Keeps the upstream sources close to unmodified.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// rtl_433 calls exit() on fatal errors. On a microcontroller that would reboot the chip, so it ends
// the rtl_433 task instead; the ESPHome component notices and reports the failure.
void rtl433_port_exit(int code) __attribute__((noreturn));

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
