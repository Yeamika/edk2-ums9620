"""Exercise real Git downloads, private source preparation and BaseTools inputs."""
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
API = runpy.run_path(str(ROOT/'tools/upstream.py'))


class UpstreamTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.project, self.cache = self.root/'platform', self.root/'cache with spaces'
        self.project.mkdir()
        self.brotli = self.make_repository('brotli')
        (self.brotli/'marker').write_text('pinned Brotli')
        self.brotli_pin = self.commit(self.brotli)
        self.edk2 = self.make_repository('edk2')
        (self.edk2/'BaseTools').mkdir()
        (self.edk2/'BaseTools/Makefile').write_text(
            'all:\n\tmkdir -p bin\n\tcp Source/C/BrotliCompress/brotli/marker bin/selected\n')
        (self.edk2/'marker').write_text('pinned EDK II')
        for path in API['MODULES']:
            self.git(self.edk2, '-c', 'protocol.file.allow=always', 'submodule', 'add', '--quiet',
                     self.brotli.as_uri(), path)
        self.edk2_pin = self.commit(self.edk2)
        self.git(self.edk2, 'tag', 'platform-tested')
        dependencies = {'edk2': {'repository': str(self.edk2), 'commit': self.edk2_pin}}
        dependencies.update({name: {'repository': self.brotli.as_uri(), 'commit': self.brotli_pin}
                             for name in API['NAMES'][1:]})
        self.lock = {'schema': 1, 'dependencies': dependencies}
        self.lock_file = self.project/'sources.lock.json'
        self.lock_file.write_text(json.dumps(self.lock))

    def git(self, root, *args):
        return subprocess.check_output(['git', '-C', str(root), *args], text=True,
                                       stderr=subprocess.PIPE).strip()

    def make_repository(self, name):
        root = self.root/name
        root.mkdir()
        self.git(root, 'init', '--quiet', '-b', 'main')
        self.git(root, 'config', 'user.name', 'Platform dependency test')
        self.git(root, 'config', 'user.email', 'platform@example.invalid')
        return root

    def commit(self, root):
        self.git(root, 'add', '--all')
        self.git(root, 'commit', '--quiet', '-m', 'fixture revision')
        return self.git(root, 'rev-parse', 'HEAD')

    def resolve(self, **kwargs):
        return API['resolve'](self.project, self.cache, **kwargs)

    def test_default_download_and_self_contained_private_sources(self):
        record, paths = self.resolve()
        prepared = API['prepare'](self.root/'output', record, paths)
        self.assertEqual(record['dependencies']['edk2']['commit'], self.edk2_pin)
        self.assertFalse((self.project/'edk2').exists())
        for name in API['NAMES'][1:]:
            self.assertEqual((Path(prepared[name])/'marker').read_text(), 'pinned Brotli')
            self.assertEqual(self.git(Path(prepared[name]), 'rev-parse', '--is-shallow-repository'), 'true')
        shutil.rmtree(self.cache)
        shutil.rmtree(self.edk2)
        shutil.rmtree(self.brotli)
        actual, _ = API['inspect'](Path(prepared['edk2']))
        self.assertEqual(API['identity'](actual), API['identity'](record))

    def test_latest_refreshes_and_saved_lock_replays_offline(self):
        first, _ = self.resolve(source_refs=['edk2=latest'])
        (self.edk2/'marker').write_text('new upstream head')
        head = self.commit(self.edk2)
        pinned, _ = self.resolve()
        self.assertEqual(pinned['dependencies']['edk2']['commit'], first['dependencies']['edk2']['commit'])
        latest, _ = self.resolve(source_refs=['edk2=latest'])
        self.assertEqual(latest['dependencies']['edk2']['commit'], head)
        replay = self.root/'resolved.json'
        replay.write_text(json.dumps(latest))
        self.edk2.rename(self.root/'edk2-offline')
        self.brotli.rename(self.root/'brotli-offline')
        repeated, _ = self.resolve(lock=replay)
        self.assertEqual(API['identity'](repeated), API['identity'](latest))

    def test_fork_branch_and_tag_select_actual_edk2_sources(self):
        fork = self.root/'fork'
        subprocess.run(['git', 'clone', '--quiet', str(self.edk2), str(fork)], check=True)
        self.git(fork, 'config', 'user.name', 'Platform dependency test')
        self.git(fork, 'config', 'user.email', 'platform@example.invalid')
        self.git(fork, 'checkout', '--quiet', '-b', 'integration')
        (fork/'marker').write_text('fork implementation')
        head = self.commit(fork)
        record, paths = self.resolve(source_repos=['edk2='+str(fork)], source_refs=['edk2=integration'])
        self.assertEqual(record['dependencies']['edk2']['commit'], head)
        self.assertEqual((Path(paths['edk2'])/'marker').read_text(), 'fork implementation')
        tagged, _ = self.resolve(source_refs=['edk2=platform-tested'])
        self.assertEqual(tagged['dependencies']['edk2']['commit'], self.edk2_pin)

    def test_brotli_override_is_compiled_without_modifying_cached_edk2(self):
        (self.brotli/'marker').write_text('selected Brotli')
        head = self.commit(self.brotli)
        self.git(self.brotli, 'tag', 'integration')
        record, paths = self.resolve(source_refs=['brotli=integration'])
        for name in API['NAMES'][1:]:
            self.assertEqual(record['dependencies'][name]['commit'], head)
            self.assertEqual(record['dependencies'][name]['gitlink'], self.brotli_pin)
        self.assertEqual(self.git(Path(paths['edk2']), 'status', '--porcelain'), '')
        output = self.root/'tools output'
        subprocess.run(['bash', str(ROOT/'build-tools.sh'), '--output', str(output),
                        '--source-lock', str(self.lock_file), '--source-cache', str(self.cache),
                        '--source-ref', 'brotli=integration'], check=True, stdout=subprocess.PIPE)
        self.assertEqual((output/'bin/selected').read_text(), 'selected Brotli')
        self.assertEqual(self.git(self.edk2/API['MODULES'][0], 'rev-parse', 'HEAD'), self.brotli_pin)

    def test_selected_edk2_updates_the_default_submodule_gitlinks(self):
        (self.brotli/'marker').write_text('new Brotli gitlink')
        head = self.commit(self.brotli)
        for path in API['MODULES']:
            self.git(self.edk2/path, 'fetch', '--quiet', 'origin', head)
            self.git(self.edk2/path, 'checkout', '--quiet', '--detach', head)
        self.commit(self.edk2)
        record, _ = self.resolve(source_refs=['edk2=latest'])
        for name in API['NAMES'][1:]:
            self.assertEqual(record['dependencies'][name]['commit'], head)

    def test_changed_cache_missing_revision_and_conflicting_alias_are_rejected(self):
        _, paths = self.resolve()
        (Path(paths['edk2'])/'marker').write_text('unrecorded edit')
        with self.assertRaisesRegex(ValueError, 'cached source was changed'):
            self.resolve()
        with self.assertRaises(subprocess.CalledProcessError):
            self.resolve(source_refs=['edk2=missing-branch'])
        with self.assertRaises(ValueError):
            API['overrides'](['brotli=latest', API['NAMES'][1]+'=main'])

    def test_external_sources_reject_edits_and_cli_rejects_in_tree_output(self):
        prepared = API['prepare'](self.root/'external', *self.resolve())
        (Path(prepared[API['NAMES'][1]])/'marker').write_text('external edit')
        with self.assertRaisesRegex(ValueError, 'External source was changed'):
            API['inspect'](Path(prepared['edk2']))
        result = subprocess.run([sys.executable, str(ROOT/'tools/upstream.py'), 'prepare',
                                 '--repo', str(self.project), '--output', str(self.project/'build')],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('outside the source tree', result.stderr)

    def test_platform_manifest_checks_sources_and_artifacts(self):
        record, paths = self.resolve()
        output = self.root/'platform build'
        paths = API['prepare'](output, record, paths)
        API['write_json'](output/'upstream.lock.json', record)
        API['write_json'](output/'source-paths.json', paths)
        fd = output/'workspace/Build/Ums9620/RELEASE_GCC5/FV/UMS9620.fd'
        fd.parent.mkdir(parents=True)
        fd.write_bytes(b'fixture firmware')
        abi = self.root/'abi'
        abi.mkdir()
        (abi/'pocketjs_boot_context.h').write_text('fixture ABI')
        command = [sys.executable, str(ROOT/'tools/platform-manifest.py')]
        options = ['--build', str(output), '--abi-include', str(abi)]
        subprocess.run([*command, 'create', *options], check=True, capture_output=True)
        subprocess.run([*command, 'verify', *options], check=True, capture_output=True)
        manifest = json.loads((output/'platform-build.json').read_text())
        self.assertEqual(API['identity'](manifest['upstream_sources']), API['identity'](record))
        fd.write_bytes(b'changed firmware')
        failed = subprocess.run([*command, 'verify', *options], capture_output=True, text=True)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn('recorded artifacts', failed.stderr)
        fd.write_bytes(b'fixture firmware')
        (Path(paths[API['NAMES'][1]])/'marker').write_text('unrecorded source edit')
        failed = subprocess.run([*command, 'verify', *options], capture_output=True, text=True)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn('External source was changed', failed.stderr)


if __name__ == '__main__':
    unittest.main()
