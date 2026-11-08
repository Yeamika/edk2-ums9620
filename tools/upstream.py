#!/usr/bin/env python3
"""Resolve platform Git sources and prepare a private EDK II checkout."""
import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
from urllib.parse import urljoin

ROOT = Path(__file__).resolve().parents[1]
MODULES = ('BaseTools/Source/C/BrotliCompress/brotli',
           'MdeModulePkg/Library/BrotliCustomDecompressLib/brotli')
NAMES = ('edk2', *('edk2/'+path for path in MODULES))


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2)+'\n')


def overrides(values):
    result = {}
    for item in values:
        name, separator, value = item.partition('=')
        names = NAMES[1:] if name == 'brotli' else (name,)
        if (not separator or not value or value.startswith('-') or '\n' in value or '\r' in value
                or any(key not in NAMES or key in result for key in names)):
            raise ValueError('Expected unique NAME=VALUE; NAME is edk2, brotli or an edk2/<submodule path>')
        result.update({key: value for key in names})
    return result


def selection(value, repository=None, ref=None):
    repository = repository or value['repository']
    ref = ref or value.get('ref', value.get('commit'))
    if not isinstance(repository, str) or not repository or repository.startswith('-'):
        raise ValueError('Invalid upstream repository')
    if not isinstance(ref, str) or not ref or ref.startswith('-'):
        raise ValueError('Supply a full commit, branch, tag or latest')
    if repository.startswith(('.', '/', '~')):
        repository = str(Path(repository).expanduser().resolve())
    return {'repository': repository, 'ref': ref,
            'requested_ref': ref if ref != value.get('commit') else value.get('requested_ref', ref)}


def identity(record):
    result = {}
    for name in NAMES:
        entry = record['dependencies'][name]
        if not re.fullmatch('[0-9a-f]{40}', entry['commit']):
            raise ValueError(f'{name}: lock must contain a resolved full commit')
        result[name] = {key: entry[key] for key in ('repository', 'commit')}
    return result


def outside(repo, path):
    path = path.resolve()
    if path == repo or repo in path.parents:
        raise ValueError(f'Output/cache must be outside the source tree: {repo}')
    return path


@contextlib.contextmanager
def cache_lock(cache):
    cache.mkdir(parents=True, exist_ok=True)
    with (cache/'.lock').open('a') as stream:
        fcntl.flock(stream, fcntl.LOCK_EX)
        yield


