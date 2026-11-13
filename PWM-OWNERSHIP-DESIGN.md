# Draft: Controller-Owned PWM Resources

Status: first implementation is under review on coordinated
`feature/central-pwm-ownership` branches in HardwarePwm, RGBWWLed (based on
`0.11/develop`) and firmware (based on `5.1/develop`). It has been flashed and
fade-timed on an ESP32-C3 controller. The original plan below remains as rationale and validation
guidance; the implementation adjustments in this section take precedence.

### Implementation adjustment: allocate the board, not the current light set

At the user's direction, APPLedCtrl loads the selected board profile from the
installed gzip catalog, or its embedded seed when no installed file exists.
All pins allowing `light.pwm` are allocated once at zero in board-profile order,
including unclaimed outputs. SoC capacity/capability checks and input/bus
conflict checks precede hardware changes. The existing uzlib component decodes
the bounded catalog; no new PWM engine or GPIO-free channel registry is added.

Light bindings map GPIOs into these stable board banks. Adding, deleting or
reordering lights does not rebuild banks; unchanged light instances retain their
animation queues and duty. Deleting the last light retains the backend.
Only board/revision/SoC or physical PWM-setting changes rebuild it. Thus the
original allocation-order planner in step 3 is superseded by board-order
mapping, and reload detaches only removed/rebound views when banks are unchanged.

HardwarePwm has channel-scoped update/pending APIs; RGBWWLed has a validated
borrowed init path alongside retained owning constructors. Explicit shutdown
destroys views before banks. Native tests exercise actual worker/output code,
board mappings and gzip profile decoding. Physical reload/failure-injection
and waveform validation remain required before considering this a release.

## Goal

Make physical PWM resources and their settings controller-owned. RGBWWLed
instances represent logical lights and consume assigned output channels; they
must not create or configure independent hardware backends for shared timers.

The application has one central PWM backend, potentially composed of two bank
instances on classic ESP32. Bank count is a hardware detail, not a function of
the number of lights. Settings remain centrally owned even when multiple
banks or timers are needed.

## Motivation and evidence

The current RGBWWLed output model constructs PWM instances per light. Although
FadeWorker is shared, it iterates registered instances and services each
instance's spread-spectrum deadline. `handleSpreadSpectrum()` changes the
frequency of the instance's LEDC timer. Multiple lights using the same physical
timer can therefore request independent changes to the same timer.

Important qualification from the current firmware core:
`planPwmChannels()` already uses `spreadSpectrumOwner[2]` to enable modulation on
only the first participating light in each bank. Duplicate modulation is
therefore a library/API ownership risk, not an established behavior of the
current firmware allocation path. The current workaround couples timer service
to one arbitrarily selected light; it does not establish central ownership.

Frequency, resolution and spread spectrum are timer properties, not independent
per-light properties. Per-light ownership obscures that relationship and can
multiply timer updates and worker wakeups.

During a steady low-brightness test on ESP32-C3, visible flicker disappeared
after spread spectrum was disabled. This implicates frequency modulation as a
trigger, but does not establish whether the cause is timer-update transients,
physical output response, redundant updates, or another interaction. Ideal
frequency modulation preserves duty ratio. This redesign must not be presented
as a proven flicker fix without waveform measurements.

## Ownership model

| Layer | Owns | Does not own |
| --- | --- | --- |
| Firmware/controller | Physical output configuration, backend lifetime, channel assignments and reconfiguration policy | Per-light animation policy |
| Hardware PWM backend | LEDC banks/timers/channels, GPIO bindings, hardware fade execution and shared scheduling | Colors, light identities or application animations |
| RGBWWLed output view | Non-owning mapping from logical channels to assigned backend channels | Timer configuration, global fade-service lifetime or backend destruction |
| RGBWWLed light | Color conversion, brightness/calibration, animation queues and logical transitions | Physical PWM resources |

Central ownership means an explicitly application-owned resource, not a
mandatory process-global singleton. It should remain possible to construct
isolated backends in tests.

### Timer settings

- Identify each timer by speed mode and timer number.
- Configure frequency, resolution and spread-spectrum policy centrally and
  exactly once per timer configuration.
- Keep phase placement centrally coordinated across the physical channel set;
  LEDC hpoint remains a per-channel value.
- Allow distinct explicit timer configurations where hardware requires them.
  One backend does not imply one frequency, one resolution or one speed mode.
- Reject conflicting channel claims and timer configurations before changing
  live hardware.
- Define channel phase distribution from physical assignments, not each
  light's local channel count.

### Channel views

