#include "rtl_433_component.h"


#include "esphome/components/network/util.h"
#include "esphome/core/log.h"

#include "rtl433_core.h"

namespace esphome::rtl_433 {

static const char *const TAG = "rtl_433";
static constexpr uint32_t TASK_STACK = 32768;

// rtl_433 levels: 1 fatal, 2 critical, 3 error, 4 warning, 5 notice, 6 info, 7 debug, 8 trace
static void log_sink(int level, char const *src, char const *msg) {
  if (level <= 3) {
    ESP_LOGE(TAG, "%s: %s", src, msg);
  } else if (level == 4) {
    ESP_LOGW(TAG, "%s: %s", src, msg);
  } else if (level <= 6) {
    ESP_LOGI(TAG, "%s: %s", src, msg);
  } else {
    ESP_LOGD(TAG, "%s: %s", src, msg);
  }
}

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
  rtl433_port_set_log_sink(log_sink);
  for (auto &band : self->band_decoders_)
    rtl433_port_set_band_decoders(band.first, band.second.c_str());
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
  for (auto &band : this->band_decoders_)
    ESP_LOGCONFIG(TAG, "  Band %d decoders: %s", band.first, band.second.c_str());
}

}  // namespace esphome::rtl_433
