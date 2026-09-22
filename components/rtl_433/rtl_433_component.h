#pragma once

#include <string>
#include <utility>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esphome/core/component.h"

namespace esphome::rtl_433 {

class Rtl433Component : public Component {
 public:
  void add_arg(const char *arg) { this->args_.emplace_back(arg); }
  void set_task_core(int core) { this->task_core_ = core; }
  void add_band_decoders(int band, const char *list) { this->band_decoders_.emplace_back(band, list); }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

 protected:
  static void task_entry(void *arg);

  std::vector<std::string> args_;
  std::vector<std::pair<int, std::string>> band_decoders_;
  TaskHandle_t task_{nullptr};
  int task_core_{-1};  // -1: the core the ESPHome loop isn't on
  int acquire_core_{0};
  bool started_{false};
};

}  // namespace esphome::rtl_433
