#!/usr/bin/env python3
"""Household-only supervised maintenance; no motor, bolt or alarm commands.

Uses the separately commissioned garageDoorController checkout for live guards.
This deployment helper is not part of the proposed upstream alarm-user API.
"""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import fcntl
import hashlib
import importlib
import json
import os
from pathlib import Path
import pwd
import shlex
import shutil
import signal
import sqlite3
import stat
import subprocess
import sys
import tempfile
import time
import uuid

sys.dont_write_bytecode = True
CONTROLLER = 'garage-door-controller.service'
GATEWAY = 'deconz.service'
HOMEBRIDGE = 'homebridge.service'
GUI = 'deconz-gui.service'
UNITS = (CONTROLLER, HOMEBRIDGE, GATEWAY, GUI)
FORMAT = 'garage-deconz-snapshot-v1'
BASELINE = 'a4c17adfa04abc63637ad17de7f85256a825a2cb'
FEATURE = '06b9c82264bbcfdf6cebf9583f8a7985f32a8683'


class Stop(Exception):
    pass


def require(ok, reason):
    if not ok:
        raise Stop(reason)


def run(*args, timeout=30):
    result = subprocess.run(args, capture_output=True, timeout=timeout)
    require(result.returncode == 0, 'command_failed_private_output_omitted')
    return result.stdout.decode()


def unit(name):
    text = run('systemctl', 'show', name,
               '--property=LoadState,ActiveState,SubState,MainPID,FragmentPath,DropInPaths,UnitFileState')
    return dict(line.split('=', 1) for line in text.splitlines() if '=' in line)


