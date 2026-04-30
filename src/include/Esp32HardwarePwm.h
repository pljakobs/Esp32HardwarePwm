/**
 * @file
 * @brief  ESP32 Hardware PWM (LEDC) driver
 * @author Peter Jakobs http://github.com/pljakobs
 *
 * Sming Framework Project - Open Source framework for high efficiency native ESP8266 development.
 * Created 2015 by Skurydin Alexey
 * http://github.com/SmingHub/Sming
 * All files of the Sming Core are provided under the LGPL v3 license.
 *
 * This library wraps the ESP32 LEDC peripheral to provide hardware PWM with:
 * - Configurable duty resolution (1–20 bits) and frequency
 * - Multiple independent instances with distinct timer configurations
 * - Phase shifting (hpoint) for EMI/noise/power-spike reduction
 * - Spread spectrum modulation
 * - Hardware-accelerated linear fading
 * - Per-channel FIFO and CYCLIC fade queues with automatic sequencing
 * - Split of large-range fades to work around the LEDC step_num ≤ 1023 limit
 * - Per-reload timing compensation to reduce accumulated latency in queued sequences
 * - Application callbacks: fade-done, queue-empty, cyclic-wrap, queue-error
 */

/** @defgroup esp32_hw_pwm ESP32 Hardware PWM
 *  @brief    ESP32 LEDC hardware PWM driver with fade queue support
 *  @{
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>
#include <driver/ledc.h>
#include <esp_timer.h>
#include <soc/soc_caps.h>
#include <array>
#include <esp_attr.h>
#include <Delegate.h>
#include <SimpleTimer.h>
#include <memory>

// ---------------------------------------------------------------------------
// Per-channel fade queue depth — override before including this header.
// ---------------------------------------------------------------------------
#ifndef FADE_QUEUE_DEPTH
#define FADE_QUEUE_DEPTH 10
#endif

// ---------------------------------------------------------------------------
// Debug level control for Esp32HardwarePwm
// Set HW_PWM_DEBUG via compiler flag (e.g. -DHW_PWM_DEBUG=2) or in component.mk.
// This define is intentionally NOT set here — it only applies to Esp32HardwarePwm.cpp.
//   0 = no logging (default)
//   1 = errors only   (debug_e)
//   2 = errors + info (debug_e, debug_i)
//   3 = full          (debug_e, debug_i, debug_d)
// ---------------------------------------------------------------------------

/**
 * @brief ESP32 Hardware PWM driver wrapping the LEDC peripheral.
 *
 * Provides a channel-indexed interface over the ESP32 LEDC hardware with:
 *
 * **Basic PWM**
 * - Arbitrary duty resolution (1–20 bit) and frequency per timer
 * - Instantaneous duty set (`setDutyChan`) and percentage helpers
 * - Phase shifting (hpoint) per channel — off, auto-distributed, or manual
 * - Spread spectrum modulation for EMI reduction
 *
 * **Hardware fading**
 * - Single-shot linear fades: `fadeChan` / `fadePercentChan`
 * - Automatic splitting of large-range fades to avoid the LEDC
 *   `step_num ≤ 1023` quantisation limit (up to −18 % timing error at 12-bit
 *   without splitting)
 *
 * **Fade queue**
 * - Per-channel ring-buffer queue (`fadeChan(..., queue=true)`) in FIFO or CYCLIC mode
 * - FIFO: auto-starts on first entry; fires `onQueueEmpty` when drained
 * - CYCLIC: seeded then started with `startQueue`; fires `onCyclicWrap` on
 *   each loop
 * - Per-reload timing correction (`reloadOverheadUs`) to compensate ISR→task
 *   dispatch latency accumulating over many queued steps
 * - `onFadeDone` callback after every individual fade (including queue steps)
 * - `onQueueError` callback for `QUEUE_FULL` and `SPLIT_DEGRADED` conditions
 *
 * **Safe teardown**
 * - `hasPendingCallbacks()` lets callers defer `delete` until all queued
 *   task-context dispatches have drained, preventing use-after-free
 *
 * Channel index is the 0-based position in the `pins[]` vector passed to the
 * constructor, independent of GPIO number or LEDC hardware channel number.
 *
 * @note RAII — all LEDC resources are released in the destructor.
 * @note Not copyable or movable.
 */
