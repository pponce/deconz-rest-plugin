# Named alarm users: phases 2–5

Development branch `alarm-users-v1`, based on upstream a4c17ad (plugin 2.33.2).
This is an experimental deCONZ REST plugin change, not keypad firmware.
Do not replace the household plugin until a full build and supervised commissioning
have passed. Database-core tests are not a physical keypad or Homebridge test.

## Scope and policy

Nine slots, 0–8, per alarm system. Slot 0 migrates the existing main PIN hash
unchanged and remains the only credential accepted by the legacy REST arm/disarm
API used by Homebridge. The main user can be renamed, edited, disabled or deleted;
disabling/deleting it intentionally disables Homebridge alarm commands until a
usable slot-0 code is restored. Changing its PIN also requires updating the
separate private homebridge-deCONZ PIN setting.

Every account has a stable random identity, name, enabled flag, remaining-use
allowance and revision. Renaming/replacing a PIN preserves identity; deletion
removes the credential; recreating a slot creates a new identity. PINs must be
unique across all accounts, including disabled accounts. New/replacement PINs
are 4–16 ASCII digits; retain the length supported by the physical keypad.
The existing legacy hash is migrated without requiring or revealing its PIN.

A use is an accepted physical keypad DISARM request, including already-disarmed.
It is committed before changing alarm state or emitting events. A subsequent
door fault, busy controller, disconnect, or crash does not refund the use.
ARM requests and HomeKit/REST requests do not consume uses, but disabled/exhausted
accounts cannot authenticate. Zero means exhausted; null means unlimited.
Re-enabling or changing a PIN does not refill the allowance. Explicitly edit
remaining_uses to replenish it.

Disabled, exhausted and incorrect PINs produce the existing invalid_code result.
The household controller therefore retains its existing invalid-code close behavior,
subject to its existing ready/busy/position guards. Storage errors produce not_ready,
not invalid_code, to avoid turning database failure into a door-close request.
Expiration and recurring schedules are phase 6 and are NOT implemented here.

## API

Uses the existing deCONZ API-key authorization boundary. No separate admin-role
system is introduced. Use only trusted local clients. Names and identities are
available from the explicit users endpoint; PINs and hashes are never returned.

- GET /api/<key>/alarmsystems/<id>/users
- GET /api/<key>/alarmsystems/<id>/users/<slot>
- PUT /api/<key>/alarmsystems/<id>/users/<slot>
- DELETE /api/<key>/alarmsystems/<id>/users/<slot>

PUT creates with revision 0 and required name/pin. Updates require the revision
returned by GET; omitted fields are preserved. Fields: revision, name, pin,
enabled, remaining_uses. No implicit retries after revision conflicts: GET again
and review. DELETE takes only a revision in its JSON body. Unknown fields, invalid
types and numeric PINs are rejected. Names allow up to 64 UTF-8 bytes without
ASCII control characters. remaining_uses accepts null or an integer 0–1000000.

Example metadata-only change (use current revision, not this illustrative number):

```json
{"revision": 7, "enabled": false}
```

PINs belong in a private client prompt/request body, not command-line arguments,
shell history, tracked settings, logs or chat. The guided management UI is a later
phase. No production PINs or device mappings are included in this branch.

## Events and retries

Original sensor action and lastupdated notifications remain available. New,
accepted, physical requests additionally publish a self-contained WebSocket event:

```json
{
  "t": "event", "e": "access", "r": "alarmsystems", "id": "1",
  "event_id": "random-event-identity", "user_id": "random-user-identity",
  "user_slot": 1, "sensor_id": "example", "action": "already_disarmed",
  "uses_consumed": 1, "remaining_uses": 4,
  "timestamp": "2026-09-21T12:00:00.000Z"
}
```

No PIN/hash/name is broadcast. Resolve names through the management API.
Do not configure both legacy sensor events and access events as motor triggers.
The existing household controller can continue consuming legacy events; consuming
identity events for attribution is a separate integration change, not deployed here.

A durable receipt identifies a keypad request by alarm, source, endpoint and ZCL
sequence. Within 10 seconds, the same matched identity/mode is acknowledged with
the original response without another count, alarm write or event. Conflicting
identity/mode in that window returns not_ready. Invalid retries are also suppressed.
The window is bounded because ZCL sequence numbers are only 8 bits: this is retry
suppression, NOT permanent replay protection or exactly-once physical operation.
Physical tests must confirm the keypad assigns fresh sequences to fresh entries.
Receipts survive process restart. Wall-clock reversal for an existing receipt
fails closed; reliable system time is required.

