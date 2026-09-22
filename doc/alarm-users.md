# Managed alarm users, schedules and optional keypad lockout

This feature extends IAS ACE alarm authentication. It does not operate doors,
locks or other actuators. Consumers decide what to do with access decisions.
All examples use synthetic values and an authenticated deCONZ API key.

## Compatibility and activation

Existing single-code installations retain their legacy authentication and sensor
events. Installing this build, reading capabilities, or listing users does not
activate managed users. The first successful user PUT or DELETE activates managed
mode for that alarm atomically. There is currently no API to revert to legacy mode.

`GET /api/<key>/alarmsystems/<alarm>/users/capabilities` reports `managed`,
`max_users` (9), `api_arm_disarm`, `schedules`, `schedule_version` (1),
`protected_primary_slot` (0), `access_event_version` (1),
`rejected_access_events` and `keypad_lockout_version` (1).

Keypad lockout is a separate opt-in, **disabled by default even after enabling
managed users**. Enabling it requires managed mode. Its settings do not change
existing user permissions or schedules.

## User API

The base path below is `/api/<key>/alarmsystems/<alarm>/users`.

| Request | Purpose |
| --- | --- |
| GET base | Object keyed by slot, containing users without PINs or hashes |
| GET base/0 through base/8 | Read one user |
| PUT base/slot | Create with revision 0, or edit with the current revision |
| DELETE base/slot | Delete with JSON body `{"revision": <current>}` |

A user has a stable opaque `id`, `slot`, `name`, `enabled`, `remaining_uses`,
`api_arm_disarm`, `schedule` and `revision`. Returned identities are not names.
Slot 0 is the protected primary user, regardless of its editable name. It cannot
be deleted, disabled, scheduled, usage-limited, or denied REST arm/disarm access.
Its name and PIN can be changed. Integrations caching its PIN must coordinate
credential rotation themselves; deCONZ does not update external applications.

To explicitly activate management while retaining an existing primary PIN, GET
slot 0 and PUT its current revision and unrestricted fields, omitting `pin`:

```json
{"revision":1,"name":"Main","enabled":true,"remaining_uses":null,"api_arm_disarm":true,"schedule":null}
```

Create a secondary user in an unused slot using a string PIN (including leading
zeroes). **Do not use this example PIN on a real system.**

```json
{"revision":0,"name":"Example visitor","pin":"012345","enabled":true,"remaining_uses":5,"api_arm_disarm":false,"schedule":null}
```

PINs must be 4–16 ASCII digits and distinct across slots. Omitting `pin` preserves
an existing credential. Names are at most 64 UTF-8 bytes without control
characters. `remaining_uses:null` means unlimited; 0 means exhausted; finite
values range from 0 through 1,000,000. Guest REST permission defaults to false.

Only accepted physical keypad **disarm** consumes a finite use, including when
already disarmed. Keypad arming and REST authentication do not consume uses.
There is no refund based on a downstream actuator outcome. Accepted keypad
requests update the user's revision, so clients must refetch after conflicts.
REST arm/disarm requires a valid eligible code plus `api_arm_disarm:true`.
Keypad access is independent of that REST permission.

## Schedules

Set `schedule:null` to remove all time restrictions. Otherwise send all four keys:

```json
{
  "timezone":"Europe/Berlin",
  "not_before":null,
  "expires_at":null,
  "windows":[{"day":1,"start":540,"end":1020}]
}
```

Weekdays are Monday=1 through Sunday=7. Times are local minutes since midnight;
start is inclusive, end exclusive, and 1440 is allowed as an end. Split overnight
windows at midnight. At most 28 windows are accepted. An empty list imposes no
weekly restriction, allowing expiry-only policies. Bounds are UTC Unix
milliseconds (or null), inclusive `not_before`, exclusive `expires_at`, between
2020-01-01 and 2100-01-01. If both exist, start must precede end.

The timezone must be available to Qt. Evaluation follows local wall time: skipped
DST minutes do not occur; repeated minutes may qualify twice. Invalid policy or
clock evaluation fails closed as not-ready, not as an invalid-code decision.
Schedules are evaluated at authorization time; no timer rewrites enabled flags.

## Optional brute-force protection

`GET base/lockout` returns a `policy` object and `keypads` status array.
`PUT base/lockout` replaces the complete policy with an optimistic revision check:

```json
{
  "enabled":true,
  "threshold":3,
  "window_seconds":60,
  "durations_seconds":[60,1200,3600],
  "reset_seconds":86400,
  "revision":0
}
```

