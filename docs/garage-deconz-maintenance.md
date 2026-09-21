# Private backup and supervised household installation

This is a household deployment procedure for the existing commissioned
garageDoorController service. It is separate from the generic alarm-user feature
and is not proposed as an upstream deCONZ deployment tool.

## Locations and privacy

Everything stays under the existing deconz-rest-plugin Git checkout:

- `.local-builds/`: staged baseline/feature binaries and build evidence.
- `.local-backups/<UTC-time>-<action>-<random-id>/`: one immutable payload per
  maintenance attempt, plus its durable progress receipt.
- `.local-backups/maintenance.lock`: prevents two helpers running concurrently.

Both directories are ignored by Git. Before writing a snapshot the helper checks
Git's actual ignore result and refuses if any backup files are already tracked.
Do not force-add them. They contain gateway credentials and private device data.
Directories are mode 700 and stored files mode 600, owned by the invoking login
user. No credentials are printed. The raw service definitions are private too.
Snapshots are retained until the owner explicitly chooses to remove them.

Each snapshot contains `deconz-data/` (the entire active application directory,
including zll.db, SQLite sidecars if present, configuration and custom devices),
`plugin.so`, `service-configs.json`, and `receipt.json` (paths, original ownership,
modes, hashes and maintenance stage). The original data directory is copied only
after deCONZ and its dependent services have stopped and no other gateway process
remains. SQLite integrity checking runs on a disposable private copy, including
WAL content, and does not modify the retained snapshot. Temporary validation
files stay inside the snapshot and are removed on success.

The tool does not change Homebridge/controller settings, unit definitions, boot
enablement, API keys, PINs, user allowances or the custom DDF. Starting deCONZ can
perform its ordinary persistence and, after explicit use of the user API, user-table migration.
Homebridge/controller settings are not separately backed up by this tool because
they are not edited; existing private project recovery files remain in place.

The helper itself runs with sudo, so its `hb-service stop/start` calls already
have root privileges. It verifies homebridge.service actually stopped before
stopping deCONZ, and verifies startup before dependency checks. It does not fall
back to direct systemctl Homebridge control if hb-service is unavailable.

## Preconditions and safeguards

Run `sudo python3 -B tools/garage-deconz-maintenance.py ...` from the checkout in
an interactive terminal. It derives the commissioned garage checkout and private
package from the owned controller service definition, without printing its
command line or credentials. Unsupported service layouts stop for review.

Before planned maintenance all three services must be active, controller ready
and idle, both inputs ready, and the door closed with the bolt reporting locked
and the alarm disarmed. The existing project code checks the real DDF hash,
device/resource identities, alarm membership, door/bolt state and durable fault
journal. Reported bolt state remains relay feedback, not a physical bolt sensor;
the owner must also confirm physical state. Keep everyone away from all garage
controls and the keypad during maintenance. Keep SSH connected; do not reboot.

The helper uses the established order:

1. Stop the controller and acquire its operation lock; recheck physical feedback.
2. Stop Homebridge with `hb-service stop`, then deCONZ; verify every service and gateway process stopped.
3. Copy and verify the private snapshot before any plugin replacement.
4. Start deCONZ, verify the same executable/account/database and loaded plugin.
5. Start Homebridge with `hb-service start`; revalidate the DDF, gateway mapping and fresh door/bolt state.
6. Ask for `STILL` after the owner checks Home accessories and closed/locked/
   disarmed physical state. Start the controller last and wait for all inputs ready.

No motor, relay, bolt or alarm commands are sent by the helper. Existing services
resume their normal behavior afterward. Failed checks never clear fault journals
or replay commands. If interrupted, read the summary and saved stage before
choosing recovery. SIGINT/TERM/HUP are handled; power loss/SIGKILL cannot be caught.
Do not assume an interrupted transaction is complete merely because services
started after a machine reboot; inspect the receipt and live state first.

## Sequence

The initial operator command is only `backup`. It briefly pauses the services,
takes a snapshot, and restarts the unchanged installed plugin. It asks for
`BACKUP`, then `STILL`. This confirms the recovery checkpoint can be created and
the ordinary service restart still passes the established readiness checks.

Next, `install-baseline --build-dir PATH` takes another fresh snapshot and installs
the staged unchanged-upstream baseline plugin. It asks for `BASELINE`, then
`STILL`. This is a startup/ABI/mapping check; no extra unchanged door cycle is
required. The snapshot name in the completed output is the baseline receipt.

