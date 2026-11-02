import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import YAML from 'yaml';
import {qualifyAndroidReadToken} from '../android-read-token.mjs';

const repo='quinn521/aegis-browser',sha='a'.repeat(40),branch='codex/android-release-acceptance';
function fixture() {
  const actor={id:7,login:'operator'};
  const environment={GITHUB_ACTIONS:'true',GH_TOKEN:'fixture-token',GITHUB_REPOSITORY:repo,GITHUB_REPOSITORY_ID:'1363949860',
    GITHUB_RUN_ID:'5',GITHUB_RUN_ATTEMPT:'1',GITHUB_JOB:'read-token-admission',GITHUB_SHA:sha,GITHUB_EVENT_NAME:'push',
    GITHUB_REF:`refs/heads/${branch}`,GITHUB_ACTOR:actor.login,GITHUB_ACTOR_ID:'7',GITHUB_TRIGGERING_ACTOR:actor.login,
    AEGIS_ANDROID_REHEARSAL_SOURCE_SHA:sha};
  const metadata={id:1363949860,full_name:repo,private:false,fork:true,parent:{id:1348409663,full_name:'gcsagroup/aegis-browser'}};
  const run={id:5,run_attempt:1,head_sha:sha,head_branch:branch,path:'.github/workflows/android-release-validation.yml',
    status:'in_progress',conclusion:null,event:'push',repository:metadata,head_repository:metadata,actor,triggering_actor:actor};
  const jobs=['contracts','read-token-admission','native-admission'].map((name,i)=>({id:10+i,name,run_id:5,run_attempt:1,head_sha:sha}));
  const f={environment,metadata,run,jobs,ref:sha,commit:sha,admins:[{...actor,permissions:{admin:true}}],requests:[]};
  f.api=async(repository,path,options={})=>{
    assert.equal(repository,repo);assert.equal(options.method??'GET','GET');assert.equal(options.body,undefined);f.requests.push(path);
    if(path==='')return f.metadata;
    if(path==='/actions/runs/5')return f.run;
    if(path.startsWith('/actions/runs/5/attempts/1/jobs?'))return {total_count:f.jobs.length,jobs:f.jobs};
    if(path.startsWith('/actions/workflows/'))return {total_count:1,workflow_runs:[f.run]};
    if(path.startsWith('/collaborators?'))return f.admins;
    if(path.startsWith('/git/ref/heads/'))return {object:{sha:f.ref}};
    if(path.startsWith('/commits/'))return {sha:f.commit};
    assert.fail(`Unexpected GET ${path}`);
  };
  return f;
}

test('unsigned token qualification reads real Admin and all source-admission endpoint types without release claims',async()=>{
  const f=fixture(),result=await qualifyAndroidReadToken(f);
  assert.equal(result.status,'unsigned_read_token_endpoints_passed');
  assert.equal(result.sourceSha,sha);assert.equal(result.runId,5);assert.equal(result.attempt,1);
  assert.equal(result.releaseReady,false);assert.equal(result.productBuilt,false);
  for(const prefix of ['/actions/runs/5','/actions/workflows/','/actions/runs/5/attempts/1/jobs?',
    '/collaborators?','/git/ref/heads/','/commits/']) assert.ok(f.requests.some(p=>p.startsWith(prefix)));
});

test('wrong runtime job ref source or repository never probes with a token',async()=>{
  for(const change of [{GITHUB_JOB:'package'},{GITHUB_REF:'refs/heads/main'},{GITHUB_EVENT_NAME:'workflow_dispatch'},
    {GITHUB_REPOSITORY:'other/aegis-browser'},{GITHUB_REPOSITORY_ID:'1'},
    {AEGIS_ANDROID_REHEARSAL_SOURCE_SHA:'b'.repeat(40)},{GH_TOKEN:''}]) {
    const f=fixture();Object.assign(f.environment,change);f.api=()=>assert.fail('reject invalid runtime before API');
    await assert.rejects(qualifyAndroidReadToken(f));
  }
});

test('foreign private stale incomplete or non-Admin metadata cannot qualify signing token access',async()=>{
  for(const modify of [f=>f.metadata.private=true,f=>f.metadata.parent.id=1,f=>f.admins=[],
    f=>f.run.run_attempt=2,f=>f.ref='b'.repeat(40),f=>f.commit='b'.repeat(40),
    f=>f.jobs[1].head_sha='b'.repeat(40),f=>f.jobs[1].run_attempt=2,f=>f.jobs=[]]) {
    const f=fixture();modify(f);await assert.rejects(qualifyAndroidReadToken(f));
  }
});

test('an unavailable required GET fails closed without alternate token or mutating fallback',async()=>{
  for(const prefix of ['/actions/runs/5','/actions/workflows/','/actions/runs/5/attempts/1/jobs?',
    '/collaborators?','/git/ref/heads/','/commits/']) {
    const f=fixture(),api=f.api;
    f.api=(repository,path,options)=>{if(path.startsWith(prefix))throw new Error('fixture unavailable');return api(repository,path,options);};
    await assert.rejects(qualifyAndroidReadToken(f));
  }
});

test('the unsigned capability job uses the same contents-only permission as packaging and gates native admission',()=>{
  const workflow=YAML.parse(readFileSync(new URL('../../../.github/workflows/android-release-validation.yml',import.meta.url),'utf8'));
  const probe=workflow.jobs['read-token-admission'];
  assert.ok(probe);assert.deepEqual(probe.permissions,{contents:'read'});
  assert.equal(probe['runs-on'],'ubuntu-24.04');assert.equal(probe.environment,undefined);
  assert.doesNotMatch(JSON.stringify(probe),/secrets\.|self-hosted/u);
  assert.deepEqual(workflow.jobs['native-admission'].needs,['contracts','read-token-admission']);
  assert.match(probe.if,/github\.sha == vars\.AEGIS_ANDROID_REHEARSAL_SOURCE_SHA/u);
  const step=probe.steps.find(s=>s.run==='node scripts/ci/android-read-token.mjs');
  assert.equal(step.env.GH_TOKEN,'${{ github.token }}');
  for(const step of probe.steps)if(step.uses?.startsWith('actions/checkout@')) {
    assert.equal(step.with.ref,'${{ github.sha }}');assert.equal(step.with['persist-credentials'],false);
  }
});