A light receives a non-owning output view with a stable logical-to-physical
mapping. Preserve channel order and named/indexed RAW behavior for all existing
light types, including a light spanning two classic ESP32 banks.

Views expose only the operations needed by RGBWWLed: duty updates, prepared
hardware fades, cancellation and progress/completion queries. They do not expose
timer-wide setters. Concrete type and method names should be selected during
implementation using the existing output abstraction, rather than introducing
a parallel rendering path.

Backend lifetime exceeds the lifetime of every attached view. Destroying a light
detaches its assignments and cancels its pending work, but does not destroy a
shared timer or affect other lights. Reassignment must invalidate old completion
notifications using assignment generations or an equivalent checked identity.

## Scheduling and threading

- Retain one shared hardware worker, not a worker per light or bank.
- Keep one spread-spectrum schedule per physical timer. A due timer gets one
  frequency update per service pass, independent of how many lights use it.
- Coalesce overdue timer work; do not replay a backlog of missed modulation
  deadlines.
- Maintain per-channel hardware fade execution, including split-segment
  progression and cycle-limited prepared fades.
- Keep application callbacks deferred to the normal Sming context. Do not move
  ConfigDB, JSON-RPC, pooled strings or network operations into the PWM worker.
- Preserve existing locking and ISR/task boundaries; channel views must not
  introduce unsynchronized access to shared channel state.
- Do not change the existing fade-done callback ordering contract incidentally.
  Prepared hardware segments may advance without waiting for a 20 ms light tick,
  but logical animation policy remains with RGBWWLed.

This changes resource ownership and timer scheduling, not application event
publication or JSON-RPC generation.

## Allocation and runtime reconfiguration

Firmware resolves the complete hardware configuration and validates bank
capacity, unique GPIO/channel claims and timer compatibility before applying it.
Reuse the existing channel planner where possible.

Reconfiguration must have a documented prepare/apply boundary:

1. Validate the prospective resource plan without disturbing current outputs.
2. Quiesce affected views and invalidate queued work for assignments being
   replaced.
3. Apply the physical configuration in a safe output state and attach new views.
4. Publish the new logical-light configuration only after successful activation.

Do not assume that old and new hardware allocations can coexist during staging:
they may claim the same LEDC channels. Validation failures retain the old setup.
Hardware-application failures must be surfaced explicitly, with a defined safe
state and recovery path rather than partial success.

Preserve the classic ESP32 limitation around cancelling a running hardware fade;
central ownership does not add unsupported hardware capabilities.

## Repository changes

### Esp32HardwarePwm (this repository)

- Establish backend ownership by bank/timer/channel rather than by light.
- Separate channel access from timer-wide configuration.
- Move spread-spectrum state/deadlines to physical timer ownership.
- Preserve shared fade service, worker synchronization and safe late-callback
  handling.
- Add native allocation, scheduling, lifetime and isolation regressions.

### RGBWWLed

- Adapt the existing PWM output abstraction to consume externally owned channel
  views.
- Stop constructing ESP32 hardware PWM instances in each light.
- Preserve color conversion, animation behavior, RAW ordering, duty scaling,
  progress reporting and completion semantics.
- Keep other platforms supported; do not force ESP32-specific ownership details
  into their output APIs.

### Firmware

- Create and own the central backend and its physical configuration.
- Allocate views for configured lights, including cross-bank mappings.
- Coordinate runtime reconfiguration and teardown.
- Continue using the existing configuration schema unless a demonstrated
  requirement needs a compatible schema change.

### Sming

No framework change is required by this proposal. Reassess only if the existing
component interfaces cannot support the chosen lifetime or channel-view model.

## Migration

1. Add backend/channel-view support and native tests in HardwarePwm without
   removing the existing API.
2. Adapt RGBWWLed to externally supplied views, retaining a deliberate
   compatibility route for existing users during migration. Both routes must
   share hardware execution logic.
3. Wire controller-owned allocation into firmware and validate runtime reloads.
4. Measure behavior on ESP32-C3 and classic ESP32 before changing defaults or
   retiring legacy ownership.

Dependency pins must follow tested library commits. Keep each repository's
change independently buildable throughout the migration.

## Acceptance criteria

- At fixed timer settings, increasing light count does not multiply
  spread-spectrum schedules or writes to the same timer.
- A classic ESP32 configuration with 15 SINGLE lights uses centrally owned
  banks and preserves all output mappings.
- A light can span banks without changing logical channel order.
- Duplicate claims, over-capacity plans and incompatible timer settings fail
  explicitly before live reconfiguration.
- Destroying, replacing or cancelling one light does not reset another light's
  duty, fades or timer settings.
