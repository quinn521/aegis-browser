import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import {once} from 'node:events';
import {mkdirSync, mkdtempSync, readFileSync, readdirSync, writeFileSync} from 'node:fs';
import {join, resolve} from 'node:path';
import {clearTimeout, setTimeout} from 'node:timers';
import {fileURLToPath} from 'node:url';
import test from 'node:test';

const root = fileURLToPath(new URL('../../../', import.meta.url));
const core = resolve(root, 'packages/core');
const preload = join(core, 'vitest-coverage-exit.mjs');

function runFork(body, {enabled = true, coverage = true} = {}) {
  mkdirSync(join(core, '.artifacts'), {recursive: true});
  const directory = mkdtempSync(join(core, '.artifacts', 'v8-exit-'));
  const raw = join(directory, 'raw');
  mkdirSync(raw);
  const receipt = join(directory, 'exit.json');
  writeFileSync(join(directory, 'fixture.test.mjs'), [
    "import {test, expect} from 'vitest';",
    "import {writeFileSync} from 'node:fs';",
    'process.on("exit", code => writeFileSync(' + JSON.stringify(receipt) + ', JSON.stringify({code})));',
    'function executedBeforeStop() { return 42; }',
    "test('owned fork fixture', async () => { " + body + ' });',
  ].join('\n'));
  const config = join(directory, 'vitest.config.mjs');
  writeFileSync(config, 'export default ' + JSON.stringify({root: directory, test: {
    include: ['fixture.test.mjs'], pool: 'forks', maxWorkers: 1, testTimeout: 200,
    execArgv: enabled ? ['--import', preload] : [],
  }}) + ';\n');
  const result = spawnSync(process.execPath, [join(core, 'node_modules/vitest/vitest.mjs'),
    'run', '--config', config], {
    cwd: core, env: {...process.env, NODE_V8_COVERAGE: coverage ? raw : ''},
    encoding: 'utf8', timeout: 15_000,
  });
  // Keep actual subprocess output and raw reports, including failure fixtures.
  writeFileSync(join(directory, 'stdout.log'), result.stdout ?? '');
  writeFileSync(join(directory, 'stderr.log'), result.stderr ?? '');
  assert.equal(result.error, undefined, result.stderr);
  const entries = readdirSync(raw).flatMap(name => {
    const bytes = readFileSync(join(raw, name));
    assert.ok(bytes.length > 0, 'Empty raw report: ' + name);
    const report = JSON.parse(bytes.toString());
    assert.ok(Array.isArray(report.result));
    return report.result;
  });
  return {result, entries, receipt, directory};
}

test('fork teardown without the preload loses executed worker coverage (baseline)', () => {
  const {result, entries} = runFork('expect(executedBeforeStop()).toBe(42);', {enabled: false});
  assert.equal(result.status, 0, result.stderr);
  assert.equal(entries.some(entry => entry.url.endsWith('/fixture.test.mjs')), false);
});

test('the actual Vitest fork finishes teardown with complete executed-source coverage', () => {
  const {result, entries, receipt} = runFork('expect(executedBeforeStop()).toBe(42);');
  assert.equal(result.status, 0, result.stderr);
  assert.equal(JSON.parse(readFileSync(receipt)).code, 0);
  const source = entries.find(entry => entry.url.endsWith('/fixture.test.mjs'));
  assert.ok(source, 'Executed worker source must remain covered');
  assert.ok(source.functions.some(fn => fn.functionName === 'executedBeforeStop' &&
    fn.ranges.some(range => range.count > 0)));
});

for (const [name, body, workerCode] of [
  ['nonzero exit', 'process.exitCode = 7; expect(executedBeforeStop()).toBe(42);', 7],
  ['assertion failure', 'expect(executedBeforeStop()).toBe(0);', null],
  ['exception', 'throw new Error("intentional owned-fixture failure");', null],
  ['unfinished test', 'await new Promise(() => {});', null],
  ['unexpected SIGTERM', 'process.kill(process.pid, "SIGTERM"); await new Promise(() => {});', 143],
]) {
  test(name + ' remains a failed Vitest run', () => {
    const {result, receipt} = runFork(body);
    assert.notEqual(result.status, 0, name + ' was silently accepted');
    if (workerCode !== null) assert.equal(JSON.parse(readFileSync(receipt)).code, workerCode);
  });
}

test('without V8 coverage the actual fork keeps its ordinary test result', () => {
  const {result, entries} = runFork('expect(executedBeforeStop()).toBe(42);', {coverage: false});
  assert.equal(result.status, 0, result.stderr);
  assert.deepEqual(entries, []);
});

test('the preload leaves a non-Vitest owned Node process on ordinary SIGTERM semantics', async () => {
  mkdirSync(join(root, '.artifacts'), {recursive: true});
  const directory = mkdtempSync(join(root, '.artifacts', 'non-vitest-'));
  const fixture = join(directory, 'owned.cjs');
  writeFileSync(fixture, 'process.send({ready:true});setInterval(()=>{},1000);');
  const child = spawn(process.execPath, ['--import', preload, fixture], {
    env: {...process.env, NODE_V8_COVERAGE: join(directory, 'coverage')},
    stdio: ['ignore', 'pipe', 'pipe', 'ipc'],
  });
  child.stdout.resume();
  child.stderr.resume();
  const closed = once(child, 'close');
  const watchdog = setTimeout(() => child.kill('SIGKILL'), 5000);
  try {
    await Promise.race([once(child, 'message'), closed.then(() => { throw new Error('Early child close'); })]);
    child.kill('SIGTERM');
    const [code, signal] = await closed;
    assert.equal(code, null);
    assert.equal(signal, 'SIGTERM');
  } finally {
    if (child.exitCode === null && child.signalCode === null) {
      child.kill('SIGKILL');
      await closed;
    }
    clearTimeout(watchdog);
  }
});