A crash after commit but before delivery can consume a use without delivering an
action. Events are not replayed after restart. This deliberately favors no repeated
motion; an operator may explicitly restore an allowance after review.

## Storage and compatibility

Additive alarm_users_v1 and alarm_user_requests_v1 tables in the existing gateway
SQLite database. No upstream table-layout or user_version change. Legacy code0
is imported once while present and mirrored on edits; deleting slot 0 also deletes
its legacy secret, preventing resurrection. Transactions use BEGIN IMMEDIATE and
prepared parameters. Counts and request receipts commit together; failed writes
roll back. Stale administration uses optimistic revision checks.

Upstream binaries ignore the new tables and do not enforce their policies. Merely
replacing this plugin with upstream does not preserve multi-user restrictions:
restore the reviewed pre-install DB snapshot too. Preserve a private post-test DB
copy first if retaining new configuration/counts matters. Do not swap databases
while deCONZ is running or let GUI/headless instances access different snapshots.

## Build, staging and recovery gate

Follow BUILDING.md for Qt5 dependencies on Linux Mint. Keep a clean upstream
checkout/worktree at a4c17ad and build it first, then build this branch in a separate
build directory. Both should match the installed deCONZ executable/Qt ABI.
A GitHub Actions workflow builds the complete plugin and runs the isolated store
tests. Fork Actions may need enabling. No action installs onto the household host.

Build without sudo; stage without replacing the live plugin:

```sh
cmake -S . -B build-alarm-users -DQT_VERSION_MAJOR=5 -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-alarm-users --parallel 2
cmake --install build-alarm-users --prefix "$PWD/stage-alarm-users"
```

Staged plugin: stage-alarm-users/share/deCONZ/plugins/libde_rest_plugin.so.
The build fetches deconz-lib according to the upstream build configuration. Record
its exact checkout SHA and build logs with the staged binary before deployment.

Before installation, identify the real headless service, live plugin path and
active DB from the existing private deployment records. Quiesce garage input and
controller operation, stop dependent Homebridge/controller services and deCONZ
in the previously tested maintenance order, and make owner-only backups of the
original plugin, consistent stopped-service DB (including any SQLite sidecars),
custom keypad DDF and service settings. Record hashes/versions privately. Never
upload these. Do not guess paths or run another deCONZ instance.

Commission under supervision: first unchanged upstream build compatibility, then
main-code/Homebridge behavior on the modified plugin, two named users, disable/
re-enable, edit/delete, all nine slots, five-use/sixth-invalid, repeated fresh entries,
restart persistence and retry handling. Invalid/exhausted entries can close the
real door under the existing controller policy; testing must account for that.
No unattended/live installation is authorized merely by publishing this branch.

If a gate fails, stop the services, preserve private failure evidence, restore the
original plugin AND matching pre-install database, preserve the custom DDF, then
restart in the known maintenance order. Confirm no unexpected motion and existing
readiness/status before resuming use. No fault-reset shortcut or command replay.

## Build-only helper

From a clean checkout of this feature branch, run `bash tools/build-alarm-users.sh`.
It stages both the unchanged baseline and current feature commit, using one exact
deconz-lib revision for both (only the dependency GIT_TAG is pinned in the
temporary source worktrees). It runs core tests and records hashes/versions under
ignored, owner-only `.local-builds/`. It does not install packages, replace the
live plugin, access the gateway database, or restart services. Preserve this output
for the separate installation review. Bash syntax is checked; this helper has not
been run on the household host.

## Verification status

Complete Qt5 plugin build and core tests passed in GitHub Actions run
35622450737 for implementation commit b36e9f0. Local address/undefined-behavior
sanitizers also passed all 128 checks with leak detection disabled because the
execution environment cannot support LeakSanitizer. This is build/core evidence,
not household commissioning.

Standalone production store code: tested with SQLite and real scrypt fixtures,
including migration, five uses, duplicate final use, restart, concurrent last-use
requests, disabled/re-enabled users, PIN edits, deletion/recreation, nine-slot cap,
stale revisions, and transaction rollback on failed receipt insertion.
Real IAS messages, Homebridge compatibility and household installation still
require supervised verification; these are not established by build/store tests.
