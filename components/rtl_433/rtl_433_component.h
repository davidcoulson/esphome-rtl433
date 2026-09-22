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
  void add_arg(const char *arg) { this->args_.emplace_back(arg); }
  void add_band_decoders(int band, const char *list) { this->band_decoders_.emplace_back(band, list); }
  void set_task_core(int core) { this->task_core_ = core; }
  void set_task_priority(int prio) { this->task_priority_ = prio; }
  void set_acquire_priority(int prio) { this->acquire_priority_ = prio; }
  void set_usb_task_priority(int prio) { this->usb_task_priority_ = prio; }
  void set_usb_buffer(uint32_t bytes) { this->usb_buffer_ = bytes; }

  void set_effective_sample_rate_sensor(sensor::Sensor *s) { this->effective_sample_rate_sensor_ = s; }
  void set_usb_overruns_sensor(sensor::Sensor *s) { this->usb_overruns_sensor_ = s; }
  void set_dropped_samples_sensor(sensor::Sensor *s) { this->dropped_samples_sensor_ = s; }
  void set_decoded_events_sensor(sensor::Sensor *s) { this->decoded_events_sensor_ = s; }
  void set_cpu_load_core0_sensor(sensor::Sensor *s) { this->cpu_load_sensor_[0] = s; }
  void set_cpu_load_core1_sensor(sensor::Sensor *s) { this->cpu_load_sensor_[1] = s; }

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

 protected:
  static void task_entry(void *arg);
  void publish_cpu_load_();

  std::vector<std::string> args_;
  std::vector<std::pair<int, std::string>> band_decoders_;
  TaskHandle_t task_{nullptr};
  bool started_{false};
  int task_core_{-1};  // -1: the core the ESPHome loop isn't on
  int acquire_core_{0};
  int task_priority_{5};
  int acquire_priority_{6};
  int usb_task_priority_{0};
  uint32_t usb_buffer_{0};

  sensor::Sensor *effective_sample_rate_sensor_{nullptr};
  sensor::Sensor *usb_overruns_sensor_{nullptr};
  sensor::Sensor *dropped_samples_sensor_{nullptr};
  sensor::Sensor *decoded_events_sensor_{nullptr};
  sensor::Sensor *cpu_load_sensor_[2]{nullptr, nullptr};
  uint32_t last_idle_[2]{0, 0};
  int64_t last_cpu_time_us_{0};
};

}  // namespace esphome::rtl_433