def control(action, name):
    # The commissioned controller intentionally waits for active operations to finish.
    if name == HOMEBRIDGE:
        require(action in ('stop', 'start'), 'unsupported_homebridge_action')
        executable = shutil.which('hb-service')
        require(executable is not None, 'hb_service_not_found')
        run(executable, action, timeout=200)  # This helper already runs under sudo.
        info = unit(HOMEBRIDGE)
        require((info.get('ActiveState') == 'active' and info.get('SubState') == 'running')
                if action == 'start' else
                (info.get('ActiveState') in ('inactive', 'failed') and info.get('MainPID') == '0'),
                'homebridge_service_transition_not_confirmed')
    else:
        run('systemctl', action, name, timeout=200)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def sync_directory(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def plain(path):
    path = Path(path)
    require(path.is_absolute() and path == path.resolve(), 'symlink_or_relative_path_refused')
    return path


def atomic_bytes(path, content, owner, mode=0o600):
    plain(path)
    fd, temporary = tempfile.mkstemp(prefix='.maintenance-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            os.fchown(stream.fileno(), *owner)
            os.fchmod(stream.fileno(), mode)
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        sync_directory(path.parent)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def metadata(path):
    info = path.lstat()
    require(stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode),
            'symlink_or_special_file_requires_review')
    row = {'kind': 'dir' if path.is_dir() else 'file', 'uid': info.st_uid,
           'gid': info.st_gid, 'mode': stat.S_IMODE(info.st_mode),
           'mtime_ns': info.st_mtime_ns}
    if row['kind'] == 'file':
        row.update(sha256=digest(path), size=info.st_size)
    return row


def inventory(root):
    plain(root)
    return {str(p.relative_to(root)): metadata(p)
            for p in [root] + sorted(root.rglob('*'))}


def content_inventory(root):
    return {key: (value['kind'], value.get('sha256'))
            for key, value in inventory(root).items()}


def private_directory(path, owner):
    plain(path)
    if not path.exists():
        path.mkdir(mode=0o700)
        os.chown(path, *owner)
    info = path.stat()
    require(path.is_dir() and info.st_uid == owner[0] and
            stat.S_IMODE(info.st_mode) == 0o700, 'private_directory_permissions_required')


def save_json(path, value, owner):
    atomic_bytes(path, (json.dumps(value, indent=2) + '\n').encode(), owner)


def safe_key(value):
    path = Path(value)
    require(not path.is_absolute() and '..' not in path.parts and
            str(path) == value, 'snapshot_relative_path_invalid')
    return path


def copy_private_tree(source, target, rows, owner):
    private_directory(target, owner)
    for name, row in rows.items():
        path = target / safe_key(name)
        if row['kind'] == 'dir':
            private_directory(path, owner)
        else:
            atomic_bytes(path, (source / name).read_bytes(), owner)
            require(digest(path) == row['sha256'], 'snapshot_copy_hash_mismatch')
    require(inventory(source) == rows, 'source_changed_during_snapshot')


def verify_tree(root, rows):
    require(set(inventory(root)) == set(rows), 'snapshot_file_set_changed')
    for name, row in rows.items():
        path = root / safe_key(name)
        require(metadata(path)['kind'] == row['kind'], 'snapshot_file_type_changed')
        if row['kind'] == 'file':
            require(digest(path) == row['sha256'], 'snapshot_hash_changed')


def restore_tree(source, target, rows):
    """Caller has stopped all gateway processes and archived the replaced tree."""
    verify_tree(source, rows)
    current = inventory(target)  # Refuse symlinks/special files before any write.
    for name in set(current) & set(rows):
        require(current[name]['kind'] == rows[name]['kind'], 'restore_type_conflict')
    for name in sorted(set(current) - set(rows), key=lambda x: len(Path(x).parts), reverse=True):
        path = target / safe_key(name)
        path.rmdir() if current[name]['kind'] == 'dir' else path.unlink()
    for name, row in rows.items():
        path = target / safe_key(name)
        if row['kind'] == 'dir':
            path.mkdir(exist_ok=True)
        else:
            atomic_bytes(path, (source / name).read_bytes(), (row['uid'], row['gid']), row['mode'])
    for name in reversed(list(rows)):
        path, row = target / name, rows[name]
        os.chown(path, row['uid'], row['gid'])
        os.chmod(path, row['mode'])
        os.utime(path, ns=(row['mtime_ns'], row['mtime_ns']))
    require(content_inventory(target) == content_inventory(source), 'restored_data_hash_mismatch')
    sync_directory(target)


def confirm(word, message):
    require(input(message + ' Type ' + word + ' (Enter cancels): ').strip() == word, 'cancelled')


class GarageGuard:
    """Reuse existing authenticated, read-only household checks, not copied secrets."""
    def __init__(self, saved=None):
        info = unit(CONTROLLER)
        require(info.get('LoadState') == 'loaded' and not info.get('DropInPaths'),
                'controller_unit_requires_review')
        text = plain(info['FragmentPath']).read_text()
        commands = [line.split('=', 1)[1] for line in text.splitlines() if line.startswith('ExecStart=')]
        require(len(commands) == 1, 'controller_command_not_unique')
        args = shlex.split(commands[0])
        require(len(args) == 6 and args[0] == '/usr/bin/python3' and args[1] == '-B' and
                args[3:5] == ['serve', '--package'], 'controller_command_requires_review')
        script = plain(args[2])
        require(script.name == 'garage_service.py', 'controller_script_requires_review')
        sys.path.insert(0, str(script.parent))
        self.h = importlib.import_module('handoff_deconz_headless')
        self.service = self.h.service
        require(script == self.service.ROOT / 'scripts/garage_service.py', 'controller_source_root_mismatch')
        self.folder = self.h.trial.package_path(Path(args[5]))
        self.config = self.service.settings(self.folder, service=True)
        self.service.owned_unit(self.folder)
        self.saved = saved

    def capture(self):
        rows = self.h.ddf.prep.process_inventory()
        require(len(rows) == 1 and len(rows[0]['database_paths']) == 1, 'one_gateway_and_database_required')
        row = rows[0]
        require(unit(GATEWAY)['MainPID'] == str(row['pid']), 'headless_process_mismatch')
        db = plain(row['database_paths'][0])
        auth = self.h.trial.read(self.folder / self.h.persistent.AUTH)
        self.saved = {'process': row, 'database_owner': [db.stat().st_uid, db.stat().st_gid],
                      'authorization_sha256': self.h.trial.digest(auth)}
        return self.saved

    def job(self):
        return self.h.Handoff(self.folder, self.config, self.saved)

    def idle(self):
        report = self.h.commissioning.idle(self.folder, self.config)
        require(report.get('button_ready') and report.get('keypad_ready') and
                report.get('homekit_current') == 1, 'all_inputs_ready_and_closed_required')
        self.physical()
        self.job().mapping()

    def physical(self):
        self.job().physical()

    def lock(self):
        return self.service.ownership_lock(self.folder)

    def dependencies(self):
        self.job().dependencies()

    def ready(self):
        return self.h.commissioning.wait_for(self.folder, self.config, lambda r:
            r.get('ready') and not r.get('busy') and not r.get('fault') and
            r.get('button_ready') and r.get('keypad_ready') and
            r.get('homekit_current') == 1, 90)


class Maintenance:
    def __init__(self, repo, owner, guard):
        self.repo, self.owner, self.guard = plain(repo), owner, guard
        self.root = self.repo / '.local-backups'
        user = pwd.getpwuid(owner[0]).pw_name
        git = ('runuser', '-u', user, '--', 'git', '-C', str(repo))
        require(not run(*git, 'ls-files', '--', '.local-backups').strip(), 'backup_files_already_tracked')
        run(*git, 'check-ignore', '--quiet', '--', '.local-backups/privacy-check')
        private_directory(self.root, owner)
        self.tx, self.record = None, None
        self.paused = False

    @contextmanager
    def lock(self):
        path = self.root / 'maintenance.lock'
        fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        try:
            os.fchown(fd, *self.owner)
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            yield
        finally:
            os.close(fd)

    def save(self, stage):
        self.record['stage'] = stage
        save_json(self.tx / 'receipt.json', self.record, self.owner)

    def load(self, name):
        require(Path(name).name == name and name not in ('', '.', '..'), 'snapshot_name_invalid')
        path = plain(self.root / name)
        record = json.loads((path / 'receipt.json').read_text())
        require(record.get('format') == FORMAT, 'snapshot_format_invalid')
        return path, record

    def system_identity(self):
        result = {}
        for key, path in [('executable', Path('/usr/bin/deCONZ')), ('library', Path('/lib/libdeCONZ.so.1').resolve())]:
            result[key] = {'path': str(path), 'sha256': digest(path)}
        return result

    def check_units(self, active):
        require(shutil.which('hb-service') is not None, 'hb_service_not_found')
        for name in UNITS:
            info = unit(name)
            require(info.get('LoadState') == 'loaded', 'missing_expected_unit')
            if name == GUI:
                require(info['ActiveState'] in ('inactive', 'failed') and info['MainPID'] == '0',
                        'gui_must_remain_stopped')
            elif active:
                require(info['ActiveState'] == 'active' and info['SubState'] == 'running',
                        'services_must_be_running')
            else:
                require(info['ActiveState'] in ('inactive', 'failed') and info['MainPID'] == '0',
                        'service_stop_not_confirmed')

    def plugin_path(self):
        pid = self.guard.saved['process']['pid']
        mappings = (Path('/proc') / str(pid) / 'maps').read_text().splitlines()
        paths = {line.split(maxsplit=5)[5] for line in mappings if 'libde_rest_plugin.so' in line}
        require(len(paths) == 1, 'one_loaded_plugin_required')
        return plain(paths.pop())

    def begin(self, action, saved=None):
        self.tx = self.root / (datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + action + '-' + uuid.uuid4().hex[:8])
        private_directory(self.tx, self.owner)
        state = saved or self.guard.capture()
        self.guard.saved = state
        db = plain(state['process']['database_paths'][0])
        require(db.name == 'zll.db' and (db.parent / 'devices').is_dir(), 'database_and_custom_devices_required')
        self.record = {'format': FORMAT, 'action': action, 'guard': state,
                       'data': str(db.parent), 'database_name': db.name,
                       'identity': self.system_identity(), 'complete': False}
        self.save('prepared')
        print('Recovery directory: ' + str(self.tx), flush=True)

    def stop(self, normal=True):
        self.save('controller_stop_intent')
        self.paused = True
        control('stop', CONTROLLER)
        with self.guard.lock():
            if normal:
                self.guard.physical()
            self.save('homebridge_stop_intent')
            control('stop', HOMEBRIDGE)
            self.save('gateway_stop_intent')
            control('stop', GATEWAY)
            self.check_units(False)
            require(not self.guard.h.ddf.prep.process_inventory(), 'unexpected_gateway_process')
            self.save('services_stopped')

    def snapshot(self, plugin, allow_invalid_database=False):
        self.check_units(False)
        require(not self.guard.h.ddf.prep.process_inventory(), 'unexpected_gateway_process')
        source = plain(self.record['data'])
        rows = inventory(source)
        self.record['data_inventory'] = rows
        self.record['plugin'] = str(plain(plugin))
        self.record['plugin_metadata'] = metadata(plugin)
        self.save('snapshot_copy_intent')
        copy_private_tree(source, self.tx / 'deconz-data', rows, self.owner)
        atomic_bytes(self.tx / 'plugin.so', plugin.read_bytes(), self.owner)
        require(digest(self.tx / 'plugin.so') == self.record['plugin_metadata']['sha256'], 'plugin_copy_mismatch')
        # Validate a disposable copy, never open the source or retained snapshot with SQLite.
        validation = self.tx / 'database-validation'
        private_directory(validation, self.owner)
        dbname = self.record['database_name']
        for name in rows:
            if name == dbname or name in (dbname + '-wal', dbname + '-shm', dbname + '-journal'):
                atomic_bytes(validation / name, (self.tx / 'deconz-data' / name).read_bytes(), self.owner)
        try:
            connection = sqlite3.connect(validation / dbname, timeout=5)
            try:
                valid = connection.execute('PRAGMA quick_check').fetchall() == [('ok',)]
            finally:
                connection.close()
        except sqlite3.DatabaseError:
            valid = False
        self.record['database_integrity_verified'] = valid
        require(valid or allow_invalid_database, 'database_backup_integrity_failed')
        shutil.rmtree(validation)
        configs = {name: run('systemctl', 'cat', name) for name in UNITS}
        save_json(self.tx / 'service-configs.json', configs, self.owner)
        self.record['service_configs_sha256'] = digest(self.tx / 'service-configs.json')
        self.record['snapshot_complete'] = True
        self.save('snapshot_verified')

    def verify(self, path, record):
        require(record.get('snapshot_complete') and record.get('database_integrity_verified'),
                'snapshot_incomplete_or_database_invalid')
        verify_tree(path / 'deconz-data', record['data_inventory'])
        require(digest(path / 'plugin.so') == record['plugin_metadata']['sha256'], 'snapshot_plugin_hash_changed')
        require(digest(path / 'service-configs.json') == record['service_configs_sha256'], 'snapshot_service_config_changed')

    def replace_plugin(self, source, desired_metadata=None):
        self.check_units(False)
        require(not self.guard.h.ddf.prep.process_inventory(), 'unexpected_gateway_process')
        path = Path(self.record['plugin'])
        info = desired_metadata or self.record['plugin_metadata']
        self.record['live_mutation_intent'] = True
        self.save('plugin_replace_intent')
        atomic_bytes(path, source.read_bytes(), (info['uid'], info['gid']), info['mode'])
        require(digest(path) == digest(source), 'installed_plugin_hash_mismatch')
        self.record['installed_plugin_sha256'] = digest(path)
        self.save('plugin_replaced')

    def restart(self, expected_plugin):
        self.save('gateway_start_intent')
        control('start', GATEWAY)
        deadline = time.monotonic() + 35
        while True:
            rows = self.guard.h.ddf.prep.process_inventory()
            info = unit(GATEWAY)
            if info.get('ActiveState') == 'active' and self.guard.h.matches(
                    rows, self.guard.saved['process'], info.get('MainPID'), same_user=True):
                break
            require(time.monotonic() < deadline, 'gateway_restart_not_verified')
            time.sleep(1)
        pid = rows[0]['pid']
        maps = (Path('/proc') / str(pid) / 'maps').read_text().splitlines()
        paths = {line.split(maxsplit=5)[5] for line in maps if 'libde_rest_plugin.so' in line}
        expected_hash = self.record.get('installed_plugin_sha256', self.record.get('original_plugin_sha256'))
        require(paths == {str(expected_plugin)} and digest(expected_plugin) == expected_hash,
                'loaded_plugin_not_verified')
        self.save('homebridge_start_intent')
        control('start', HOMEBRIDGE)
        self.guard.dependencies()
        self.save('dependencies_verified')
        confirm('STILL', 'Check Home accessories are present; door CLOSED, bolt LOCKED, alarm DISARMED, no unexpected movement.')
        self.guard.job().mapping()
        self.guard.physical()
        self.save('controller_start_intent')
        control('start', CONTROLLER)
        self.guard.ready()
        self.check_units(True)
        self.record['complete'] = True
        self.save('completed')
        self.paused = False

    def build(self, folder):
        folder = plain(folder)
        require(folder.is_relative_to(self.repo / '.local-builds'), 'build_must_be_inside_project')
        versions = dict(line.split('=', 1) for line in (folder / 'versions.txt').read_text().splitlines() if '=' in line)
        require(versions.get('baseline') == BASELINE and versions.get('feature') == FEATURE,
                'build_revision_requires_review')
        installed = run('dpkg-query', '-W', '-f=${Version}', 'deconz').strip()
        require(installed == versions.get('installed_deconz') == '2.33.2', 'installed_deconz_version_changed')
        require('PASS: 223 checks' in (folder / 'tests.log').read_text(), 'passing_build_tests_required')
        hashes = {}
        for line in (folder / 'plugin-hashes.txt').read_text().splitlines():
            value, filename = line.split(maxsplit=1)
            hashes[str(plain(filename.lstrip('*')))] = value
        result = {}
        for variant in ('baseline', 'feature'):
            path = folder / (variant + '-stage/share/deCONZ/plugins/libde_rest_plugin.so')
            require(hashes.get(str(path)) == digest(path), 'staged_plugin_hash_changed')
            result[variant] = path
        return result

    def upgrade_receipt(self, plugin, name=None):
        # Discovery is limited to completed local feature-install receipts whose
        # installed hash and executable/library identity match the current host.
        installed, identity = digest(plugin), self.system_identity()
        def matches(record):
            return (record.get('action') in ('install-feature', 'upgrade-feature') and
                    record.get('complete') is True and
                    not record.get('resumed_without_installation') and
                    record.get('installed_plugin_sha256') == installed and
                    record.get('identity') == identity)
        if name:
            path, record = self.load(name)
            require(matches(record), 'verified_current_feature_required')
        else:
            candidates = []
            for child in self.root.iterdir():
                if child.is_symlink() or not child.is_dir() or not (child / 'receipt.json').is_file():
                    continue
                try:
                    path, record = self.load(child.name)
                except (Stop, OSError, ValueError):
                    continue
                if matches(record):
                    candidates.append((path, record))
            require(bool(candidates), 'verified_current_feature_required')
            path, record = max(candidates, key=lambda pair: pair[0].name)
        self.verify(path, record)
        return path.name

    def forward(self, action, build=None, previous=None):
        self.check_units(True)
        self.guard.capture()
        self.guard.idle()
        plugin = self.plugin_path()
        stages = self.build(build) if action != 'backup' else None
        if action == 'install-feature':
            _, receipt = self.load(previous or '')
            require(receipt.get('action') == 'install-baseline' and receipt.get('complete') and
                    not receipt.get('resumed_without_installation') and
                    receipt.get('installed_plugin_sha256') == digest(plugin) == digest(stages['baseline']) and
                    receipt.get('identity') == self.system_identity(), 'verified_baseline_required')
        if action == 'upgrade-feature':
            previous = self.upgrade_receipt(plugin, previous)
        word = {'backup': 'BACKUP', 'install-baseline': 'BASELINE', 'install-feature': 'FEATURE', 'upgrade-feature': 'UPGRADE'}[action]
        print('Start CLOSED, LOCKED and DISARMED. Keep ALL controls unused until this tool finishes.', flush=True)
        confirm(word, 'Briefly stop controller, Homebridge and deCONZ; take a private snapshot' +
                (' and replace only the plugin.' if stages else '; restart the unchanged system.'))
        self.guard.idle()
        self.begin(action)
        if action == 'upgrade-feature':
            self.record['previous_feature_receipt'] = previous
        self.record['plugin'] = str(plugin)
        self.record['original_plugin_sha256'] = digest(plugin)
        self.save('pre_stop')
        self.stop()
        self.snapshot(plugin)
        if stages:
            self.replace_plugin(stages['baseline' if action == 'install-baseline' else 'feature'])
        self.restart(plugin)

    def rollback(self, name):
        source, saved = self.load(name)
        self.verify(source, saved)
        require(saved['identity'] == self.system_identity(), 'gateway_executable_or_library_changed')
        configs = json.loads((source / 'service-configs.json').read_text())
        require(all(run('systemctl', 'cat', name) == configs[name] for name in UNITS),
                'service_configuration_changed_since_snapshot')
        self.guard.saved = saved['guard']
        plugin = plain(saved['plugin'])
        # Refuse a newly selected live database or a manually started GUI.
        rows = self.guard.h.ddf.prep.process_inventory()
        require(not rows or self.guard.h.matches(rows, saved['guard']['process'], unit(GATEWAY)['MainPID'], True),
                'rollback_runtime_changed')
        print('Rollback restores the snapshot\'s ENTIRE deCONZ data directory and plugin.', flush=True)
        print('Later user/PIN/count, pairing and gateway changes will be reverted. Current data will be preserved first.', flush=True)
        confirm('ROLLBACK', 'Door must be CLOSED, LOCKED, DISARMED and stationary; leave controls unused.')
        self.begin('rollback', saved['guard'])
        self.record['restore_source'] = source.name
        self.save('rollback_pre_stop')
        self.stop(normal=False)
        self.snapshot(plugin, allow_invalid_database=True)
        self.verify(source, saved)
        self.record['live_mutation_intent'] = True
        self.save('data_restore_intent')
        self.check_units(False)
        require(not self.guard.h.ddf.prep.process_inventory(), 'unexpected_gateway_process')
        restore_tree(source / 'deconz-data', Path(saved['data']), saved['data_inventory'])
        self.save('data_restored')
        self.replace_plugin(source / 'plugin.so', saved['plugin_metadata'])
        self.restart(plugin)

    def resume_unchanged(self, name):
        self.tx, self.record = self.load(name)
        require(self.record.get('action') in ('backup', 'install-baseline', 'install-feature', 'upgrade-feature') and
                not self.record.get('live_mutation_intent') and not self.record.get('complete'),
                'resume_requires_no_live_file_changes')
        self.guard.saved = self.record['guard']
        require(self.record['identity'] == self.system_identity(), 'gateway_executable_or_library_changed')
        require(digest(Path(self.record['plugin'])) == self.record['original_plugin_sha256'],
                'original_plugin_changed')
        confirm('RESUME', 'Restart the unchanged system after interrupted maintenance; no data or plugin restoration.')
        self.paused = True
        control('stop', CONTROLLER)
        control('stop', HOMEBRIDGE)
        control('stop', GATEWAY)
        self.check_units(False)
        require(not self.guard.h.ddf.prep.process_inventory(), 'unexpected_gateway_process')
        # A resumed attempt did not install the requested variant. Do not allow
        # that restart alone to qualify as the verified baseline prerequisite.
        self.record['resumed_without_installation'] = True
        self.restart(Path(self.record['plugin']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['backup', 'install-baseline', 'install-feature', 'upgrade-feature', 'rollback', 'resume-unchanged'])
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--snapshot', help='snapshot directory name, never a path outside .local-backups')
    args = parser.parse_args()
    job = None
    summary = {'completed': False, 'action': args.action, 'motor_bolt_alarm_commands': 0}
    try:
        require(os.geteuid() == 0 and sys.stdin.isatty() and os.environ.get('SUDO_UID'), 'sudo_interactive_terminal_required')
        os.umask(0o077)
        signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(Stop('terminated')))
        signal.signal(signal.SIGHUP, lambda *_: (_ for _ in ()).throw(Stop('terminal_disconnected')))
        repo = Path(__file__).resolve().parents[1]
        owner = (int(os.environ['SUDO_UID']), int(os.environ['SUDO_GID']))
        require(owner[0] != 0 and repo.stat().st_uid == owner[0], 'run_from_login_user_checkout')
        guard = GarageGuard()
        job = Maintenance(repo, owner, guard)
        with job.lock():
            if args.action == 'rollback':
                job.rollback(args.snapshot or '')
            elif args.action == 'resume-unchanged':
                job.resume_unchanged(args.snapshot or '')
            else:
                require(args.action == 'backup' or args.build_dir is not None, 'build_directory_required')
                job.forward(args.action, args.build_dir, args.snapshot)
        summary['completed'] = True
        summary['snapshot'] = job.tx.name
        print('Completed. Existing controller checks passed; no movement test was commanded.', flush=True)
        return 0
    except (Exception, KeyboardInterrupt) as error:
        summary['reason'] = str(error) if isinstance(error, Stop) else (
            'cancelled' if isinstance(error, (KeyboardInterrupt, EOFError)) else 'maintenance_stopped_private_details_omitted')
        if job and job.tx:
            summary['snapshot'] = job.tx.name
            summary['stage'] = job.record.get('stage')
        if job and job.paused:
            try:
                control('stop', CONTROLLER)
                summary['controller_left_stopped'] = True
            except Exception:
                summary['controller_stop_not_confirmed'] = True
            print('STOPPED: leave garage controls unused. Preserve this summary for recovery; do not clear faults.', flush=True)
        return 1
    finally:
        print(json.dumps(summary, indent=2), flush=True)
        print('Share this summary only; keep .local-backups private.', flush=True)


if __name__ == '__main__':
    sys.exit(main())

