import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import YAML from 'yaml';
import {requireAndroidAdministrator} from '../android-operator.mjs';

const workflow = YAML.parse(readFileSync(new URL('../../../.github/workflows/android-delivery.yml', import.meta.url), 'utf8'));
const actor = {id: 7, login: 'owner'};
const rerunner = {id: 8, login: 'rerunner'};
function fixture() {
  const environment = {GITHUB_ACTIONS: 'true', GH_TOKEN: 'fixture', GITHUB_REPOSITORY: 'fixture/aegis', GITHUB_REPOSITORY_ID: '42',
    GITHUB_RUN_ID: '123', GITHUB_RUN_ATTEMPT: '2', GITHUB_SHA: 'a'.repeat(40), GITHUB_EVENT_NAME: 'workflow_dispatch',
    GITHUB_REF: 'refs/heads/main', GITHUB_ACTOR: 'owner', GITHUB_ACTOR_ID: '7', GITHUB_TRIGGERING_ACTOR: 'rerunner'};
  const run = {id: 123, run_attempt: 2, head_sha: environment.GITHUB_SHA, repository: {id: 42, full_name: environment.GITHUB_REPOSITORY},
    head_repository: {full_name: environment.GITHUB_REPOSITORY}, path: '.github/workflows/android-delivery.yml',
    event: 'workflow_dispatch', head_branch: 'main', actor, triggering_actor: rerunner};
  const administrators = [actor, rerunner].map(u => ({...u, permissions: {admin: true}}));
  const api = async (_repo, path) => path === '' ? {id: 42, full_name: environment.GITHUB_REPOSITORY} : path.startsWith('/actions/runs/') ? run : administrators;
  return {environment, run, administrators, api};
}

test('four bilingual form inputs; one manual entrypoint and existing candidate tag workflow', () => {
  assert.deepEqual(Object.keys(workflow.on.workflow_dispatch.inputs), ['operation', 'product', 'target', 'advanced']);
  assert.equal(workflow.on.push.tags, undefined);
  assert.deepEqual(workflow.on.push.branches, ['codex/android-release-handoff']);
  assert.equal(workflow.jobs.admission.if, "${{ github.event_name == 'workflow_dispatch' }}");
  assert.doesNotMatch(JSON.stringify(workflow.jobs.contracts), /secrets\.|self-hosted/u);
  for (const field of Object.values(workflow.on.workflow_dispatch.inputs)) {
    assert.match(field.description, /[\u4e00-\u9fff]/u);
    assert.match(field.description, /[A-Za-z]/u);
    for (const option of field.options || []) assert.match(option, /[\u4e00-\u9fff].*[A-Za-z]/u);
  }
  assert.match(workflow.jobs.package['runs-on'], /admission.outputs.runner/u);
  assert.equal(workflow.jobs.store['runs-on'], 'ubuntu-24.04');
  assert.match(workflow.jobs.store.environment, /aegis-android-submit.*aegis-android-distribute.*aegis-play/u);
  const legacy = YAML.parse(readFileSync(new URL('../../../.github/workflows/release.yml', import.meta.url), 'utf8'));
  assert.ok(!legacy.on.workflow_dispatch.inputs.platform.options.includes('android'));
  assert.ok(legacy.on.workflow_call.inputs.platform);
  assert.equal(workflow.jobs.draft.permissions.contents, 'write');
  assert.equal(workflow.permissions.contents, 'read');
});

test('separate signing/Play credentials and import/Admin before any Play credential load', () => {
  const packageText = JSON.stringify(workflow.jobs.package);
  const store = workflow.jobs.store.steps;
  assert.doesNotMatch(packageText, /secrets\.AEGIS_PLAY/u);
  assert.doesNotMatch(JSON.stringify(store), /ANDROID_(?:STORE|KEY)_PASSWORD/u);
  const credentials = store.findIndex(s => s.env?.AEGIS_PLAY_JSON_KEY_DATA);
  assert.ok(credentials > store.findIndex(s => s.run?.includes('android-delivery.py import')));
  assert.ok(credentials > store.findIndex(s => s.run?.includes('android-operator.mjs')));
  assert.doesNotMatch(JSON.stringify(workflow.jobs.draft), /secrets\./u);
  for (const job of Object.values(workflow.jobs)) {
    assert.doesNotMatch(JSON.stringify(job['runs-on']), /macos|ios/u);
    for (const step of job.steps) assert.doesNotMatch(step.run || '', /\$\{\{\s*inputs\./u);
  }
});

test('current original actor and rerunner Admin grants admit current run', async () => {
  const f = fixture();
  assert.equal((await requireAndroidAdministrator(f)).authorized, true);
  f.administrators.pop();
  await assert.rejects(requireAndroidAdministrator(f), /Admin/u);
});

test('reject actor spoofing, stale attempt, changed source/repository/workflow/event/ref', async () => {
  for (const change of [{run_attempt: 1}, {head_sha: 'b'.repeat(40)}, {path: '.github/workflows/ios-delivery.yml'},
    {event: 'pull_request'}, {head_branch: 'other'}, {actor: {id: 9, login: 'owner'}}, {head_repository: {full_name: 'other/repo'}}]) {
    const f = fixture();
    Object.assign(f.run, change);
    await assert.rejects(requireAndroidAdministrator(f));
  }
});

test('Admin metadata errors, empty, duplicates and non-Admin rows fail closed', async () => {
  for (const rows of [[], [{...actor, permissions: {admin: false}}], [actor], [actor, actor]]) {
    const f = fixture();
    f.api = async (_repo, path) => path === '' ? {id: 42, full_name: f.environment.GITHUB_REPOSITORY} : path.startsWith('/actions/runs/') ? f.run : rows;
    await assert.rejects(requireAndroidAdministrator(f));
  }
  const f = fixture();
  f.api = async () => {throw new Error('metadata unavailable');};
  await assert.rejects(requireAndroidAdministrator(f));
});

test('Android candidate tag Admin admission stays on Android', async () => {
  const f = fixture();
  Object.assign(f.run, {path: '.github/workflows/release-candidate-tag.yml', event: 'push', head_branch: 'candidate/android/6.1.0.266-alpha.2006000266'});
  Object.assign(f.environment, {GITHUB_EVENT_NAME: 'push', GITHUB_REF: 'refs/tags/'+f.run.head_branch});
  assert.equal((await requireAndroidAdministrator(f)).authorized, true);
  f.environment.GITHUB_REF = 'refs/tags/candidate/ios/1.0.0-alpha.33';
  await assert.rejects(requireAndroidAdministrator(f));
});
