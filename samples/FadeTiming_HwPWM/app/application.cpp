#include <SmingCore.h>
#include <Esp32HardwarePwm.h>
#include <esp_timer.h>
#include <atomic>

#ifndef FADE_TIMING_FREQUENCY
#define FADE_TIMING_FREQUENCY 8000
#endif

namespace
{
struct Trace {
	int64_t time;
	uint32_t duty;
	uint32_t duration;
	uint8_t event;
	uint8_t channel;
};
constexpr size_t TRACE_CAPACITY = 1024;
Trace traces[TRACE_CAPACITY];
std::atomic<uint32_t> traceCount{0};
std::atomic<bool> recording{false};
std::vector<uint8_t> pins{4, 1, 3, 5, 0};
Esp32HardwarePwm* pwm;
SimpleTimer nextTimer;
SimpleTimer pollTimer;
SimpleTimer watchdog;
unsigned testIndex;
unsigned finishedChannels;
unsigned externalSteps[5];
int64_t startTime;
int64_t endTimes[5];
bool failed;

struct Test {
	const char* name;
	bool queued;
	bool external;
	bool micro;
	bool spread;
	bool corrected;
	unsigned startPercent = 0;
	unsigned targetPercent = 100;
	bool cyclic = false;
};
const Test tests[] = {
	{"single", false, false, false, false, true},
	{"single-uncorrected", false, false, false, false, false},
	{"queued-500ms", true, false, false, false, true},
	{"external-500ms", false, true, false, false, true},
	{"queued-20ms", true, false, true, false, true},
	{"queued-20ms-uncorrected", true, false, true, false, false},
	{"single-spread", false, false, false, true, true},
	{"external-spread", false, true, false, true, true},
	{"5-to-95pct", false, false, false, false, false, 5, 95},
	{"full-down", false, false, false, false, false, 100, 0},
	{"cyclic-500ms", true, false, false, false, true, 0, 100, true},
};
constexpr unsigned REPEATS = 3;
constexpr unsigned TEST_COUNT = sizeof(tests) / sizeof(tests[0]);

const Test& currentTest()
{
	return tests[testIndex / REPEATS];
}

void runTest();

void finishTest()
{
	recording.store(false, std::memory_order_relaxed);
	watchdog.stop();
	pollTimer.stop();
	const auto& test = currentTest();
	uint32_t count = traceCount.load(std::memory_order_relaxed);
	Serial.printf("RESULT,%s,%u,failed=%u,records=%u,overflow=%u\n", test.name, testIndex % REPEATS, unsigned(failed),
				  unsigned(count), unsigned(count > TRACE_CAPACITY));
	unsigned channels = test.micro ? 1 : pins.size();
	for(unsigned ch = 0; ch < channels; ++ch) {
		Serial.printf("END,%u,%lld,duty=%u\n", ch, endTimes[ch] ? endTimes[ch] - startTime : -1LL,
					  unsigned(pwm->getDutyChan(ch)));
	}
	for(uint32_t i = 0; i < std::min<uint32_t>(count, TRACE_CAPACITY); ++i) {
		const auto& t = traces[i];
		Serial.printf("TRACE,%u,%u,%lld,%u,%u\n", unsigned(t.event), unsigned(t.channel), t.time - startTime,
					  unsigned(t.duty), unsigned(t.duration));
	}
	++testIndex;
	nextTimer.initializeMs<500>(runTest).startOnce();
}

void completeChannel(uint8_t ch)
{
	if(endTimes[ch] != 0) {
		return;
	}
	endTimes[ch] = esp_timer_get_time();
	++finishedChannels;
	if(finishedChannels == (currentTest().micro ? 1u : pins.size())) {
		System.queueCallback(finishTest);
	}
}

void pollExternal()
{
	for(uint8_t ch = 0; ch < pins.size(); ++ch) {
		if(endTimes[ch] || pwm->isFadingChan(ch)) {
			continue;
		}
		if(externalSteps[ch] == 6) {
			completeChannel(ch);
			continue;
		}
		++externalSteps[ch];
		if(!pwm->fadeChan(ch, pwm->getMaxDuty() * externalSteps[ch] / 6, 500)) {
			failed = true;
		}
	}
}

void runTest()
{
	if(pwm && pwm->hasPendingCallbacks()) {
		nextTimer.initializeMs<10>(runTest).startOnce();
		return;
	}
	delete pwm;
	pwm = nullptr;
	if(testIndex == TEST_COUNT * REPEATS) {
		Serial.println("ALL_TESTS_DONE");
		return;
	}
	const auto& test = currentTest();
	Esp32HardwarePwm::Config config;
	config.timer.frequency = FADE_TIMING_FREQUENCY;
	config.timer.resolution = LEDC_TIMER_12_BIT;
	config.phaseShift.mode = Esp32HardwarePwm::PhaseShiftMode::AUTO;
	if(test.spread) {
		config.spreadSpectrum.mode = Esp32HardwarePwm::SpreadSpectrumMode::ON;
		config.spreadSpectrum.WidthPercent = 15;
		config.spreadSpectrum.Subsampling = 4;
	}
	pwm = new Esp32HardwarePwm(pins, config);
	if(!pwm->isInitialized()) {
		Serial.println("FATAL,PWM initialization failed");
		return;
	}
	failed = false;
	finishedChannels = 0;
	for(unsigned ch = 0; ch < pins.size(); ++ch) {
		endTimes[ch] = 0;
		externalSteps[ch] = 1;
		pwm->setDutyChan(ch, pwm->getMaxDuty() * test.startPercent / 100);
		if(!test.corrected) {
			pwm->setReloadOverheadUs(ch, 0);
		}
		if(!pwm->setQueueCapacity(ch, test.micro ? 160 : 40)) {
			Serial.println("FATAL,queue capacity failed");
			return;
		}
		pwm->setQueueAutoStart(ch, false);
		if(test.cyclic) {
			pwm->setQueueMode(ch, Esp32HardwarePwm::QueueMode::CYCLIC);
			externalSteps[ch] = 0;
		}
	}
	pwm->setOnQueueErrorCallback([](uint8_t, Esp32HardwarePwm::QueueError) { failed = true; });
	if(!test.external) {
		pwm->setOnQueueEmptyCallback(completeChannel);
	}
	if(test.cyclic) {
		pwm->setOnFadeDoneCallback([](uint8_t ch) {
			if(++externalSteps[ch] == 6) {
				pwm->resetQueue(ch);
			}
		});
	}
	unsigned channels = test.micro ? 1 : pins.size();
	if(test.queued) {
		unsigned steps = test.micro ? 150 : (test.cyclic ? 2 : 6);
		for(unsigned ch = 0; ch < channels; ++ch) {
			for(unsigned step = 1; step <= steps; ++step) {
				uint32_t target = test.micro ? ((step % 2) ? pwm->getMaxDuty() : 0)
											 : test.cyclic ? ((step % 2) ? pwm->getMaxDuty() / 6 : 0)
														   : pwm->getMaxDuty() * step / steps;
				if(!pwm->fadeChan(ch, target, test.micro ? 20 : 500, true)) {
					failed = true;
				}
			}
		}
	}
	Serial.printf("BEGIN,%s,%u,overhead=%u\n", test.name, testIndex % REPEATS, unsigned(pwm->getReloadOverheadUs(0)));
	traceCount.store(0, std::memory_order_relaxed);
	startTime = esp_timer_get_time();
	recording.store(true, std::memory_order_relaxed);
	for(unsigned ch = 0; ch < channels; ++ch) {
		bool ok = test.queued
					  ? pwm->startQueue(ch)
					  : pwm->fadeChan(
							ch, test.external ? pwm->getMaxDuty() / 6 : pwm->getMaxDuty() * test.targetPercent / 100,
							test.external ? 500 : 3000);
		if(!ok) {
			failed = true;
		}
	}
	if(test.external) {
		pollTimer.initializeMs<20>(pollExternal).start();
	}
	watchdog
		.initializeMs<15000>([]() {
			failed = true;
			finishTest();
		})
		.startOnce();
}
} // namespace

void IRAM_ATTR hwPwmTimingTrace(uint8_t event, uint8_t channel, uint32_t duty, uint32_t duration)
{
	if(!recording.load(std::memory_order_relaxed)) {
		return;
	}
	uint32_t index = traceCount.fetch_add(1, std::memory_order_relaxed);
	if(index < TRACE_CAPACITY) {
		traces[index] = {esp_timer_get_time(), duty, duration, event, channel};
	}
}

void init()
{
#ifdef UART_ID_SERIAL_USB_JTAG
	Serial.setPort(UART_ID_SERIAL_USB_JTAG);
#endif
	Serial.begin(SERIAL_BAUD_RATE);
	Serial.systemDebugOutput(false);
	Serial.printf("FadeTiming_HwPWM,%uHz,12bit,GPIO=4/1/3/5/0\n", unsigned(FADE_TIMING_FREQUENCY));
	nextTimer.initializeMs<2000>(runTest).startOnce();
}