class Esp32HardwarePwm
{
public:
	static constexpr uint8_t BadChannel = 0xff; ///< Invalid PWM channel indicator

	/**
     * @brief Defines PWM duty cycle percentage (0.0 = off, 100.0 = full on)
     */
	using DutyCycle = float;

	// -----------------------------------------------------------------------
	// Fade queue types
	// -----------------------------------------------------------------------

	enum class QueueMode : uint8_t {
		FIFO,   ///< Queue drains and stops; onQueueEmpty fires when exhausted
		CYCLIC, ///< Playback loops back to entry 0 endlessly; onCyclicWrap fires on each loop
	};

	/** @brief Error codes delivered to the onQueueError callback */
	enum class QueueError : uint8_t {
		QUEUE_FULL,		///< No space in the queue; the fade was not enqueued
		SPLIT_DEGRADED, ///< Fade needed splitting but segments didn't fit; pushed unsplit (timing accuracy reduced)
	};

	// -----------------------------------------------------------------------
	// Configuration types — declared before constructors so they are visible
	// in constructor parameter lists.
	// -----------------------------------------------------------------------

	enum class PhaseShiftMode : uint8_t {
		OFF,	///< No phase shifting
		AUTO,   ///< Automatic phase shifting based on channel index
		MANUAL, ///< Manual phase shifting using provided hpoint values
	};

	enum class SpreadSpectrumMode : uint8_t {
		OFF, ///< Spread spectrum disabled
		ON,  ///< Spread spectrum enabled
	};

	struct PhaseShiftConfig {
		PhaseShiftMode mode = PhaseShiftMode::OFF; ///< Phase shift mode
		std::vector<int> manual_hpoints = {};	  ///< hpoint values for MANUAL mode, one per pin
	};

	struct SpreadSpectrumConfig {
		SpreadSpectrumMode mode = SpreadSpectrumMode::OFF; ///< Spread spectrum mode
		uint8_t WidthPercent = 0;						   ///< Frequency deviation as percentage of base frequency
		uint16_t Subsampling = 0;						   ///< Number of PWM cycles between frequency updates
	};

	struct TimerConfig {
		ledc_mode_t speed_mode = LEDC_LOW_SPEED_MODE;	///< LEDC speed mode
		ledc_timer_bit_t resolution = LEDC_TIMER_10_BIT; ///< Duty resolution in bits
		ledc_timer_t timer_num = LEDC_TIMER_0;			 ///< LEDC timer index
		uint32_t frequency = 1000;						 ///< PWM frequency in Hz
		ledc_clk_cfg_t clk_cfg = LEDC_AUTO_CLK;			 ///< Clock source
	};

	struct Config {
		ledc_channel_t channelStart = LEDC_CHANNEL_0; ///< First LEDC channel to allocate
		TimerConfig timer = {};						  ///< Timer configuration
		PhaseShiftConfig phaseShift = {};			  ///< Phase shift configuration
		SpreadSpectrumConfig spreadSpectrum = {};	 ///< Spread spectrum configuration
	};

	// -----------------------------------------------------------------------
	// Constructors / destructor
	// -----------------------------------------------------------------------

	/**
     * @brief Construct PWM instance with default configuration
     * @param pins Vector of GPIO pins to control
     */
	Esp32HardwarePwm(std::vector<uint8_t>& pins) : Esp32HardwarePwm(pins, Config{})
	{
	}

	/**
     * @brief Construct PWM instance with custom configuration
     * @param pins Vector of GPIO pins to control
     * @param config PWM configuration parameters
     */
	Esp32HardwarePwm(std::vector<uint8_t>& pins, const Config& config);

	/**
     * @brief Destructor - automatically releases all allocated resources
     */
	~Esp32HardwarePwm();

	// Disable copy constructor and assignment operator to prevent resource conflicts
	Esp32HardwarePwm(const Esp32HardwarePwm&) = delete;
	Esp32HardwarePwm& operator=(const Esp32HardwarePwm&) = delete;

	// -----------------------------------------------------------------------
	// Timer / global configuration
	// -----------------------------------------------------------------------

	/** @brief Change PWM frequency for all channels
     * @param frequency New frequency in Hz
     * @return true if successful, false otherwise
     * @note This affects all channels sharing the same timer
     */
	bool setFrequency(uint32_t frequency);