| Field | Range / behavior |
| --- | --- |
| enabled | Boolean; false by default, independent of managed-user activation |
| threshold | 1–100 distinct wrong-PIN submissions |
| window_seconds | 1–3,600; rolling window, excluding its oldest boundary |
| durations_seconds | Exactly three nondecreasing integer durations, each 1–3,600 seconds |
| reset_seconds | 3,600–604,800; quiet interval before escalation returns to level 1 |
| revision | Required current policy revision; 0 when no policy has been saved |

Defaults are 3 failures within 60 seconds, then 60, 1,200 and 3,600 seconds, with
an 86,400-second quiet reset. Subsequent qualifying bursts repeat level 3.
Settings, deadlines, escalation and counted failures persist in SQLite.
Enforcement is per alarm, source IEEE address and endpoint, not per guessed user.
Failures across valid IAS arm/disarm modes share the same counter for that keypad.

Only PINs matching no stored user count. Recognized but disabled, exhausted or
out-of-schedule users are rejected without counting as guesses. Outside lockout,
a recognized PIN clears the consecutive-failure counter; it does not immediately
reset escalation. The quiet interval is measured from the last counted wrong PIN
and evaluated on the next request. Radio retries do not count again.

The threshold-crossing request starts the next lockout. During it, **all keypad
PINs, including the primary PIN**, receive an invalid-code response. No alarm
state changes, accepted access events, or usage consumption occur. Attempts do
not extend the deadline, count failures, or advance escalation. After expiry a
new burst is required. REST authentication remains separate and unaffected; this
is not protection against guessing through REST or abuse of an authorized API key.

Changing an enabled policy preserves an active deadline. Explicitly disabling
protection clears its counters and state. A backward wall clock relative to a
recorded failure fails closed; operators should correct the clock or explicitly
reset. Expiry uses the gateway wall clock, so trustworthy system time matters.

Each status row has `source`, `endpoint`, stored `level`, `locked_until` (Unix
milliseconds), and `remaining_seconds`. A stored level may remain nonzero after
expiry until the next request applies quiet-time reset. These are administrative
fields; clients should avoid displaying device identifiers unnecessarily.

To immediately clear **all keypad lockouts, failure counters and escalation for
this alarm**, send `DELETE base/lockout` with `{"reset":true}`. The policy remains
enabled. Reset does not validate a PIN, arm/disarm, or issue actuator commands.
All routes require normal deCONZ API authentication. There is no separate owner
role or extra per-key administrator permission in this feature. Restrict API-key
possession; applications should add their own authenticated session and CSRF checks.

## Events and downstream behavior

Managed decisions produce WebSocket events with `t:"event"`, `e:"access"`,
`r:"alarmsystems"`, alarm `id`, `sensor_id`, opaque `event_id`, UTC `timestamp`,
`result`, `action`, and `uses_consumed`. Accepted decisions also carry `user_id`,
`user_slot`, and `remaining_uses`. Rejected decisions omit user identity, names,
remaining counts and credentials, including when a known user is ineligible.

During lockout (including the threshold-crossing request), a rejected event adds
`lockout:true`, `locked_until` and `lockout_level`. Its action remains
`invalid_code` for compatibility. **Consumers interpreting rejection as a
close-only request can retain that behavior during lockout**, subject to their own
motion guards. This plugin makes no inference about actuator movement or success.

Legacy sensor actions/timestamps remain available. For activity counting, use
immutable access `event_id`, not mutable sensor snapshots. The same request
sequence from a keypad within the 10-second duplicate window reuses its receipt
and emits no new access event, sensor action, alarm write or use consumption.
That duplicate window is not a brute-force cooldown. Distinct sequence numbers
represent distinct requests. A sequence collision with different mode or matched
identity fails closed. Storage errors return not-ready and emit no access decision.

## Implementation and review

Schema additions are isolated `*_v1` tables; upstream tables/user_version are not
redefined. The primary hash continues to mirror into legacy secrets. Authorization,
counting, lockout and request receipt commit together before success. Credentials
use the gateway's existing scrypt helpers and never appear in user/event responses.

Regression coverage includes legacy opt-in boundaries, protected primary policy,
PIN rotation and uniqueness, finite uses, REST permissions, schedules/DST,
duplicate receipts, concurrent authorization, rejected-event privacy, rolling
failure windows, escalation/cap, per-keypad isolation, administrative reset,
SQLite reopen persistence, and storage failure. Full Qt plugin builds run in CI.
Physical keypad firmware, Zigbee delivery, and downstream actuator operation still
require integration testing; unit tests do not establish physical outcomes.
