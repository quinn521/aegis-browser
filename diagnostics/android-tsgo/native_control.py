"""Unsigned, input-bound native Linux-x64 counterfactual; never a release job."""
import concurrent.futures
import datetime
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import resource
import signal
import stat
import subprocess
import tarfile
import time
import urllib.request
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = Path('/Volumes/ExternalSSD/repositories/aegis-release-build/android-vm-arm64-20261029')
INPUT_SHA = '8b62182773c2737bb0fcfafbc7dff86275385a540c74f345449ceafb71b64b84'
ZIP_SHA = '848c8198d586273d0cd6f21c66d52510c27bdecd03954d9acc5a0c95894a0a71'
COMPILER_SHA = '4f2de678286401759b3fb4475bafe35b8f32b4b3a07d92642bbf37eadc9b34a4'
URL = 'https://chrome-infra-packages.appspot.com/dl/chromium/third_party/typescript/linux-amd64/+/version:2@7.0.2'
PROJECT = 'gen/third_party/devtools-frontend/src/front_end/models/trace/helpers/bundle-tsconfig.json'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extract_inputs(archive, root, expected):
    if digest(archive) != expected:
        raise ValueError('INPUT_ARCHIVE_HASH')
    with tarfile.open(archive, 'r:gz') as tar:
        members = tar.getmembers()
        if not 0 < len(members) <= 1200 or sum(v.size for v in members) > 20*1024**2:
            raise ValueError('INPUT_MEMBER_LIMIT')
        names = set()
        for member in members:
            path = PurePosixPath(member.name)
            if (not member.isfile() or path.is_absolute() or '..' in path.parts or
                    path.parts[:2] != ('chromium','src') or member.name in names):
                raise ValueError('INPUT_MEMBER')
            names.add(member.name)
        for member in members:
            destination = root / member.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            with destination.open('xb') as output, tar.extractfile(member) as source:
                output.write(source.read())
            destination.chmod(0o444)


def compiler_environment():
    return {'PATH':'/usr/bin:/bin', 'HOME':'/tmp', 'TMPDIR':'/tmp', 'LANG':'C.UTF-8',
            'GOMAXPROCS':'1', 'QEMU_RESERVED_VA':'1099511627776'}