	/** @brief Get current PWM frequency
     * @return Frequency in Hz
     */
	uint32_t getFrequency() const;

	/** @brief Set PWM period in microseconds
     * @param period_us Period in microseconds
     * @return true if successful, false otherwise
     */
	bool setPeriod(uint32_t period_us);

	/** @brief Get PWM period in microseconds
     * @return Period in microseconds
     */
	uint32_t getPeriod() const;

	/** @brief Get maximum duty cycle value
     * @return Maximum duty value based on resolution
     */
	uint32_t getMaxDuty() const;

	/** @brief Get duty resolution in bits
     * @return Resolution in bits (1-20)
     */
	uint8_t getResolution() const;

	/** @brief Get total number of configured channels
     * @return Number of channels
     */
	uint8_t getPinCount() const
	{
		return static_cast<uint8_t>(pins_.size());
	}

	/** @brief Check if PWM instance is properly initialized
     * @return true if initialized, false otherwise
     */
	bool isInitialized() const
	{
		return initialized_;
	}

	/** @brief Apply pending duty changes to all channels
     * @note Only needed when update_immediately was set to false
     */
	void update();

	/** @brief Stop PWM output on all channels
     * @param idle_level Level to set pins when stopped (0 or 1)
     */
	void stopAll(bool idle_level = false);

	// -----------------------------------------------------------------------
	// Primary interface — channel-indexed (preferred)
	// Channel index is the 0-based position in the pins[] vector passed to
	// the constructor, independent of GPIO number or LEDC hardware channel.
	// -----------------------------------------------------------------------

	/** @brief Set duty cycle for a channel (absolute value)
     * @param channel Channel index
     * @param duty Duty cycle value (0 to getMaxDuty())
     * @param update_immediately Apply changes immediately (default: true)
     * @return true if successful, false otherwise
     */
	bool setDutyChan(uint8_t channel, uint32_t duty, bool update_immediately = true);

	/** @brief Get duty cycle for a channel (absolute value)
     * @param channel Channel index
     * @return Current duty cycle value (0 to getMaxDuty())
     */
	uint32_t getDutyChan(uint8_t channel);

	/** @brief Set duty cycle for a channel as a percentage
     * @param channel Channel index
     * @param percentage Duty cycle percentage (0.0 to 100.0)
     * @param update_immediately Apply changes immediately (default: true)
     * @param cie Apply CIE 1931 perceptual correction (default: false)
     * @return true if successful, false otherwise
     */
	bool setDutyChanPercent(uint8_t channel, DutyCycle percentage, bool update_immediately = true, bool cie = false)
	{
		if(percentage < 0.0f) {
			percentage = 0.0f;
		}
		if(percentage > 100.0f) {
			percentage = 100.0f;
		}
		float Y = cie ? cie1931Linear(percentage) : (percentage / 100.0f);
		return setDutyChan(channel, static_cast<uint32_t>(Y * getMaxDuty()), update_immediately);
	}

	/** @brief Get duty cycle for a channel as a percentage
     * @param channel Channel index
     * @param cie Return CIE 1931 perceptual percentage (default: false = linear)
     * @return Duty cycle percentage (0.0 to 100.0)
     */
	DutyCycle getDutyChanPercent(uint8_t channel, bool cie = false)
	{
		uint32_t duty = getDutyChan(channel);
		uint32_t max_duty = getMaxDuty();
		if(max_duty == 0) {
			return 0.0f;
		}
		float Y = static_cast<float>(duty) / max_duty;
		return cie ? cie1931Inverse(Y) : (Y * 100.0f);
	}

	/** @brief Set phase shift for a channel (absolute hpoint value)
     * @param channel Channel index
     * @param phase_shift Phase shift value in PWM resolution counts (0 to getMaxDuty())
     * @param update_immediately Apply changes immediately (default: true)
     * @return true if successful, false otherwise
     */
	bool setPhaseShiftChan(uint8_t channel, uint32_t phase_shift, bool update_immediately = true);

