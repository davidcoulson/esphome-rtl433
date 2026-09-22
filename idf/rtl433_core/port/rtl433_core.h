// Entry points the ESPHome component uses
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

int rtl_433_main(int argc, char **argv);
extern volatile int rtl433_port_exit_code;

typedef void (*rtl433_port_log_sink_t)(int level, char const *src, char const *msg);
void rtl433_port_set_log_sink(rtl433_port_log_sink_t sink);

#ifdef __cplusplus
}
#endif
