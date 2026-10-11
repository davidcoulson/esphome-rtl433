#include "rtl_433_component.h"


#include <cstring>
#include <ctime>

#include <esp_heap_caps.h>
#include <esp_pthread.h>
#include <esp_timer.h>

#include "esphome/components/network/util.h"
#include "esphome/core/log.h"

#include "rtl433_core.h"

namespace esphome::rtl_433 {

static const char *const TAG = "rtl_433";
// rtl_433's decode chain (Mongoose poll -> pulse slicer -> decoder -> output formatting) peaks near 50 KB
static constexpr uint32_t TASK_STACK = 65536;

// rtl_433 levels: 1 fatal, 2 critical, 3 error, 4 warning, 5 notice, 6 info, 7 debug, 8 trace
static void log_sink(int level, char const *src, char const *msg) {
  // -Y autolevel reports every noise-floor change as a warning: routine, and with squelch judging
  // 8 ms pieces it can fire many times a second. Demote it, and let one through every 10 s at most:
  // the UART at 115200 baud is a shared, blocking resource and a flood of these stalls the ESPHome loop.
  if (level == 4 && src != nullptr && strcmp(src, "Auto Level") == 0) {
    static uint32_t last_auto_level_ms = 0;
    uint32_t now = millis();
    if (last_auto_level_ms != 0 && now - last_auto_level_ms < 10000)
      return;
    last_auto_level_ms = now;
    level = 6;
  }
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

bool Rtl433Component::clock_ready_() {
  // Anything before 2024 is the RTC's power-on epoch, not a synced clock
  return ::time(nullptr) > 1704067200;
}

void Rtl433Component::loop() {
  // rtl_433 opens its HTTP server at start-up, which needs lwIP running
  if (this->started_ || !network::is_connected())
    return;
  // Every event carries a wall-clock timestamp that the Home Assistant integration checks, so wait for
  // the clock (SNTP / homeassistant time) before starting, but not forever
  uint32_t now = millis();
  if (this->network_up_ms_ == 0)
    this->network_up_ms_ = now;
  if (!this->clock_ready_() && now - this->network_up_ms_ < this->time_sync_timeout_ms_) {
    if (!this->waiting_logged_) {
      ESP_LOGI(TAG, "Waiting up to %us for the clock before starting rtl_433",
               static_cast<unsigned>(this->time_sync_timeout_ms_ / 1000));
      this->waiting_logged_ = true;
    }
    return;
  }
  if (!this->clock_ready_())
    ESP_LOGW(TAG, "Clock still not set after %us: starting anyway, early events will have wrong timestamps",
             static_cast<unsigned>(this->time_sync_timeout_ms_ / 1000));
  this->started_ = true;
  rtl433_port_http_read_only = this->remote_control_ ? 0 : 1;
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
  rtl433_port_set_main_task();
  rtl433_port_set_log_sink(log_sink);
  // rtl_433's acquire thread (created by this task) mostly waits on USB reads: put it with the USB
  // host stack and the ESPHome loop, leaving the decoding core to the decoders
  esp_pthread_cfg_t pcfg = esp_pthread_get_default_config();
  // Measured peak ~1.6 KB (acquire_stack_free sensor); 8 KB leaves room for its error/log paths
  pcfg.stack_size = 8192;
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
  rtl433_port_exit_code = rc;
  vTaskDelete(nullptr);
}

void Rtl433Component::update() {
  // rtl_433 ending means nothing is decoding; say so. Every way out (an exit(), or main returning in
  // task_entry) sets the exit code before the task deletes itself, so the handle is never needed, and
  // querying it after the task is gone would be a use-after-free.
  if (this->task_ != nullptr && !this->stopped_ && rtl433_port_exit_code >= 0) {
    this->stopped_ = true;
    this->status_set_error(LOG_STR("rtl_433 stopped"));
    ESP_LOGE(TAG, "rtl_433 is no longer running (exit code %d); reboot to recover", rtl433_port_exit_code);
  }
  rtl433_usb_stats stats{};
  bool streaming = false;
  if (!this->stopped_ && rtl433_port_usb_stats(&stats)) {
    streaming = stats.effective_sps > 0;
    // The driver's counters restart with every stream start (each rate-change hop): accumulate the deltas
    this->overruns_total_ += stats.overruns >= this->last_overruns_ ? stats.overruns - this->last_overruns_ : stats.overruns;
    // consumer_drops is bytes the driver dropped from its ring because rtl_433 read too slowly (plus 1 for each
    // USB transfer it had no slot for, which is also an overrun and next to nothing beside a transfer's bytes)
    this->drop_bytes_total_ += stats.consumer_drops >= this->last_drops_ ? stats.consumer_drops - this->last_drops_ : stats.consumer_drops;
    this->last_overruns_ = stats.overruns;
    this->last_drops_ = stats.consumer_drops;
    if (this->effective_sample_rate_sensor_ != nullptr)
      this->effective_sample_rate_sensor_->publish_state(stats.effective_sps);
    if (this->usb_overruns_sensor_ != nullptr)
      this->usb_overruns_sensor_->publish_state(this->overruns_total_);
    if (this->dropped_samples_sensor_ != nullptr)
      this->dropped_samples_sensor_->publish_state(static_cast<float>(this->drop_bytes_total_ / 2));  // CU8: 2 B/sample
    ESP_LOGD(TAG, "USB: %u S/s at %u S/s set, %.1f MHz, overruns %u, dropped %u B", static_cast<unsigned>(stats.effective_sps),
             static_cast<unsigned>(stats.sample_rate), stats.frequency / 1e6, static_cast<unsigned>(stats.overruns),
             static_cast<unsigned>(stats.consumer_drops));
  }
  if (!this->stopped_)
    this->check_liveness_(streaming);
  if (this->decoded_events_sensor_ != nullptr)
    this->decoded_events_sensor_->publish_state(rtl433_port_events & 0xFFFFFF);  // exact in a float
  this->publish_health_();
  this->log_task_stats_();
  this->publish_cpu_load_();
}

void Rtl433Component::check_liveness_(bool streaming) {
  // rtl_433's main loop passes at least twice a second, and while the USB stream delivers samples the
  // demodulator takes frames. Either stopping for this long means it is hung, not just busy.
  static constexpr uint32_t STALL_MS = 30000;
  uint32_t now = millis();
  uint32_t loops = rtl433_port_main_loops, frames = rtl433_port_frames;
  if (loops != this->last_loops_ || loops == 0) {  // 0: the loop has not started (start-up, dongle wait)
    this->last_loops_ = loops;
    this->last_loops_ms_ = now;
  }
  if (frames != this->last_frames_ || !streaming) {  // no stream (no dongle, restarting): nothing to take
    this->last_frames_ = frames;
    this->last_frames_ms_ = now;
  }
  bool loop_stalled = now - this->last_loops_ms_ > STALL_MS;
  bool frames_stalled = now - this->last_frames_ms_ > STALL_MS;
  if ((loop_stalled || frames_stalled) && !this->stalled_) {
    this->stalled_ = true;
    if (loop_stalled) {
      this->status_set_error(LOG_STR("rtl_433 main loop stalled"));
      ESP_LOGE(TAG, "rtl_433's main loop has not run for %us", static_cast<unsigned>((now - this->last_loops_ms_) / 1000));
    } else {
      this->status_set_error(LOG_STR("rtl_433 takes no samples"));
      ESP_LOGE(TAG, "The USB stream runs but rtl_433 has taken no samples for %us",
               static_cast<unsigned>((now - this->last_frames_ms_) / 1000));
    }
  } else if (!loop_stalled && !frames_stalled && this->stalled_) {
    this->stalled_ = false;
    this->status_clear_error();
    ESP_LOGI(TAG, "rtl_433 is running again");
  }
}

void Rtl433Component::publish_health_() {
  // ESP-IDF's high-water marks are in bytes: the least free stack each task has ever had. Both tasks
  // report their own mark: the acquire thread is recreated on every input restart (dongle replug) and the
  // decoding task is gone once rtl_433 exits, and querying a handle that has been freed is a load fault
  // (seen on a Blog V3 replug).
  if (this->decode_stack_free_sensor_ != nullptr && rtl433_port_decode_stack_free != 0 && !this->stopped_)
    this->decode_stack_free_sensor_->publish_state(rtl433_port_decode_stack_free);
  if (this->acquire_stack_free_sensor_ != nullptr && rtl433_port_acquire_stack_free != 0 && !this->stopped_)
    this->acquire_stack_free_sensor_->publish_state(rtl433_port_acquire_stack_free);
  if (this->heap_free_sensor_ != nullptr)
    this->heap_free_sensor_->publish_state(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  if (this->psram_free_sensor_ != nullptr)
    this->psram_free_sensor_->publish_state(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void Rtl433Component::log_task_stats_() {
#if defined(USE_RTL433_TASK_STATS) && configUSE_TRACE_FACILITY && configGENERATE_RUN_TIME_STATS
  // Share of wall time per task since the previous update, for the tasks that used more than 1%
  static std::vector<std::pair<TaskHandle_t, uint32_t>> last;
  static int64_t last_us = 0;
  UBaseType_t n = uxTaskGetNumberOfTasks();
  std::vector<TaskStatus_t> st(n + 4);
  n = uxTaskGetSystemState(st.data(), st.size(), nullptr);
  // The snapshot's pcTaskName points into each task's TCB, which a task deleted from now on frees (an
  // acquire thread is joined and deleted on every replug). Copy the names right away, and take the core
  // from the snapshot itself: never touch a snapshot handle except as a key.
  struct Row {
    TaskHandle_t handle;
    uint32_t runtime;
    unsigned prio;
    int core;
    char name[configMAX_TASK_NAME_LEN];
  };
  std::vector<Row> rows(n);
  for (UBaseType_t i = 0; i < n; i++) {
    rows[i].handle = st[i].xHandle;
    rows[i].runtime = st[i].ulRunTimeCounter;
    rows[i].prio = st[i].uxCurrentPriority;
#if configTASKLIST_INCLUDE_COREID
    rows[i].core = static_cast<int>(st[i].xCoreID);
#else
    rows[i].core = -2;  // not in the snapshot
#endif
    strncpy(rows[i].name, st[i].pcTaskName != nullptr ? st[i].pcTaskName : "?", sizeof(rows[i].name) - 1);
    rows[i].name[sizeof(rows[i].name) - 1] = '\0';
  }
  int64_t now = esp_timer_get_time();
  int64_t wall = now - last_us;
  std::vector<std::pair<TaskHandle_t, uint32_t>> cur;
  for (auto &row : rows) {
    cur.emplace_back(row.handle, row.runtime);
    if (last_us == 0 || wall <= 0)
      continue;
    uint32_t prev = 0;
    for (auto &p : last)
      if (p.first == row.handle)
        prev = p.second;
    float pct = 100.0f * static_cast<float>(row.runtime - prev) / static_cast<float>(wall);
    if (pct >= 1.0f)
      ESP_LOGD(TAG, "task %-16s core %2d prio %2u  %5.1f%% of one core", row.name, row.core, row.prio, pct);
  }
  last = std::move(cur);
  last_us = now;
#endif
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
  ESP_LOGCONFIG(TAG, "  Command line: %s", this->command_line_.c_str());
  ESP_LOGCONFIG(TAG, "  Remote control via /cmd: %s", this->remote_control_ ? "enabled" : "disabled (read-only)");
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
  LOG_SENSOR("  ", "Decode Stack Free", this->decode_stack_free_sensor_);
  LOG_SENSOR("  ", "Acquire Stack Free", this->acquire_stack_free_sensor_);
  LOG_SENSOR("  ", "Heap Free", this->heap_free_sensor_);
  LOG_SENSOR("  ", "PSRAM Free", this->psram_free_sensor_);
  for (auto &band : this->band_decoders_)
    ESP_LOGCONFIG(TAG, "  Band %d decoders: %s", band.first, band.second.c_str());
}

}  // namespace esphome::rtl_433
