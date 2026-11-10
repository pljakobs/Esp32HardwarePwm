#include "stubs/sdk.h"
#include <algorithm>
#include <array>
#define private public
#include "../../src/include/Esp32HardwarePwm.h"
#include "../../src/FadeWorker.h"
#undef private
#include <iostream>

using Pwm = Esp32HardwarePwm;
static void poll() { FadeWorker::service(); }
static void drain()
{
	for(int i = 0; i < 100; ++i) {
		poll();
		if(Mock::callbacks.empty()) return;
		Mock::dispatch();
	}
	assert(false);
}
static void unlocked() { assert(Mock::locks == 0); }

int main()
{
	std::vector<uint8_t> pins{4};
	Pwm::Config config;
	config.timer.resolution = LEDC_TIMER_12_BIT;
	config.timer.frequency = 8000;
	{
		Mock::timerCreateError = ESP_ERR_INVALID_STATE;
		Pwm pwm(pins, config);
		assert(pwm.isInitialized() && Mock::tasks == 1 && Mock::timers == 0);
		poll();
		assert(Mock::timers == 0);
		Mock::timerCreateError = ESP_OK;
		poll();
		assert(Mock::timers == 1);
		int done = 0, empty = 0;
		pwm.setOnFadeDoneCallback([&](uint8_t) { unlocked(); ++done; assert(pwm.hasPendingCallbacks()); });
		pwm.setOnQueueEmptyCallback([&](uint8_t) { unlocked(); ++empty; assert(pwm.hasPendingCallbacks()); });
		// Exercise the real ISR callback concurrently with task-side signal
		// snapshots. The native portMUX mock is an actual cross-thread mutex.
		auto signalIrq = Mock::irq[0][0];
		auto signalArg = Mock::arg[0][0];
		std::thread signalProducer([=] {
			for(uint32_t i = 0; i < 10000; ++i) {
				ledc_cb_param_t param{0, 0, (i & 1) ? 100U : 200U};
				assert(signalIrq(&param, signalArg));
			}
		});
		std::thread signalConsumer([] {
			for(int i = 0; i < 10000; ++i) {
				auto snapshot = FadeWorker::takeSignal(0, 0);
				if(snapshot.pending) {
					assert(snapshot.index == 0);
					assert(snapshot.duty == 100 || snapshot.duty == 200);
				}
			}
		});
		signalProducer.join();
		signalConsumer.join();
		assert(Mock::isrNotifications > 0 && Mock::lastIsrWoken);
		FadeWorker::clearSignal(0, 0);
		assert(pwm.getReloadOverheadUs(0) == 0);
		assert(pwm.fadeChan(0, 4095, 3000));
		assert(pwm.getQueueEntries(0) == 4);
		for(int i = 0; i < 4; ++i) {
			Mock::complete(0, 0);
			poll();
			assert(done == 0 && Mock::callbacks.empty());
		}
		assert(pwm.fadeChan(0, 100, 10, true));
		Mock::complete(0, 0);
		poll();
		assert(pwm.hasPendingCallbacks());
		const auto starts = Mock::starts;
		poll();
		assert(Mock::starts == starts); // Logical barrier waits for Sming.
		Mock::dispatch();
		assert(done == 1 && empty == 0 && Mock::starts == starts + 1);
		Mock::complete(0, 0);
		drain();
		assert(done == 2 && empty == 1 && !pwm.hasPendingCallbacks());

		// Hold expiration does not need Sming. Failed notifications retry.
		assert(pwm.fadeChan(0, 100, 25));
		assert(pwm.isFadingChan(0));
		Mock::now += 24999;
		poll();
		assert(pwm.isFadingChan(0));
		Mock::rejectPosts = true;
		Mock::now += 1;
		poll();
		assert(!pwm.isFadingChan(0) && pwm.hasPendingCallbacks());
		assert(Mock::callbacks.empty());
		for(int i = 0; i < 5; ++i) poll();
		Mock::rejectPosts = false;
		drain();
		assert(done == 3 && empty == 2);
		assert(pwm.fadeChan(0, 100, 0));
		drain();
		assert(done == 4 && empty == 3);
		--done;
		--empty;

		// An old hardware ISR must not terminate a replacement hold.
		assert(pwm.fadeChan(0, 200, 10));
		auto savedIrq = Mock::irq[0][0];
		auto savedArg = Mock::arg[0][0];
		assert(pwm.fadeChan(0, 100, 20));
		ledc_cb_param_t stale{0, 0, 200};
		savedIrq(&stale, savedArg);
		poll();
		assert(pwm.isFadingChan(0));
		pwm.setDutyChan(0, 100);
		drain();
		assert(!pwm.hasPendingCallbacks());

		// Reentrant enqueue is sequenced after onFadeDone returns.
		pwm.setOnFadeDoneCallback([&](uint8_t) {
			unlocked();
			if(++done == 4) {
				const auto before = Mock::starts;
				assert(pwm.fadeChan(0, 300, 10, true));
				assert(before == Mock::starts);
			}
		});
		assert(pwm.fadeChan(0, 200, 10));
		Mock::complete(0, 0);
		drain();
		assert(Mock::target[0][0] == 300);
		Mock::complete(0, 0);
		drain();
		assert(done == 5 && empty == 3);

		// Clear invalidates the old continuation/empty notification.
		pwm.setOnFadeDoneCallback([&](uint8_t) { unlocked(); pwm.resetQueue(0); });
		assert(pwm.fadeChan(0, 400, 10));
		assert(pwm.fadeChan(0, 500, 10, true));
		Mock::complete(0, 0);
		drain();
		assert(!pwm.isFadingChan(0) && pwm.getQueueEntries(0) == 0 && empty == 3);

		// Replacement from a callback starts exactly once.
		pwm.setOnFadeDoneCallback([&](uint8_t) { unlocked(); pwm.fadeChan(0, 600, 10); });
		assert(pwm.fadeChan(0, 450, 10));
		assert(pwm.fadeChan(0, 550, 10, true));
		const auto before = Mock::starts;
		Mock::complete(0, 0);
		drain();
		assert(Mock::target[0][0] == 600 && Mock::starts == before + 1);
		pwm.setDutyChan(0, 600);
		drain();
		assert(!pwm.hasPendingCallbacks());

		// Capacity/degradation and errors run without the mutex.
		int degraded = 0, full = 0;
		pwm.setOnQueueErrorCallback([&](uint8_t, Pwm::QueueError e) {
			unlocked();
			e == Pwm::QueueError::QUEUE_FULL ? ++full : ++degraded;
		});
		assert(pwm.setQueueCapacity(0, 1));
		pwm.setQueueAutoStart(0, false);
		assert(pwm.fadeChan(0, 4095, 3000, true));
		assert(!pwm.fadeChan(0, 0, 3000, true));
		drain();
		assert(degraded == 1 && full == 1);
		pwm.resetQueue(0);

		// Cyclic wrapping remains ordered with logical completions.
		pwm.setOnFadeDoneCallback([&](uint8_t) { unlocked(); ++done; });
		int wraps = 0;
		pwm.setOnCyclicWrapCallback([&](uint8_t) { unlocked(); ++wraps; });
		pwm.setQueueMode(0, Pwm::QueueMode::CYCLIC);
		assert(pwm.fadeChan(0, 700, 10, true));
		assert(pwm.startQueue(0));
		drain();
		assert(wraps == 1);
		Mock::complete(0, 0);
		drain();
		assert(wraps == 2 && pwm.isFadingChan(0)); // Next cycle is a hold.
		pwm.disableFade();
		drain();
		assert(!pwm.isFadingChan(0));
		assert(pwm.enableFade());

		// Calibration/setter semantics remain explicit and reset carries.
		pwm.setReloadOverheadUs(0, 123);
		assert(pwm.getReloadOverheadUs(0) == 123);
		const Pwm::CalibrationEntry table[] = {{8000, LEDC_TIMER_12_BIT, 321}};
		Pwm::setCalibrationTable(table, 1);
		assert(pwm.setFrequency(8000));
		assert(pwm.getReloadOverheadUs(0) == 321);
		Pwm::setCalibrationTable(nullptr, 0);

		// A second independent instance uses the same worker and timer.
		auto otherConfig = config;
		otherConfig.channelStart = 1;
		otherConfig.timer.timer_num = 1;
		Pwm other(pins, otherConfig);
		assert(Mock::tasks == 1 && Mock::timers == 1);
		int otherDone = 0;
		other.setOnFadeDoneCallback([&](uint8_t) { unlocked(); ++otherDone; });
		assert(other.fadeChan(0, 100, 10));
		Mock::complete(0, 1);
		drain();
		assert(otherDone == 1);
		// Public mutations and the actual worker service serialize concurrently.
		std::thread worker([] { for(int i = 0; i < 1000; ++i) poll(); });
		for(int i = 0; i < 1000; ++i) {
			other.setDutyChan(0, i % 100);
			other.setQueueCapacity(0, 5);
			other.getQueueEntries(0);
		}
		worker.join();
	}
	drain();
	// A normally initialized instance must release hardware without setFrequency().
	{
		const auto stops = Mock::stops;
		const auto deconfigures = Mock::deconfigures;
		{
			Pwm temporary(pins, config);
			assert(temporary.isInitialized());
		}
		assert(Mock::stops == stops + 1);
		assert(Mock::deconfigures == deconfigures + 1);
		assert(Mock::irq[0][0] == nullptr);
	}
	// Destruction before an already-posted trampoline is also safe.
	{
		auto* discarded = new Pwm(pins, config);
		discarded->setOnFadeDoneCallback([](uint8_t) { assert(false); });
		assert(discarded->fadeChan(0, 0, 0));
		poll();
		assert(discarded->hasPendingCallbacks());
		delete discarded;
		drain();
	}
	// No deferred entry contains an object address, including self-deletion.
	auto* pwm = new Pwm(pins, config);
	pwm->setOnFadeDoneCallback([&](uint8_t) { unlocked(); delete pwm; pwm = nullptr; });
	assert(pwm->fadeChan(0, 100, 10));
	Mock::complete(0, 0);
	drain();
	assert(pwm == nullptr);
	pwm = new Pwm(pins, config);
	assert(pwm->fadeChan(0, 200, 10));
	Mock::complete(0, 0);
	poll();
	auto oldIrq = Mock::irq[0][0];
	auto oldArg = Mock::arg[0][0];
	delete pwm;
	ledc_cb_param_t late{0, 0, 200};
	oldIrq(&late, oldArg); // ISR already running at destruction.
	drain();
	assert(FadeWorker::instanceCount() == 0 && FadeWorker::notificationCount() == 0);
	std::cout << "production worker/state-machine tests passed\n";
}
