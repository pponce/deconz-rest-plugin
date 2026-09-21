# Alarm user schedules and expiry — staged, not installed

Builds on the API-permission runtime. The installed household plugin does not yet
include this feature. Run the store and Qt schedule tests and a full plugin build
before updating the separately pinned maintenance installer.

The users API adds `schedule` (null = unrestricted), and capabilities reports
`schedules:true, schedule_version:1`. Existing policies default unrestricted;
GET does not activate managed mode. API permission and physical keypad behavior
remain separate. Any eligible physical PIN still permits both arm and disarm.

A schedule contains exactly:

```
{"timezone":"America/Los_Angeles","not_before":null,"expires_at":null,
 "windows":[{"day":1,"start":540,"end":1020}]}
```

Days are ISO Monday=1 through Sunday=7. Start/end are minutes after local midnight,
start-inclusive/end-exclusive, with 24:00 allowed as an end. Up to 28 windows;
empty windows means no weekly restriction, allowing expiry-only policies.
Split overnight access over separate days. Bounds are nullable UTC epoch
milliseconds, start-inclusive/expiry-exclusive. Accepted range is 2020–2100.
The management interface converts an entered local expiration to one exact UTC
instant, rejecting nonexistent or ambiguous local clock-change times.

Timezone is an installed IANA zone interpreted by Qt's timezone database.
Weekly windows use local wall time: both occurrences of a repeated fall-back
hour match; nonexistent spring-forward times never occur and do not create
catch-up access. Policy is evaluated for each fresh keypad or REST authentication,
not through scheduled jobs or web polling. Restart immediately resumes evaluation
against current time. Stored UTC expiry remains absolute if timezone rules change.

Manual enabled=false always denies access; re-enabling preserves schedule, expiry
and count. No mutation automatically refills uses. Out-of-window/expired physical
codes return ordinary invalid_code, which the household controller may interpret
as closing. Invalid policy, missing timezone evaluator or implausible clock fail
closed; the physical path uses not_ready for policy errors, never invalid_code.
Reliable host time remains required; this is not a trusted-clock implementation.

Existing duplicate-request receipts are checked first. A valid retry crossing a
schedule boundary acknowledges the previously committed request without a new
use, event or alarm action. A fresh request outside the window is rejected.
REST requests still do not consume keypad allowances.

Policies are stored in additive `alarm_user_schedules_v1` rows keyed by stable
user identity, in the same transaction as a user mutation. Deletion removes the
policy; recreating a slot gets a fresh identity and unrestricted default. Main
is not a backend exception; a coordinated Homebridge UI protects whichever user
is designated. `setMainCode` preserves all restrictions.

Validation: database policy tests run without Qt through an injected evaluator;
`tests/alarm_schedule_test.cpp` exercises the production Qt evaluator, including
DST and exact boundaries. `tools/build-alarm-users.sh` now requires both suites.
`tools/build-schedule-update.py` builds only and never changes running services.
No physical result or completed Qt build is claimed by this source change.