def prepare_compiler():
    archive = ROOT / 'locked-compiler.zip'
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    size = 0
    with opener.open(URL,timeout=45) as response, archive.open('xb') as output:
        while True:
            chunk = response.read(1024*1024)
            if not chunk:
                break
            size += len(chunk)
            if size > 64*1024**2:
                raise ValueError('COMPILER_DOWNLOAD_LIMIT')
            output.write(chunk)
    if digest(archive) != ZIP_SHA:
        raise ValueError('COMPILER_ARCHIVE_HASH')
    tool = ROOT / 'chromium/src/third_party/typescript/linux-amd64/src'
    names = set()
    with zipfile.ZipFile(archive) as zipped:
        entries = zipped.infolist()
        if len(entries) > 200 or sum(v.file_size for v in entries) > 96*1024**2:
            raise ValueError('COMPILER_MEMBER_LIMIT')
        for info in entries:
            path = PurePosixPath(info.filename)
            if path.is_absolute() or '..' in path.parts or stat.S_ISLNK(info.external_attr >> 16) or info.filename in names:
                raise ValueError('COMPILER_MEMBER')
            names.add(info.filename)
            if info.is_dir() or not (info.filename == 'lib/tsc' or
                                    (path.parts[0] == 'lib' and info.filename.endswith('.d.ts'))):
                continue
            target = tool.joinpath(*path.parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open('xb') as output:
                output.write(zipped.read(info))
            target.chmod(0o555 if info.filename == 'lib/tsc' else 0o444)
    compiler = tool / 'lib/tsc'
    if digest(compiler) != COMPILER_SHA:
        raise ValueError('COMPILER_BINARY_HASH')
    return compiler


def invoke(compiler, output, index):
    path = output / ('compiler-%02d.log' % index)
    began = time.monotonic()
    native_seen = False
    timed_out = False
    with path.open('xb') as log:
        child = subprocess.Popen([str(compiler),'-p',PROJECT], env=compiler_environment(),
            cwd=ROOT/'chromium/src/out/AegisAndroid', stdout=log, stderr=subprocess.STDOUT,
            start_new_session=True)
        try:
            while child.poll() is None:
                try:
                    native_seen |= os.readlink('/proc/%s/exe' % child.pid) == str(compiler)
                except (FileNotFoundError, ProcessLookupError):
                    pass
                if time.monotonic()-began > 30:
                    timed_out = True
                    os.killpg(child.pid,signal.SIGKILL)
                    break
                time.sleep(0.01)
            code = child.wait(timeout=5)
        finally:
            if child.poll() is None:
                os.killpg(child.pid,signal.SIGKILL)
                child.wait(timeout=5)
    data = path.read_text(errors='replace')
    return dict(round=index, returncode=code, timedOut=timed_out,
                elapsed=round(time.monotonic()-began,3), logBytes=path.stat().st_size,
                nativeExecutableObserved=native_seen,
                runtimeFault=any(s in data for s in ['SIGSEGV','fatal error:','panic:']))


def main():
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    if (os.environ.get('GITHUB_ACTIONS') != 'true' or os.environ.get('ANDROID_TSGO_HOSTED') != 'github-hosted' or
            platform.system() != 'Linux' or platform.machine() != 'x86_64' or
            os.environ.get('GITHUB_REPOSITORY') != 'quinn521/aegis-browser' or
            os.environ.get('GITHUB_REF') != 'refs/heads/codex/android-tsgo-native-control'):
        raise ValueError('NATIVE_HOSTED_DIAGNOSTIC_ONLY')
    if not ROOT.is_dir() or ROOT.stat().st_uid != os.getuid() or list(ROOT.iterdir()):
        raise ValueError('FRESH_OWNED_DIAGNOSTIC_ROOT_REQUIRED')
    affinity = sorted(os.sched_getaffinity(0))
    if len(affinity) < 2:
        raise ValueError('TWO_NATIVE_CPUS_REQUIRED')
    os.sched_setaffinity(0,affinity[:2])
    output = ROOT/'control-results'
    output.mkdir()
    extract_inputs(HERE/'helpers-inputs.tar.gz',ROOT,INPUT_SHA)
    manifest = json.loads((HERE/'fixture-manifest.json').read_text())
    for path, value in manifest.items():
        if digest(ROOT/'chromium/src'/path) != value['sha256']:
            raise ValueError('INPUT_FILE_HASH')
    compiler = prepare_compiler()
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        for i in range(32):
            rows.extend(pool.map(lambda n:invoke(compiler,output,n),[i*2,i*2+1]))
            (output/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
    inputs_unchanged = all(digest(ROOT/'chromium/src'/p)==v['sha256'] for p,v in manifest.items())
    summary = dict(observedAt=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        diagnosticSourceSha=os.environ.get('GITHUB_SHA'), diagnosticRunId=os.environ.get('GITHUB_RUN_ID'),
        diagnosticAttempt=os.environ.get('GITHUB_RUN_ATTEMPT'), platform=platform.platform(),
        nativeHostedLinuxX64=True, nativeExecutableObservedEveryRun=all(v['nativeExecutableObserved'] for v in rows),
        cpuAffinity=affinity[:2], runs=len(rows), passed=sum(v['returncode']==0 for v in rows),
        runtimeFaults=sum(v['runtimeFault'] for v in rows), timedOut=sum(v['timedOut'] for v in rows),
        compilerSha256=digest(compiler), compilerArchiveSha256=ZIP_SHA, fixtureSha256=INPUT_SHA,
        inputFiles=len(manifest), inputsUnchanged=inputs_unchanged, coreLimit=0,
        childCredentialsForwarded=False, signingKeyMounted=False, playCredentialsUsed=False,
        productBuilt=False, signingQualified=False, storeUploaded=False, tagDraftCreated=False)
    (output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary),flush=True)
    return 0 if (len(rows)==64 and summary['passed']==64 and inputs_unchanged and
                 summary['nativeExecutableObservedEveryRun'] and digest(compiler)==COMPILER_SHA) else 1


if __name__=='__main__':
    try:
        raise SystemExit(main())
    except Exception as error:
        print(json.dumps(dict(status='diagnostic_failed',errorClass=type(error).__name__)),flush=True)
        raise SystemExit(2)