	/** @brief Set phase shift for a channel as a percentage of the PWM period
     * @param channel Channel index
     * @param percentage Phase shift percentage (0.0 = no shift, 100.0 = full period)
     * @param update_immediately Apply changes immediately (default: true)
     * @return true if successful, false otherwise
     */
	bool setPhaseShiftChanPercent(uint8_t channel, DutyCycle percentage, bool update_immediately = true)
	{
		if(percentage < 0.0f) {
			percentage = 0.0f;
		}
		if(percentage > 100.0f) {
			percentage = 100.0f;
		}
		uint32_t phase_shift = static_cast<uint32_t>(percentage * getMaxDuty() / 100.0f);
		return setPhaseShiftChan(channel, phase_shift, update_immediately);
	}

	/** @brief Enable hardware fade functionality
     * @return true if successful, false otherwise
     * @note Called automatically by the constructor; exposed for manual control.
     */
	bool enableFade();

	/** @brief Disable hardware fade functionality */
	void disableFade();

	/** @brief Hardware linear fade on a channel (absolute duty target).
	 * @param channel_idx Channel index
	 * @param target_duty Target duty value (0 to getMaxDuty())
	 * @param fade_time_ms Duration in milliseconds
	 * @param queue If false (default), resets the queue and starts immediately.
	 *              If true, enqueues the fade for sequential playback.
	 * @return true if started/enqueued successfully
	 */
	bool fadeChan(uint8_t channel_idx, uint32_t target_duty, uint32_t fade_time_ms, bool queue = false);

	/** @brief Hardware linear fade on a channel (percentage target).
	 * @param channel_idx Channel index
	 * @param target_pct Target duty as percentage (0.0–100.0)
	 * @param fade_time_ms Duration in milliseconds
	 * @param cie Apply CIE 1931 perceptual correction (default: false)
	 * @param queue If false (default), resets the queue and starts immediately.
	 *              If true, enqueues the fade for sequential playback.
	 * @return true if started/enqueued successfully
	 */
	bool fadePercentChan(uint8_t channel_idx, DutyCycle target_pct, uint32_t fade_time_ms, bool cie = false,
						 bool queue = false)
	{
		if(target_pct < 0.0f) {
			target_pct = 0.0f;
		}
		if(target_pct > 100.0f) {
			target_pct = 100.0f;
		}
		float Y = cie ? cie1931Linear(target_pct) : (target_pct / 100.0f);
		return fadeChan(channel_idx, static_cast<uint32_t>(Y * getMaxDuty()), fade_time_ms, queue);
	}

	/** @brief Returns true while a hardware fade is in progress on the given channel */
	bool isFadingChan(uint8_t channel_idx) const;

	// -----------------------------------------------------------------------
	// Fade queue interface
	// -----------------------------------------------------------------------

	/** @brief Set queue mode for a channel.
	 * Must be called before filling the queue. Changing mode while the queue
	 * is running has undefined behaviour.
	 */
	void setQueueMode(uint8_t channel, QueueMode mode);

	/** @brief Get current queue mode for a channel */
	QueueMode getQueueMode(uint8_t channel) const;

	uint16_t getQueueEntries(uint8_t channel) const;

	/** @brief Clear the queue for a channel and reset mode to FIFO.
	 * The currently-running hardware fade (if any) completes normally, but no
	 * further queue entries are started and no callbacks fire afterwards.
	 * Queue capacity is preserved.
	 */
	void resetQueue(uint8_t channel);

	/** @brief Explicitly start a queued sequence.
	 * Must be called after seeding a CYCLIC queue (autoStart=false).  For FIFO
	 * queues with autoStart=true this is normally not needed, but can be used
	 * to restart an idle queue.  Returns false if the queue is empty or the
	 * channel is already fading.
	 */
	bool startQueue(uint8_t channel);

	/** @brief Set the maximum number of entries for a channel's queue.
	 * Can only be changed while the queue is empty.  Returns false if the
	 * queue is not empty or depth is zero.
	 */
	bool setQueueCapacity(uint8_t channel, uint16_t depth);

	/** @brief Get the maximum number of entries for a channel's queue */
	uint16_t getQueueCapacity(uint8_t channel) const;

	/** @brief Override the auto-start behaviour for a channel's queue.
	 * setQueueMode() sets autoStart automatically (true for FIFO, false
	 * for CYCLIC).  Use this to override that default.
	 */
	void setQueueAutoStart(uint8_t channel, bool autoStart);

	/** @brief Returns true if the queue starts automatically on first fadeChan(..., queue=true) call */
	bool getQueueAutoStart(uint8_t channel) const;

	// -----------------------------------------------------------------------
	// Hardware calibration
	// -----------------------------------------------------------------------

