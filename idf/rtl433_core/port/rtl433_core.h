// Entry points the ESPHome component uses
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

int rtl_433_main(int argc, char **argv);
extern volatile int rtl433_port_exit_code;

#ifdef __cplusplus
}
#endif
