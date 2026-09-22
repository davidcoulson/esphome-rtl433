#include "rtl_433_component.h"

#include "esphome/components/network/util.h"
#include "esphome/core/log.h"

#include "rtl433_core.h"

namespace esphome::rtl_433 {

static const char *const TAG = "rtl_433";
static constexpr uint32_t TASK_STACK = 32768;

void Rtl433Component::setup() {}

void Rtl433Component::loop() {
  // rtl_433 opens its HTTP server at start-up, which needs lwIP running
  if (this->started_ || !network::is_connected())
    return;
  this->started_ = true;
  if (xTaskCreate(Rtl433Component::task_entry, "rtl_433", TASK_STACK, this, 5, &this->task_) != pdPASS) {
    ESP_LOGE(TAG, "Could not start the rtl_433 task");
    this->mark_failed();
  }
}

void Rtl433Component::task_entry(void *arg) {
  auto *self = static_cast<Rtl433Component *>(arg);
  std::vector<char *> argv;
  for (auto &a : self->args_)
    argv.push_back(a.data());
  argv.push_back(nullptr);
  int rc = rtl_433_main(static_cast<int>(argv.size()) - 1, argv.data());
  ESP_LOGE(TAG, "rtl_433 returned %d", rc);
  vTaskDelete(nullptr);
}

void Rtl433Component::dump_config() {
  ESP_LOGCONFIG(TAG, "rtl_433:");
  std::string line;
  for (auto &a : this->args_)
    line += a + " ";
  ESP_LOGCONFIG(TAG, "  Command line: %s", line.c_str());
}

}  // namespace esphome::rtl_433