	/**
	 * @brief Per-(frequency, resolution) measured reload overhead entry.
	 *
	 * Generated by TimingTest_HwPWM: the measured per-step overhead is the
	 * model value plus the latPerStep residual from the CH1 column:
	 *   overheadUs = model + latPerStep_us   (clamped to 0 if negative)
	 */
	struct CalibrationEntry {
		uint32_t frequency;			 ///< PWM frequency in Hz
		ledc_timer_bit_t resolution; ///< Timer resolution
		uint32_t overheadUs;		 ///< Measured per-reload overhead (µs)
	};

	/**
	 * @brief Install a hardware-measured calibration table.
	 *
	 * When a (frequency, resolution) pair matches an entry, its overheadUs
	 * is used instead of the formula (1500 µs + 1 × period).  Call once
	 * before constructing any Esp32HardwarePwm instance — the pointer is
	 * stored but not copied, so the table must remain valid for the
	 * lifetime of the application.
	 *
	 * @param table  Pointer to CalibrationEntry array
	 * @param count  Number of entries
	 */
	static void setCalibrationTable(const CalibrationEntry* table, size_t count);

	/** @brief Override the per-reload overhead used to correct queued fade durations.
	 * Normally auto-computed as (1 PWM period + DISPATCH_LATENCY_US) by the
	 * constructor and by setFrequency().  Set to 0 to disable correction.
	 * @param channel Channel index
	 * @param overheadUs Overhead to deduct per reload, in microseconds
	 */
	void setReloadOverheadUs(uint8_t channel, uint32_t overheadUs);

	/** @brief Get the per-reload overhead currently configured for a channel */
	uint32_t getReloadOverheadUs(uint8_t channel) const;

	/** @brief Callback fired after every individual fade completes (even if more are queued) */
	void setOnFadeDoneCallback(Delegate<void(uint8_t)> cb)
	{
		onFadeDone_ = cb;
	}

	/** @brief Callback fired when a FIFO queue drains to empty */
	void setOnQueueEmptyCallback(Delegate<void(uint8_t)> cb)
	{
		onQueueEmpty_ = cb;
	}

	/** @brief Callback fired each time a CYCLIC queue wraps back to entry 0 */
	void setOnCyclicWrapCallback(Delegate<void(uint8_t)> cb)
	{
		onCyclicWrap_ = cb;
	}

	/** @brief Callback fired when a fadeChan(queue=true) call fails or degrades.
	 *  The callback receives the channel index and the QueueError reason.
	 *  For QUEUE_FULL the fade was not enqueued; for SPLIT_DEGRADED the fade
	 *  was enqueued unsplit (hardware timing accuracy may be reduced). */
	void setOnQueueErrorCallback(Delegate<void(uint8_t, QueueError)> cb)
	{
		onQueueError_ = cb;
	}

	/** @brief Returns true if there are unprocessed ISR results or a
	 *  dispatchFadeCallbacks task already queued in the Sming task queue.
	 *
	 *  Use this before deleting the object from task context to ensure no
	 *  stale task-queue entry still holds a pointer to it:
	 *  @code
	 *    if(pwm->hasPendingCallbacks()) {
	 *        System.queueCallback(tryTeardown);  // re-defer
	 *        return;
	 *    }
	 *    delete pwm;
	 *  @endcode
	 */
	bool hasPendingCallbacks() const
	{
		return pendingFadeCallbacks_.load(std::memory_order_relaxed) != 0 || fadeCallbackQueued_;
	}

	// -----------------------------------------------------------------------
	// Legacy interface — GPIO-pin-indexed (use channel interface for new code)
	// -----------------------------------------------------------------------

	/** @brief Set PWM duty cycle for a specific pin (legacy pin-indexed interface)
     * @param pin GPIO pin number
     * @param duty Duty cycle value (0 to getMaxDuty())
     * @param update_immediately Apply changes immediately (default: true)
     * @return true if successful, false otherwise
     */
	bool setDutyPin(uint8_t pin, uint32_t duty, bool update_immediately = true)
	{
		int idx = getPinIndex(pin);
		if(idx < 0) {
			return false;
		}
		return setDutyChan((uint8_t)idx, duty, update_immediately);
	}

