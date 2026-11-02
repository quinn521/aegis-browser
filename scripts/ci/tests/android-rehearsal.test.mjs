import assert from 'node:assert/strict';
import test from 'node:test';
import {releasePreflight} from '../release-preflight.mjs';

const sha = 'a'.repeat(40);
const repo = 'fixture/aegis-browser';
const branch = 'codex/android-release-acceptance';
const path = '.github/workflows/android-release-validation.yml';
function fixture() {
  const environment = {GH_TOKEN:'fixture', GITHUB_ACTIONS:'true', GITHUB_REPOSITORY:repo, GITHUB_REPOSITORY_ID:'777',
    GITHUB_EVENT_NAME:'workflow_dispatch', GITHUB_REF:`refs/heads/${branch}`, GITHUB_SHA:sha,
    AEGIS_RELEASE_ENABLED:'false', AEGIS_RELEASE_REHEARSAL:'false', AEGIS_RELEASE_TEST_REPOSITORY_ID:'777',
    AEGIS_ANDROID_RELEASE_ENABLED:'true',
    AEGIS_ANDROID_REHEARSAL_ENABLED:'true', AEGIS_ANDROID_REHEARSAL_SOURCE_SHA:sha,
    RELEASE_PLATFORM:'android', RELEASE_DESTINATION:'artifacts'};
  const run = {id:5, run_attempt:1, head_sha:sha, head_branch:branch, event:'push', path,status:'completed',conclusion:'success',
    repository:{id:777,full_name:repo}, head_repository:{id:777,full_name:repo}};
  const jobs = ['contracts','native-admission'].map((name,i)=>({id:10+i,name,run_id:5,head_sha:sha,run_attempt:1,status:'completed',conclusion:'success'}));
  const f = {environment,run,jobs,ref:sha,finalRun:run,requests:[]};
  f.api = async (repository, endpoint) => {
    assert.equal(repository,repo); f.requests.push(endpoint);
    if (endpoint === '') return {id:777,full_name:repo,fork:true,parent:{id:1348409663,full_name:'gcsagroup/aegis-browser'}};
    if (endpoint.startsWith('/git/ref/heads/') || endpoint.startsWith('/commits/')) return {sha:f.ref,object:{sha:f.ref}};
    if (endpoint.startsWith('/actions/workflows/')) return {total_count:1,workflow_runs:[f.run]};
    if (endpoint.includes('/jobs?')) return {total_count:f.jobs.length,jobs:f.jobs};
    if (endpoint === '/actions/runs/5') return f.finalRun;
    assert.fail(`Unexpected API request ${endpoint}`);
  };
  return f;
}
const call = f => releasePreflight(sha,{environment:f.environment,api:f.api,quality:()=>assert.fail('not upstream qualification')});

test('Android fork admission requires its exact contracts and unsigned native job, without iOS archive',async()=>{
  const f=fixture(); const result=await call(f);
  assert.equal(result.admission,'fork_android_rehearsal_only');
  assert.equal(result.releaseReady,false);
  assert.ok(f.requests.some(p=>p.includes('/android-release-validation.yml/')));
  assert.ok(!f.requests.some(p=>p.includes('release-validation.yml/runs') && !p.includes('android-release-validation')));
});

test('Android rehearsal rejects missing opt-in, wrong pin, production, arbitrary branch or external event before API',async()=>{
  for (const change of [{AEGIS_ANDROID_REHEARSAL_ENABLED:'false'},{AEGIS_ANDROID_REHEARSAL_SOURCE_SHA:'b'.repeat(40)},
    {RELEASE_DESTINATION:'play-production'},{GITHUB_REF:'refs/heads/main'},{GITHUB_EVENT_NAME:'pull_request'},
    {AEGIS_ANDROID_RELEASE_ENABLED:'false'},{AEGIS_RELEASE_TEST_REPOSITORY_ID:'888'},
    {AEGIS_ANDROID_RELEASE_ENABLED:'',AEGIS_RELEASE_ENABLED:'true',AEGIS_RELEASE_REHEARSAL:'true'}]) {
    const f=fixture();Object.assign(f.environment,change);f.api=()=>assert.fail('invalid configuration must fail before network');
    await assert.rejects(call(f));
  }
});

test('Android-only release opt-in cannot admit upstream or another platform with shared flags disabled',async()=>{
  for (const change of [{GITHUB_REPOSITORY:'gcsagroup/aegis-browser',GITHUB_REPOSITORY_ID:'1348409663',GITHUB_REF:'refs/heads/main'},
    {RELEASE_PLATFORM:'ios'},{RELEASE_PLATFORM:'macos'}]) {
    const f=fixture();Object.assign(f.environment,change);f.api=()=>assert.fail('Android opt-in cannot enable other release paths');
    await assert.rejects(call(f));
  }
});

test('skipped duplicate wrong-source and incomplete native evidence cannot admit a package',async()=>{
  for (const modify of [f=>f.jobs[1].conclusion='skipped',f=>f.jobs.push({...f.jobs[1],id:44}),
    f=>f.jobs[1].head_sha='b'.repeat(40),f=>f.jobs[1].run_attempt=2,f=>f.jobs[1].run_id=6,
    f=>f.jobs[1].status='in_progress',f=>f.jobs=[],f=>f.run={...f.run,path:'.github/workflows/elsewhere.yml'},
    f=>f.run={...f.run,head_repository:{id:888,full_name:'external/fork'}}]) {
    const f=fixture();modify(f);await assert.rejects(call(f));
  }
});

test('rerun or ref change during admission invalidates otherwise successful source',async()=>{
  for (const modify of [f=>f.finalRun={...f.run,run_attempt:2},f=>f.ref='b'.repeat(40)]) {
    const f=fixture();modify(f);await assert.rejects(call(f));
  }
});

test('a newer validation run appearing during GETs invalidates the earlier run',async()=>{
  const f=fixture(), api=f.api; let listings=0;
  f.api=async(...args)=>{
    if (args[1].startsWith('/actions/workflows/') && ++listings>1) return {total_count:1,workflow_runs:[{...f.run,id:6}]};
    return api(...args);
  };
  await assert.rejects(call(f),/latest/u);
});

test('running or cancelled workflow is not admitted even when both prerequisite jobs succeeded',async()=>{
  for (const change of [{status:'in_progress',conclusion:null},{status:'completed',conclusion:'cancelled'},{status:'completed',conclusion:'failure'}]) {
    const f=fixture();Object.assign(f.run,change);await assert.rejects(call(f));
  }
});

test('branch moving during the final workflow listing is caught by the final ref read',async()=>{
  const f=fixture(),api=f.api;let listings=0;
  f.api=async(...args)=>{
    const result=await api(...args);
    if(args[1].startsWith('/actions/workflows/') && ++listings===2) f.ref='b'.repeat(40);
    return result;
  };
  await assert.rejects(call(f),/branch changed/u);
});

test('Android candidate tag must resolve to the same independently validated source',async()=>{
  const f=fixture();const tag='candidate/android/6.1.0.266-alpha.2006000266';
  Object.assign(f.environment,{GITHUB_EVENT_NAME:'push',GITHUB_REF:`refs/tags/${tag}`,RELEASE_TAG:tag,RELEASE_DESTINATION:'github-draft'});
  assert.equal((await call(f)).sourceValidationRun,5);
  f.ref='b'.repeat(40);await assert.rejects(call(f));
});
