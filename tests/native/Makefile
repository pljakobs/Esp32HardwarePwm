CXX ?= g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wno-unused-variable -Wno-unused-parameter -pthread -g

.PHONY: test clean
test: test_worker test_worker_classic test_cycles
	./test_worker
	./test_worker_classic
	./test_cycles

test_worker: test_worker.cpp ../../src/Esp32HardwarePwm.cpp ../../src/FadeWorker.cpp ../../src/FadeWorker.h ../../src/include/Esp32HardwarePwm.h stubs/sdk.h
	$(CXX) $(CXXFLAGS) -Istubs test_worker.cpp ../../src/Esp32HardwarePwm.cpp ../../src/FadeWorker.cpp -o $@

test_worker_classic: test_worker.cpp ../../src/Esp32HardwarePwm.cpp ../../src/FadeWorker.cpp ../../src/FadeWorker.h ../../src/include/Esp32HardwarePwm.h stubs/sdk.h
	$(CXX) $(CXXFLAGS) -DSOC_LEDC_SUPPORT_FADE_STOP=0 -Istubs test_worker.cpp ../../src/Esp32HardwarePwm.cpp ../../src/FadeWorker.cpp -o $@

test_cycles: ../../samples/FadeTiming_HwPWM/test_cycle_selection.cpp ../../src/FadeTiming.h
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f test_worker test_worker_classic test_cycles
