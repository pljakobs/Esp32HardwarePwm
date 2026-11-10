#pragma once
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#define IRAM_ATTR
#define DRAM_ATTR
#define SOC_LEDC_CHANNEL_NUM 8
#ifndef SOC_LEDC_SUPPORT_FADE_STOP
#define SOC_LEDC_SUPPORT_FADE_STOP 1
#endif
#define PERIPH_LEDC_MODULE 0
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_TIMER_TASK 0
#define LEDC_INTR_DISABLE 0
#define LEDC_AUTO_CLK 0
#define LEDC_FADE_NO_WAIT 0
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define tskIDLE_PRIORITY 0
#define pdMS_TO_TICKS(x) TickType_t(x)
#define debug_e(...) ((void)0)
#define debug_w(...) ((void)0)
#define debug_i(...) ((void)0)
#define debug_d(...) ((void)0)
using TickType_t = uint32_t;
using BaseType_t = int;
using TaskHandle_t = void*;
using SemaphoreHandle_t = std::recursive_mutex*;
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
using esp_timer_handle_t = void*;
using esp_err_t = int;
using ledc_mode_t = int;
using ledc_channel_t = int;
using ledc_timer_t = int;
using ledc_clk_cfg_t = int;
enum ledc_timer_bit_t { LEDC_TIMER_10_BIT = 10, LEDC_TIMER_12_BIT = 12 };
constexpr int LEDC_LOW_SPEED_MODE = 0, LEDC_CHANNEL_0 = 0, LEDC_TIMER_0 = 0;
constexpr int LEDC_SPEED_MODE_MAX = 2;
template <typename T> using Delegate = std::function<T>;

