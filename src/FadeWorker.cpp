#include "FadeWorker.h"
#include <Platform/System.h>
#include <algorithm>
#include <cassert>
#include <esp_attr.h>
#include <freertos/semphr.h>

#ifdef HWPWM_TIMING_TRACE
extern void IRAM_ATTR hwPwmTimingTrace(uint8_t event, uint8_t channel, uint32_t duty, uint32_t duration);
#define PWM_TIMING_TRACE(event, channel, duty, duration) hwPwmTimingTrace(event, channel, duty, duration)
#else
#define PWM_TIMING_TRACE(event, channel, duty, duration) ((void)0)
#endif

SemaphoreHandle_t FadeWorker::mutex_ = xSemaphoreCreateRecursiveMutex();
std::vector<Esp32HardwarePwm*> FadeWorker::instances_;
uint32_t FadeWorker::nextInstanceId_ = 0;
std::deque<FadeWorker::Notification> FadeWorker::notifications_;
bool FadeWorker::dispatchQueued_ = false;
DRAM_ATTR portMUX_TYPE FadeWorker::signalMux_ = portMUX_INITIALIZER_UNLOCKED;
DRAM_ATTR FadeWorker::Signal FadeWorker::signals_[2][SOC_LEDC_CHANNEL_NUM];
DRAM_ATTR TaskHandle_t FadeWorker::task_ = nullptr;
esp_timer_handle_t FadeWorker::timer_ = nullptr;

/**
 * @brief Acquire the worker's recursive task-context mutex.
 *
 * @note Must not be called from an ISR.
 */
void FadeWorker::lock()
{
	assert(mutex_ != nullptr);
	xSemaphoreTakeRecursive(mutex_, portMAX_DELAY);
}

/**
 * @brief Release the worker's recursive task-context mutex.
 *
 * @note Must only be called by a task that holds the mutex.
 */
void FadeWorker::unlock()
{
	xSemaphoreGiveRecursive(mutex_);
}

/**
 * @brief Return the ISR-shared completion signal for an LEDC channel.
 * @param speedMode LEDC speed mode indexing the signal table.
 * @param channel LEDC channel indexing the signal table.
 * @return Reference to the channel's static signal storage.
 */
FadeWorker::Signal& FadeWorker::signal(int speedMode, uint8_t channel)
{
	return signals_[speedMode][channel];
}

/**
 * @brief Clear a channel signal and associate it with a new fade index.
 * @param speedMode LEDC speed mode.
 * @param channel LEDC channel.
 * @param index Index recorded for the next ISR completion.
 *
 * The critical section synchronizes this update with the LEDC ISR callback.
 */
void FadeWorker::resetSignal(int speedMode, uint8_t channel, uint32_t index)
{
	portENTER_CRITICAL_SAFE(&signalMux_);
	auto& current = signal(speedMode, channel);
	current.pending = false;
	current.duty = 0;
	current.index = index;
	portEXIT_CRITICAL_SAFE(&signalMux_);
}

/**
 * @brief Clear the pending flag for a channel signal.
 * @param speedMode LEDC speed mode.
 * @param channel LEDC channel.
 */
void FadeWorker::clearSignal(int speedMode, uint8_t channel)
{
	portENTER_CRITICAL_SAFE(&signalMux_);
	signal(speedMode, channel).pending = false;
	portEXIT_CRITICAL_SAFE(&signalMux_);
}

/**
 * @brief Atomically read and consume a channel completion signal.
 * @param speedMode LEDC speed mode.
 * @param channel LEDC channel.
 * @return Snapshot of the signal, with its pending flag cleared.
 */
FadeWorker::SignalSnapshot FadeWorker::takeSignal(int speedMode, uint8_t channel)
{
	portENTER_CRITICAL_SAFE(&signalMux_);
	auto& current = signal(speedMode, channel);
	SignalSnapshot snapshot{current.pending, current.duty, current.index};
	current.pending = false;
	portEXIT_CRITICAL_SAFE(&signalMux_);
	return snapshot;
}

/**
 * @brief Check whether an ISR completion signal is pending.
 * @param speedMode LEDC speed mode.
 * @param channel LEDC channel.
 * @return @c true if the signal is pending; otherwise @c false.
 */
