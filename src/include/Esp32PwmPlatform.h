/**
 * @author  Peter Jakobs http://github.com/pljakobs
 */

/****
 * ESP32 Hardware PWM - Platform Compatibility Layer
 * 
 * This header provides compatibility definitions and feature detection
 * for different ESP32 variants (ESP32, ESP32-C3, ESP32-S2, ESP32-S3, etc.)
 *
 ****/

#pragma once

#include <soc/soc_caps.h>
#include <hal/ledc_types.h>

// Platform-specific capability detection

#define ESP32_PWM_MAX_CHANNELS SOC_LEDC_CHANNEL_NUM

#define ESP32_PWM_MAX_TIMERS LEDC_TIMER_MAX
#define ESP32_PWM_MAX_RESOLUTION SOC_LEDC_TIMER_BIT_WIDTH

// High-speed mode availability
#define ESP32_PWM_HAS_HIGH_SPEED_MODE SOC_LEDC_SUPPORT_HS_MODE

// Speed mode definitions based on platform capabilities
#if ESP32_PWM_HAS_HIGH_SPEED_MODE
#define ESP32_PWM_SPEED_MODE_COUNT 2
#define ESP32_PWM_DEFAULT_SPEED_MODE LEDC_LOW_SPEED_MODE
#define ESP32_PWM_AVAILABLE_SPEED_MODES                                                                                \
	{                                                                                                                  \
		LEDC_LOW_SPEED_MODE, LEDC_HIGH_SPEED_MODE                                                                      \
	}
#endif

// Channel availability per speed mode
#if ESP32_PWM_HAS_HIGH_SPEED_MODE
#define ESP32_PWM_CHANNELS_PER_SPEED_MODE ESP32_PWM_MAX_CHANNELS
#define ESP32_PWM_TOTAL_CHANNELS (ESP32_PWM_MAX_CHANNELS * 2)
#endif

// Timer availability per speed mode
#define ESP32_PWM_TIMERS_PER_SPEED_MODE ESP32_PWM_MAX_TIMERS

// Platform-specific feature flags
#ifndef ESP32_PWM_SUPPORTS_FADE
#define ESP32_PWM_SUPPORTS_FADE 1 // All ESP32 variants support fade
#endif

#ifndef ESP32_PWM_SUPPORTS_PHASE_SHIFT
#define ESP32_PWM_SUPPORTS_PHASE_SHIFT 1 // All ESP32 variants support hpoint
#endif

// Configuration validation macros
#define ESP32_PWM_VALIDATE_RESOLUTION(res) ((res) >= 1 && (res) <= ESP32_PWM_MAX_RESOLUTION)

#define ESP32_PWM_VALIDATE_CHANNEL(ch) ((ch) >= 0 && (ch) < ESP32_PWM_MAX_CHANNELS)

#define ESP32_PWM_VALIDATE_TIMER(tim) ((tim) >= 0 && (tim) < ESP32_PWM_MAX_TIMERS)

#define ESP32_PWM_VALIDATE_SPEED_MODE(mode)                                                                            \
	((mode) == LEDC_LOW_SPEED_MODE || (ESP32_PWM_HAS_HIGH_SPEED_MODE && (mode) == LEDC_HIGH_SPEED_MODE))

// Frequency calculation helpers
#define ESP32_PWM_MAX_FREQ_FOR_RESOLUTION(res, clk_freq) ((clk_freq) / (1U << (res)))

#define ESP32_PWM_MAX_RESOLUTION_FOR_FREQ(freq, clk_freq)                                                              \
	(31 - __builtin_clz((clk_freq) / (freq))) // Log2 of max divisor
