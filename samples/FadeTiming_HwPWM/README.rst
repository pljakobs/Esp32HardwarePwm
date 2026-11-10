Fade timing diagnostic
=====================

No-network ESP32-C3 timing probe for issue #36. The default GPIOs
(5, 4, 1, 0, 3) follow catalog L1 through L5 on the QuinLED An-Penta Mini; change ``pins``
before using another board. Outputs ramp to full duty during the tests.

On the experimental worker branch, split segments reload on the shared
FreeRTOS worker, but logical fade completion and the next logical fade still
wait for Sming. Zero-range holds use independent worker deadlines. The default
reload correction is zero; the previous 1500us estimate and measurements below
predate this architecture and are not worker calibration. Worker measurements
are recorded separately below.

Build with ``make SMING_ARCH=Esp32 SMING_SOC=esp32c3``; add
``SMING_RELEASE=1`` for a release build. Flash using the normal Sming tools.
The sample enables ``HWPWM_TIMING_TRACE`` globally; without that define the
library has no tracing overhead or external hook dependency.

Each case runs three times at 8 kHz/12-bit with automatic phase shifting.
Set ``FADE_TIMING_FREQUENCY=1000`` to repeat at 1 kHz.
Cases compare a single 3000 ms fade, six queued 500 ms fades, six externally
chained fades polled every 20 ms, and 150 alternating full-range 20 ms fades.
Additional cases cover 5-to-95%, downward and cyclic fades, disable reload compensation, or enable 15% spread spectrum
with subsampling 4. Quantisation compensation remains enabled in all cases.
``single-main-load`` blocks the Sming task for two seconds starting 300 ms
into the fade, alternating 1 ms CPU work and one FreeRTOS tick of sleep.
``LOAD`` reports the actual blocked interval; intermediate ``TRACE,0``
records inside it demonstrate worker-independent segment continuation.
The cyclic test explicitly marks completion after its reentrant reset:
reset invalidates the dispatch generation and suppresses queue-empty delivery.

Records are buffered in RAM and printed only after a run. ``TRACE`` columns
are event, channel, microseconds since run start, target duty, and the duration
requested in milliseconds (after overhead subtraction on the fast path).
Events: 0 before the IDF start call, 1 after it returns, 2 fade-completion ISR,
3 Sming dispatcher. Event 4 uses the final two columns for live duty range and
selected cycle count (0 means the IDF time-based path). ``END`` measures
task-context completion, not physical output edges. ISR timestamps include
IDF interrupt handling latency; use a logic analyser to measure physical edges.

``RESULT`` reports API/queue failures and trace overflow. A 15-second watchdog
reports stalled runs. ``ALL_TESTS_DONE`` marks the end of the suite.

Offline timing calculator
-------------------------

Run ``python3 fade_math.py`` for a frequency/resolution matrix, or
``python3 fade_math.py --frequency 8000 --bits 12`` for the test board.
No dependencies or hardware are required. Run its tests with
``python3 -m unittest -v test_fade_math`` from this directory.

The calculator mirrors proportional splitting with enough queue space, then
compares IDF's truncated cycle count against the nearest integer cycle count.
A third column carries timing error between segments to distribute rounding.
The same split boundaries are used in all three methods; no extra segments
are inserted. Cycle counts are clamped to the hardware range 1..1023.
Fast fades retain IDF's scale selection, so this experiment does not improve
the scale>1 regime. Up/down fades use the same absolute range model.

These are duty-step durations, not predicted callback timestamps: endpoint
snaps, IDF internal reloads, timer-edge alignment, spread spectrum, application
polling, and the library's existing compensation are excluded. An optional
``--reload-us 125`` adds a hypothetical constant gap between library segments.
The carried method accounts for that gap when selecting subsequent counts.
Errors are reported against the original requested duration, including any
millisecond truncation introduced when splitting.

Native selection tests
----------------------

The firmware uses ``src/FadeTiming.h`` for nearest-cycle selection. Test that
exact implementation on the host without Sming or IDF:

::

    g++ -std=c++17 -Wall -Wextra -Werror -pedantic test_cycle_selection.cpp -o test-cycle-selection
    ./test-cycle-selection
    rm test-cycle-selection

Tests compare against every legal cycle count for 1152 input combinations,
exercise signed carry over 10000 steps, and check saturation, tie breaking,
overhead subtraction and large durations. The firmware rounds residual time
to microseconds; the Python experiment uses exact rational arithmetic.
For production-source worker/state-machine tests, run
``make -C ../../tests/native test``.

Worker hardware results (2026-11-10)
-----------------------------------

ESP32-C3 QuinLED An-Penta Mini, release, 8 kHz/12-bit, catalog channel
order, zero reload correction, three repetitions per case. All 36 runs
completed without API/queue failures or trace overflow. End duties matched
their targets; no panic or watchdog was observed.

===========================  ==========================
3000 ms nominal              Observed END range (us)
===========================  ==========================
Single full range            2970115--2970472
Six queued 500 ms chunks     2987079--2987689
Six external 500 ms chunks   3120901--3120978
150 queued 20 ms chunks      3113010--3113135
5-to-95%                     2995938--2996225
Full downward                2970175--2970451
Six cyclic 500 ms chunks     2985049--2985499
Single under main-task load  2970123--2970694
Single, spread spectrum      2871370--3175052
External, spread spectrum    2960956--3040999
===========================  ==========================

Default/explicit-zero cases are equivalent: the explicit-zero single case
was 2970188--2970488 us, and its 20 ms sequence was 3112993--3113013 us.
The approximately 30 ms short single fade and 113 ms long microfade chain
remain timing limitations, not lost callbacks. Spread spectrum is not a
strict wall-clock timing mode.

With the Sming task blocked from approximately 301 ms to 2301--2308 ms,
CH0 restarted split segments near 615, 1229 and 1844 ms in every run.
Across the unloaded single-fade cases, 60 intermediate ISR-to-start-call
gaps measured 19--49 us. This excludes the IDF call's execution time and
hardware timer-edge alignment; it is not a calibrated overhead setting.
``END`` includes callback dispatch, not physical output timing.

The callback sample separately validated FIFO rearm, streaming refill,
CYCLIC reset, zero-range holds and two instances. Hardware startup exposed
an early ``esp_timer_create`` invalid-state failure for global objects;
the worker now retries after timer-service startup. Teardown now records
successful timer configuration even without a frequency setter.

Earlier dispatcher results
--------------------------

ESP32-C3, 8 kHz/12-bit, five configured outputs, three runs per case in
debug and release builds, with networking and spread spectrum off:

========================  ===================  ====================
3000 ms requested         Before correction    With carried cycles
========================  ===================  ====================
Full range                about 2560 ms        about 2970 ms
Six queued 500 ms chunks   about 2560 ms        about 2987 ms
5-to-95%                  not measured         about 2996 ms
Full range downward       not measured         about 2970 ms
========================  ===================  ====================

Endpoints and completion callbacks were checked. Fast 20 ms sequences retain
their previous timing (about 2872 ms with reload compensation for 150 steps).
Externally restarting six 500 ms chunks resets carry on every call and
takes about 3121 ms; application scheduling remains outside this correction.
Spread spectrum produces additional variation and is not included in the table.

A separate 1 kHz/12-bit debug run confirms the remaining scale-based limitation:
a nominal 3000 ms full-range fade still takes about 4100 ms, and a 5-to-95%
fade about 3690 ms. Existing reload compensation does not remove that error.
