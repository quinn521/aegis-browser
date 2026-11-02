#!/usr/bin/env python3
"""Unsigned runtime qualification; skipped checks never qualify a runner."""
import io
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import unittest

ROOT=Path(__file__).resolve().parents[2]


def run_suite(suite, stream=None):
    result=unittest.TextTestRunner(stream=stream or io.StringIO(),verbosity=2).run(suite)
    if not result.wasSuccessful() or result.skipped or result.testsRun!=8:
        raise ValueError('All eight native admission checks must pass without skipped or missing tests')
    return result


def main():
    if platform.system()!='Linux' or platform.machine() not in {'aarch64','arm64'}:
        raise ValueError('Actual Linux ARM64 runtime is required')
    if os.environ.get('AEGIS_ANDROID_EXPERIMENTAL_ARM64')!='true' or os.environ.get('QEMU_RESERVED_VA')!='1099511627776':
        raise ValueError('Explicit pinned Android ARM64 runtime settings are required')
    for key in ['AEGIS_ANDROID_NATIVE_SOURCE','AEGIS_ANDROID_GN_BINARY']:
        if not os.environ.get(key) or not Path(os.environ[key]).exists():
            raise ValueError('Actual locked native inputs are required')
    sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    tree=subprocess.check_output(['git','rev-parse','HEAD^{tree}'],cwd=ROOT,text=True).strip()
    if sha!=os.environ.get('GITHUB_SHA'):
        raise ValueError('Native admission checkout differs from source pin')
    suite=unittest.defaultTestLoader.discover(str(ROOT/'scripts/release'),pattern='test_android_host.py')
    result=run_suite(suite,sys.stdout)
    folder=ROOT/'.artifacts/android-native-admission';folder.mkdir(parents=True,exist_ok=False)
    proof=dict(status='unsigned_native_admission_passed',sourceSha=sha,sourceTree=tree,
               runId=os.environ['GITHUB_RUN_ID'],attempt=os.environ['GITHUB_RUN_ATTEMPT'],tests=result.testsRun,skips=0,
               productBuilt=False,productSigned=False,playUploadPerformed=False)
    (folder/'result.json').write_text(json.dumps(proof,indent=2)+'\n')
    print(json.dumps(proof))


if __name__=='__main__':
    try: main()
    except Exception:
        print('Android unsigned native admission failed; release readiness remains unqualified',file=sys.stderr)
        sys.exit(1)
