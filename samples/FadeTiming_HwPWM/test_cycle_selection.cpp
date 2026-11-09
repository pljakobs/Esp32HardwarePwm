#include "../../src/FadeTiming.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

int main()
{
	using HwPwmTiming::selectCycles;
	int64_t errorUs = 0;
	uint32_t sum = 0;
	unsigned shortSegments = 0;
	for(unsigned i = 0; i < 5; ++i) {
		auto choice = selectCycles(819, 600, 8000, 0, errorUs);
		assert(choice.cycles == 5 || choice.cycles == 6);
		shortSegments += choice.cycles == 5;
		sum += choice.cycles;
		errorUs = choice.errorUs;
	}
	assert(shortSegments == 1 && sum == 29);
	assert(errorUs == 31125);
	auto exact = selectCycles(800, 600, 8000, 0, 0);
	assert(exact.cycles == 6 && exact.errorUs == 0);
	assert(selectCycles(1000, 550, 10000, 0, 0).cycles == 5); // Ties prefer shorter.
	assert(selectCycles(10, 3000, 8000, 0, 0).cycles == 1023);
	assert(selectCycles(10, 3000, 8000, 0, 0).errorUs == 0);
	assert(selectCycles(819, 600, 8000, 0, -1000000).cycles == 1);
	assert(selectCycles(819, 600, 8000, 0, -1000000).errorUs == 0);
	assert(selectCycles(800, 600, 8000, 100000, 0).cycles == 5);
	assert(selectCycles(800, 600, 8000, std::numeric_limits<uint32_t>::max(), 0).cycles == 1);

	unsigned checked = 0;
	for(uint32_t frequency : {1, 1000, 2000, 4000, 8000, 16000, 32000, 40000000}) {
		for(uint32_t range : {1, 10, 255, 682, 819, 1023, 4095, 1048575}) {
			for(uint32_t duration : {1u, 20u, 500u, 600u, 3000u, std::numeric_limits<uint32_t>::max()}) {
				for(int64_t carry : {-100000LL, 0LL, 100000LL}) {
					auto choice = selectCycles(range, duration, frequency, 1625, carry);
					const long double budget = int64_t(duration) * 1000 - 1625 + carry;
					long double minimum = std::numeric_limits<long double>::max();
					for(uint32_t cycles = 1; cycles <= 1023; ++cycles) {
						const long double actual = static_cast<long double>(range) * cycles * 1000000 / frequency;
						minimum = std::min(minimum, std::abs(budget - actual));
					}
					const long double actual = static_cast<long double>(range) * choice.cycles * 1000000 / frequency;
					assert(std::abs(budget - actual) <= minimum + 0.000001L);
					++checked;
				}
			}
		}
	}
	// Alternating small ranges remain bounded over a long cyclic sequence.
	errorUs = 0;
	for(unsigned i = 0; i < 10000; ++i) {
		uint32_t range = i % 2 ? 19 : 23;
		auto choice = selectCycles(range, 20, 8000, 125, errorUs);
		errorUs = choice.errorUs;
		assert(std::abs(errorUs) <= int64_t(range) * 1000000 / (2 * 8000) + 1);
	}
	std::cout << checked << " exhaustive nearest-cycle comparisons and carry regressions passed\n";
}
