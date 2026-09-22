#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "rtl433_port.h"

static const char *const TAG = "rtl_433";

volatile int rtl433_port_exit_code = -1;

size_t rtl433_port_usb_ring_bytes = 0;
uint8_t rtl433_port_usb_task_priority = 0;
uint8_t rtl433_port_usb_task_core = 0xFF;

volatile uint32_t rtl433_port_events = 0;
void rtl433_port_count_event(void) { rtl433_port_events++; }

static rtl433_port_log_sink_t log_sink = NULL;

void rtl433_port_set_log_sink(rtl433_port_log_sink_t sink) { log_sink = sink; }

void rtl433_port_log(int level, char const *src, char const *msg) {
  if (log_sink != NULL)
    log_sink(level, src, msg);
  else
    ESP_LOGI(TAG, "%s: %s", src, msg);
}

void rtl433_port_exit(int code) {
  rtl433_port_exit_code = code;
  ESP_LOGE(TAG, "rtl_433 exited with code %d", code);
  fflush(stdout);
  fflush(stderr);
  vTaskDelete(NULL);  // only ever called from the rtl_433 task (or its acquire thread)
  for (;;) {
  }
}

const char *gai_strerror(int ecode) {
  static char buf[32];
  snprintf(buf, sizeof(buf), "getaddrinfo error %d", ecode);
  return buf;
}

// POSIX calls newlib/lwIP on ESP-IDF don't provide. rtl_433 only uses them for signals (there are none),
// config-file probing, and naming itself in MQTT/syslog/influx output.

int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
  (void) sig;
  (void) act;
  if (old != NULL)
    memset(old, 0, sizeof(*old));
  return 0;
}

int access(const char *path, int mode) {
  (void) mode;
  struct stat st;
  return stat(path, &st);  // fails with ENOENT: there is no filesystem mounted
}

int gethostname(char *name, size_t len) {
  if (len == 0)
    return -1;
  strncpy(name, "rtl433-esp", len);
  name[len - 1] = '\0';
  return 0;
}

int getnameinfo(const struct sockaddr *addr, socklen_t addrlen, char *host, socklen_t hostlen, char *serv,
                socklen_t servlen, int flags) {
  (void) addrlen;
  (void) flags;
  const void *src;
  unsigned port;
  if (addr->sa_family == AF_INET) {
    src = &((const struct sockaddr_in *) addr)->sin_addr;
    port = ntohs(((const struct sockaddr_in *) addr)->sin_port);
#if LWIP_IPV6
  } else if (addr->sa_family == AF_INET6) {
    src = &((const struct sockaddr_in6 *) addr)->sin6_addr;
    port = ntohs(((const struct sockaddr_in6 *) addr)->sin6_port);
#endif
  } else {
    return -1;
  }
  if (host != NULL && hostlen > 0 && inet_ntop(addr->sa_family, src, host, hostlen) == NULL)
    return -1;
  if (serv != NULL && servlen > 0)
    snprintf(serv, servlen, "%u", port);
  return 0;
}

// write_sigrok.c launches sigrok-cli/pulseview; there are no processes to launch
int execvp(const char *file, char *const argv[]) {
  (void) file;
  (void) argv;
  errno = ENOSYS;
  return -1;
}
