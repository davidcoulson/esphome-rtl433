#include "rtl_433_component.h"


#include <esp_pthread.h>
#include <esp_timer.h>

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
  // The driver's USB task goes with the acquire thread, which consumes what it delivers
  rtl433_port_usb_ring_bytes = this->usb_buffer_;
  rtl433_port_usb_task_priority = static_cast<uint8_t>(this->usb_task_priority_);
  rtl433_port_usb_task_core = static_cast<uint8_t>(this->acquire_core_);
  if (xTaskCreatePinnedToCore(Rtl433Component::task_entry, "rtl_433", TASK_STACK, this, this->task_priority_,
                              &this->task_, this->task_core_) != pdPASS) {
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
  pcfg.prio = self->acquire_priority_;
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

void Rtl433Component::update() {
  rtl433_usb_stats stats{};
  if (rtl433_port_usb_stats(&stats)) {
    if (this->effective_sample_rate_sensor_ != nullptr)
      this->effective_sample_rate_sensor_->publish_state(stats.effective_sps);
    if (this->usb_overruns_sensor_ != nullptr)
      this->usb_overruns_sensor_->publish_state(stats.overruns);
    if (this->dropped_samples_sensor_ != nullptr)
      this->dropped_samples_sensor_->publish_state(stats.consumer_drops);
    ESP_LOGD(TAG, "USB: %u S/s at %u S/s set, %.1f MHz, overruns %u, dropped %u", static_cast<unsigned>(stats.effective_sps),
             static_cast<unsigned>(stats.sample_rate), stats.frequency / 1e6, static_cast<unsigned>(stats.overruns),
             static_cast<unsigned>(stats.consumer_drops));
  }
  if (this->decoded_events_sensor_ != nullptr)
    this->decoded_events_sensor_->publish_state(rtl433_port_events & 0xFFFFFF);  // exact in a float
  this->publish_cpu_load_();
}

void Rtl433Component::publish_cpu_load_() {
#if defined(USE_RTL433_CPU_LOAD) && configGENERATE_RUN_TIME_STATS
  // Run-time counters tick in microseconds; load = 1 - (idle task time / wall time) per core
  int64_t now = esp_timer_get_time();
  int64_t elapsed = now - this->last_cpu_time_us_;
  for (int core = 0; core < 2; core++) {
    uint32_t idle_now = static_cast<uint32_t>(ulTaskGetIdleRunTimeCounterForCore(core));
    uint32_t idle_delta = idle_now - this->last_idle_[core];
    this->last_idle_[core] = idle_now;
    if (this->last_cpu_time_us_ != 0 && elapsed > 0 && this->cpu_load_sensor_[core] != nullptr) {
      float load = 100.0f * (1.0f - static_cast<float>(idle_delta) / static_cast<float>(elapsed));
      this->cpu_load_sensor_[core]->publish_state(load < 0 ? 0 : (load > 100 ? 100 : load));
    }
  }
  this->last_cpu_time_us_ = now;
#endif
}

void Rtl433Component::dump_config() {
  ESP_LOGCONFIG(TAG, "rtl_433:");
  std::string line;
  for (auto &a : this->args_)
    line += a + " ";
  ESP_LOGCONFIG(TAG, "  Command line: %s", line.c_str());
  ESP_LOGCONFIG(TAG,
                "  Decoding task: core %d, priority %d\n"
                "  Acquire thread: core %d, priority %d\n"
                "  USB task priority: %d (0 = driver default)\n"
                "  USB buffer: %u bytes (0 = driver default)",
                this->task_core_, this->task_priority_, this->acquire_core_, this->acquire_priority_,
                this->usb_task_priority_, static_cast<unsigned>(this->usb_buffer_));
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "Effective Sample Rate", this->effective_sample_rate_sensor_);
  LOG_SENSOR("  ", "USB Overruns", this->usb_overruns_sensor_);
  LOG_SENSOR("  ", "Dropped Samples", this->dropped_samples_sensor_);
  LOG_SENSOR("  ", "Decoded Events", this->decoded_events_sensor_);
  LOG_SENSOR("  ", "CPU Load Core 0", this->cpu_load_sensor_[0]);
  LOG_SENSOR("  ", "CPU Load Core 1", this->cpu_load_sensor_[1]);
  for (auto &band : this->band_decoders_)
    ESP_LOGCONFIG(TAG, "  Band %d decoders: %s", band.first, band.second.c_str());
}

}  // namespace esphome::rtl_433
