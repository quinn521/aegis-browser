#!/usr/bin/env python3
"""Explicit temporary ad-hoc signing, deepest Mach-O and bundle first.

The caller owns the build/staging lock. No keychain, quarantine or Gatekeeper
settings are touched. A later certificate package must use a separate attempt.
"""
import argparse
import json
import os
from pathlib import Path
import plistlib
import subprocess
import tempfile

MACHO = {bytes.fromhex(value) for value in (
    'feedface', 'cefaedfe', 'feedfacf', 'cffaedfe', 'cafebabe', 'bebafeca', 'cafebabf', 'bfbafeca')}
BUNDLES = {'.app', '.framework', '.xpc', '.appex', '.bundle'}


def targets(app):
    if app.is_symlink() or not app.is_dir() or app.suffix != '.app' or app.resolve() != app.absolute():
        raise ValueError('Expected a real application bundle without symlink parents')
    candidates = set()
    for directory, folders, files in os.walk(app, followlinks=False):
        parent = Path(directory)
        for name in folders + files:
            path = parent / name
            if path.is_symlink() and not path.resolve(strict=True).is_relative_to(app):
                raise ValueError('Application symlink escapes its bundle')
        folders[:] = [name for name in folders if not (parent / name).is_symlink()]
        if parent.suffix in BUNDLES:
            candidates.add(parent)
        for name in files:
            path = parent / name
            if not path.is_symlink():
                with path.open('rb') as stream:
                    if stream.read(4) in MACHO:
                        candidates.add(path)
    return sorted(candidates, key=lambda path: (-len(path.parts), str(path)))


def run(*args):
    result = subprocess.run(args, capture_output=True, timeout=120)
    if result.returncode:
        raise ValueError(f'Temporary signing command failed ({result.returncode}): ' +
                         result.stderr.decode(errors='replace'))
    return result


def sign(app):
    ordered = targets(app)
    if not ordered or ordered[-1] != app:
        raise ValueError('Application must be signed last')
    with tempfile.TemporaryDirectory(prefix='aegis-temporary-entitlements-') as temporary:
        entitlements = Path(temporary) / 'entitlements.plist'
        for path in ordered:
            # Preserve existing Chromium entitlements if present. An unsigned
            # target has no entitlements; malformed nonempty output fails closed.
            displayed = subprocess.run(['/usr/bin/codesign', '-d', '--entitlements', ':-', str(path)],
                                       capture_output=True, timeout=120)
            options = []
            if displayed.stdout.strip():
                value = plistlib.loads(displayed.stdout)
                if not isinstance(value, dict):
                    raise ValueError('Unexpected code entitlements')
                entitlements.write_bytes(plistlib.dumps(value))
                options = ['--entitlements', str(entitlements)]
            run('/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', *options, str(path))
            run('/usr/bin/codesign', '--verify', '--strict', str(path))
            signature = run('/usr/bin/codesign', '-d', '--verbose=4', str(path)).stderr.decode().splitlines()
            if 'Signature=adhoc' not in signature:
                raise ValueError('Expected an explicit ad-hoc signature for every code target')
    run('/usr/bin/codesign', '--verify', '--deep', '--strict', str(app))
    return {'signing': 'ad-hoc-not-notarized', 'temporary': True, 'targetsVerified': len(ordered),
            'notarized': False, 'gatekeeperAssessment': 'not_performed', 'runtimeAcceptance': False,
            'formalDistributionQualified': False, 'releaseReady': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('app', type=Path)
    args = parser.parse_args()
    print(json.dumps(sign(args.app.absolute()), indent=2))