- Stale ISR/deferred completions cannot act on a reassigned channel.
- Existing fade timing, queue progression, callback ordering and idle-duty
  regressions continue to pass.
- Other output backends remain buildable and behaviorally compatible.

## Measurement plan

Compare matched workloads before and after the migration, at identical PWM
settings and brightnesses:

- Hardware timer frequency writes per second, worker passes, timer wakeups and
  scheduling lateness, grouped by physical timer.
- LED update cadence, callback duration distributions and dispatcher batch
  tails; distinguish total work from deadline misses.
- Static low-duty output and fades, with spread spectrum on/off and phase shift
  on/off.
- GPIO pulse widths and duty ratio around frequency changes, plus physical
  output response where measurement equipment permits.
- One light, several lights sharing a timer, and 15 SINGLE lights on classic
  ESP32.

Do not infer CPU utilization from inclusive wall-clock measurements or claim
the low-brightness flicker resolved from software timing data alone.

## Core-grounded implementation plan

The following is the proposed first implementation, based on the checked-out
sources. API names below are proposed, not existing methods.

### Relevant existing code

| Repository / file | Current responsibility | Required change |
| --- | --- | --- |
| HardwarePwm `src/Esp32HardwarePwm.cpp`, constructor / destructor | Allocates contiguous channels for a pin vector; configures a timer; registers with FadeWorker; tears down its channels | Reuse as the centrally owned bank resource, not a light resource |
| HardwarePwm `src/FadeWorker.cpp`, `service()` | Iterates instances, drives each instance's modulation deadline and channel fades | With one instance per configured bank/timer, keep this worker; no second scheduler |
| HardwarePwm `src/FadeWorker.cpp`, `dispatch()` | Uses instance identity and per-channel generation, but instance-wide callback delegates/in-flight count | Add channel-scoped pending query; do not attach per-light delegates to a shared bank |
| RGBWWLed `RGBWWLedOutput.cpp`, `collectActivePins()` / constructors | Compacts logical pins into local bank indices and constructs up to two PWM objects per light | Add externally supplied channel bindings; do not compact them back to per-light indices |
| RGBWWLed `RGBWWLedOutput.cpp`, destructor / `setOutput()` | Deletes both PWM objects; calls each bank's whole-instance `update()` | Separate owned compatibility resources from borrowed bindings; flush only bound channels |
| RGBWWLed `RGBWWLedOutput.h`, `hasPendingCallbacks()` | Tests both complete per-light instances | Query only this view's assigned channels |
| RGBWWLed `RGBWWLed.cpp`, `init()` | Configures animation channels and allocates an owned PWMOutput | Add externally bound init overload with explicit success/failure |
| RGBWWLed `RGBWWAnimatedChannel.cpp`, `process()` | Polls channel fade progress and submits cycle-limited fades through PWMOutput | Preserve this path; no new animation implementation |
| Firmware `app/ledctrl.cpp`, `planPwmChannels()` | Allocates physical channel ranges, bank mask and per-light timer configs; elects spread-spectrum owners | Produce central bank pin lists and per-light bindings instead |
| Firmware `app/ledctrl.cpp`, `reload()` | Stages light objects, zeros/destroys old lights, then initializes replacements | Own bank lifetime separately and check every bank/view initialization before publishing success |
| Firmware `include/pwmchannelplanner.h`, `plan()` | Allocates bank capacities and identifies split-bank logical channels | Retain algorithm; derive explicit mappings from its output |

### Decision: reuse bank objects; avoid a new hardware engine

For the firmware's current schema, create at most two `Esp32HardwarePwm`
objects, one per populated speed-mode bank using its configured timer. Store
them in a firmware `PwmResources` owner (`unique_ptr` per bank), with complete
physical pin lists in LEDC channel allocation order. Do not construct objects
for empty banks.

`APPLedCtrl` owns `PwmResources`: declare the resource owner in `include/ledctrl.h`
and implement its creation, configuration and teardown in `app/ledctrl.cpp`.
Do not add another PWM owner to Application or introduce a file-global PWM
singleton. APPLedCtrl already coordinates light allocation, reload and update
cadence, so physical output ownership belongs at that boundary.

Logical lights currently live in `app.lights`, outside APPLedCtrl. Do not rely on
Application member declaration order to protect borrowed bank pointers.
APPLedCtrl must explicitly stop its update timer, prevent further reload work,
detach/clear all light views and only then release its banks during teardown.
Check the Application shutdown path to ensure the light container remains alive
when this cleanup runs. If necessary, call an idempotent APPLedCtrl shutdown
method from Application before member destruction. Moving the light container
is not required for this migration.