Then `install-feature --build-dir PATH --snapshot BASELINE_RECEIPT_NAME` requires
that completed baseline receipt, the expected current baseline binary hash, and
unchanged deCONZ executable/library. It takes a fresh pre-feature snapshot, then
installs the feature plugin. It asks for `FEATURE`, then `STILL`.

The helper pins the reviewed opt-in implementation and baseline a4c17ad
on deCONZ 2.33.2. The earlier feature build 79551b7 is superseded and refused. Documentation/maintenance-helper commits do not require rebuilding
those binaries. An updated feature implementation requires deliberate revision
review, fresh builds and adjusted gates; there is no silent latest-build selection.
Pass the reviewed feature SHA as the optional argument to
`bash tools/build-alarm-users.sh <reviewed-feature-commit>` when the checkout also
contains newer documentation or maintenance-only commits.

### First functional test: existing behavior on the new deCONZ plugin

After feature startup passes, keep the existing main PIN, alarm configuration,
controller settings and user allowance unchanged. Do NOT add a guest user yet.
The first functional acceptance is the current household workflow:

1. With the area clear and door closed/bolted, use the existing correct PIN.
   Verify the existing disarm/open behavior, bolt retraction and Home statuses.
2. Once fully open and stationary, enter a known incorrect PIN. Verify the
   existing close behavior, bolt extension only after closure, and Home statuses.
3. Under supervision, verify the indoor button and HomeKit controls still follow
   their existing routes and status behavior. Do not change interruption settings
   or enable new access-event triggers during this test.

This step intentionally moves the real door; the maintenance helper itself does
not conduct it or claim success. Record owner observations separately. If the
alarm was already disarmed in step 1, that proves already-disarmed access only;
do not claim an armed-to-disarmed transition was tested. Retain the separate
native arming/disarming regression check in later commissioning.

Only after this compatibility test passes should named-user creation, usage
allowances, disabling, deletion and retry/restart cases be commissioned. The new
access event must not become a second motor trigger alongside legacy actions.

## Recovery

Every installation prints the snapshot directory name before changing anything.
Do not infer that the newest directory is a complete backup: only a verified
snapshot with a passing database integrity check is accepted as a restore source.

`resume-unchanged --snapshot NAME` is for an interrupted backup or installation
that never reached the live-file mutation step. It verifies the unchanged plugin
and executable/library, then restarts and revalidates the existing setup. It asks
for `RESUME` and `STILL`. It cannot certify an uninstalled baseline build.

`rollback --snapshot NAME` restores that snapshot's plugin AND entire deCONZ data
directory, including its custom DDF. It asks for `ROLLBACK` and `STILL`. It does
not merely swap binaries while leaving feature-era access policies behind.

Rollback intentionally reverts all gateway changes since the selected snapshot,
including user/PIN/count, device pairing and configuration changes. Prefer the
fresh pre-feature checkpoint to return to the verified baseline; the original
backup returns to the packaged plugin. Before restoration, the helper stops all
services and preserves the current plugin/data in a NEW rollback snapshot. Even
a corrupt current database can be preserved as evidence without blocking recovery
from a valid source; such an evidence copy cannot itself be selected as a valid
restore source. All prior snapshots remain untouched.

Restore checks hashes before writing, refuses symlinks/special files and changed
service definitions or gateway executable/library, and restores file ownership
and permissions. Whole-directory restore is not one atomic operation. Durable
stages and the intact original snapshot allow retry after an interrupted restore;
no services are started until all file restoration succeeds. Snapshot metadata
and payload are private recovery material, not a tamper-proof security boundary.

Any failure leaves the controller stopped when that stop can be confirmed. A
gateway or Homebridge process may remain running if failure occurred during
restart. Do not use garage controls, clear faults, delete snapshots or blindly
retry. Share only the sanitized summary. The rollback path handles a stopped or
failed gateway; unknown manual gateway processes and changed mappings require
review instead of guessing.

## Validation

Offline tests exercise real SQLite/WAL snapshot consistency, snapshot corruption,
private permissions, restored ownership/modes/content, extra-file removal,
symlink rejection, invalid-database evidence preservation, service stop order,
failure-before-install, baseline gating and interrupted-operation guards.
Service/device interactions are simulated. Actual household backup, installation,
restart and rollback are not verified until the owner runs the guided steps.

Run offline checks with:

`python3 -B -m unittest discover -s tests -p 'test_garage_deconz_maintenance.py' -v`

Reviewed optional-adoption feature build: `039853eb786889aebc565cd5eece7b8978bb353f`.
Build it with `bash tools/build-alarm-users.sh 039853eb786889aebc565cd5eece7b8978bb353f`.
This revision passes 150 access-policy checks and 23 offline maintenance checks.