bool FadeWorker::hasPendingSignal(int speedMode, uint8_t channel)
{
	portENTER_CRITICAL_SAFE(&signalMux_);
	const bool pending = signal(speedMode, channel).pending;
	portEXIT_CRITICAL_SAFE(&signalMux_);
	return pending;
}

/**
 * @brief Read the worker task handle safely against ISR access.
 * @return Current task handle, or @c nullptr before worker creation.
 */
TaskHandle_t FadeWorker::taskHandle()
{
	portENTER_CRITICAL_SAFE(&signalMux_);
	auto task = task_;
	portEXIT_CRITICAL_SAFE(&signalMux_);
	return task;
}

/**
 * @brief Create the one-shot timer used to wake the worker at deadlines.
 * @return ESP-IDF timer creation result.
 */
esp_err_t FadeWorker::createTimer()
{
	esp_timer_create_args_t args = {};
	args.callback = wakeTimer;
	args.dispatch_method = ESP_TIMER_TASK;
	args.name = "pwm-deadline";
	return esp_timer_create(&args, &timer_);
}

/**
 * @brief Lazily create the shared timer and FreeRTOS worker task.
 * @return @c true if the worker exists or was created successfully.
 *
 * Timer initialization may not yet be available during global object
 * construction; in that case, the worker task retries timer creation while
 * servicing.
 */
bool FadeWorker::ensure()
{
	if(taskHandle()) {
		return true;
	}
	const auto result = createTimer();
	// Global PWM objects may precede IDF's esp_timer initialization.
	// The worker retries once startup has made the timer service available.
	if(result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
		return false;
	}
	// No affinity: works with CONFIG_FREERTOS_UNICORE and multi-core targets.
	TaskHandle_t task = nullptr;
	if(xTaskCreate(workerTask, "pwm-worker", 4096, nullptr, tskIDLE_PRIORITY + 2, &task) != pdPASS) {
		if(timer_) {
			esp_timer_delete(timer_);
		}
		timer_ = nullptr;
		return false;
	}
	portENTER_CRITICAL_SAFE(&signalMux_);
	task_ = task;
	portEXIT_CRITICAL_SAFE(&signalMux_);
	return true;
}

/**
 * @brief Allocate a monotonically increasing PWM instance identifier.
 * @return The newly allocated identifier.
 */
uint32_t FadeWorker::allocateInstanceId()
{
	return ++nextInstanceId_;
}

/**
 * @brief Add an initialized PWM instance to the worker registry.
 * @param instance PWM instance to register; the caller holds the worker mutex.
 *
 * Wakes the worker so it can observe the newly registered instance.
 */
void FadeWorker::registerInstance(Esp32HardwarePwm* instance)
{
	instances_.push_back(instance);
	wake();
}

/**
 * @brief Remove a PWM instance and its deferred notifications.
 * @param instance PWM instance to unregister; the caller holds the worker mutex.
 */
void FadeWorker::unregisterInstance(Esp32HardwarePwm* instance)
{
	instances_.erase(std::remove(instances_.begin(), instances_.end(), instance), instances_.end());
	clearNotifications(instance->instanceId_);
}

/**
 * @brief Check whether an LEDC channel is owned by a registered PWM instance.
 * @param speedMode LEDC speed mode.
 * @param channel LEDC channel.
 * @return @c true if the channel is in use; otherwise @c false.
 */
bool FadeWorker::ownsChannel(ledc_mode_t speedMode, ledc_channel_t channel)
{
	for(const auto* instance : instances_) {
		for(const auto& pin : instance->pins_) {
			if(instance->timer_.speed_mode == speedMode && pin.channel == channel) {
				return true;
			}
		}
	}
	return false;
}

/**
 * @brief Check whether another registered instance uses an LEDC timer.
 * @param except Instance excluded from the search.
 * @param speedMode LEDC speed mode.
 * @param timer LEDC timer number.
 * @return @c true if another instance uses the timer; otherwise @c false.
 */
bool FadeWorker::timerInUse(const Esp32HardwarePwm* except, ledc_mode_t speedMode, ledc_timer_t timer)
{
	for(const auto* instance : instances_) {
		if(instance != except && instance->timer_.speed_mode == speedMode && instance->timer_.timer_num == timer) {
			return true;
		}
	}
	return false;
}

