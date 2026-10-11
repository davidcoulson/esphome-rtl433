#include <errno.h>
#include <pthread.h>
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
#include "rtl_433.h"
#include "r_private.h"

static const char *const TAG = "rtl_433";

volatile int rtl433_port_exit_code = -1;

size_t rtl433_port_usb_ring_bytes = 0;
uint8_t rtl433_port_usb_task_priority = 0;
uint8_t rtl433_port_usb_task_core = 0xFF;

volatile uint32_t rtl433_port_events = 0;
volatile int rtl433_port_http_read_only = 0;

static TaskHandle_t main_task = NULL;
void rtl433_port_set_main_task(void) { main_task = xTaskGetCurrentTaskHandle(); }
int rtl433_port_on_main_task(void) { return main_task == NULL || xTaskGetCurrentTaskHandle() == main_task; }
void rtl433_port_count_event(void) { rtl433_port_events++; }

static TaskHandle_t acquire_task = NULL;
volatile unsigned rtl433_port_acquire_stack_free = 0;
void rtl433_port_note_acquire_task(void) { acquire_task = xTaskGetCurrentTaskHandle(); }
int rtl433_port_on_acquire_task(void) { return acquire_task != NULL && xTaskGetCurrentTaskHandle() == acquire_task; }
void *rtl433_port_acquire_task_handle(void) { return acquire_task; }

volatile unsigned rtl433_port_decode_stack_free = 0;
volatile uint32_t rtl433_port_main_loops = 0;
volatile uint32_t rtl433_port_frames = 0;

// From rtl_433's main loop, at least every 500 ms (mg_mgr_poll's timeout)
void rtl433_port_main_tick(struct r_cfg *cfg) {
  rtl433_port_main_loops++;
  if (cfg->demod != NULL)
    rtl433_port_frames = cfg->demod->total_frames_count;
  rtl433_port_bands_poll(cfg);
  static TickType_t last_hwm = 0;
  TickType_t now = xTaskGetTickCount();
  if (last_hwm == 0 || now - last_hwm > pdMS_TO_TICKS(1000)) {
    last_hwm = now;
    rtl433_port_decode_stack_free = (unsigned) uxTaskGetStackHighWaterMark(NULL);
  }
}

static rtl433_port_log_sink_t log_sink = NULL;

void rtl433_port_set_log_sink(rtl433_port_log_sink_t sink) { log_sink = sink; }

void rtl433_port_log(int level, char const *src, char const *msg) {
  if (log_sink != NULL)
    log_sink(level, src, msg);
  else
    ESP_LOGI(TAG, "%s: %s", src, msg);
}

static void (*exit_hook)(void) = NULL;
void rtl433_port_set_exit_hook(void (*hook)(void)) { exit_hook = hook; }

void rtl433_port_exit(int code) {
  rtl433_port_exit_code = code;
  ESP_LOGE(TAG, "rtl_433 exited with code %d", code);
  fflush(stdout);
  fflush(stderr);
  if (!rtl433_port_on_main_task()) {
    // From the acquire pthread: leave it joinable, otherwise sdr_stop()'s pthread_join() hangs the main task
    pthread_exit(NULL);
  }
  static int exiting = 0;
  if (exit_hook != NULL && !exiting) {
    exiting = 1;  // an exit() from inside the hook just ends the task
    exit_hook();
  }
  vTaskDelete(NULL);
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
