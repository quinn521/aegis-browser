import assert from 'node:assert/strict';
import {execFileSync, spawn, spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {once} from 'node:events';
import {copyFileSync, existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, realpathSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {dirname, join} from 'node:path';
import test from 'node:test';
import {setTimeout as delay} from 'node:timers/promises';
import YAML from 'yaml';

const workflow = YAML.parse(readFileSync('.github/workflows/ios-release-validation.yml', 'utf8'));
const archiveSteps = workflow.jobs['ios-archive'].steps;
const prepare = archiveSteps.find(step => step.name === 'Prepare locked EmbeddedCore XCFramework');
const token = 'synthetic-embedded-core-proxy-token';
const proxy = `http://fixture:${token}@proxy.invalid:8080`;
const moduleVersion = 'v0.0.0-20261011034528-197770444867';
const lock = 'apps/ios/EmbeddedCore/embedded-core-lock.json';
const moduleSum = 'h1:rav73cdd7OQUPgt1aIFbZQTiPNnjfueKXvfOiLCY0Ss=';
const sourceCommit = 'a'.repeat(40);
const arch = process.arch === 'arm64' ? 'arm64' : 'amd64';
const goArchive = `go1.27.3.darwin-${arch}.tar.gz`;

const packageFixture = String.raw`
import os
from pathlib import Path
import plistlib
import shutil
import sys

mode, *arguments = sys.argv[1:]
name = "AegisEmbeddedCore"
if mode == "generate":
    tmp = Path(os.environ["TMPDIR"]) / "gomobile-work-123"
    output, = arguments
    frameworks = []
    for platform, sdk in (("ios", "iphoneos"), ("iossimulator", "iphonesimulator")):
        framework = tmp / platform / sdk / (name + ".framework")
        (framework / "Headers").mkdir(parents=True)
        (framework / "Modules").mkdir()
        (framework / "Headers" / (name + ".h")).write_text("fixture header")
        (framework / "Modules/module.modulemap").write_text("fixture module map")
        (framework / name).write_text("fixture binary " + sdk + "\n")
        if platform == "ios" and os.environ["PREP_SCENARIO"] == "redirected-framework":
            (framework / name).unlink()
            (framework / name).symlink_to(Path(os.environ["PREP_PACKAGE"]))
        (framework / "Info.plist").write_bytes(plistlib.dumps({
            "CFBundleExecutable": name, "CFBundleIdentifier": name, "CFBundlePackageType": "FMWK",
            "MinimumOSVersion": "100.0", "CFBundleVersion": "0.0.12345",
            "CFBundleShortVersionString": "0.0.999" if os.environ["PREP_SCENARIO"] == "unexpected-version" else "0.0.12345",
        }))
        frameworks.append(str(framework))
    os.execvp("xcodebuild", ["xcodebuild", "-create-xcframework", "-framework", frameworks[0],
                          "-framework", frameworks[1], "-framework", frameworks[1], "-output", output])
else:
    assert arguments[0] == "-create-xcframework"
    output = Path(arguments[-1])
    libraries = []
    for framework in map(Path, dict.fromkeys(arguments[2:-2:2])):
        sim = framework.parent.name == "iphonesimulator"
        identifier = "ios-arm64_x86_64-simulator" if sim else "ios-arm64"
        shutil.copytree(framework, output / identifier / framework.name)
        fields = dict(LibraryIdentifier=identifier, LibraryPath=framework.name,
                      BinaryPath=framework.name + "/" + name,
                      SupportedPlatform="ios", SupportedArchitectures=["arm64"])
        if sim:
            fields.update(SupportedPlatformVariant="simulator", SupportedArchitectures=["arm64", "x86_64"])
        libraries.append(fields)
        if not sim and os.environ["PREP_SCENARIO"] == "changed-packaged-slice":
            (output / identifier / framework.name / name).write_text("not the generated archive")
    (output / "Info.plist").write_bytes(plistlib.dumps({
        "AvailableLibraries": libraries[::-1], "CFBundlePackageType": "XFWK", "XCFrameworkFormatVersion": "1.0"}))
`;

const tools = String.raw`#!/bin/bash
set -eo pipefail
tool="$(basename "$0")"
printf '%s %s %s\n' "$tool" "$1" "$2" >> "$PREP_CALLS"
[[ "$HTTPS_PROXY" == "$PREP_PROXY" && "$GOPROXY" == direct && "$GONOPROXY" == 'example.invalid/*' ]]
[[ "$GIT_CONFIG_KEY_0" == http.proxy && "$GIT_CONFIG_VALUE_0" == "$PREP_PROXY" ]]
if [[ "$tool" != curl ]]; then
  [[ -z "$GIT_DIR" || "$PWD" == "$IOS_CI_SOURCE" && "$1" == version ]]
fi
diagnostic() {
  printf 'tool stdout %s\n' "$HTTPS_PROXY"
  printf 'tool stderr %s\n' "$HTTPS_PROXY" >&2
}
fail() {
  diagnostic
  exit "$1"
}
case "$tool:$1" in
  curl:--disable)
    [[ "$2 $3 $4 $5 $6" == '--proto =https --fail --silent --show-error' ]]
    diagnostic
    printf '%s\n' "$HTTPS_PROXY" > "$TMPDIR/download-diagnostics"
    while [[ "$1" != --output ]]; do shift; done
    case "$3" in
      'https://go.dev/dl/?mode=json&include=all')
        printf 'catalog\n' >> "$PREP_CALLS"
        [[ "$PREP_SCENARIO" == catalog-error ]] && fail 22
        cp "$PREP_CATALOG" "$2"
        ;;
      "https://dl.google.com/go/$PREP_ARCHIVE_NAME")
        printf 'archive\n' >> "$PREP_CALLS"
        if [[ "$PREP_SCENARIO" == download-cancel ]]; then
          printf '%s\n' "$$" > "$PREP_READY"
          exec /bin/sleep 15
        fi
        [[ "$PREP_SCENARIO" == archive-error ]] && fail 28
        cp "$PREP_ARCHIVE" "$2"
        ;;
      *) exit 99 ;;
    esac
    ;;
  go:version)
    [[ "$0" == "$GOROOT/bin/go" && "$GOTOOLCHAIN" == local ]]
    if [[ "$2" == -m ]]; then
      [[ "$3" == -json ]]
      [[ "$PREP_SCENARIO" == record && -f "$IOS_CI_ROOT/EmbeddedCore/slice-digests.txt" ]] && fail 47
      binding="$(basename "$4")"
      if [[ "$binding" == gobind ]]; then
        case "$PREP_SCENARIO:$4" in
          wrong-gobind:*"/go path/"*|stale-install:*"/go path/"*|wrong-private-gobind:*"/go path/"*)
            PREP_MODULE_SUM='h1:invalid' ;;
          wrong-private-gobind:*/tools/gobind) PREP_MODULE_SUM='h1:invalid' ;;
        esac
      fi
      printf '{"GoVersion":"go1.27.3","Path":"golang.org/x/mobile/cmd/%s","Main":{"Path":"golang.org/x/mobile","Version":"%s","Sum":"%s"}}\n' "$binding" "$PREP_MODULE_VERSION" "$PREP_MODULE_SUM"
      if [[ "$PREP_SCENARIO" == metadata-gomobile ]]; then printf '%s\n' "$HTTPS_PROXY"; fi
    elif [[ "$PREP_SCENARIO" == version-failure ]]; then
      fail 42
    elif [[ "$PREP_SCENARIO" == version-mismatch ]]; then
      diagnostic
      printf 'go version go1.26.0 darwin/%s\n' "$PREP_ARCH"
    else
      printf 'go version go1.27.3 darwin/%s\n' "$PREP_ARCH"
      if [[ "$PREP_SCENARIO" == metadata-go && -f "$IOS_CI_ROOT/EmbeddedCore/slice-digests.txt" ]]; then
        printf 'unexpected diagnostic: %s\n' "$HTTPS_PROXY"
      fi
    fi
    printf 'version diagnostic %s\n' "$HTTPS_PROXY" >&2
    ;;
  go:tool)
    [[ "$2 $3" == 'compile -V=full' ]]
    printf 'compile version go1.27.3\n'
    ;;
  go:env)
    [[ "$2" == -json ]]
    printf '{"GOFLAGS":"%s","GOBIN":"%s","GOPATH":"%s","GOPROXY":"%s","GONOPROXY":"%s"}\n' "$GOFLAGS" "$GOBIN" "$GOPATH" "$GOPROXY" "$GONOPROXY"
    ;;
  go:install)
    [[ "$#" == 3 && "$2" == "golang.org/x/mobile/cmd/gomobile@$PREP_MODULE_VERSION"       && "$3" == "golang.org/x/mobile/cmd/gobind@$PREP_MODULE_VERSION" ]]
    [[ "$GOBIN" == "$IOS_CI_SCRATCH/tmp/"*/tools || -z "$IOS_CI_SCRATCH" ]]
    [[ "$GOTMPDIR" == "$PWD/go-tmp" && -d "$GOTMPDIR" && "$GOTMPDIR" != "$PREP_FOREIGN_TMP" ]]
    printf '%s\n' 'synthetic compiler intermediate' > "$GOTMPDIR/install-work"
    [[ "$PREP_SCENARIO" == install || "$PREP_SCENARIO" == stale-install ]] && fail 43
    diagnostic
    cp "$PREP_TOOLS" "$GOBIN/gomobile"
    cp "$PREP_TOOLS" "$GOBIN/gobind"
    ;;
  go:mod)
    if [[ "$2" == edit ]]; then
      [[ "$3" == -json ]]
      printf '{"Module":{"Path":"aegis.browser/ios/embeddedcore"}}\n'
    else
      [[ "$2" == tidy && "$PWD" == */aegis-core-*/bridge ]]
      diagnostic
      git fixture-fetch
    fi
    ;;
  git:fixture-fetch)
    if [[ -n "$GIT_TRACE" ]]; then printf '%s\n' "$HTTPS_PROXY" >> "$GIT_TRACE"; fi
    [[ "$PREP_SCENARIO" == tidy ]] && fail 45
    diagnostic
    ;;
  gomobile:bind)
    [[ "$TOOLCHAINS" == com.apple.dt.toolchain.XcodeDefault && "$DEVELOPER_DIR" == "$PREP_DEVELOPER" ]]
    [[ "$GOPACKAGESDRIVER" == off && -d "$GOTMPDIR" && "$GOTMPDIR" == "$(dirname "$PWD")/go-tmp" ]]
    printf '%s\n' 'synthetic binding intermediate' > "$GOTMPDIR/bind-work"
    [[ "$PREP_SCENARIO" == bind ]] && fail 46
    diagnostic
    if [[ "$PREP_SCENARIO" == cancel ]]; then
      printf '%s\n' "$$" > "$PREP_READY"
      exec /bin/sleep 15
    fi
    if [[ "$PREP_SCENARIO" == changed-bridge ]]; then
      printf '\n// changed during bind\n' >> go.mod
    fi
    if [[ "$PREP_SCENARIO" == changed-clang ]]; then printf 'replacement compiler' >> "$PREP_CLANG"; fi
    while [[ "$1" != -o ]]; do shift; done
    python3 "$PREP_PACKAGE" generate "$2"
    ;;
  xcodebuild:-create-xcframework)
    [[ "$TOOLCHAINS" == com.apple.dt.toolchain.XcodeDefault && "$DEVELOPER_DIR" == "$PREP_DEVELOPER" ]]
    python3 "$PREP_PACKAGE" pack "$@"
    ;;
  git:--no-replace-objects)
    [[ "$2" == rev-parse ]]
    printf '%s\n' "$PREP_SOURCE_COMMIT"
    if [[ "$PREP_SCENARIO" == metadata-commit ]]; then printf '%s\n' "$HTTPS_PROXY"; fi
    ;;
  xcodebuild:-version)
    [[ "$TOOLCHAINS" == com.apple.dt.toolchain.XcodeDefault && "$DEVELOPER_DIR" == "$PREP_DEVELOPER" ]]
    printf 'Xcode 27.0\nBuild version 27A266a\n'
    if [[ "$PREP_SCENARIO" == metadata-xcode ]]; then printf '%s\n' "$HTTPS_PROXY"; fi
    ;;
  xcrun:--sdk)
    [[ "$TOOLCHAINS" == com.apple.dt.toolchain.XcodeDefault && "$DEVELOPER_DIR" == "$PREP_DEVELOPER" ]]
    if [[ "$3 $4" == "--find clang" ]]; then
      printf '%s\n' "$PREP_CLANG"
    elif [[ "$3" == "--show-sdk-build-version" ]]; then
      printf '24A111\n'
    else
      printf '27.0\n'
    fi
    if [[ "$PREP_SCENARIO" == "metadata-$2" ]]; then printf '%s\n' "$HTTPS_PROXY"; fi
    ;;
  *) exit 99 ;;
esac
`;

function fixture(t, scenario) {
  const root = mkdtempSync(join(realpathSync(tmpdir()), 'ios-prepare-'));
  t.after(() => rmSync(root, {recursive: true, force: true}));
  const source = join(root, 'Source with spaces');
  const job = join(root, 'job');
  const bin = join(root, 'bin');
  const goPath = join(root, 'go path');
  const distribution = join(root, 'distribution');
  const archive = join(root, goArchive);
  const catalog = join(root, 'catalog');
  const toolScript = join(root, 'tool-stub');
  const clang = join(bin, 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang');
  const foreignTemp = join(root, 'foreign-go-tmp');
  const calls = join(root, 'calls');
  const ready = join(root, 'ready');
  const products = join(job, 'EmbeddedCore');
  const trace = join(job, 'git-trace.log');
  const python = execFileSync('python3', ['-I', '-c', 'import sys; print(sys.executable)'], {encoding: 'utf8'}).trim();
  mkdirSync(job, {mode: 0o700});
  mkdirSync(foreignTemp);
  writeFileSync(join(foreignTemp, 'sentinel'), 'other job temp\n');
  mkdirSync(dirname(clang), {recursive: true});
  writeFileSync(clang, 'selected fixture compiler\n');
  for (const directory of [bin, join(goPath, 'bin'), join(distribution, 'go/bin')]) mkdirSync(directory, {recursive: true});
  for (const file of [
    lock,
    'apps/ios/EmbeddedCore/bridge/go.mod',
    'apps/ios/EmbeddedCore/bridge/go.sum',
    'scripts/ci/ios-archive-supervisor.py',
    'scripts/ci/ios-archive-go.py',
    'scripts/ci/run-ios-embedded-core-prepare.sh',
    'scripts/release/prepare-ios-access-core.sh',
    'scripts/release/build-ios-access-core.sh',
    'scripts/release/record-ios-access-core-digests.sh',
    'scripts/release/ios_embedded_core.py',
    'scripts/release/ios_core_build.py',
  ]) {
    mkdirSync(join(source, file, '..'), {recursive: true});
    copyFileSync(file, join(source, file));
  }
  if (process.platform !== 'darwin') {
    const installer = join(source, 'scripts/ci/ios-archive-go.py');
    copyFileSync(installer, `${installer}.fixture`);
    writeFileSync(installer, `import runpy, sys\nsys.platform = "darwin"\nrunpy.run_path(__file__ + ".fixture", run_name="__main__")\n`);
  }
  writeFileSync(toolScript, tools, {mode: 0o755});
  for (const tool of ['curl', 'git', 'xcodebuild', 'xcrun']) copyFileSync(toolScript, join(bin, tool));
  const unexpectedTool = '#!/bin/bash\necho unexpected tool selection >> "$PREP_CALLS"\nexit 98\n';
  for (const tool of ['go', 'mise']) writeFileSync(join(bin, tool), unexpectedTool, {mode: 0o755});
  copyFileSync(toolScript, join(distribution, 'go/bin/go'));
  execFileSync('/usr/bin/tar', ['-czf', archive, '-C', distribution, 'go']);
  const payload = readFileSync(archive);
  const entry = {filename: goArchive, os: 'darwin', arch, version: 'go1.27.3', kind: 'archive', size: payload.length,
    sha256: createHash('sha256').update(payload).digest('hex')};
  if (scenario === 'bad-digest') entry.sha256 = '0'.repeat(64);
  if (scenario === 'bad-size') entry.size += 1;
  const files = scenario === 'missing-release' ? [] : scenario === 'duplicate-release' ? [entry, entry] : [entry];
  writeFileSync(catalog, JSON.stringify([{version: entry.version, files}]));
  const warm = ['installed-gomobile', 'wrong-gobind', 'wrong-private-gobind', 'stale-install'].includes(scenario);
  const globalPair = warm ? ['gomobile', 'gobind'].map(name => join(goPath, 'bin', name)) : [];
  for (const path of globalPair) copyFileSync(toolScript, path);
  const packageScript = join(root, 'package.py');
  writeFileSync(packageScript, packageFixture);
  const shared = join(goPath, 'pkg/gomobile');
  mkdirSync(shared, {recursive: true});
  writeFileSync(join(shared, 'sentinel'), 'runner-owned gomobile state\n');
  writeFileSync(join(bin, 'python3'), '#!/bin/bash\nexec "$PREP_PYTHON" "$@"\n', {mode: 0o755});
  const options = {cwd: root, encoding: 'utf8', env: {
    PATH: `${bin}:/usr/bin:/bin`, HOME: root, GOPATH: goPath, GOBIN: join(goPath, 'bin'),
    GOPROXY: 'direct', GONOPROXY: 'example.invalid/*', GOTOOLCHAIN: 'auto',
    TOOLCHAINS: 'org.fixture.alternative', DEVELOPER_DIR: bin, GOTMPDIR: foreignTemp,
    GOPACKAGESDRIVER: 'must-not-run',
    HTTPS_PROXY: proxy, GIT_CONFIG_COUNT: '1', GIT_CONFIG_KEY_0: 'http.proxy', GIT_CONFIG_VALUE_0: proxy,
    GIT_CONFIG_NOSYSTEM: '1', GIT_ALLOW_PROTOCOL: 'file', GIT_DIR: join(root, 'wrong checkout'), GIT_TRACE: trace,
    IOS_CI_ROOT: job, IOS_CI_SOURCE: source, PREP_SCENARIO: scenario, PREP_PYTHON: python,
    PREP_ARCH: arch, PREP_ARCHIVE: archive, PREP_ARCHIVE_NAME: goArchive, PREP_CATALOG: catalog,
    PREP_MODULE_VERSION: moduleVersion, PREP_MODULE_SUM: moduleSum, PREP_PACKAGE: packageScript, PREP_SOURCE_COMMIT: sourceCommit, PREP_TOOLS: toolScript, PREP_PROXY: proxy,
    PREP_CALLS: calls, PREP_READY: ready, PREP_CLANG: clang, PREP_DEVELOPER: bin, PREP_FOREIGN_TMP: foreignTemp,
  }};
  return {options, job, products, trace, calls, ready, shared, clang, foreignTemp, globalPair,
    globalToolBytes: readFileSync(toolScript)};
}

const bash = ['-e', '-o', 'pipefail', '-c', prepare.run];

function assertPrivate({job, trace, foreignTemp, globalPair, globalToolBytes}, stdout, stderr) {
  const sinks = [];
  if (stdout.includes(token)) sinks.push('Actions stdout');
  if (stderr.includes(token)) sinks.push('Actions stderr');
  for (const entry of readdirSync(job, {recursive: true, withFileTypes: true})) {
    if (!entry.isFile()) continue;
    if (readFileSync(join(entry.parentPath, entry.name)).includes(token)) sinks.push(entry.name);
  }
  assert.deepEqual(sinks, [], 'tool diagnostics escaped the supervisor');
  assert(!existsSync(trace), 'Git tracing created a diagnostic file');
  assert(!readdirSync(dirname(job)).some(name => name.startsWith('.aegis-ios-scratch-')), 'private setup scratch survived');
  assert.deepEqual(readdirSync(foreignTemp), ['sentinel']);
  assert.equal(readFileSync(join(foreignTemp, 'sentinel'), 'utf8'), 'other job temp\n');
  for (const path of globalPair) assert.deepEqual(readFileSync(path), globalToolBytes);
  assert.equal(stdout, '');
}

const starting = 'EmbeddedCore preparation starting (tool diagnostics withheld)\n';
const setupExited = code => `locked Go setup exited with status ${code}\n`;
const exited = code => `EmbeddedCore preparation exited with status ${code}\n`;

for (const [scenario, phase, status] of [
  ['version-failure', 'go version', 42],
  ['version-mismatch', 'go version', 2],
  ['install', 'go install', 43],
  ['wrong-private-gobind', 'go install', 1],
  ['stale-install', 'go install', 43],
  ['tidy', 'git fixture-fetch', 45],
  ['bind', 'gomobile bind', 46],
  ['changed-bridge', 'gomobile bind', 1],
  ['redirected-framework', 'gomobile bind', 1],
  ['unexpected-version', 'gomobile bind', 1],
  ['changed-packaged-slice', 'xcodebuild -create-xcframework', 1],
  ['record', 'go version -m', 47],
  ['metadata-go', 'go version -m', 1],
  ['metadata-commit', 'git --no-replace-objects rev-parse', 1],
  ['changed-clang', 'gomobile bind', 1],
  ['metadata-xcode', 'xcodebuild -version', 1],
  ['metadata-iphoneos', 'xcrun --sdk iphoneos', 1],
  ['metadata-iphonesimulator', 'xcrun --sdk iphonesimulator', 1],
  ['metadata-gomobile', 'go version -m', 1],
]) {
  test(`EmbeddedCore preparation contains ${scenario} failure output and returns its status`, t => {
    const setup = fixture(t, scenario);
    const result = spawnSync('bash', bash, {...setup.options, timeout: 20000});
    assert.ifError(result.error);
    assertPrivate(setup, result.stdout, result.stderr);
    assert.equal(result.status, status, result.stderr);
    assert.equal(result.stderr, starting + setupExited(scenario.startsWith('version-') ? status : 0) + exited(status));
    assert(readFileSync(setup.calls, 'utf8').includes(phase), `fixture must reach ${phase}`);
    assert(!existsSync(join(setup.products, 'build-manifest.json')));
    assert(!existsSync(join(setup.products, 'embedded-core-lock.json')));
    assert(!existsSync(join(setup.products, 'build-inputs.xcfilelist')));
  });
}

for (const [scenario, status, reachesArchive] of [
  ['catalog-error', 22, false], ['missing-release', 2, false], ['duplicate-release', 2, false],
  ['archive-error', 28, true], ['bad-digest', 2, true], ['bad-size', 2, true],
]) {
  test(`locked Go setup contains ${scenario} output and gates preparation`, t => {
    const setup = fixture(t, scenario);
    const result = spawnSync('bash', bash, {...setup.options, timeout: 20000});
    assert.ifError(result.error);
    assertPrivate(setup, result.stdout, result.stderr);
    assert.equal(result.status, status, result.stderr);
    assert.equal(result.stderr, starting + setupExited(status) + exited(status));
    const calls = readFileSync(setup.calls, 'utf8');
    assert.equal(calls.includes('\narchive\n'), reachesArchive);
    assert.doesNotMatch(calls, /^go (version|install|env)|unexpected tool selection/mu);
    assert(!existsSync(setup.products));
  });
}

for (const scenario of ['cold-install', 'installed-gomobile', 'wrong-gobind']) {
  test(`EmbeddedCore preparation retains the archive inputs on ${scenario}`, t => {
    const setup = fixture(t, scenario);
    const result = spawnSync('bash', bash, {...setup.options, timeout: 20000});
    assert.ifError(result.error);
    assertPrivate(setup, result.stdout, result.stderr);
    assert.equal(result.status, 0, result.stderr);
    assert.equal(result.stderr, starting + setupExited(0) + exited(0));
    const calls = readFileSync(setup.calls, 'utf8');
    assert.match(calls, /catalog\n/u);
    assert.match(calls, /archive\n/u);
    assert.doesNotMatch(calls, /unexpected tool selection/u);
    assert.equal(calls.includes('go install'), scenario !== 'installed-gomobile');
    assert(!calls.includes('gomobile init'));
    assert.equal(readFileSync(join(setup.shared, 'sentinel'), 'utf8'), 'runner-owned gomobile state\n');
    const manifest = JSON.parse(readFileSync(join(setup.products, 'build-manifest.json'), 'utf8'));
    assert.equal(manifest.sourceCommit, sourceCommit);
    assert.match(manifest.compiledBridgeSha256, /^[a-f0-9]{64}$/u);
    assert.equal(manifest.compiledBridgeSha256, manifest.bridgeSha256);
    assert.equal(manifest.compiledBridgeSha256, readFileSync(join(setup.products, 'compiled-bridge.sha256'), 'utf8').trim());
    assert.equal(manifest.digestMode, 'ci-rebuild');
    assert.equal(manifest.gomobileModule, `golang.org/x/mobile@${moduleVersion}`);
    assert.equal(manifest.gobindModule, manifest.gomobileModule);
    assert.equal(manifest.goVersion, `go version go1.27.3 darwin/${arch}`);
    assert.equal(manifest.xcodeVersion, 'Xcode 27.0\nBuild version 27A266a');
    assert.equal(manifest.appleToolchain, 'com.apple.dt.toolchain.XcodeDefault');
    const clangHash = createHash('sha256').update(readFileSync(setup.clang)).digest('hex');
    assert.deepEqual(manifest.clangSha256, {iphoneos: clangHash, iphonesimulator: clangHash});
    assert.deepEqual(manifest.sdkVersions, {iphoneos: '27.0', iphonesimulator: '27.0'});
    const archive = archiveSteps.find(step => step.name === 'Compile uncredentialed native archive');
    assert(archiveSteps.indexOf(prepare) < archiveSteps.indexOf(archive));
    assert.equal(archive.env.IOS_CI_ROOT, prepare.env.IOS_CI_ROOT);
    assert.equal(archive.env.IOS_CI_SOURCE, prepare.env.IOS_CI_SOURCE);
    for (const [slice, sdk] of [['ios-arm64', 'iphoneos'], ['ios-arm64_x86_64-simulator', 'iphonesimulator']]) {
      const binary = join(setup.products, 'AegisEmbeddedCore.xcframework', slice, 'AegisEmbeddedCore.framework', 'AegisEmbeddedCore');
      assert.equal(readFileSync(binary, 'utf8'), `fixture binary ${sdk}\n`);
      assert(readFileSync(join(setup.products, 'build-inputs.xcfilelist'), 'utf8').includes(binary + '\n'));
    }
    const effective = JSON.parse(readFileSync(join(setup.products, 'embedded-core-lock.json'), 'utf8'));
    for (const [key, value] of Object.entries(manifest.slices)) {
      assert.match(value, /^[a-f0-9]{64}$/u);
      assert.equal(effective.artifacts[key], value);
    }
  });
}

test('local EmbeddedCore preparation records the toolchain selected by the build', t => {
  const setup = fixture(t, 'installed-gomobile');
  const {env, cwd} = setup.options;
  writeFileSync(join(cwd, 'bin', 'mise'), '#!/bin/bash\n[[ "$*" == "where go@1.27.3" ]] || exit 98\nprintf "%s\\n" "$PREP_LOCAL_GO"\n',
    {mode: 0o755});
  const result = spawnSync('bash', ['scripts/release/prepare-ios-access-core.sh', lock, '../job/EmbeddedCore'], {
    ...setup.options, cwd: env.IOS_CI_SOURCE, timeout: 20000,
    env: {...env, GIT_DIR: '', GIT_TRACE: '', GOTOOLCHAIN: 'local',
      PREP_LOCAL_GO: join(cwd, 'distribution', 'go'), AEGIS_EMBEDDED_CORE_DIGEST_MODE: 'ci-rebuild'},
  });
  assert.ifError(result.error);
  assert.equal(result.status, 0);
  const manifest = JSON.parse(readFileSync(join(setup.products, 'build-manifest.json'), 'utf8'));
  assert.equal(manifest.goVersion, `go version go1.27.3 darwin/${arch}`);
  assert.equal(manifest.sourceCommit, sourceCommit);
  const calls = readFileSync(setup.calls, 'utf8');
  assert.doesNotMatch(calls, /unexpected tool selection/u);
  assert.match(calls, /go mod tidy[\s\S]+gomobile bind[\s\S]+go version -m/u);
});

test('offline preparation worker replaces a stale pair once and fails closed on bad private tools or compiler drift', async t => {
  const worker = String.raw`
import os
from pathlib import Path
import runpy
import shutil
import subprocess

source = Path(os.environ["IOS_CI_SOURCE"])
supervisor = runpy.run_path(str(source / "scripts/ci/ios-archive-supervisor.py"))
scratch = supervisor["JobScratch"](Path(os.environ["IOS_CI_ROOT"]))
try:
    env = scratch.environment(supervisor["archive_environment"]())
    assert env["GOTMPDIR"] == env["TMPDIR"] == str(scratch.path / "tmp")
    shutil.copytree(os.environ["PREP_LOCAL_GO"], scratch.path / "go")
    status = subprocess.run(["/bin/bash", "scripts/ci/run-ios-embedded-core-prepare.sh"], cwd=source, env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15).returncode
finally:
    assert scratch.clean()
raise SystemExit(status)
`;
  for (const [scenario, expected] of [['wrong-gobind', 0], ['stale-install', 43],
    ['wrong-private-gobind', 1], ['changed-clang', 1]]) {
    await t.test(scenario, subtest => {
      const setup = fixture(subtest, scenario);
      const {cwd, env} = setup.options;
      const result = spawnSync(env.PREP_PYTHON, ['-I', '-B', '-c', worker], {
        ...setup.options, timeout: 18000,
        env: {...env, PREP_LOCAL_GO: join(cwd, 'distribution', 'go')},
      });
      assert.ifError(result.error);
      assert.equal(result.status, expected, result.stderr);
      assertPrivate(setup, result.stdout, result.stderr);
      const calls = readFileSync(setup.calls, 'utf8');
      assert.equal(calls.split('\n').filter(line => line.startsWith('go install ')).length, 1);
      const manifest = join(setup.products, 'build-manifest.json');
      if (expected === 0) {
        const receipt = JSON.parse(readFileSync(manifest, 'utf8'));
        assert.equal(receipt.gobindModule, `golang.org/x/mobile@${moduleVersion}`);
        assert.equal(receipt.appleToolchain, 'com.apple.dt.toolchain.XcodeDefault');
      } else {
        assert(!existsSync(manifest));
      }
      if (scenario === 'wrong-private-gobind' || scenario === 'stale-install') assert(!calls.includes('gomobile bind'));
    });
  }
});

for (const [scenario, signal, status] of [['cancel', 'SIGINT', 130], ['cancel', 'SIGTERM', 143], ['download-cancel', 'SIGTERM', 143]]) {
  test(`EmbeddedCore ${scenario} handles ${signal} and stops the active tool`, async t => {
    const setup = fixture(t, scenario);
    const child = spawn('bash', bash, {...setup.options, stdio: ['ignore', 'pipe', 'pipe']});
    const closed = once(child, 'close');
    t.after(() => child.kill('SIGTERM'));
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', data => { stdout += data; });
    child.stderr.on('data', data => { stderr += data; });
    for (let attempt = 0; attempt < 120 && !existsSync(setup.ready) && child.exitCode === null; attempt += 1) {
      await delay(50);
    }
    assert(existsSync(setup.ready), stderr);
    const pid = Number(readFileSync(setup.ready, 'utf8'));
    assert(child.kill(signal));
    assert.deepEqual(await closed, [status, null]);
    assertPrivate(setup, stdout, stderr);
    assert.match(stderr, /EmbeddedCore preparation exited with status 143/u);
    assert(stderr.includes(setupExited(scenario === 'download-cancel' ? 143 : 0)));
    assert(stderr.endsWith(`EmbeddedCore preparation cancelled with status ${status}\n`));
    const state = spawnSync('/bin/ps', ['-p', String(pid), '-o', 'stat='], {encoding: 'utf8'});
    assert.ifError(state.error);
    assert(state.status === 1 || state.stdout.startsWith('Z'), 'cancelled tool survived');
  });
}
