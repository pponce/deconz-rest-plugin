"""Offline filesystem/recovery tests. No household services or devices are used."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import stat
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

SPEC = importlib.util.spec_from_file_location('maintenance',
    Path(__file__).resolve().parents[1] / 'tools/garage-deconz-maintenance.py')
m = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(m)


class SnapshotTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.owner = (os.getuid(), os.getgid())
        self.data = self.root / 'live'
        self.data.mkdir()
        (self.data / 'devices').mkdir()
        (self.data / 'devices/keypad.json').write_text('{"generic": true}')
        with sqlite3.connect(self.data / 'zll.db') as db:
            db.execute('CREATE TABLE codes (value TEXT)')
            db.execute('INSERT INTO codes VALUES (?)', ('synthetic fixture only',))
        self.plugin = self.root / 'live-plugin.so'
        self.plugin.write_bytes(b'original plugin fixture')
        self.plugin.chmod(0o644)
        self.job = m.Maintenance.__new__(m.Maintenance)
        self.job.repo = self.root
        self.job.owner = self.owner
        self.job.root = self.root / '.local-backups'
        m.private_directory(self.job.root, self.owner)
        self.job.tx = self.job.root / 'test-snapshot'
        m.private_directory(self.job.tx, self.owner)
        self.job.record = {'format': m.FORMAT, 'data': str(self.data),
                           'database_name': 'zll.db', 'stage': 'prepared'}
        self.job.check_units = Mock()
        self.job.guard = SimpleNamespace(h=SimpleNamespace(ddf=SimpleNamespace(
            prep=SimpleNamespace(process_inventory=Mock(return_value=[])))))

    def tearDown(self):
        self.temp.cleanup()

    def snapshot(self):
        with patch.object(m, 'run', return_value='synthetic unit fixture'):
            self.job.snapshot(self.plugin)

    def test_complete_snapshot_private_and_original_unchanged(self):
        before = m.inventory(self.data)
        self.snapshot()
        self.job.verify(self.job.tx, self.job.record)
        self.assertEqual(m.inventory(self.data), before)
        self.assertTrue(self.job.record['database_integrity_verified'])
        self.assertFalse((self.job.tx / 'database-validation').exists())
        for path in [self.job.tx] + list(self.job.tx.rglob('*')):
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o700 if path.is_dir() else 0o600)

    def test_corrupt_backup_refuses_restore_before_live_write(self):
        self.snapshot()
        (self.job.tx / 'deconz-data/zll.db').write_bytes(b'corrupt')
        before = m.inventory(self.data)
        with self.assertRaises(m.Stop):
            m.restore_tree(self.job.tx / 'deconz-data', self.data, self.job.record['data_inventory'])
        self.assertEqual(before, m.inventory(self.data))

    def test_rollback_restores_data_ddf_modes_and_removes_new_files(self):
        before = m.inventory(self.data)
        self.snapshot()
        (self.data / 'devices/keypad.json').write_text('{"changed": true}')
        (self.data / 'new-feature-file').write_bytes(b'new')
        with sqlite3.connect(self.data / 'zll.db') as db:
            db.execute('DELETE FROM codes')
        m.restore_tree(self.job.tx / 'deconz-data', self.data, self.job.record['data_inventory'])
        self.assertEqual(m.inventory(self.data), before)

    def test_symlink_source_refused(self):
        (self.data / 'leak').symlink_to(self.plugin)
        with self.assertRaises(m.Stop):
            self.snapshot()
        self.assertFalse(self.job.record.get('snapshot_complete', False))

    def test_rollback_live_symlink_refused_before_write(self):
        self.snapshot()
        (self.data / 'extra').symlink_to(self.plugin)
        original = self.plugin.read_bytes()
        with self.assertRaises(m.Stop):
            m.restore_tree(self.job.tx / 'deconz-data', self.data, self.job.record['data_inventory'])
        self.assertEqual(self.plugin.read_bytes(), original)

    def test_invalid_database_not_marked_recoverable(self):
        (self.data / 'zll.db').write_bytes(b'not sqlite')
        with self.assertRaises(m.Stop):
            self.snapshot()
        self.assertFalse(self.job.record.get('snapshot_complete', False))

    def test_corrupt_current_database_can_be_archived_for_rollback(self):
        (self.data / 'zll.db').write_bytes(b'broken live db')
        with patch.object(m, 'run', return_value='fixture'):
            self.job.snapshot(self.plugin, allow_invalid_database=True)
        self.assertTrue(self.job.record['snapshot_complete'])
        self.assertFalse(self.job.record['database_integrity_verified'])
        with self.assertRaises(m.Stop):
            self.job.verify(self.job.tx, self.job.record)

    def test_plugin_changed_in_snapshot_refused(self):
        self.snapshot()
        (self.job.tx / 'plugin.so').write_bytes(b'changed')
        with self.assertRaises(m.Stop):
            self.job.verify(self.job.tx, self.job.record)

    def test_wal_contents_included_without_mutating_raw_snapshot(self):
        # A synthetic open SQLite connection supplies an uncheckpointed WAL fixture.
        # Production snapshots require all gateway services and processes stopped.
        db = sqlite3.connect(self.data / 'zll.db')
        try:
            db.execute('PRAGMA journal_mode=WAL')
            db.execute('PRAGMA wal_autocheckpoint=0')
            db.execute('INSERT INTO codes VALUES (?)', ('WAL fixture',))
            db.commit()
            self.snapshot()
            self.assertIn('zll.db-wal', self.job.record['data_inventory'])
            self.job.verify(self.job.tx, self.job.record)
            disposable = self.root / 'inspect'
            import shutil
            shutil.copytree(self.job.tx / 'deconz-data', disposable)
            with sqlite3.connect(disposable / 'zll.db') as saved:
                self.assertEqual(saved.execute('SELECT COUNT(*) FROM codes').fetchone()[0], 2)
        finally:
            db.close()

    def test_no_plugin_change_before_snapshot(self):
        self.snapshot()
        target = self.root / 'new.so'
        target.write_bytes(b'new plugin fixture')
        with patch.object(m, 'atomic_bytes', side_effect=OSError('disk full')):
            with self.assertRaises(OSError):
                self.job.replace_plugin(target)
        self.assertEqual(self.plugin.read_bytes(), b'original plugin fixture')

    def test_staged_build_hash_validation(self):
        folder = self.root / '.local-builds/test'
        folder.mkdir(parents=True)
        (folder / 'versions.txt').write_text(f'baseline={m.BASELINE}\nfeature={m.FEATURE}\ninstalled_deconz=2.33.2\n')
        (folder / 'tests.log').write_text('PASS: 223 checks (fixtures)\n')
        lines = []
        for variant in ('baseline', 'feature'):
            file = folder / (variant + '-stage/share/deCONZ/plugins/libde_rest_plugin.so')
            file.parent.mkdir(parents=True)
            file.write_bytes(variant.encode())
            lines.append(m.digest(file) + '  ' + str(file))
        (folder / 'plugin-hashes.txt').write_text('\n'.join(lines))
        with patch.object(m, 'run', return_value='2.33.2'):
            result = self.job.build(folder)
            self.assertEqual(set(result), {'baseline', 'feature'})
            result['feature'].write_bytes(b'tampered')
            with self.assertRaises(m.Stop):
                self.job.build(folder)

    def test_no_backup_paths_escape(self):
        for name in ('../outside', '/tmp/outside', 'a/../../outside'):
            with self.assertRaises(m.Stop):
                m.safe_key(name)
        with self.assertRaises(m.Stop):
            self.job.load('../outside')

    def runtime(self):
        plugin = self.root / 'libde_rest_plugin.so'
        plugin.write_bytes(b'plugin fixture')
        self.job.record['original_plugin_sha256'] = m.digest(plugin)
        self.job.guard = SimpleNamespace(
            saved={'process': {}},
            h=SimpleNamespace(ddf=SimpleNamespace(prep=SimpleNamespace(
                process_inventory=lambda: [{'pid': 123}])) , matches=lambda *args, **kw: True),
            dependencies=Mock(), job=lambda: SimpleNamespace(mapping=Mock()),
            physical=Mock(), ready=Mock())
        return plugin

    def test_restart_order_and_gate_before_controller(self):
        plugin = self.runtime()
        events = []
        self.job.guard.dependencies.side_effect = lambda: events.append('dependencies checked')
        with patch.object(m, 'unit', return_value={'ActiveState': 'active', 'MainPID': '123'}), \
             patch.object(Path, 'read_text', return_value='a b c d e ' + str(plugin)), \
             patch.object(m, 'control', side_effect=lambda a, n: events.append((a, n))), \
             patch.object(m, 'confirm', side_effect=lambda *args: events.append('STILL')):
            self.job.restart(plugin)
        self.assertEqual(events, [('start', m.GATEWAY), ('start', m.HOMEBRIDGE),
            'dependencies checked', 'STILL', ('start', m.CONTROLLER)])
        self.assertTrue(self.job.record['complete'])

    def test_mapping_failure_prevents_controller_restart(self):
        plugin = self.runtime()
        self.job.guard.dependencies.side_effect = m.Stop('DDF mismatch')
        with patch.object(m, 'unit', return_value={'ActiveState': 'active', 'MainPID': '123'}), \
             patch.object(Path, 'read_text', return_value='a b c d e ' + str(plugin)), \
             patch.object(m, 'control') as commands, patch.object(m, 'confirm') as prompt:
            with self.assertRaises(m.Stop):
                self.job.restart(plugin)
            self.assertNotIn(unittest.mock.call('start', m.CONTROLLER), commands.call_args_list)
            prompt.assert_not_called()

    def test_missing_physical_confirmation_prevents_controller_restart(self):
        plugin = self.runtime()
        with patch.object(m, 'unit', return_value={'ActiveState': 'active', 'MainPID': '123'}), \
             patch.object(Path, 'read_text', return_value='a b c d e ' + str(plugin)), \
             patch.object(m, 'control') as commands, \
             patch.object(m, 'confirm', side_effect=m.Stop('cancelled')):
            with self.assertRaises(m.Stop):
                self.job.restart(plugin)
            self.assertNotIn(unittest.mock.call('start', m.CONTROLLER), commands.call_args_list)

    def test_rollback_preserves_replaced_state_and_original_snapshot(self):
        self.job.record.update(identity={'fixture': True}, guard={
            'process': {'database_paths': [str(self.data / 'zll.db')]},
            'database_owner': list(self.owner), 'authorization_sha256': 'fixture'})
        self.snapshot()
        original = self.job.tx
        original_hashes = m.content_inventory(original)
        self.plugin.write_bytes(b'feature plugin fixture')
        (self.data / 'devices/keypad.json').write_text('new private data fixture')
        self.job.system_identity = Mock(return_value={'fixture': True})
        self.job.stop = Mock()
        self.job.restart = Mock()
        with patch.object(m, 'run', return_value='synthetic unit fixture'), patch.object(m, 'confirm'):
            self.job.rollback(original.name)
        self.assertEqual(self.plugin.read_bytes(), b'original plugin fixture')
        self.assertEqual((self.job.tx / 'plugin.so').read_bytes(), b'feature plugin fixture')
        self.assertEqual((self.job.tx / 'deconz-data/devices/keypad.json').read_text(), 'new private data fixture')
        self.assertEqual(m.content_inventory(original), original_hashes)
        self.assertEqual((self.data / 'devices/keypad.json').read_text(), '{"generic": true}')
        self.job.restart.assert_called_once_with(self.plugin)


class FlowTests(unittest.TestCase):
    def test_homebridge_uses_hb_service_stop_and_start(self):
        for action, state in [('stop', {'ActiveState': 'inactive', 'MainPID': '0'}),
                              ('start', {'ActiveState': 'active', 'SubState': 'running'})]:
            with patch.object(m.shutil, 'which', return_value='/usr/bin/hb-service'), \
                 patch.object(m, 'run') as command, patch.object(m, 'unit', return_value=state):
                m.control(action, m.HOMEBRIDGE)
                command.assert_called_once_with('/usr/bin/hb-service', action, timeout=200)

    def test_homebridge_stop_must_actually_stop_owned_service(self):
        with patch.object(m.shutil, 'which', return_value='/usr/bin/hb-service'), \
             patch.object(m, 'run'), patch.object(m, 'unit', return_value={'ActiveState': 'active', 'MainPID': '9'}):
            with self.assertRaises(m.Stop):
                m.control('stop', m.HOMEBRIDGE)

    def test_missing_hb_service_does_not_fall_back_to_systemctl(self):
        with patch.object(m.shutil, 'which', return_value=None), patch.object(m, 'run') as command:
            with self.assertRaises(m.Stop):
                m.control('stop', m.HOMEBRIDGE)
            command.assert_not_called()

    def test_stop_order_and_post_stop_physical_check(self):
        events = []
        job = m.Maintenance.__new__(m.Maintenance)
        job.save = lambda stage: events.append(stage)
        class Lock:
            def __enter__(self):
                return self
            def __exit__(self, *args):
                return False
        job.guard = SimpleNamespace(lock=Lock, physical=lambda: events.append('physical'),
            h=SimpleNamespace(ddf=SimpleNamespace(prep=SimpleNamespace(process_inventory=lambda: []))))
        job.check_units = lambda active: events.append('confirmed stopped')
        with patch.object(m, 'control', side_effect=lambda action, name: events.append((action, name))):
            job.stop()
        self.assertEqual([e for e in events if isinstance(e, tuple)], [
            ('stop', m.CONTROLLER), ('stop', m.HOMEBRIDGE), ('stop', m.GATEWAY)])
        self.assertLess(events.index(('stop', m.CONTROLLER)), events.index('physical'))
        self.assertLess(events.index('physical'), events.index(('stop', m.HOMEBRIDGE)))
        self.assertTrue(job.paused)

    def test_resume_refuses_once_mutation_was_intended(self):
        job = m.Maintenance.__new__(m.Maintenance)
        job.load = lambda name: (Path('/unused'), {'action': 'install-feature',
                                                  'live_mutation_intent': True})
        with patch.object(m, 'control') as commands:
            with self.assertRaises(m.Stop):
                job.resume_unchanged('test')
            commands.assert_not_called()

    def test_feature_requires_completed_baseline_before_confirmation(self):
        job = m.Maintenance.__new__(m.Maintenance)
        job.guard = SimpleNamespace(capture=Mock(), idle=Mock())
        job.check_units = Mock()
        job.plugin_path = Mock(return_value=Path('/unused'))
        job.build = Mock(return_value={})
        job.load = Mock(return_value=(None, {'action': 'install-baseline', 'complete': False}))
        with patch.object(m, 'confirm') as prompt, patch.object(m, 'control') as commands:
            with self.assertRaises(m.Stop):
                job.forward('install-feature', Path('/unused'), 'baseline')
            prompt.assert_not_called()
            commands.assert_not_called()

    def test_snapshot_failure_prevents_install_and_restart(self):
        job = m.Maintenance.__new__(m.Maintenance)
        job.guard = SimpleNamespace(capture=Mock(), idle=Mock())
        job.check_units = Mock()
        job.plugin_path = Mock(return_value=Path('/unused'))
        job.build = Mock(return_value={'baseline': Path('/unused-baseline')})
        job.record = {}
        job.begin = Mock()
        job.save = Mock()
        job.stop = Mock()
        job.snapshot = Mock(side_effect=m.Stop('snapshot failed'))
        job.replace_plugin = Mock()
        job.restart = Mock()
        with patch.object(m, 'confirm'), patch.object(m, 'digest', return_value='fixture'):
            with self.assertRaises(m.Stop):
                job.forward('install-baseline', Path('/unused'))
        job.replace_plugin.assert_not_called()
        job.restart.assert_not_called()


class FeatureUpgradeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.job = m.Maintenance.__new__(m.Maintenance)
        self.job.root = self.root
        self.plugin = self.root / 'plugin.so'
        self.plugin.write_bytes(b'current feature')
        self.job.system_identity = Mock(return_value={'executable': 'same'})
        self.job.verify = Mock()
        self.record = {'format': m.FORMAT, 'action': 'install-feature', 'complete': True,
                       'installed_plugin_sha256': m.digest(self.plugin),
                       'identity': {'executable': 'same'}}

    def save(self, name, record=None):
        path = self.root / name
        path.mkdir()
        (path / 'receipt.json').write_text(json.dumps(record or self.record))
        return path

    def test_matching_feature_receipt_selected_and_payload_verified(self):
        path = self.save('20260101-install-feature')
        self.assertEqual(self.job.upgrade_receipt(self.plugin), path.name)
        self.job.verify.assert_called_once_with(path, self.record)

    def test_most_recent_matching_receipt_but_not_unrelated_backup(self):
        self.save('20260101-install-feature')
        latest = self.save('20260102-upgrade-feature', dict(self.record, action='upgrade-feature'))
        self.save('20260103-backup', dict(self.record, action='backup'))
        self.assertEqual(self.job.upgrade_receipt(self.plugin), latest.name)

    def test_invalid_receipts_cannot_authorize_upgrade(self):
        for index, change in enumerate(({'complete': False}, {'resumed_without_installation': True},
                 {'installed_plugin_sha256': 'different'}, {'identity': {}}, {'action': 'install-baseline'})):
            name = 'invalid-' + str(index)
            self.save(name, dict(self.record, **change))
            with self.assertRaises(m.Stop):
                self.job.upgrade_receipt(self.plugin, name)
        with self.assertRaises(m.Stop):
            self.job.upgrade_receipt(self.plugin)
        self.job.verify.assert_not_called()

    def test_corrupt_payload_refused(self):
        self.save('feature')
        self.job.verify.side_effect = m.Stop('corrupt')
        with self.assertRaises(m.Stop):
            self.job.upgrade_receipt(self.plugin)

    def test_missing_receipt_stops_before_confirmation_or_services(self):
        self.job.guard = SimpleNamespace(capture=Mock(), idle=Mock())
        self.job.check_units = Mock()
        self.job.plugin_path = Mock(return_value=self.plugin)
        self.job.build = Mock(return_value={'feature': Path('/unused')})
        with patch.object(m, 'confirm') as prompt, patch.object(m, 'control') as commands:
            with self.assertRaises(m.Stop):
                self.job.forward('upgrade-feature', Path('/unused'))
            prompt.assert_not_called()
            commands.assert_not_called()

    def test_upgrade_snapshots_before_replacement(self):
        self.save('feature')
        self.job.guard = SimpleNamespace(capture=Mock(), idle=Mock())
        self.job.check_units = Mock()
        self.job.plugin_path = Mock(return_value=self.plugin)
        self.job.build = Mock(return_value={'feature': self.root / 'staged.so'})
        self.job.record = {}
        calls = []
        self.job.begin = Mock()
        self.job.save = Mock()
        self.job.stop = Mock(side_effect=lambda: calls.append('stop'))
        self.job.snapshot = Mock(side_effect=lambda *_: calls.append('snapshot'))
        self.job.replace_plugin = Mock(side_effect=lambda *_: calls.append('replace'))
        self.job.restart = Mock(side_effect=lambda *_: calls.append('restart'))
        with patch.object(m, 'confirm'):
            self.job.forward('upgrade-feature', self.root)
        self.assertEqual(calls, ['stop', 'snapshot', 'replace', 'restart'])
        self.job.replace_plugin.assert_called_once_with(self.root / 'staged.so')
        self.assertEqual(self.job.record['previous_feature_receipt'], 'feature')

    def test_upgrade_snapshot_failure_never_replaces_or_restarts(self):
        self.save('feature')
        self.job.guard = SimpleNamespace(capture=Mock(), idle=Mock())
        self.job.check_units = Mock()
        self.job.plugin_path = Mock(return_value=self.plugin)
        self.job.build = Mock(return_value={'feature': Path('/unused')})
        self.job.record = {}
        for name in ('begin', 'save', 'stop', 'replace_plugin', 'restart'):
            setattr(self.job, name, Mock())
        self.job.snapshot = Mock(side_effect=m.Stop('snapshot failed'))
        with patch.object(m, 'confirm'):
            with self.assertRaises(m.Stop):
                self.job.forward('upgrade-feature', self.root)
        self.job.replace_plugin.assert_not_called()
        self.job.restart.assert_not_called()


class UpgradeRunnerTests(unittest.TestCase):
    def run_case(self, failed=None, build_only=False):
        spec = importlib.util.spec_from_file_location('upgrade_runner',
            Path(__file__).resolve().parents[1] / 'tools/upgrade-alarm-api.py')
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            commands = []
            def fake_git(*args):
                if args[:2] == ('rev-parse', '--show-toplevel'): return str(root)
                if args[:2] == ('branch', '--show-current'): return 'alarm-users-v1'
                if args[:2] == ('rev-parse', '--verify'): return runner.FEATURE
                return ''
            def run(command, **kwargs):
                commands.append(command)
                if command[0] == 'bash':
                    folder = root / '.local-builds' / 'new-build'
                    folder.mkdir()
                    (folder / 'versions.txt').write_text('feature=' + runner.FEATURE)
                    (folder / 'tests.log').write_text('PASS: 223 checks')
                    return SimpleNamespace(returncode=1 if failed == 'build' else 0)
                if command[0] == 'sudo':
                    return SimpleNamespace(returncode=0)
                return SimpleNamespace(returncode=1 if failed == 'tests' else 0)
            with patch.object(runner, 'REPO', root), patch.object(runner, 'git', side_effect=fake_git), \
                 patch.object(runner.os, 'geteuid', return_value=1000), \
                 patch.object(runner.sys.stdin, 'isatty', return_value=True), \
                 patch.object(runner.sys, 'argv', ['runner'] + (['--build-only'] if build_only else [])), \
                 patch.object(runner.subprocess, 'run', side_effect=run):
                code = runner.main()
            return code, commands

    def test_failed_tests_prevent_build_and_install(self):
        code, commands = self.run_case(failed='tests')
        self.assertEqual(code, 1)
        self.assertFalse(any(c[0] in ('bash', 'sudo') for c in commands))

    def test_failed_build_prevents_install(self):
        code, commands = self.run_case(failed='build')
        self.assertEqual(code, 1)
        self.assertFalse(any(c[0] == 'sudo' for c in commands))

    def test_build_only_never_invokes_sudo(self):
        code, commands = self.run_case(build_only=True)
        self.assertEqual(code, 0)
        self.assertFalse(any(c[0] == 'sudo' for c in commands))

    def test_success_invokes_supervised_upgrade_with_new_build(self):
        code, commands = self.run_case()
        self.assertEqual(code, 0)
        install = [c for c in commands if c[0] == 'sudo']
        self.assertEqual(len(install), 1)
        self.assertIn('upgrade-feature', install[0])
        self.assertEqual(Path(install[0][-1]).name, 'new-build')


if __name__ == '__main__':
    unittest.main()

