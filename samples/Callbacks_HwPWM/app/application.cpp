/*
 * Callbacks_HwPWM — Focused demonstration of all three fade-queue callbacks
 *
 * Four channels, each showcasing one callback or pattern:
 *
 *   CH0  onFadeDone   — fires after every individual fade step.
 *                       Runs a short FIFO sequence of 5 steps; prints
 *                       channel + current duty after each step.
 *
 *   CH1  onQueueEmpty — fires when a FIFO queue drains to empty.
 *                       Runs a 3-step FIFO sequence; the callback
 *                       re-arms the sequence twice more, then stops.
 *
 *   CH2  onCyclicWrap — fires each time a CYCLIC queue wraps back
 *                       to entry 0.  Loops a 4-step heartbeat pattern
 *                       and stops after MAX_LOOPS complete cycles.
 *
 *   CH3  onFadeDone streaming — queue is filled to capacity upfront;
 *                       onFadeDone refills exactly one slot per step so
 *                       the queue stays perpetually full and playback is
 *                       gapless.  Runs a triangle-wave pattern for
 *                       TOTAL_STREAM_STEPS total fades then stops.
 *
 * All callbacks share the same Esp32HardwarePwm instance; the channel
 * number passed to each callback identifies which channel fired.
 */

#include <SmingCore.h>
#include <Esp32HardwarePwm.h>

