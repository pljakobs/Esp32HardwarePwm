Callbacks_HwPWM
===============

Demonstrates the per-channel fade queue API of ``Esp32HardwarePwm``.

Four channels demonstrate callback-driven sequencing at 8 kHz/12-bit:

Channel 0 — **FIFO mode** (default)
    Five fades, including an initial 500 ms zero-range hold.
    ``onFadeDone`` reports each logical completion.

Channel 1 — **FIFO rearm**
    ``onQueueEmpty`` re-arms a three-step sequence twice, giving nine fades.

Channel 2 — **CYCLIC mode**
    Four logical entries repeat until the third wrap callback resets the
    queue. Wrap is reported when the final physical entry is selected,
    not after it completes. Reset suppresses that entry's completion;
    eleven logical completion callbacks and three wraps are expected.

Channel 3 — **Streaming FIFO**
    A four-entry queue is refilled from ``onFadeDone`` until twenty steps
    have completed.

Queue capacities allow for split hardware segments at 12-bit resolution.
``DONE``, ``EMPTY`` and ``WRAP`` records contain channel, count and elapsed
microseconds. ``ALL_TESTS_DONE,ok=1`` confirms expected callback counts and
no queue errors; all outputs are then set to zero.

Hardware
--------

On ESP32-C3 the default is the catalog QuinLED An-Penta Mini mapping:
L1/L2/L3/L4 = GPIO 5/4/1/0. A second instance on L5 = GPIO 3 uses hardware
channel 5 and timer 1, demonstrating a 750 ms hold plus two fades while the
first instance runs. Its three ``AUX_DONE`` callbacks and one queue-empty
callback are also checked. USB Serial/JTAG is used when available.

Other targets retain GPIO 13/12/14/27; adjust for your board and avoid
ESP32 classic GPIO 6–11 (flash) and GPIO 34–39 (input-only).

Hardware validation on ESP32-C3 passed with completion counts 5/9/11/20,
queue-empty counts 1/3/0/1, three wraps and three auxiliary completions.
Serial timestamps validate software behavior, not physical light output.

Experimental worker branch
--------------------------

Intermediate hardware segments and zero-range hold deadlines run independently
on one shared FreeRTOS worker. All user delegates still run on the Sming task,
outside the worker mutex. ``onFadeDone`` is a sequencing hook: it runs before
the next logical queue entry starts and may clear, replace or enqueue work.
Wrap and error callbacks are deferred (including errors returned synchronously
by the API); failed Sming posts retry without dropping notifications.
``hasPendingCallbacks()`` reports pending/executing notifications, not the
duration of a running fade. Destruction safely cancels outstanding events.
