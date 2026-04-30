/*
 * StaggeredFade_HwPWM
 *
 * Five channels run the same cyclic pattern forever:
 *   0% → 80% in 200 ms  (attack)
 *   80% → 0% in 800 ms  (release)
 *   repeat
 *
 * Stagger: channel 0 starts immediately.  The moment its first fade
 * (the attack) completes, channel 1 starts.  When channel 1's first
 * fade completes, channel 2 starts — and so on for all five channels.
 *
 * Pins (adjust to suit your board):
 *   CH0 → GPIO 13
 *   CH1 → GPIO 12
 *   CH2 → GPIO 14
 *   CH3 → GPIO 27
 *   CH4 → GPIO 26
 */

#include <SmingCore.h>
#include <Esp32HardwarePwm.h>

namespace
{
// ---------------------------------------------------------------------------
// Hardware config
// ---------------------------------------------------------------------------
constexpr uint32_t ATTACK_MS = 200;  ///< 0% → 80%
constexpr uint32_t RELEASE_MS = 200; ///< 80% → 0%

constexpr float PEAK_PCT = 80.0f;
constexpr float RELEASE_PCT = 10.0f;

// Pins — avoid GPIO 6-11 (flash), 34-39 (input-only on ESP32 classic).
std::vector<uint8_t> pinList{13, 12, 14, 27, 26, 25};

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

// Whether each channel has already fired its first fadeDone (the attack).
// Used to trigger the next channel exactly once.
std::vector<bool> firstFadeFired(pinList.size(), false);

// ---------------------------------------------------------------------------
// startChannel: seed a 2-entry CYCLIC queue and start it.
// ---------------------------------------------------------------------------
void startChannel(uint8_t ch)
{
	pwm.setQueueMode(ch, Esp32HardwarePwm::QueueMode::CYCLIC);
	pwm.setQueueCapacity(ch, 3);
	pwm.fadePercentChan(ch, PEAK_PCT, ATTACK_MS, true, true);	 // 0% → 80%
	pwm.fadePercentChan(ch, RELEASE_PCT, RELEASE_MS, true, true); // 80% → 10%
	pwm.fadePercentChan(ch, 0, 600, false, true);				  // 10% → 0%

	pwm.startQueue(ch);
	Serial.printf("CH%u started\n", (unsigned)ch);
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------
void setupCallbacks()
{
	// onFadeDone fires after every individual fade step.
	// The first time it fires on channel N = the attack just completed.
	// Use that single event to start channel N+1.
	pwm.setOnFadeDoneCallback([](uint8_t ch) {
		if(!firstFadeFired[ch]) {
			firstFadeFired[ch] = true;
			uint8_t next = ch + 1;
			if(next < pinList.size()) {
				Serial.printf("CH%u attack done → starting CH%u\n", (unsigned)ch, (unsigned)next);
				startChannel(next);
			} else {
				Serial.println(_F("All channels running."));
			}
		}
	});
}

} // namespace

void init()
{
	Serial.begin(SERIAL_BAUD_RATE);
	Serial.systemDebugOutput(false);

	Serial.println(_F("\nStaggeredFade_HwPWM"));
	Serial.printf("  Attack:  %lu ms (0%% → %.0f%%)\n", (unsigned long)ATTACK_MS, (double)PEAK_PCT);
	Serial.printf("  Release: %lu ms (%.0f%% → 0%%)\n", (unsigned long)RELEASE_MS, (double)PEAK_PCT);
	Serial.println(_F("  Each channel starts when the previous channel's first attack completes.\n"));

	if(!pwm.isInitialized()) {
		Serial.println(_F("PWM init failed — check pin list"));
		return;
	}

	setupCallbacks();

	// CH0 starts immediately; CH1-4 are started from the fadeDone callback.
	startChannel(0);
}