namespace
{
// Pins — adjust to suit your board (avoid GPIO 6-11, 34-39 on ESP32 classic)
std::vector<uint8_t> pinList{13, 12, 14, 27};

// clang-format off
Esp32HardwarePwm pwm(pinList, Esp32HardwarePwm::Config{
.timer = {
.resolution = LEDC_TIMER_10_BIT,
.frequency  = 4000,
},
.phaseShift = {
.mode = Esp32HardwarePwm::PhaseShiftMode::OFF,
},
.spreadSpectrum = {
.mode = Esp32HardwarePwm::SpreadSpectrumMode::OFF,
},
});
// clang-format on

constexpr uint8_t CH_FADE_DONE = 0;   ///< onFadeDone demo
constexpr uint8_t CH_QUEUE_EMPTY = 1; ///< onQueueEmpty demo
constexpr uint8_t CH_CYCLIC_WRAP = 2; ///< onCyclicWrap demo
constexpr uint8_t CH_STREAM = 3;	  ///< onFadeDone streaming demo

constexpr uint32_t STEP_MS = 500;			///< Fade duration for CH0/CH1/CH2
constexpr uint8_t MAX_LOOPS = 3;			///< Stop CH1/CH2 after this many runs/wraps
constexpr uint32_t STREAM_STEP_MS = 300;	///< Fade duration for CH3 streaming steps
constexpr uint32_t TOTAL_STREAM_STEPS = 20; ///< Total steps to stream on CH3
constexpr uint8_t STREAM_QUEUE_DEPTH = 4;   ///< CH3 queue capacity (filled to brim)

uint8_t ch1RearmCount = 0;		///< How many times CH1 has been re-armed
uint8_t ch2WrapCount = 0;		///< How many times CH2 has wrapped
uint32_t streamStepsPushed = 0; ///< Steps pushed to CH3 so far

// -----------------------------------------------------------------------
// CH1 helper: load 3 FIFO steps (0% -> 50% -> 100%)
// -----------------------------------------------------------------------
void armCh1Fifo()
{
	pwm.fadePercentChan(CH_QUEUE_EMPTY, 100.0f, STEP_MS, false, true);
	pwm.fadePercentChan(CH_QUEUE_EMPTY, 50.0f, STEP_MS, false, true);
	pwm.fadePercentChan(CH_QUEUE_EMPTY, 0.0f, STEP_MS, false, true);
}

// -----------------------------------------------------------------------
// CH3 helper: push one step of the triangle-wave stream onto CH_STREAM.
// Triangle: steps 0..9 ramp 10%..100%, steps 10..19 ramp 90%..0%.
// Consecutive targets always differ by 10%, so LEDC always fires the ISR.
// -----------------------------------------------------------------------
void pushStreamStep()
{
	uint8_t pos = static_cast<uint8_t>(streamStepsPushed % 20);
	float pct = (pos < 10) ? (pos + 1) * 10.0f : (19 - pos) * 10.0f;
	pwm.fadePercentChan(CH_STREAM, pct, STREAM_STEP_MS, false, true);
	++streamStepsPushed;
	Serial.printf("[stream      ] ch=%u  step %2lu/%lu -> %.0f%%\n", (unsigned)CH_STREAM,
				  (unsigned long)streamStepsPushed, (unsigned long)TOTAL_STREAM_STEPS, (double)pct);
}

void startDemo()
{
	if(!pwm.isInitialized()) {
		Serial.println(_F("PWM not initialized — check pin list"));
		return;
	}

	Serial.println(_F("\n=== Callbacks_HwPWM demo starting ==="));
	Serial.printf("  CH%u -> onFadeDone   (fires after every step)\n", CH_FADE_DONE);
	Serial.printf("  CH%u -> onQueueEmpty (fires when FIFO drains)\n", CH_QUEUE_EMPTY);
	Serial.printf("  CH%u -> onCyclicWrap (fires on each loop cycle)\n", CH_CYCLIC_WRAP);
	Serial.printf("  CH%u -> onFadeDone streaming (queue full, refill per step)\n", CH_STREAM);
	Serial.println();

	// -------------------------------------------------------------------
	// onFadeDone: fires after EVERY individual fade on any channel.
	// We only print for CH0 here to keep output readable.
	// -------------------------------------------------------------------
	pwm.setOnFadeDoneCallback([](uint8_t ch) {
		if(ch == CH_FADE_DONE) {
			Serial.printf("[onFadeDone  ] ch=%u  duty=%lu/%lu\n", ch, (unsigned long)pwm.getDutyChan(ch),
						  (unsigned long)pwm.getMaxDuty());
		}
		if(ch == CH_STREAM && streamStepsPushed < TOTAL_STREAM_STEPS) {
			pushStreamStep();
		}
	});

	// -------------------------------------------------------------------
	// onQueueEmpty: fires once when a FIFO queue drains.
	// Re-arm CH1 up to (MAX_LOOPS-1) more times, then leave it idle.
	// -------------------------------------------------------------------
	pwm.setOnQueueEmptyCallback([](uint8_t ch) {
		if(ch != CH_QUEUE_EMPTY)
			return;
		++ch1RearmCount;
		if(ch1RearmCount < MAX_LOOPS) {
			Serial.printf("[onQueueEmpty] ch=%u — FIFO drained, re-arming (run %u/%u)\n", ch,
						  (unsigned)(ch1RearmCount + 1), (unsigned)MAX_LOOPS);
			armCh1Fifo();
		} else {
			Serial.printf("[onQueueEmpty] ch=%u — all %u runs done — channel idle\n", ch, (unsigned)MAX_LOOPS);
		}
	});

	// -------------------------------------------------------------------
	// onCyclicWrap: fires each time the CYCLIC queue restarts from step 0.
	// Stop CH2 after MAX_LOOPS wraps.
	// -------------------------------------------------------------------
	pwm.setOnCyclicWrapCallback([](uint8_t ch) {
		if(ch != CH_CYCLIC_WRAP)
			return;
		++ch2WrapCount;
		Serial.printf("[onCyclicWrap] ch=%u — cycle #%u complete\n", ch, (unsigned)ch2WrapCount);
		if(ch2WrapCount >= MAX_LOOPS) {
			Serial.printf("[onCyclicWrap] ch=%u — reached %u loops, stopping\n", ch, (unsigned)MAX_LOOPS);
			pwm.resetQueue(ch);
		}
	});

	// -------------------------------------------------------------------
	// CH0 — onFadeDone: 5-step FIFO ramp up then down
	// -------------------------------------------------------------------
	pwm.setQueueCapacity(CH_FADE_DONE, 6);
	pwm.setQueueAutoStart(CH_FADE_DONE, true);
	pwm.fadePercentChan(CH_FADE_DONE, 0.0f, STEP_MS, false, true);
	pwm.fadePercentChan(CH_FADE_DONE, 25.0f, STEP_MS, false, true);
	pwm.fadePercentChan(CH_FADE_DONE, 75.0f, STEP_MS, false, true);
	pwm.fadePercentChan(CH_FADE_DONE, 100.0f, STEP_MS, false, true);
	pwm.fadePercentChan(CH_FADE_DONE, 50.0f, STEP_MS, false, true);

	// -------------------------------------------------------------------
	// CH1 — onQueueEmpty: initial 3-step FIFO load
	// -------------------------------------------------------------------
	pwm.setQueueCapacity(CH_QUEUE_EMPTY, 4);
	pwm.setQueueAutoStart(CH_QUEUE_EMPTY, true);
	armCh1Fifo();

	// -------------------------------------------------------------------
	// CH2 — onCyclicWrap: 4-step heartbeat in CYCLIC mode
	// -------------------------------------------------------------------
	pwm.setQueueCapacity(CH_CYCLIC_WRAP, 5);
	pwm.setQueueMode(CH_CYCLIC_WRAP, Esp32HardwarePwm::QueueMode::CYCLIC);
	pwm.fadePercentChan(CH_CYCLIC_WRAP, 0.0f, STEP_MS, false, true);
	pwm.fadePercentChan(CH_CYCLIC_WRAP, 100.0f, STEP_MS, false, true);
	pwm.fadePercentChan(CH_CYCLIC_WRAP, 20.0f, STEP_MS, false, true);
	pwm.fadePercentChan(CH_CYCLIC_WRAP, 100.0f, STEP_MS, false, true);
	pwm.startQueue(CH_CYCLIC_WRAP);

	// -------------------------------------------------------------------
	// CH3 — onFadeDone streaming: fill queue to brim, then push 1 per step
	// -------------------------------------------------------------------
	Serial.printf("CH3: streaming — queue depth %u, %lu total steps\n", (unsigned)STREAM_QUEUE_DEPTH,
				  (unsigned long)TOTAL_STREAM_STEPS);
	pwm.setQueueCapacity(CH_STREAM, STREAM_QUEUE_DEPTH);
	pwm.setQueueAutoStart(CH_STREAM, true);
	for(uint8_t i = 0; i < STREAM_QUEUE_DEPTH && streamStepsPushed < TOTAL_STREAM_STEPS; ++i) {
		pushStreamStep();
	}
}

} // namespace

void init()
{
	Serial.begin(SERIAL_BAUD_RATE);
	Serial.systemDebugOutput(true);
	startDemo();
}
