import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {test} from 'node:test';
import YAML from 'yaml';
import {checkPlatformResults, selectPlatformChecks} from '../check-platform-gate.mjs';

const base = 'a'.repeat(40);
const tested = 'b'.repeat(40);
const plan = {
  schemaVersion: 3, tested, rulesSha: base, gate: true, docs: false,
  integrate: true, deep: false, android: true, ios: false, linux: false, macos: false, windows: true,
};
const needs = {
  plan: {result: 'success'}, android: {result: 'success'}, ios: {result: 'skipped'},
  linux: {result: 'skipped'}, macos: {result: 'skipped'}, windows: {result: 'success'},
};

test('platform gate accepts only decisions from the bound policy and tested source', () => {
  assert.deepEqual(selectPlatformChecks(plan, tested, base), ['android', 'windows']);
  assert.throws(() => selectPlatformChecks(plan, base, base), /identity mismatch/u);
  assert.throws(() => selectPlatformChecks({...plan, rulesSha: tested}, tested, base), /identity mismatch/u);
  assert.throws(() => selectPlatformChecks({...plan, schemaVersion: 2}, tested, base), /schema/u);
  assert.throws(() => selectPlatformChecks({...plan, windows: 'true'}, tested, base), /decisions/u);
  assert.throws(() => selectPlatformChecks({...plan, docs: true}, tested, base), /documentation/u);
  const docs = {...plan, docs: true, integrate: false, android: false, windows: false};
  assert.deepEqual(selectPlatformChecks(docs, tested, base), []);
});

test('selected jobs must succeed and a skipped job is valid only for an unselected platform', () => {
  const required = ['android', 'windows'];
  assert.deepEqual(checkPlatformResults(required, needs),
    {result: 'PASS', required, notApplicable: ['ios', 'linux', 'macos']});
  for (const result of ['failure', 'cancelled', 'timed_out', 'skipped', 'neutral', '']) {
    assert.throws(() => checkPlatformResults(required, {...needs, windows: {result}}), /windows=/u);
  }
  assert.throws(() => checkPlatformResults(required, {...needs, windows: undefined}), /windows=missing/u);
  assert.throws(() => checkPlatformResults(required, {...needs, ios: {result: 'success'}}), /expected skipped/u);
  assert.throws(() => checkPlatformResults(required, {...needs, plan: {result: 'failure'}}), /plan did not succeed/u);
  assert.throws(() => checkPlatformResults(['android', 'android'], needs), /selection/u);
  assert.throws(() => checkPlatformResults(['quality'], needs), /selection/u);
  assert.deepEqual(checkPlatformResults([], {...needs, android: {result: 'skipped'}, windows: {result: 'skipped'}}).required, []);
});

test('the observational check is separate from quality and always summarizes its own five jobs', () => {
  const workflow = YAML.parse(readFileSync('.github/workflows/platform-gate.yml', 'utf8'));
  assert.deepEqual(Object.keys(workflow.on).sort(), ['pull_request', 'push']);
  for (const trigger of Object.values(workflow.on)) {
    assert.deepEqual(trigger.branches, ['develop', 'main']);
    assert.ok(!trigger.paths && !trigger['paths-ignore']);
  }
  assert.deepEqual(workflow.on.pull_request.types,
    ['opened', 'reopened', 'synchronize', 'ready_for_review', 'converted_to_draft']);
  assert.deepEqual(workflow.permissions, {contents: 'read'});
  const summary = workflow.jobs['platform-summary'];
  assert.deepEqual(summary.needs, ['plan', 'android', 'ios', 'linux', 'macos', 'windows']);
  assert.match(summary.if, /always\(\).*draft\) == 'false'/u);
  assert.match(summary.name, /'platform-gate'.*'platform-deferred-draft-or-missing-metadata'/u);
  const planning = workflow.jobs.plan.steps.find(step => step.id === 'route').run;
  assert.match(planning, /git show "\$rules:scripts\/ci\/platform-impact\.mjs"/u);
  assert.match(planning, /git show "\$rules:scripts\/ci\/check-platform-gate\.mjs"/u);
  for (const name of ['android', 'ios', 'linux', 'macos', 'windows']) {
    const job = Object.entries(workflow.jobs).find(([id]) => id === name)[1];
    assert.equal(job.needs, 'plan');
    assert.match(job.if, /needs\.plan\.result == 'success'/u);
    assert.ok(job.if.includes("'" + name + "'"));
    assert.ok(!('continue-on-error' in job));
  }
  for (const [name, job] of [['linux', workflow.jobs.linux], ['windows', workflow.jobs.windows]]) {
    assert.equal(job.uses, './.github/workflows/other-platform-coverage.yml');
    assert.deepEqual(job.with, {lane: name});
  }
  const nativeCoverage = YAML.parse(readFileSync('.github/workflows/other-platform-coverage.yml', 'utf8'));
  const report = nativeCoverage.jobs['lane-result'];
  assert.deepEqual(report.needs, ['shell-coverage', 'powershell-coverage']);
  assert.match(report.steps[0].run, /test "\$LINUX_RESULT" = success && test "\$WINDOWS_RESULT" = skipped/u);
  assert.match(report.steps[0].run, /test "\$WINDOWS_RESULT" = success && test "\$LINUX_RESULT" = skipped/u);
});
