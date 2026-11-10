#pragma once

#include "include/Esp32HardwarePwm.h"
#include <deque>
#include <vector>
#include <freertos/semphr.h>

class FadeWorker
{
private:
	friend class Esp32HardwarePwm;

	struct Signal {
		bool pending{false};
		uint32_t duty{0};
		uint32_t index{0};
	};

	struct SignalSnapshot {
		bool pending;
		uint32_t duty;
		uint32_t index;
	};

	enum : uint8_t { Complete, Wrap, Full, Degraded };

	struct Notification {
		uint32_t instance;
		uint32_t generation;
		uint8_t channel;
		uint8_t kind;
	};

	static void lock();
	static void unlock();
	struct Guard {
		Guard() { lock(); }
		~Guard() { unlock(); }
	};
	static Signal& signal(int speedMode, uint8_t channel);
	static void resetSignal(int speedMode, uint8_t channel, uint32_t index);
	static void clearSignal(int speedMode, uint8_t channel);
	static SignalSnapshot takeSignal(int speedMode, uint8_t channel);
	static bool hasPendingSignal(int speedMode, uint8_t channel);
	static TaskHandle_t taskHandle();
	static bool ensure();
	static esp_err_t createTimer();
	static uint32_t allocateInstanceId();
	static void registerInstance(Esp32HardwarePwm* instance);
	static void unregisterInstance(Esp32HardwarePwm* instance);
	static bool ownsChannel(ledc_mode_t speedMode, ledc_channel_t channel);
	static bool timerInUse(const Esp32HardwarePwm* except, ledc_mode_t speedMode, ledc_timer_t timer);
	static bool hasNotifications(uint32_t instance);
	static void clearNotifications(uint32_t instance, int channel = -1);
	static size_t instanceCount();
	static size_t notificationCount();
	static void wake();
	static void notify(Esp32HardwarePwm* instance, uint8_t channel, uint8_t kind);
	static void service();
	static void dispatch(uint32_t param);
	static void workerTask(void*);
	static void wakeTimer(void*);

	static SemaphoreHandle_t mutex_;
	static std::vector<Esp32HardwarePwm*> instances_;
	static uint32_t nextInstanceId_;
	static std::deque<Notification> notifications_;
	static bool dispatchQueued_;
	static portMUX_TYPE signalMux_;
	static Signal signals_[2][SOC_LEDC_CHANNEL_NUM];
	static TaskHandle_t task_;
	static esp_timer_handle_t timer_;
};