This makes existing per-instance modulation deadlines per-timer deadlines for
the migrated path. Do not add a timer registry to FadeWorker merely to duplicate
the ownership already established by the owner. If future configuration adds
several timers in one speed mode, extend the owner to keyed bank/timer resources;
the first implementation need not expose that unused configuration.

Legacy direct constructors can still create multiple instances on a timer.
Document that the centralized firmware path does not use this arrangement;
do not claim a library-wide unique-timer invariant until a separate shared-timer
API migration enforces it.

### Step 1: channel-scoped HardwarePwm operations

Modify `src/include/Esp32HardwarePwm.h`, `src/Esp32HardwarePwm.cpp` and
`src/FadeWorker.cpp`:

1. Add `bool updateChan(uint8_t channel)`. Extract the existing pending-write
   latch into a shared helper used by this method and `update()`. Preserve the
   completed-fade replay fix: never call `ledc_update_duty()` without a pending
   write, and never latch an active fade. Return/log hardware failures.
2. Add `bool hasPendingCallbacks(uint8_t channel) const`, retaining the existing
   aggregate query. Check ISR signals, awaiting/dispatching state, queued
   notifications and executing callbacks for that channel. Add channel identity
   to any execution bookkeeping currently represented only by
   `callbacksInFlight_`; preserve aggregate lifetime semantics.
3. Keep channel cancellation on `resetQueue(channel)` / existing abort helpers.
   Verify cancellation invalidates queued completions before reassignment.
   Do not add worker-side light callbacks or a second queue.
4. Retain the current instance-wide delegate API for standalone clients.
   RGBWWLed currently polls fades and does not install these delegates, so
   per-light callback routing is not required for this migration. If that
   changes, introduce channel-scoped subscriptions instead of overwriting
   shared-bank delegates.

Exit gate: native tests cover staged-write isolation, channel-specific pending
queries including an executing callback, cancellation and old-generation
notifications. Existing `make -C tests/native test` still passes.

### Step 2: borrowed RGBWWLed channel mappings

Modify `RGBWWLedOutput.h/.cpp` and `RGBWWLed.h/.cpp`:

1. Define an ESP32-only binding value with `Esp32HardwarePwm* bank` and a bank
   local channel index; a five-element mapping follows RED/GREEN/BLUE/WW/CW.
   A null bank means an unused logical channel. Backend-local indices differ
   from GPIO numbers and from each light's local compacted indices.
2. Add a PWMOutput construction path for bindings. Validate initialized banks,
   channel bounds and duplicate bindings. Expose initialization status rather
   than silently accepting an unusable output.
3. Add a `bool RGBWWLed::init(...)` overload taking light type, bindings and
   the SINGLE channel choice. It owns the PWMOutput adapter, not its banks.
   Allocate/validate the new adapter before replacing an existing valid output.
4. Retain existing pin/config constructors as compatibility overloads. Store
   their owned banks separately (`unique_ptr`); both constructor paths populate
   the same binding map and use exactly the same duty/fade methods.
5. Route `setChannel`, `getChannel`, fade/cycle-limit operations, queue reset
   and pending queries through bindings. Preserve the existing conversion from
   16-bit logical duty using the bound bank's `getMaxDuty()`, rounding and
   hardware-range readback semantics.
6. `setOutput()` stages only mapped channels, then calls `updateChan()` only
   for those channels. Never use whole-bank `update()`, `stopAll()` or global
   callback setters from a borrowed view.
7. Borrowed-view destruction cancels only its channels and releases its bindings.
   It neither deletes banks nor reconfigures timers. Firmware must quiesce views
   before channel reassignment; concurrent destruction remains unsupported.

Do not modify color conversion, animation queues, command wire format or the
cycle-limited fade algorithm. Keep ESP8266 and fallback output branches intact.

Exit gate: native adapter tests exercise two lights sharing a bank, a split-bank
light, every logical channel mapping, duty rounding, per-view pending queries,
view destruction and compatibility-owner destruction.

### Step 3: central firmware allocation

Modify `include/pwmchannelplanner.h` only if a shared mapping helper is needed;
otherwise retain it and add conversion in `app/ledctrl.cpp`:

1. Keep `makePwmBankConfigs()` as the source of central bank settings.
2. Replace `ResolvedLight::pwm` with resolved logical channel assignments.
   Build ordered bank GPIO vectors using the planner's primary/secondary start
   positions and secondary-channel mask.
3. For each light, map its active logical channels to `(bank, bank-local index)`.
   Inactive logical channels consume no physical slots. Test the boundary where
   a single light crosses from primary to secondary.
