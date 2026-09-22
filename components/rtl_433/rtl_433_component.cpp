#include "rtl_433_component.h"


#include <esp_pthread.h>

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
  // Demodulation and decoding run in rtl_433's main task: keep it off the core the ESPHome loop uses
  BaseType_t loop_core = xPortGetCoreID();
  if (this->task_core_ < 0)
    this->task_core_ = loop_core == 0 ? 1 : 0;
  this->acquire_core_ = loop_core;
  if (xTaskCreatePinnedToCore(Rtl433Component::task_entry, "rtl_433", TASK_STACK, this, 5, &this->task_,
                              this->task_core_) != pdPASS) {
    ESP_LOGE(TAG, "Could not start the rtl_433 task");
    this->mark_failed();
  }
}

void Rtl433Component::task_entry(void *arg) {
  auto *self = static_cast<Rtl433Component *>(arg);
  rtl433_port_set_log_sink(log_sink);
  // rtl_433's acquire thread (created by this task) mostly waits on USB reads: put it with the USB
  // host stack and the ESPHome loop, leaving the decoding core to the decoders
  esp_pthread_cfg_t pcfg = esp_pthread_get_default_config();
  pcfg.stack_size = 16384;
  pcfg.pin_to_core = self->acquire_core_;
  pcfg.thread_name = "rtl_433_acq";
  esp_pthread_set_cfg(&pcfg);
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
  ESP_LOGCONFIG(TAG, "  Decoding core: %d, acquire core: %d", this->task_core_, this->acquire_core_);
  for (auto &band : this->band_decoders_)
    ESP_LOGCONFIG(TAG, "  Band %d decoders: %s", band.first, band.second.c_str());
}

}  // namespace esphome::rtl_433