/**
 * @brief Check whether deferred notifications remain for an instance.
 * @param instance PWM instance identifier.
 * @return @c true if a notification is queued; otherwise @c false.
 */
bool FadeWorker::hasNotifications(uint32_t instance, int channel)
{
	for(const auto& notification : notifications_) {
		if(notification.instance == instance && (channel < 0 || notification.channel == channel)) {
			return true;
		}
	}
	return false;
}

/**
 * @brief Remove queued notifications matching an instance and optional channel.
 * @param instance PWM instance identifier.
 * @param channel Channel to clear, or a negative value to clear all its channels.
 */
void FadeWorker::clearNotifications(uint32_t instance, int channel)
{
	notifications_.erase(std::remove_if(notifications_.begin(), notifications_.end(),
										[instance, channel](const Notification& n) {
											return n.instance == instance && (channel < 0 || n.channel == channel);
										}),
						 notifications_.end());
}

/**
 * @brief Return the number of registered PWM instances.
 * @return Registered instance count.
 */
size_t FadeWorker::instanceCount()
{
	return instances_.size();
}

/**
 * @brief Return the number of deferred notifications awaiting dispatch.
 * @return Queued notification count.
 */
size_t FadeWorker::notificationCount()
{
	return notifications_.size();
}

/**
 * @brief Wake the worker task if it has been created.
 *
 * Safe for use from task context; the LEDC ISR uses its dedicated FromISR
 * notification path instead.
 */
void FadeWorker::wake()
{
	if(auto task = taskHandle()) {
		xTaskNotifyGive(task);
	}
}

/**
 * @brief Append a deferred callback notification and wake the worker.
 * @param instance PWM instance that owns the callback.
 * @param channel PWM instance channel index.
 * @param kind Notification kind (completion, wrap, or queue error).
 *
 * The caller must hold the worker mutex.
 */
void FadeWorker::notify(Esp32HardwarePwm* instance, uint8_t channel, uint8_t kind)
{
	notifications_.push_back({instance->instanceId_, instance->states_[channel].generation, channel, kind});
	wake();
}

/**
 * @brief Service fade signals and deadlines for all registered instances.
 *
 * Runs on the worker task with the worker mutex held. It advances fade
 * segments, processes spread-spectrum deadlines, arms the next timer deadline,
 * and queues deferred user notifications in Sming's callback queue.
 */
void FadeWorker::service()
{
	Guard guard;
	if(!timer_ && createTimer() != ESP_OK) {
		return;
	}
	int64_t nextDeadline = 0;
	const auto now = esp_timer_get_time();
	for(auto* self : instances_) {
		if(self->nextSpectrumUs_ && self->nextSpectrumUs_ <= now) {
			self->handleSpreadSpectrum();
			self->nextSpectrumUs_ = now + self->spectrumIntervalUs_;
		}
		if(self->nextSpectrumUs_ && (!nextDeadline || self->nextSpectrumUs_ < nextDeadline)) {
			nextDeadline = self->nextSpectrumUs_;
		}
		for(uint8_t i = 0; i < self->pins_.size(); ++i) {
			auto& state = self->states_[i];
			const auto snapshot = takeSignal(self->timer_.speed_mode, self->pins_[i].channel);
			const bool pending = snapshot.pending;
			// A cancelled fade's ISR can arrive after a replacement was armed.
			// Never let it finish a timed hold or a different hardware target.
			bool done = pending && !state.deadlineUs &&
						snapshot.duty == self->pins_[i].targetDuty &&
						ledc_get_duty(self->timer_.speed_mode, self->pins_[i].channel) == self->pins_[i].targetDuty;
			if(state.deadlineUs != 0 && state.deadlineUs <= now) {
				done = true;
			}
			auto& queue = self->fadeQueues_[i];
			if(done && !self->fadeDone_[i] && queue.workerHoldUntilUs > now) {
				state.deadlineUs = queue.workerHoldUntilUs;
				queue.workerHoldUntilUs = 0;
				done = false;
			}
			if(done && !self->fadeDone_[i]) {
				state.deadlineUs = 0;
				self->pins_[i].currentDuty = self->pins_[i].targetDuty;
				self->fadeDone_[i] = true;
				if(queue.workerFadeActive && !queue.activeIsIntermediate) {
					queue.workerFadeActive = false;
				}
				if(state.notifyCompletion) {
					if(queue.activeIsIntermediate) {
						self->startNextFade(i);
					} else if(self->onFadeDone_) {
						state.awaitingDispatch = true;
						notify(self, i, Complete);
					} else {
						const bool more = self->startNextFade(i);
						if(!more && self->onQueueEmpty_) {
							state.awaitingDispatch = true;
							notify(self, i, Complete);
						}
					}
				} else {
					self->startNextFade(i);
				}
			}
			if(state.deadlineUs && (!nextDeadline || state.deadlineUs < nextDeadline)) {
				nextDeadline = state.deadlineUs;
			}
		}
	}
	esp_timer_stop(timer_);
	if(nextDeadline) {
		esp_timer_start_once(timer_, std::max(int64_t(1), nextDeadline - esp_timer_get_time()));
	}
	if(!notifications_.empty() && !dispatchQueued_) {
		dispatchQueued_ = System.queueCallback(dispatch, 0);
	}
}