4. Have APPLedCtrl construct and own one bank from each nonempty vector with
   `channelStart=0`, central
   timer config and spread-spectrum policy. Require `isInitialized()` on every
   bank; surface failure through `reload(error)`.
5. Remove `spreadSpectrumOwner[2]` election and per-light frequency/resolution/
   spread settings. The bank now owns the policy directly.
6. Pass bindings to the new RGBWWLed init overload and check its result.
7. AUTO phase placement is computed over the complete populated bank vector,
   not separately per light. This intentionally changes phase positions for
   multi-light setups: document and test it, and compare peak current/flicker.
   Do not imply phase synchronization between independent speed-mode timers.

Keep the existing hardware configuration JSON unchanged. Independent lights
remain independent at the command/animation layer.

### Step 4: reload and failure handling

Implement the following state transitions in `APPLedCtrl::reload()`:

- **Prepare:** validate pins, timer feasibility, capacity and mappings; allocate
  light/adaptor bookkeeping before disrupting output. Preserve the active setup
  on all failures in this phase.
- **Quiesce:** stop new light updates on the dispatcher, honor pending callback
  handling, clear each old light's animations and request zero output. Preserve
  classic hardware cancellation limitations; do not spin waiting on a callback
  that requires the dispatcher currently executing reload.
- **Detach:** destroy old views/lights before old banks. No borrowed pointer may
  survive bank destruction.
- **Apply:** construct new banks, verify hardware initialization, attach views,
  apply light calibration, then replace active state and resume updates.
- **Failure after detach:** log and return a specific error, destroy staged
  lights before staged banks, leave no active logical lights, and attempt to
  place all affected outputs in a safe off state. Do not claim physical ramp
  cancellation on unsupported classic hardware. Do not update
  `_activeConfiguration` or emit `lights_changed` as a success.

First-version rollback is deliberately not automatic hardware rollback: old and
new channel allocations cannot coexist, and the current code already destroys
the old setup before initializing the new one. An explicit safe failure is
preferable to pretending the old state survived. A later rollback feature would
need a separately tested reconstruction plan.

Handle an empty configuration by detaching all lights and releasing all banks.
Keep `PwmResources` under APPLedCtrl ownership throughout. Ensure its shutdown
and failure cleanup are explicit, not dependent on incidental local-variable
or Application member destruction order. APPLedCtrl's destructor/shutdown must
invalidate queued reload callbacks, stop LED updates, destroy light views and
then release banks; shutdown must be safe to call more than once.

### Step 5: required tests and build gates

| Scope | Required checks / command |
| --- | --- |
| HardwarePwm | Extend `tests/native/test_worker.cpp` and mocks; `make -C tests/native test` for both fade-stop capability variants |
| RGBWWLed | Add a native mock-bank binding test and runner; extend `samples/MultiLight` to demonstrate controller-owned banks without removing legacy sample coverage |
| Firmware planner | Extend `tests/pwm_channel_planner_test.cpp`; run `python3 -m pytest -q tests/test_pwm_channel_planner.py` |
| Firmware reload | Add injected bank/view initialization failures, invalid plans preserving the old setup, post-detach safe failure, empty config and repeated reload tests |
| Existing behavior | Run light notification, RAW/HSV profile and fade progression regressions; ensure SINGLE remains mapped to its configured logical channel |
| Platform builds | Build ESP32-C3, classic ESP32 and Host; verify unaffected ESP8266 support in RGBWWLed/compatible firmware line |
| Physical checks | One-channel steady duty and fades, shared-bank lights, split-bank light and classic 15-SINGLE setup; change one light while others fade |

Measure one central backend on C3 and two populated banks on classic ESP32.
For each populated timer, count spread-spectrum frequency writes at fixed
frequency/subsampling while varying only the number of logical views. Counts
must not grow with view count. Test return codes from timer writes explicitly;
the existing `handleSpreadSpectrum()` ignores `ledc_set_freq()` failure, so this
surface needs checked errors as part of central timer control.

### Commit sequence and scope limits

1. HardwarePwm channel-scoped latch/pending APIs and native tests.
2. RGBWWLed borrowed bindings, compatibility ownership and adapter tests.
3. Firmware APPLedCtrl-owned banks/planner/reload/shutdown integration, tests and
   dependency pins.
4. Physical validation report and any separately justified waveform fix.

Do not combine this migration with JSON-RPC deferral, ConfigDB threading,
brightness/gamma changes, a new fade engine, or a speculative flicker fix.
Do not rework all legacy shared-timer users merely to migrate the firmware.
