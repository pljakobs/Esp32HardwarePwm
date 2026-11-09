#pragma once

#include <algorithm>
#include <cstdint>

namespace HwPwmTiming
{
struct Selection {
	uint32_t cycles;
	int64_t errorUs;
};

// Inputs must have a nonzero frequency and duty range.
inline Selection selectCycles(uint32_t range, uint32_t durationMs, uint32_t frequency, uint32_t overheadUs,
							  int64_t errorUs)
{
	constexpr uint32_t MAX_CYCLES = 1023;
	const int64_t unit = int64_t(range) * 1000000;
	const int64_t budgetUs = int64_t(durationMs) * 1000 - overheadUs + errorUs;
	// Do not carry unachievable timing debt into later, unrelated fades.
	if(budgetUs <= unit / frequency) {
		return {1, 0};
	}
	if(budgetUs >= unit * MAX_CYCLES / frequency) {
		return {MAX_CYCLES, 0};
	}
	const int64_t scaledBudget = budgetUs * frequency;
	uint32_t cycles = scaledBudget / unit;
	if((scaledBudget % unit) * 2 > unit) {
		++cycles;
	}
	cycles = std::max(uint32_t(1), std::min(MAX_CYCLES, cycles));
	const int64_t actualUs = (unit * cycles + frequency / 2) / frequency;
	return {cycles, budgetUs - actualUs};
}
} // namespace HwPwmTiming