namespace Mock {
inline thread_local int locks = 0;
inline thread_local int criticalDepth = 0;
inline thread_local int isrCriticalDepth = 0;
inline std::atomic<int64_t> now{100};
inline std::atomic<int> isrNotifications{0};
inline std::atomic<bool> lastIsrWoken{false};
inline bool rejectPosts = false;
inline int tasks = 0;
inline int timers = 0;
inline int timerCreateError = ESP_OK;
inline int starts = 0;
inline int stops = 0;
inline int deconfigures = 0;
inline int startError = 0;
inline uint32_t duty[2][8]{};
inline uint32_t target[2][8]{};
inline uint32_t frequency[2][4]{};
inline std::deque<std::pair<void (*)(uint32_t), uint32_t>> callbacks;
}
struct ledc_cb_param_t { int speed_mode; int channel; uint32_t duty; };
using ledc_cb_t = bool (*)(const ledc_cb_param_t*, void*);
struct ledc_cbs_t { ledc_cb_t fade_cb; };
namespace Mock {
inline ledc_cb_t irq[2][8]{};
inline void* arg[2][8]{};
inline bool complete(int mode, int channel) {
	duty[mode][channel] = target[mode][channel];
	ledc_cb_param_t param{mode, channel, duty[mode][channel]};
	assert(locks == 0);
	return irq[mode][channel] && irq[mode][channel](&param, arg[mode][channel]);
}
inline void dispatch() {
	assert(!callbacks.empty());
	auto cb = callbacks.front();
	callbacks.pop_front();
	cb.first(cb.second);
}
}
struct ledc_timer_config_t {
	int speed_mode = 0;
	ledc_timer_bit_t duty_resolution = LEDC_TIMER_10_BIT;
	int timer_num = 0;
	uint32_t freq_hz = 0;
	int clk_cfg = 0;
	bool deconfigure = false;
};
struct ledc_channel_config_t {
	int gpio_num, speed_mode, channel, intr_type, timer_sel;
	uint32_t duty;
	int hpoint;
};
struct esp_timer_create_args_t {
	void (*callback)(void*) = nullptr;
	void* arg = nullptr;
	int dispatch_method = 0;
	const char* name = nullptr;
};
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return new std::recursive_mutex; }
inline void xSemaphoreTakeRecursive(SemaphoreHandle_t m, uint32_t) { m->lock(); ++Mock::locks; }
inline void xSemaphoreGiveRecursive(SemaphoreHandle_t m) { --Mock::locks; m->unlock(); }
inline void mockEnterCritical(portMUX_TYPE* mux, bool fromIsr) {
	mux->lock();
	++Mock::criticalDepth;
	if(fromIsr) ++Mock::isrCriticalDepth;
}
inline void mockExitCritical(portMUX_TYPE* mux, bool fromIsr) {
	if(fromIsr) --Mock::isrCriticalDepth;
	--Mock::criticalDepth;
	mux->unlock();
}
#define portENTER_CRITICAL(mux) mockEnterCritical((mux), false)
#define portEXIT_CRITICAL(mux) mockExitCritical((mux), false)
#define portENTER_CRITICAL_SAFE(mux) mockEnterCritical((mux), false)
#define portEXIT_CRITICAL_SAFE(mux) mockExitCritical((mux), false)
#define portENTER_CRITICAL_ISR(mux) mockEnterCritical((mux), true)
#define portEXIT_CRITICAL_ISR(mux) mockExitCritical((mux), true)
inline int xTaskCreate(void (*)(void*), const char*, int, void*, int, TaskHandle_t* task) {
	++Mock::tasks; *task = reinterpret_cast<void*>(1); return pdPASS;
}
inline void xTaskNotifyGive(TaskHandle_t) {}
inline void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t* woken) {
	assert(Mock::locks == 0 && Mock::criticalDepth == 0 && Mock::isrCriticalDepth == 0);
	assert(task == reinterpret_cast<void*>(1));
	Mock::isrNotifications.fetch_add(1);
	*woken = pdTRUE;
	Mock::lastIsrWoken = true;
}
inline uint32_t ulTaskNotifyTake(int, uint32_t) { return 0; }
inline int64_t esp_timer_get_time() { return Mock::now.load(); }
inline int esp_timer_create(const esp_timer_create_args_t*, esp_timer_handle_t* timer) {
	if(Mock::timerCreateError != ESP_OK) return Mock::timerCreateError;
	++Mock::timers; *timer = reinterpret_cast<void*>(1); return ESP_OK;
}
inline int esp_timer_stop(esp_timer_handle_t) { return ESP_OK; }
inline int esp_timer_delete(esp_timer_handle_t) { return ESP_OK; }
inline int esp_timer_start_once(esp_timer_handle_t, uint64_t) { return ESP_OK; }
inline uint32_t esp_random() { return 0; }
inline void periph_module_enable(int) {}
inline const char* esp_err_to_name(int) { return "mock"; }
inline int ledc_fade_func_install(int) { return ESP_OK; }
inline int ledc_timer_config(const ledc_timer_config_t* c) {
	if(!c->deconfigure) Mock::frequency[c->speed_mode][c->timer_num] = c->freq_hz;
	else ++Mock::deconfigures;
	return ESP_OK;
}
inline int ledc_channel_config(const ledc_channel_config_t* c) {
	Mock::duty[c->speed_mode][c->channel] = c->duty;
	Mock::target[c->speed_mode][c->channel] = c->duty;
	return ESP_OK;
}
inline int ledc_cb_register(int mode, int ch, ledc_cbs_t* cb, void* arg) {
	Mock::irq[mode][ch] = cb->fade_cb; Mock::arg[mode][ch] = arg; return ESP_OK;
}
inline int ledc_timer_pause(int, int) { return ESP_OK; }
inline int ledc_stop(int, int, bool) { ++Mock::stops; return ESP_OK; }
inline int ledc_set_freq(int mode, int timer, uint32_t freq) {
	Mock::frequency[mode][timer] = freq; return ESP_OK;
}
inline uint32_t ledc_get_freq(int mode, int timer) { return Mock::frequency[mode][timer]; }
inline uint32_t ledc_get_duty(int mode, int ch) { return Mock::duty[mode][ch]; }
inline int ledc_set_duty(int mode, int ch, uint32_t duty) { Mock::duty[mode][ch] = duty; return ESP_OK; }
inline int ledc_update_duty(int, int) { return ESP_OK; }
inline int ledc_set_duty_with_hpoint(int mode, int ch, uint32_t duty, int) {
	return ledc_set_duty(mode, ch, duty);
}
inline int ledc_fade_stop(int, int) { return ESP_OK; }
inline int ledc_set_fade_step_and_start(int mode, int ch, uint32_t duty, int, uint32_t, int) {
	++Mock::starts; Mock::target[mode][ch] = duty; return Mock::startError;
}
inline int ledc_set_fade_time_and_start(int mode, int ch, uint32_t duty, uint32_t, int) {
	return ledc_set_fade_step_and_start(mode, ch, duty, 0, 0, 0);
}
struct MockSystem {
	bool queueCallback(void (*cb)(uint32_t), uint32_t arg) {
		if(Mock::rejectPosts) return false;
		Mock::callbacks.emplace_back(cb, arg); return true;
	}
};
inline MockSystem System;