	/** @brief Get PWM duty cycle for a specific pin (legacy pin-indexed interface)
     * @param pin GPIO pin number
     * @return Current duty cycle value
     */
	uint32_t getDutyPin(uint8_t pin)
	{
		int idx = getPinIndex(pin);
		if(idx < 0) {
			return 0;
		}
		return getDutyChan((uint8_t)idx);
	}

	/** @brief Arduino-style analogWrite (legacy)
     * @param pin GPIO pin number
     * @param duty Duty cycle value
     * @return true if successful, false otherwise
     */
	bool analogWrite(uint8_t pin, uint32_t duty)
	{
		return setDutyPin(pin, duty);
	}

	/** @brief Start PWM output on a specific pin (legacy)
     * @param pin GPIO pin number
     * @return true if successful, false otherwise
     */
	bool start(uint8_t pin);

	/** @brief Stop PWM output on a specific pin (legacy)
     * @param pin GPIO pin number
     * @param idle_level Level to set pin when stopped (0 or 1)
     * @return true if successful, false otherwise
     */
	bool stop(uint8_t pin, bool idle_level = false);

	/** @brief Start PWM output on all pins (legacy) */
	void startAll();

private:
	/** @brief Internal: enqueue a fade (absolute duty). Used by fadeChan(). */
	bool queueDutyChan(uint8_t channel, uint32_t targetDuty, uint32_t fadeTimeMs);

	struct FadeEntry {
		uint32_t targetDuty;
		uint32_t fadeTimeMs;
		bool isPartial = false; ///< True for intermediate segments of a split long-range fade
	};

	struct ChannelFadeQueue {
		std::vector<FadeEntry> entries; ///< Ring-buffer storage (size = queue capacity)
		uint16_t head = 0;				///< Next entry to consume
		uint16_t tail = 0;				///< Next free write slot
		uint16_t count = 0;				///< FIFO: decrements on pop; CYCLIC: fixed after seeding
		uint16_t cycleLen = 0;			///< CYCLIC: number of entries in the cycle
		QueueMode mode = QueueMode::FIFO;
		bool autoStart = true; ///< If true, playback starts on first fadeChan(queue=true); false requires startQueue()
		uint32_t reloadOverheadUs = 0;	 ///< Per-reload overhead subtracted from each step (µs)
		int32_t carryUs = 0;			   ///< Sub-ms accumulator for reload overhead correction
		int32_t quantCarryUs = 0;		   ///< Sub-µs accumulator for cycle_num truncation correction
		bool activeIsIntermediate = false; ///< True when the executing entry is a partial (intermediate) split segment
		bool warnedLowCycleNum = false;	///< Suppress repeat low-cycle_num warnings for this channel
	};

	struct PinConfig {
		uint8_t gpioPin = 0;					 ///< GPIO pin number
		ledc_channel_t channel = LEDC_CHANNEL_0; ///< LEDC hardware channel
		uint32_t currentDuty = 0;				 ///< Last duty value written
		uint32_t targetDuty = 0;				 ///< Target duty for in-progress fade
		int hpoint = 0;							 ///< Phase shift hpoint
		bool isActive = false;					 ///< True when channel is running
	};

	// Context block passed to steadyFadeTimerCb — one per channel, stable address.
	struct SteadyFadeContext {
		Esp32HardwarePwm* self;
		uint8_t channel_idx;
	};
	std::vector<SteadyFadeContext> steadyFadeCtx_;
	std::vector<std::unique_ptr<SimpleTimer>> steadyFadeTimers_;

	TimerConfig timer_;
	SpreadSpectrumConfig spreadSpectrum_;
	esp_timer_handle_t spreadSpectrumTimer_ = nullptr;
	PhaseShiftConfig phaseShift_;
	std::vector<PinConfig> pins_;
	std::vector<ChannelFadeQueue> fadeQueues_;

	bool initialized_ = false;
	bool fadeInstalled_ = false;

	// Per-channel fade-done flags, set from LEDC fade callback (ISR-safe)
	std::array<volatile bool, SOC_LEDC_CHANNEL_NUM> fadeDone_{};

	// ISR → task handoff for fade completion
	// Written from the LEDC fade ISR (potentially on either core) and
	// read+cleared in task context — must be atomic to avoid torn reads
	// on the dual-core ESP32.
	std::atomic<uint32_t> pendingFadeCallbacks_{0};
	volatile bool fadeCallbackQueued_ = false;