/**
 * @brief FreeRTOS entry point for the shared PWM service task.
 *
 * Services pending work, then sleeps until notified or briefly retries when
 * timer creation or callback-queue dispatch needs another attempt. The task
 * argument is unused.
 */
void FadeWorker::workerTask(void*)
{
	for(;;) {
		service();
		lock();
		const bool retry = !timer_ || (!notifications_.empty() && !dispatchQueued_);
		unlock();
		ulTaskNotifyTake(pdTRUE, retry ? std::max(TickType_t(1), pdMS_TO_TICKS(1)) : portMAX_DELAY);
	}
}

/**
 * @brief ESP timer callback that wakes the worker task.
 *
 * The timer argument is unused. This callback runs in the ESP timer task
 * context, not in an ISR.
 */
void FadeWorker::wakeTimer(void*)
{
	wake();
}

/**
 * @brief Dispatch one queued notification in Sming callback context.
 *
 * The worker mutex is released while invoking application callbacks. Instance
 * identity and fade generation are checked before dispatch to discard stale
 * notifications safely. The callback parameter is unused.
 */
void FadeWorker::dispatch(uint32_t)
{
	Guard guard;
	dispatchQueued_ = false;
	if(notifications_.empty()) {
		return;
	}
	const auto n = notifications_.front();
	notifications_.pop_front();
	auto findInstance = [&](bool checkGeneration = true) -> Esp32HardwarePwm* {
		for(auto* instance : instances_) {
			if(instance->instanceId_ == n.instance &&
			   (!checkGeneration || instance->states_[n.channel].generation == n.generation)) {
				return instance;
			}
		}
		return nullptr;
	};
	auto* self = findInstance();
	if(self) {
		PWM_TIMING_TRACE(3, n.channel, self->pins_[n.channel].targetDuty, 0);
		auto cb = n.kind == Complete ? self->onFadeDone_ : self->onCyclicWrap_;
		auto errorCb = self->onQueueError_;
		if(n.kind == Complete) {
			self->states_[n.channel].dispatching = true;
		}
		++self->callbacksInFlight_;
		++self->states_[n.channel].callbacksInFlight;
		unlock();
		if(n.kind == Full || n.kind == Degraded) {
			if(errorCb) {
				errorCb(n.channel, n.kind == Full ? Esp32HardwarePwm::QueueError::QUEUE_FULL
												 : Esp32HardwarePwm::QueueError::SPLIT_DEGRADED);
			}
		} else if(cb) {
			cb(n.channel);
		}
		lock();
		self = findInstance(false);
		if(self) {
			--self->callbacksInFlight_;
			--self->states_[n.channel].callbacksInFlight;
		}
		self = findInstance();
		if(self && n.kind == Complete) {
			auto& state = self->states_[n.channel];
			state.dispatching = false;
			state.awaitingDispatch = false;
			bool more = !self->fadeDone_[n.channel] || self->startNextFade(n.channel);
			if(!more) {
				auto emptyCb = self->onQueueEmpty_;
				++self->callbacksInFlight_;
				++self->states_[n.channel].callbacksInFlight;
				unlock();
				if(emptyCb) {
					emptyCb(n.channel);
				}
				lock();
				self = findInstance(false);
				if(self) {
					--self->callbacksInFlight_;
					--self->states_[n.channel].callbacksInFlight;
				}
			}
		}
	}
	wake();
}
