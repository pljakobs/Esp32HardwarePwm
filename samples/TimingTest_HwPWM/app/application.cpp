/*
 * TimingTest_HwPWM — timing accuracy sweep across multiple PWM configurations
 *
 * Runs the same two-channel measurement for each entry in the `configs[]` table:
 *
 *   CH0 — single TOTAL_FADE_MS hardware fade (0% → 100%).
 *          Measures duty-step quantisation error: LEDC always rounds the fade
 *          duration down to an integer number of timer cycles, so CH0 is
 *          typically a few hundred µs short of the requested duration.
 *
 *   CH1 — TOTAL_STEPS × MICROFADE_MS micro-fades chained via the FIFO queue.
 *          Measures per-step reload latency: after each step the LEDC ISR
 *          fires, Sming posts a task-queue message, the main task wakes and
 *          calls ledc_set_fade_time_and_start.  LEDC then waits for the next
 *          timer edge before starting.  This accumulates over all steps.
 *
 * After all configs complete a summary table is printed:
 *
 *   Config        | Quant err (CH0)         | Reload overhead (CH1)
 *                 | deviation µs   % total  | total dev ms   per step µs
 */

#include <SmingCore.h>
#include <Esp32HardwarePwm.h>
#include <esp_timer.h>

namespace
{
// ---------------------------------------------------------------------------
// Test parameters — shared across all configs
// ---------------------------------------------------------------------------
constexpr uint32_t TOTAL_FADE_MS = 5000;					   ///< Total fade duration (ms) per run
constexpr uint32_t MICROFADE_MS = 20;						   ///< Each micro-step duration (ms) — 50 Hz
constexpr uint32_t TOTAL_STEPS = TOTAL_FADE_MS / MICROFADE_MS; ///< 1000
constexpr uint8_t CH_SINGLE = 0;							   ///< Single long fade channel
constexpr uint8_t CH_MICRO = 1;								   ///< Micro-fade queue channel
constexpr uint8_t CH_TRI = 2;								   ///< Continuous triangle wave (visual monitoring)
constexpr uint8_t MICROFADE_QUEUE_DEPTH = 20;				   ///< FIFO depth for CH1

// ---------------------------------------------------------------------------
// PWM configurations to benchmark
// ---------------------------------------------------------------------------
struct TestConfig {
	ledc_timer_bit_t resolution;
	uint32_t frequency;
	char label[16]; ///< e.g. " 1kHz/ 8-bit"
};

// Full test matrix: all valid (resolution, frequency) combinations for the
// ESP32 LEDC peripheral with an 80 MHz clock source.
//
// Constraint: freq × 2^bits ≤ 80,000,000
//   8–12 bit : 1–16 kHz all valid  (16k × 4096 = 65.5 MHz < 80 MHz)
//   13-bit   : 1– 9 kHz valid      (9k  × 8192 = 73.7 MHz < 80 MHz)
//   14-bit   : 1– 4 kHz valid      (4k  × 16384 = 65.5 MHz < 80 MHz)
//
// Configs are generated at runtime in buildConfigs() so the list stays
// compact and the validity constraint is enforced in one place.
static std::vector<TestConfig> configs;

void buildConfigs()
{
	struct Band {
		ledc_timer_bit_t res;
		uint8_t bits;
		uint32_t maxFreqHz;
	};
	static const Band bands[] = {
		{LEDC_TIMER_8_BIT, 8, 16000},   {LEDC_TIMER_9_BIT, 9, 16000},   {LEDC_TIMER_10_BIT, 10, 16000},
		{LEDC_TIMER_11_BIT, 11, 16000}, {LEDC_TIMER_12_BIT, 12, 16000}, {LEDC_TIMER_13_BIT, 13, 9000},
		{LEDC_TIMER_14_BIT, 14, 4000},
	};
	configs.clear();
	for(const auto& b : bands) {
		for(uint32_t f = 1000; f <= b.maxFreqHz; f += 1000) {
			TestConfig cfg{};
			cfg.resolution = b.res;
			cfg.frequency = f;
			snprintf(cfg.label, sizeof(cfg.label), "%2ukHz/%2u-bit", (unsigned)(f / 1000), (unsigned)b.bits);
			configs.push_back(cfg);
		}
	}
}

// ---------------------------------------------------------------------------
// Per-run result storage
// ---------------------------------------------------------------------------
struct TestResult {
	bool valid;
	int64_t devCh0_us;	 ///< Quantisation error: elapsedCh0 − expected (µs)
	int64_t devCh1_us;	 ///< Total CH1 deviation from expected (µs)
	int64_t latPerStep_us; ///< devCh1 / TOTAL_STEPS (µs)
};
static std::vector<TestResult> results;
static size_t currentConfig = 0;

// ---------------------------------------------------------------------------
// Per-run mutable state
// ---------------------------------------------------------------------------
static std::vector<uint8_t> pinList{13, 12, 14}; // GPIO 14 = CH_TRI triangle output
static Esp32HardwarePwm* pwm = nullptr;
static int64_t tStart = 0;
static int64_t tEndCh0 = 0;
static int64_t tEndCh1 = 0;
static uint32_t stepsPushed = 0;
static bool ch0Done = false;
static bool ch1Done = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Triangle wave: alternate 0%↔100% so every step is a real duty transition.
float stepDuty(uint32_t step)
{
	return (step % 2 == 0) ? 0.0f : 100.0f;
}

void pushMicroSteps()
{
	// Reload overhead correction is applied automatically by the library
	// (1 PWM period + DISPATCH_LATENCY_US per step, via carry accumulator).
	while(stepsPushed < TOTAL_STEPS) {
		if(!pwm->fadePercentChan(CH_MICRO, stepDuty(stepsPushed), MICROFADE_MS, false, true))
			break;
		++stepsPushed;
	}
}

// Forward declarations
void runConfig(size_t idx);
void printTable();

// Called from the Sming main task after dispatchFadeCallbacks fully returns.
// Safe to delete the old pwm object and create the next one here.
// Re-defers itself if the library still has a pending dispatchFadeCallbacks
// entry in the task queue (guards against a stale dispatch firing on a freed object).
static void teardownAndRunNext()
{
	if(pwm && pwm->hasPendingCallbacks()) {
		System.queueCallback(teardownAndRunNext);
		return;
	}
	delete pwm;
	pwm = nullptr;

	if(currentConfig < configs.size())
		runConfig(currentConfig);
	else
		printTable();
}

// Called from within dispatchFadeCallbacks via onQueueEmpty — must NOT delete
// pwm here because dispatchFadeCallbacks still holds a pointer to it.  Just
// record the result, advance the counter, and queue the actual teardown.
void onRunComplete()
{
	constexpr int64_t expectedUs = (int64_t)TOTAL_FADE_MS * 1000LL;

	TestResult& r = results[currentConfig];
	r.valid = true;
	r.devCh0_us = (tEndCh0 - tStart) - expectedUs;
	r.devCh1_us = (tEndCh1 - tStart) - expectedUs;
	r.latPerStep_us = TOTAL_STEPS > 0 ? r.devCh1_us / (int64_t)TOTAL_STEPS : 0LL;

	Serial.printf("[%u/%u done] quant=%+lld us  reload=%+lld ms  lat/step=%+lld us\n", (unsigned)(currentConfig + 1),
				  (unsigned)configs.size(), r.devCh0_us, r.devCh1_us / 1000LL, r.latPerStep_us);

	++currentConfig;
	// Defer teardown to after dispatchFadeCallbacks returns
	System.queueCallback(teardownAndRunNext);
}

void tryComplete()
{
	if(ch0Done && ch1Done)
		onRunComplete();
}

// ---------------------------------------------------------------------------
// Run one configuration
// ---------------------------------------------------------------------------
void runConfig(size_t idx)
{
	const TestConfig& cfg = configs[idx];
	Serial.printf("\n--- Config %u/%u: %s  (%lu x %lu ms) ---\n", (unsigned)(idx + 1), (unsigned)configs.size(),
				  cfg.label, (unsigned long)TOTAL_STEPS, (unsigned long)MICROFADE_MS);

	// Reset per-run state
	tStart = tEndCh0 = tEndCh1 = 0;
	stepsPushed = 0;
	ch0Done = ch1Done = false;

	// clang-format off
    pwm = new Esp32HardwarePwm(pinList, Esp32HardwarePwm::Config{
        .timer = {
            .resolution = cfg.resolution,
            .frequency  = cfg.frequency,
        },
        .phaseShift = {
            .mode = Esp32HardwarePwm::PhaseShiftMode::OFF,
        },
        .spreadSpectrum = {
            .mode = Esp32HardwarePwm::SpreadSpectrumMode::OFF,
        },
    });
	// clang-format on

	if(!pwm->isInitialized()) {
		Serial.println(_F("PWM init failed — skipping config"));
		results[idx].valid = false;
		delete pwm;
		pwm = nullptr;
		++currentConfig;
		if(currentConfig < configs.size())
			runConfig(currentConfig);
		else
			printTable();
		return;
	}

	pwm->setOnFadeDoneCallback([](uint8_t ch) {
		if(ch == CH_MICRO && !ch1Done)
			pushMicroSteps();
	});

	pwm->setOnQueueEmptyCallback([](uint8_t ch) {
		int64_t now = esp_timer_get_time();
		if(ch == CH_SINGLE && !ch0Done) {
			tEndCh0 = now;
			ch0Done = true;
			Serial.printf("[CH0] done  elapsed=%lld ms\n", (now - tStart) / 1000LL);
			tryComplete();
		} else if(ch == CH_MICRO && !ch1Done) {
			if(stepsPushed < TOTAL_STEPS) {
				pushMicroSteps(); // spurious drain during pre-fill
				return;
			}
			tEndCh1 = now;
			ch1Done = true;
			Serial.printf("[CH1] done  elapsed=%lld ms\n", (now - tStart) / 1000LL);
			tryComplete();
		}
	});

	// CH_SINGLE uses the default FADE_QUEUE_DEPTH (10) — sufficient for all resolutions
	// (worst case: 12-bit needs 5 segments, ceil(4095/1023)=5)
	pwm->setQueueAutoStart(CH_SINGLE, true);
	pwm->setQueueCapacity(CH_MICRO, MICROFADE_QUEUE_DEPTH);
	pwm->setQueueAutoStart(CH_MICRO, true);

	// CH_TRI: continuous triangle wave 0→100→0% every 600 ms (CYCLIC, independent of test)
	// Capacity = 2 × ceil(maxDuty/1023) to hold all split segments for the full cycle.
	{
		uint32_t mxd = (1u << (uint8_t)cfg.resolution) - 1;
		uint16_t halfSeg = (uint16_t)((mxd + 1022u) / 1023u);
		if(halfSeg == 0)
			halfSeg = 1;
		pwm->setQueueMode(CH_TRI, Esp32HardwarePwm::QueueMode::CYCLIC);
		pwm->setQueueCapacity(CH_TRI, (uint16_t)(2u * halfSeg));
		pwm->fadePercentChan(CH_TRI, 100.0f, 300, false, true); // 0% → 100% in 300 ms
		pwm->fadePercentChan(CH_TRI, 0.0f, 300, false, true);   // 100% → 0% in 300 ms
		pwm->startQueue(CH_TRI);
	}

	tStart = esp_timer_get_time();
	pwm->fadePercentChan(CH_SINGLE, 100.0f, TOTAL_FADE_MS, false, true);
	pushMicroSteps();

	Serial.printf("Running... (~%lu s)\n", (unsigned long)(TOTAL_FADE_MS / 1000));
}

// ---------------------------------------------------------------------------
// Print final comparison table
// ---------------------------------------------------------------------------

// Stringify SMING_SOC token (e.g. esp32, esp32c3, esp32s3) at compile time.
#define _HWPWM_SOC_STR2(x) #x
#define _HWPWM_SOC_STR(x) _HWPWM_SOC_STR2(x)
static constexpr const char* kSocName = _HWPWM_SOC_STR(SMING_SOC);

/**
 * @brief Returns true when the CH1 measurement cannot be used for calibration.
 *
 * The ESP-IDF ledc_set_fade_time_and_start path (simplified):
 *   total_cycles = fade_time_ms × freq / 1000
 *   if total_cycles > duty_delta:
 *       scale=1, cycle_num=total_cycles/duty_delta  → actual ≈ requested  (accurate)
 *   else:
 *       cycle_num=1, scale=duty_delta/total_cycles
 *       if scale >= 2: actual ≈ (duty_delta/scale)/freq ≈ requested        (usable)
 *       if scale == 1: actual = duty_delta/freq  (FIXED, unaffected by requested)
 *
 * The library deducts its overhead estimate before calling LEDC, so the actual
 * LEDC fade_time is roughly MICROFADE_MS - formula_overhead ≈ MICROFADE_MS - 2ms.
 * Integer-ms rounding means the actual call alternates between N and N+1 ms;
 * if that ±1ms straddles a scale=1/scale=2 boundary the timing becomes erratic.
 *
 * This function probes both ±1ms boundary values and flags the config as
 * hw-limited if either boundary yields scale == 1.
 */
static bool isHwLimitedMicrofade(uint32_t freq, ledc_timer_bit_t res)
{
	uint32_t maxDuty = (1u << (uint8_t)res) - 1;
	if(maxDuty == 0 || freq == 0)
		return true;

	// Approximate adjusted fade time (after library deducts formula overhead).
	static constexpr uint32_t DISPATCH_US = 1500;
	uint32_t overheadUs = DISPATCH_US + 1000000u / freq;
	uint32_t overheadMs = overheadUs / 1000;
	if(overheadMs + 2 >= MICROFADE_MS)
		return true;

	// Probe both integer-ms values the library might pass to LEDC (±1ms).
	for(int delta = -1; delta <= 0; ++delta) {
		uint32_t adjMs = MICROFADE_MS - overheadMs + (uint32_t)delta;
		uint32_t cyl = freq * adjMs / 1000;
		if(cyl == 0)
			return true;
		if(cyl >= maxDuty)
			continue; // scale=1, cycle_num≥1: accurate
		if((maxDuty / cyl) < 2)
			return true; // scale=1: actual time fixed at maxDuty/freq
	}
	return false;
}

void printTable()
{
	constexpr int64_t expectedUs = (int64_t)TOTAL_FADE_MS * 1000LL;

	Serial.println();
	Serial.println(_F("========= Timing Accuracy Results ========="));
	Serial.printf("Test: %lu ms total  |  %lu x %lu ms micro-fades\n\n", (unsigned long)TOTAL_FADE_MS,
				  (unsigned long)TOTAL_STEPS, (unsigned long)MICROFADE_MS);

	Serial.println(_F("Config        | Quantisation error (CH0)      | Reload overhead (CH1)"));
	Serial.println(_F("              | deviation      %% of total     | total dev ms  per step us  timer cyc"));
	Serial.println(_F("--------------|-------------------------------|---------------------------"));

	for(size_t i = 0; i < configs.size(); ++i) {
		const TestResult& r = results[i];
		if(!r.valid) {
			Serial.printf("%-13s | (skipped — init failed)\n", configs[i].label);
			continue;
		}
		bool hwLim = isHwLimitedMicrofade(configs[i].frequency, configs[i].resolution);
		double pctCh0 = 100.0 * (double)r.devCh0_us / (double)expectedUs;
		// Timer cycles = per-step error / one timer period = latPerStep_us * freq / 1_000_000
		double cyclesDev = (double)r.latPerStep_us * (double)configs[i].frequency / 1000000.0;
		Serial.printf("%-13s%s| %+9lld us  %+8.4f%%     | %+10lld ms  %+8lld us  %+7.2f cyc%s\n", configs[i].label,
					  hwLim ? "*" : " ", r.devCh0_us, pctCh0, r.devCh1_us / 1000LL, r.latPerStep_us, cyclesDev,
					  hwLim ? "  [hw-limited]" : "");
	}
	Serial.println(_F("==========================================="));
	Serial.println(_F("* = IDF scale==1 at adjusted fade time: actual duration = maxDuty/freq"));
	Serial.println(_F("    (fixed regardless of requested time, uncorrectable via calibration)"));

	// ---------------------------------------------------------------------------
	// Emit calibration header file to serial.
	//
	// To apply calibration:
	//   1. Capture the block between "---- copy from here ----" and
	//      "---- copy to here ----" from the serial monitor.
	//   2. Save it as:
	//        Esp32HardwarePwm/src/calibration/HwPwmCalib_<soc>.h
	//   3. Rebuild — component.mk detects the file and the library auto-installs
	//      the table at startup (no application code changes needed).
	//   4. Re-run TimingTest_HwPWM to verify CH1 deviation is near zero.
	// ---------------------------------------------------------------------------
	Serial.println();
	Serial.println(_F("// ---- copy from here ----"));

	// File banner
	Serial.printf("// HwPwmCalib_%s.h\n", kSocName);
	Serial.println(_F("// Auto-generated by TimingTest_HwPWM — do not edit manually."));
	Serial.println(_F("// Re-run TimingTest_HwPWM to regenerate."));
	Serial.println(_F("//"));
	Serial.printf("// Copy to: Esp32HardwarePwm/src/calibration/HwPwmCalib_%s.h\n", kSocName);
	Serial.println(_F("// and rebuild. The library detects and applies this table automatically."));
	Serial.println();
	Serial.println(_F("#pragma once"));
	Serial.printf("#ifndef HWPWM_CALIB_%s_H\n", kSocName);
	Serial.printf("#define HWPWM_CALIB_%s_H\n", kSocName);
	Serial.println();
	Serial.println(_F("// clang-format off"));
	Serial.printf("static const Esp32HardwarePwm::CalibrationEntry hwpwmCalib_%s[] = {\n", kSocName);
	Serial.println(_F("    // { frequency, resolution, overheadUs }"));

	size_t entryCount = 0;
	size_t skippedScale = 0;
	for(size_t i = 0; i < configs.size(); ++i) {
		const TestResult& r = results[i];
		if(!r.valid)
			continue;
		// Skip hardware-limited configs — their residual reflects LEDC duty clamping
		// (cycle_num forced to 1), not fixed OS reload overhead.  The clamping error
		// depends on the requested step duration and is not portable.
		if(isHwLimitedMicrofade(configs[i].frequency, configs[i].resolution))
			continue;
		// Skip small-scale (IDF scale 2–9) configs.  When scale is small, a 1 ms shift
		// in the requested fade time causes a discrete scale change (e.g. 5→6) that moves
		// the actual LEDC duration by 2–5 ms.  The carry-based overhead correction cannot
		// converge: each iteration flips the sign of the residual.  Let these configs fall
		// back to the formula in computeReloadOverhead().
		{
			uint32_t mxd = (1u << (uint8_t)configs[i].resolution) - 1;
			uint32_t oh_ms = (1500u + 1000000u / configs[i].frequency) / 1000u;
			uint32_t adj_ms = (MICROFADE_MS > oh_ms) ? (MICROFADE_MS - oh_ms) : 1u;
			uint32_t cyc = configs[i].frequency * adj_ms / 1000u;
			uint32_t sc = (cyc > 0u && cyc < mxd) ? (mxd / cyc) : 0u;
			if(sc >= 2u && sc < 10u) {
				++skippedScale;
				continue;
			}
		}
		// Measured overhead = formula model + per-step residual (clamped to 0)
		int64_t model = 1500LL + 1000000LL / (int64_t)configs[i].frequency;
		int64_t actual = model + r.latPerStep_us;
		uint32_t overhead = (actual > 0) ? (uint32_t)actual : 0;
		Serial.printf("    { %5lu, LEDC_TIMER_%u_BIT, %5lu }, // %s\n", (unsigned long)configs[i].frequency,
					  (unsigned)(uint8_t)configs[i].resolution, (unsigned long)overhead, configs[i].label);
		++entryCount;
	}

	Serial.println(_F("};"));
	Serial.println(_F("// clang-format on"));
	Serial.println();
	Serial.printf("#define HWPWM_CALIB_TABLE hwpwmCalib_%s\n", kSocName);
	Serial.printf("#define HWPWM_CALIB_COUNT (sizeof(hwpwmCalib_%s)/sizeof(hwpwmCalib_%s[0]))\n", kSocName, kSocName);
	Serial.println();
	Serial.printf("#endif // HWPWM_CALIB_%s_H\n", kSocName);
	Serial.println(_F("// ---- copy to here ----"));
	Serial.printf("// (%u entries; %u hw-limited + %u small-scale configs excluded)\n", (unsigned)entryCount,
				  (unsigned)(configs.size() - entryCount - skippedScale), (unsigned)skippedScale);
}

} // namespace

void init()
{
	Serial.begin(SERIAL_BAUD_RATE);
	Serial.systemDebugOutput(true);

	Serial.println(_F("HwPWM Timing Accuracy Sweep"));
	Serial.printf("  Total fade:   %lu ms\n", (unsigned long)TOTAL_FADE_MS);
	Serial.printf("  Micro-step:   %lu ms  (%lu steps)\n", (unsigned long)MICROFADE_MS, (unsigned long)TOTAL_STEPS);
	Serial.printf("  Queue depth:  %u\n", MICROFADE_QUEUE_DEPTH);
	buildConfigs();
	results.assign(configs.size(), TestResult{});

	Serial.printf("  Configs:      %u  (~%lu min total)\n\n", (unsigned)configs.size(),
				  (unsigned long)((configs.size() * (TOTAL_FADE_MS + 10000)) / 60000));

	runConfig(0);
}
