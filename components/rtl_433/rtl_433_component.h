#pragma once

#include <string>
#include <utility>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"

namespace esphome::rtl_433 {

class Rtl433Component : public PollingComponent {
 public:
  void add_arg(const char *arg) {
    this->args_.emplace_back(arg);
    // rtl_433 edits its argv in place while parsing (splits "http:host:port" etc.), so keep a copy to show
    this->command_line_ += this->command_line_.empty() ? arg : std::string(" ") + arg;
  }
  void add_band_decoders(int band, const char *list) { this->band_decoders_.emplace_back(band, list); }
  void set_task_core(int core) { this->task_core_ = core; }
  void set_task_priority(int prio) { this->task_priority_ = prio; }
  void set_acquire_priority(int prio) { this->acquire_priority_ = prio; }
  void set_usb_task_priority(int prio) { this->usb_task_priority_ = prio; }
  void set_usb_buffer(uint32_t bytes) { this->usb_buffer_ = bytes; }
  void set_remote_control(bool on) { this->remote_control_ = on; }
  void set_time_sync_timeout(uint32_t ms) { this->time_sync_timeout_ms_ = ms; }

  void set_effective_sample_rate_sensor(sensor::Sensor *s) { this->effective_sample_rate_sensor_ = s; }
  void set_usb_overruns_sensor(sensor::Sensor *s) { this->usb_overruns_sensor_ = s; }
  void set_dropped_samples_sensor(sensor::Sensor *s) { this->dropped_samples_sensor_ = s; }
  void set_decoded_events_sensor(sensor::Sensor *s) { this->decoded_events_sensor_ = s; }
  void set_cpu_load_core0_sensor(sensor::Sensor *s) { this->cpu_load_sensor_[0] = s; }
  void set_cpu_load_core1_sensor(sensor::Sensor *s) { this->cpu_load_sensor_[1] = s; }
  void set_decode_stack_free_sensor(sensor::Sensor *s) { this->decode_stack_free_sensor_ = s; }
  void set_acquire_stack_free_sensor(sensor::Sensor *s) { this->acquire_stack_free_sensor_ = s; }
  void set_heap_free_sensor(sensor::Sensor *s) { this->heap_free_sensor_ = s; }
  void set_psram_free_sensor(sensor::Sensor *s) { this->psram_free_sensor_ = s; }

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

 protected:
  static void task_entry(void *arg);
  void publish_cpu_load_();
  void publish_health_();
  void log_task_stats_();
  bool clock_ready_();

  std::vector<std::string> args_;
  std::string command_line_;
  bool remote_control_{true};
  uint32_t time_sync_timeout_ms_{120000};
  uint32_t network_up_ms_{0};
  bool waiting_logged_{false};
  std::vector<std::pair<int, std::string>> band_decoders_;
  TaskHandle_t task_{nullptr};
  bool started_{false};
  int task_core_{-1};  // -1: the core the ESPHome loop isn't on
  int acquire_core_{0};
  int task_priority_{5};
  int acquire_priority_{6};
  int usb_task_priority_{0};
  uint32_t usb_buffer_{0};
  bool stopped_{false};
  uint32_t last_overruns_{0}, last_drops_{0}, overruns_total_{0}, drops_total_{0};

  sensor::Sensor *effective_sample_rate_sensor_{nullptr};
  sensor::Sensor *usb_overruns_sensor_{nullptr};
  sensor::Sensor *dropped_samples_sensor_{nullptr};
  sensor::Sensor *decoded_events_sensor_{nullptr};
  sensor::Sensor *cpu_load_sensor_[2]{nullptr, nullptr};
  sensor::Sensor *decode_stack_free_sensor_{nullptr};
  sensor::Sensor *acquire_stack_free_sensor_{nullptr};
  sensor::Sensor *heap_free_sensor_{nullptr};
  sensor::Sensor *psram_free_sensor_{nullptr};
  uint32_t last_idle_[2]{0, 0};
  int64_t last_cpu_time_us_{0};
};

}  // namespace esphome::rtl_433