	// Application-level callbacks
	Delegate<void(uint8_t)> onFadeDone_;
	Delegate<void(uint8_t)> onQueueEmpty_;
	Delegate<void(uint8_t)> onCyclicWrap_;
	Delegate<void(uint8_t, QueueError)> onQueueError_;

	/**
     * @brief Initialize PWM instance
     * @param pins Array of GPIO pins
     * @param pin_count Number of pins
     * @return true if successful, false otherwise
     */
	bool initialize();

	int getPinIndex(uint8_t gpioPin) const
	{
		for(size_t i = 0; i < pins_.size(); ++i) {
			if(pins_[i].gpioPin == gpioPin) {
				return (int)i;
			}
		}
		return -1;
	}

	/**
     * @brief Get channel info for a specific pin
     * @param pin GPIO pin number
     * @return Pointer to channel info, or nullptr if pin not found
     */
	PinConfig* getPinConfig(uint8_t gpioPin)
	{
		for(auto& pin : pins_) {
			if(pin.gpioPin == gpioPin) {
				return &pin;
			}
		}
		return nullptr;
	}

	/**
     * @brief Calculate hpoint for phase shifting 
	 *        this calculates hpoits such that n channels
	 *        are evenly distributed across the PWM period
     * @param channel_index Index of the channel
     * @return Calculated hpoint value
     */
	int calculateHpoint(uint8_t channel_index) const
	{
		uint32_t max_duty = getMaxDuty();
		return (int)(max_duty * channel_index) / pins_.size();
	};

	/**
     * @brief Start spread spectrum modulation
     * @param frequency Center frequency in Hz
     * @param config Spread spectrum configuration
     * @return true if successful, false otherwise
     **/
	bool setupSpreadSpectrum(int frequency, SpreadSpectrumConfig& config);

	/**
     * @brief Handle spread spectrum modulation
     */
	void handleSpreadSpectrum();

	// Fade callback registered with ledc_cb_register per channel — runs in ISR context
	static bool IRAM_ATTR fadeDoneCallback(const ledc_cb_param_t* param, void* arg);

	// Task-context dispatcher — deferred from ISR via System.queueCallback
	static void dispatchFadeCallbacks(uint32_t param);

	// Dequeue the head entry from a FIFO queue (advances head, decrements count)
	FadeEntry dequeueFifo(ChannelFadeQueue& q);

	// Start the next fade from the queue; returns false if queue empty
	bool startNextFade(uint8_t channel_idx);

	// Raw LEDC hardware fade — used by startNextFade only; bypasses queue and split logic
	bool fadeHwChan(uint8_t channel_idx, uint32_t target_duty, uint32_t fade_time_ms);

	// esp_timer callback — runs in task context, static wrapper required for C function pointer
	static void spreadSpectrumTimerCb(void* arg);

	// SimpleTimer callback for zero-range (x→x) fades — fires after fadeTimeMs elapses
	static void steadyFadeTimerCb(void* arg);

	// -----------------------------------------------------------------
	// CIE 1931 perceptual correction
	// Maps a perceptual lightness percentage (0–100) to a linear
	// duty cycle value using the standard CIE 1931 formula:
	//   L <= 8  →  Y = L / 902.3
	//   L  > 8  →  Y = ((L + 16) / 116)^3
	// -----------------------------------------------------------------
	static float cie1931Linear(float L)
	{
		if(L <= 0.0f) {
			return 0.0f;
		}
		if(L >= 100.0f) {
			return 1.0f;
		}
		if(L <= 8.0f) {
			return L / 902.3f;
		}
		float t = (L + 16.0f) / 116.0f;
		return t * t * t;
	}

	// CIE 1931 inverse: maps linear duty fraction Y (0–1) back to perceptual
	// lightness L (0–100).  Inverse of cie1931Linear.
	//   Y <= 0.008856  →  L = 903.3 × Y
	//   Y  > 0.008856  →  L = 116 × ∛Y − 16
	static float cie1931Inverse(float Y)
	{
		if(Y <= 0.0f) {
			return 0.0f;
		}
		if(Y >= 1.0f) {
			return 100.0f;
		}
		if(Y <= 0.008856f) {
			return Y * 903.3f;
		}
		return 116.0f * cbrtf(Y) - 16.0f;
	}
};

/** @} */