def checkout(cache, name, selected):
    repository, ref = selected['repository'], selected['ref']
    key = hashlib.sha256(repository.encode()).hexdigest()
    database = cache/'git'/key
    database.parent.mkdir(parents=True, exist_ok=True)
    if not database.exists():
        subprocess.run(['git', 'init', '--quiet', '--bare', str(database)], check=True)
    pinned = bool(re.fullmatch('[0-9a-f]{40}', ref))
    available = pinned and subprocess.run(
        ['git', '-C', str(database), 'cat-file', '-e', ref+'^{commit}'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
    if not available:
        print(f'Fetching {name}: {ref}', flush=True)
        subprocess.run(['git', '-C', str(database), 'fetch', '--quiet', '--depth=1', '--no-tags',
                        '--', repository, 'HEAD' if ref == 'latest' else ref], check=True)
    commit = git(database, 'rev-parse', (ref if available else 'FETCH_HEAD')+'^{commit}')
    if pinned and commit != ref:
        raise ValueError(f'{name}: fetched commit differs from the requested commit')
    reference = 'refs/heads/pinned-'+commit
    git(database, 'update-ref', reference, commit)
    git(database, 'symbolic-ref', 'HEAD', reference)
    destination = cache/'checkouts'/key/commit
    if destination.exists():
        if (git(destination, 'rev-parse', 'HEAD') != commit
                or git(destination, 'status', '--porcelain', '--untracked-files=all')):
            raise ValueError(f'{name}: cached source was changed: {destination}; use a fresh cache')
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_name(commit+f'.tmp-{os.getpid()}')
        try:
            subprocess.run(['git', 'clone', '--quiet', '--shared', '--no-checkout',
                            str(database), str(temporary)], check=True)
            subprocess.run(['git', '-C', str(temporary), 'checkout', '--quiet', '--detach', commit], check=True)
            temporary.rename(destination)
        finally:
            if temporary.exists():
                shutil.rmtree(temporary)
    return {'repository': repository, 'commit': commit, 'requested_ref': selected['requested_ref'],
            'tree': git(database, 'rev-parse', commit+'^{tree}')}, destination


def gitlink(root, path):
    entry = git(root, 'ls-tree', 'HEAD', '--', path)
    if not entry.startswith('160000 '):
        raise ValueError(f'Selected EDK II has no required submodule: {path}')
    return entry.split()[2]


def module_repository(root, path, parent_repository):
    entries = git(root, 'config', '-f', '.gitmodules', '--get-regexp', r'^submodule\..*\.path$')
    for entry in entries.splitlines():
        key, value = entry.split(None, 1)
        if value == path:
            url = git(root, 'config', '-f', '.gitmodules', '--get', key[:-4]+'url')
            if url.startswith(('./', '../')):
                if '://' in parent_repository:
                    url = urljoin(parent_repository.rstrip('/')+'/', url)
                elif Path(parent_repository).is_absolute():
                    url = str((Path(parent_repository)/url).resolve())
                else:
                    raise ValueError('Use --source-repo brotli=URL for relative SSH submodule URLs')
            return url
    raise ValueError(f'Selected EDK II has no submodule URL for {path}')


def resolve(repo, cache, lock=None, source_repos=(), source_refs=()):
    defaults = json.loads((repo/'sources.lock.json').read_text())['dependencies']
    supplied = json.loads(Path(lock).read_text())['dependencies'] if lock else {}
    repos, refs = overrides(source_repos), overrides(source_refs)
    record, paths = {'schema': 1, 'dependencies': {}}, {}
    with cache_lock(cache):
        core = selection(supplied.get('edk2', defaults['edk2']), repos.get('edk2'), refs.get('edk2'))
        record['dependencies']['edk2'], edk2 = checkout(cache, 'edk2', core)
        paths['edk2'] = str(edk2)
        for path in MODULES:
            name, pin = 'edk2/'+path, gitlink(edk2, path)
            default = (defaults[name] if record['dependencies']['edk2']['commit'] == defaults['edk2']['commit']
                       else {'repository': module_repository(edk2, path, core['repository']), 'commit': pin})
            value = supplied.get(name, default)
            selected = selection(value, repos.get(name), refs.get(name))
            record['dependencies'][name], source = checkout(cache, name, selected)
            record['dependencies'][name]['gitlink'] = pin
            paths[name] = str(source)
    return record, paths


def inspect(root):
    record, paths = {'schema': 1, 'dependencies': {}}, {}
    for name in NAMES:
        path = root if name == 'edk2' else root/name.removeprefix('edk2/')
        if not (path/'.git').exists():
            raise ValueError(f'Initialize the external Git checkout first: {path}')
        if git(path, 'status', '--porcelain', '--untracked-files=all', '--ignore-submodules=all'):
            raise ValueError(f'External source was changed: {path}')
        commit = git(path, 'rev-parse', 'HEAD')
        try:
            repository = subprocess.check_output(['git', '-C', str(path), 'remote', 'get-url', 'origin'],
                                                 text=True, stderr=subprocess.DEVNULL).strip()
        except subprocess.CalledProcessError:
            repository = str(path)
        record['dependencies'][name] = {'repository': repository, 'commit': commit,
                                        'requested_ref': commit, 'tree': git(path, 'rev-parse', 'HEAD^{tree}')}
        if name != 'edk2':
            record['dependencies'][name]['gitlink'] = gitlink(root, name.removeprefix('edk2/'))
        paths[name] = str(path)
    return record, paths


def prepare(output, record, paths):
    destination = output/'edk2-source'
    if destination.exists():
        raise ValueError('Prepared sources exist; choose a fresh output directory')
    prepared = {}
    for name in NAMES:
        path = destination if name == 'edk2' else destination/name.removeprefix('edk2/')
        path.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['git', 'clone', '--quiet', '--no-local', '--depth=1', '--no-checkout',
                        paths[name], str(path)], check=True)
        subprocess.run(['git', '-C', str(path), 'checkout', '--quiet', '--detach',
                        record['dependencies'][name]['commit']], check=True)
        git(path, 'remote', 'set-url', 'origin', record['dependencies'][name]['repository'])
        prepared[name] = str(path)
    return prepared


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=('resolve', 'prepare'))
    p.add_argument('--repo', type=Path, default=ROOT)
    p.add_argument('--output', type=Path, required=True, help='lock file for resolve; build directory for prepare')
    p.add_argument('--source-cache', type=Path)
    p.add_argument('--source-lock', type=Path)
    p.add_argument('--source-repo', action='append', default=[], metavar='NAME=URL')
    p.add_argument('--source-ref', action='append', default=[], metavar='NAME=COMMIT|BRANCH|TAG|latest')
    p.add_argument('--edk2', type=Path, help='use an initialized external EDK II checkout')
    a = p.parse_args()
    repo, output = a.repo.resolve(), a.output.resolve()
    outside(repo, output)
    cache = outside(repo, a.source_cache or Path(os.environ.get('PJB_SOURCE_CACHE') or
                    str((output if a.action == 'prepare' else output.parent)/'upstream-cache')))
    if a.edk2:
        if a.source_lock or a.source_repo or a.source_ref:
            p.error('--edk2 selects an external checkout; omit source lock/repository/ref selections')
        outside(a.edk2.resolve(), output)
        outside(a.edk2.resolve(), cache)
        record, paths = inspect(a.edk2.resolve())
    else:
        record, paths = resolve(repo, cache, a.source_lock, a.source_repo, a.source_ref)
        if a.action == 'prepare':
            paths = prepare(output, record, paths)
    if a.action == 'resolve':
        write_json(output, record)
    else:
        write_json(output/'upstream.lock.json', record)
        write_json(output/'source-paths.json', paths)
    print(json.dumps(identity(record), indent=2))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error
