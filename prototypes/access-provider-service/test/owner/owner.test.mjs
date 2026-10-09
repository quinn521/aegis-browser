import test from 'node:test';
import assert from 'node:assert/strict';
import { performance } from 'node:perf_hooks';
import { promises as fs } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
import { EventEmitter } from 'node:events';
import { Client } from 'pg';
import { exportJWK,compactVerify } from 'jose';
import { admitPostgres,assertAdmitted } from '../pg-harness.mjs';
import { fixtureBindings,keyPair } from '../fixtures.mjs';
import { PostgresStore,createOwnedPool } from '../../store.mjs';
import { ProviderError,canonical } from '../../proof.mjs';
import { FrameDecoder,CommitTracker,WireProxy,WireError,WireLifetime,classifySql,errorSqlState,WIRE_LIMITS } from './wire-proxy.mjs';
import { createPlan,command,colimaStartArgs,containerArgs,residualCapacity,ConnectionBudget,PhaseGate,OwnerSupervisor,OwnerError,ownedRace,CAPS,PIN,digest,remainingMilliseconds,resolveExecutable,regular,createContainerWithSecret,assertFixtureEnvironment,RUNTIME_SOURCE_FILES,verifyReviewedSources,capabilityAuthorizer } from './owner.mjs';
import { budgetHarness,runCli,createResourceLifecycle,finishRuntime,persistentBindings } from './runner.mjs';
import { validateAckReceipt,validateDeadlockEvidence,isClockDisconnect,registerFixtureAndClose } from './fault-cases.mjs';
// Migrated from the prior artifact-only 23 tests; the original packet stays immutable.
// These are pure unit/regression tests; no real resource/SQL admission is claimed.
const testFiles=await fs.mkdtemp(path.join(process.env.M_OWNER_TEST_TMPDIR??tmpdir(),'owner-pure-'));
test.after(()=>fs.rm(testFiles,{recursive:true,force:true}));
const root='/Volumes/ExternalSSD/repositories/pure-owner-fixture',U='a1234567-1234-4321-8765-123456789abc';
const plan=createPlan({worktree:root,runId:U,vmImage:root+'/.artifacts/cache.raw',ociLayout:root+'/.artifacts/oci',importArchive:root+'/.artifacts/cache.tar'});
const frame=(type,body=Buffer.alloc(0))=>{const raw=Buffer.alloc(body.length+5);raw[0]=type.charCodeAt(0);raw.writeUInt32BE(body.length+4,1);body.copy(raw,5);return new FrameDecoder().push(raw)[0];};
const str=s=>Buffer.from(s+'\0');
const query=s=>frame('Q',str(s)),ready=s=>frame('Z',Buffer.from(s)),complete=s=>frame('C',str(s));
const startup=()=>{const body=Buffer.from('user\0b1_app\0database\0b1\0\0'),b=Buffer.alloc(8+body.length);b.writeUInt32BE(b.length);b.writeUInt32BE(196608,4);body.copy(b,8);return b;};
const sqlError=code=>frame('E',Buffer.concat([Buffer.from('S'),str('ERROR'),Buffer.from('C'),str(code),Buffer.from('M'),str('password secret SQL body must not appear'),Buffer.from([0])]));
const step=(t,s,tag='SELECT 1',status='T')=>{t.frontend(query(s));assert.equal(t.backend(complete(tag)).forward,true);assert.equal(t.backend(ready(status)).forward,true);};
const flush=()=>new Promise(r=>setImmediate(r));
test('all imports and constructors are inert, default gate refuses runtime',async()=>{
  assert.equal(process.versions.node,PIN.node);
  const owner=new OwnerSupervisor({plan,manifest:{},authorize:async()=>false});const proxy=new WireProxy({backendPort:54321,authorize:async()=>{throw new Error('denied');}});
  assert.equal(owner.phase,'new');assert.equal(owner.owner,null);assert.deepEqual(owner.events,[]);assert.deepEqual(proxy.metadata,[]);
  await assert.rejects(owner.preflight(),e=>e.code==='OWNER_NOT_AUTHORIZED');assert.equal(owner.denied.length,1);assert.equal(owner.phase,'new');
  await assert.rejects(proxy.start(),/denied/);await proxy.close();
});
test('exact UUID-owned paths and structured commands cannot select a default daemon',()=>{
  assert.equal(plan.colimaHome,'/Volumes/ExternalSSD/b1pg-'+U+'/c');assert.ok(Buffer.byteLength(plan.socket)<104);
  const d=command(plan,'docker',['inspect','a'.repeat(64)],{stdin:'private',secret:true});assert.deepEqual(d.args.slice(0,2),['-H','unix://'+plan.socket]);assert.equal(d.env.DOCKER_CONFIG,plan.dockerConfig);assert.equal(d.env.DOCKER_HOST,undefined);assert.equal(d.env.SSH_AUTH_SOCK,undefined);assert.ok(!d.args.includes('private'));
  assert.throws(()=>command(plan,'docker',['context','use','default']));assert.throws(()=>command(plan,'docker',['system','prune']));assert.throws(()=>command(plan,'docker',['image','pull',PIN.image]));assert.throws(()=>command(plan,'docker',['inspect','--context','default']));
  const c=command(plan,'colima',colimaStartArgs(plan));assert.deepEqual(c.args.slice(0,2),['--profile','b1-owner']);assert.ok(c.args.includes('--mount'));assert.ok(c.args.includes('none'));assert.ok(c.args.includes('--activate=false'));assert.equal(c.env.COLIMA_HOME,plan.colimaHome);
  const args=containerArgs(plan,'a'.repeat(64));assert.ok(args.includes('--pull=never'));assert.ok(args.includes('127.0.0.1:0:5432'));assert.ok(args.includes('--read-only'));assert.equal(args[args.indexOf('--memory-swap')+1],'1g');assert.equal(args[args.indexOf('--log-driver')+1],'none');assert.ok(!args.includes('--privileged'));
  for(const runId of ['../../default','1','A'.repeat(36)])assert.throws(()=>createPlan({worktree:root,runId}));assert.throws(()=>createPlan({worktree:'/tmp/repo',runId:U}));
});
test('capacity is residual after worst-case reserve, equality accepted and unsafe values rejected',()=>{
  assert.equal(residualCapacity(9*1024**3,7*1024**3,{hostReserve:3*1024**3,guestReserve:1024**3}),6*1024**3);
  assert.throws(()=>residualCapacity(9*1024**3-1,7*1024**3,{hostReserve:3*1024**3,guestReserve:1024**3}),e=>e.code==='OWNER_CAPACITY');
  for(const n of [NaN,Infinity,-1,Number.MAX_SAFE_INTEGER+1])assert.throws(()=>residualCapacity(n,100*1024**3));
});
test('two app reservations and twelve total clients hold until idempotent release',()=>{
  const b=new ConnectionBudget(),a=b.reserve('app'),c=b.reserve('app'),helper=b.reserve('helper',8);assert.equal(b.state.used,12);assert.throws(()=>b.reserve('app'));assert.throws(()=>b.reserve('helper',1));a();a();assert.equal(b.state.used,10);c();helper();assert.equal(b.state.used,0);b.close();assert.throws(()=>b.reserve('helper'));
});
test('denied and unknown operations remain remembered and cannot be silently retried',async()=>{
  const gate=new PhaseGate({runId:U,authorize:async()=>false});await assert.rejects(gate.require('colima-start'));await assert.rejects(gate.require('prune'));assert.deepEqual(gate.denied.map(e=>e.reason),['OWNER_NOT_AUTHORIZED','UNKNOWN_ACTION']);
});
test('cancellation consumes late checkout and late rejection without duplicate cleanup',async()=>{
  let resolve,closed=0;const p=new Promise(r=>{resolve=r;}),controller=new AbortController();
  const raced=ownedRace(()=>p,{signal:controller.signal,deadline:performance.now()+1000,onLate:()=>{closed++;}});await flush();controller.abort();await assert.rejects(raced,e=>e.code==='OWNER_CANCELLED');resolve({});await flush();await flush();assert.equal(closed,1);
  const c=new AbortController();await assert.rejects(ownedRace(()=>{c.abort();return {};},{signal:c.signal,deadline:performance.now()+1000,onLate:()=>{closed++;}}));await flush();assert.equal(closed,2);
  let reject;const late=new Promise((_,no)=>{reject=no;});await assert.rejects(ownedRace(()=>late,{deadline:performance.now()+1}));reject(new Error('late failure'));await flush();
});
test('real decoder preserves every split point and coalesced startup/auth/query frame bytes',()=>{
  const bytes=Buffer.concat([startup(),frame('p',Buffer.from('SCRAM-SECRET')).raw,query('BEGIN').raw]);
  for(let split=0;split<=bytes.length;split++){const decoder=new FrameDecoder({startup:true});const out=[...decoder.push(bytes.subarray(0,split)),...decoder.push(bytes.subarray(split))];assert.deepEqual(out.map(f=>f.type),['Startup','p','Q']);assert.deepEqual(Buffer.concat(out.map(f=>f.raw)),bytes);assert.equal(decoder.bufferedBytes,0);}
});
test('decoder rejects SSL, bad frame/startup lengths and bounded buffered overrun',()=>{
  const ssl=Buffer.alloc(8);ssl.writeUInt32BE(8);ssl.writeUInt32BE(80877103,4);assert.throws(()=>new FrameDecoder({startup:true}).push(ssl),e=>e.code==='WIRE_SSL_CANCEL_OR_PROTOCOL');
  for(const size of [0,3,WIRE_LIMITS.frame+1]){const raw=Buffer.alloc(5);raw[0]=81;raw.writeUInt32BE(size,1);assert.throws(()=>new FrameDecoder().push(raw));}
  const big=Buffer.alloc(4);big.writeUInt32BE(WIRE_LIMITS.startup+1);assert.throws(()=>new FrameDecoder({startup:true}).push(big));assert.throws(()=>new FrameDecoder().push(Buffer.alloc(WIRE_LIMITS.buffered+1)));
});
for(const mode of ['issuance','marker','observation'])test('exact '+mode+' ACK suppression requires real COMMIT CommandComplete then ReadyForQuery I',()=>{
  let selected=0;const t=new CommitTracker({select:m=>{if(m!==mode)return false;selected++;return true;}});
  step(t,mode==='issuance'?'BEGIN ISOLATION LEVEL SERIALIZABLE':'BEGIN ISOLATION LEVEL READ COMMITTED','BEGIN');
  step(t,mode==='issuance'?'INSERT INTO provider_b1.operations (id) VALUES ($1)':'SELECT * FROM provider_b1.clock_'+(mode==='marker'?'begin':'finish')+'($1)',mode==='issuance'?'INSERT 0 1':'SELECT 1');
  t.frontend(query('COMMIT'));assert.equal(selected,1);assert.deepEqual(t.backend(complete('COMMIT')),{forward:false});assert.deepEqual(t.backend(ready('I')),{forward:false,disconnect:true,committed:mode});
});
test('ordinary business commit, clock acquisition and ROLLBACK cannot satisfy an issuance fault',()=>{
  let selected=0;const t=new CommitTracker({select:()=>{selected++;return true;}});step(t,'BEGIN ISOLATION LEVEL SERIALIZABLE','BEGIN');step(t,'SELECT 1');step(t,'COMMIT','COMMIT','I');assert.equal(selected,0);
  step(t,'BEGIN ISOLATION LEVEL READ COMMITTED','BEGIN');step(t,'SELECT * FROM provider_b1.clock_try_lock($1)');step(t,'COMMIT','COMMIT','I');assert.equal(selected,0);
  step(t,'BEGIN ISOLATION LEVEL SERIALIZABLE','BEGIN');step(t,'INSERT INTO provider_b1.operations (id) VALUES ($1)','INSERT 0 1');step(t,'ROLLBACK','ROLLBACK','I');assert.equal(selected,0);
});
for(const response of ['error','rollback','early-ready','ready-T'])test('selected COMMIT fails closed for '+response,()=>{
  const t=new CommitTracker({select:()=>true});step(t,'BEGIN ISOLATION LEVEL READ COMMITTED','BEGIN');step(t,'SELECT * FROM provider_b1.clock_begin($1)');t.frontend(query('COMMIT'));
  if(response==='error')assert.throws(()=>t.backend(sqlError('40P01')),e=>e.code==='WIRE_SELECTED_COMMIT_ERROR');
  if(response==='rollback')assert.throws(()=>t.backend(complete('ROLLBACK')));
  if(response==='early-ready')assert.throws(()=>t.backend(ready('I')));
  if(response==='ready-T'){t.backend(complete('COMMIT'));assert.throws(()=>t.backend(ready('T')));}
});
test('extended Parse/Bind/Describe/Execute/Sync tracks SQL while keeping parameter bytes opaque',()=>{
  const t=new CommitTracker();const parse=sql=>frame('P',Buffer.concat([str(''),str(sql),Buffer.from([0,0])]));
  const value=Buffer.from('password=SENSITIVE_BIND_VALUE'),len=Buffer.alloc(4);len.writeUInt32BE(value.length);
  const bind=frame('B',Buffer.concat([str(''),str(''),Buffer.from([0,0,0,1]),len,value,Buffer.from([0,0])]));
  t.frontend(parse('BEGIN ISOLATION LEVEL SERIALIZABLE'));t.frontend(bind);t.frontend(frame('D',Buffer.from('P\0')));t.frontend(frame('E',Buffer.alloc(5)));t.frontend(frame('S'));t.backend(complete('BEGIN'));t.backend(ready('T'));
  assert.equal(t.transaction,'business');assert.throws(()=>t.frontend(query('SELECT 1; SELECT 2')));
  const p=new CommitTracker();p.frontend(query('SELECT 1'));assert.throws(()=>p.frontend(query('SELECT 2')),e=>e.code==='WIRE_PIPELINING');assert.throws(()=>new CommitTracker().frontend(frame('d',value)),e=>e.code==='WIRE_COPY_UNSUPPORTED');
});
test('error metadata contains SQLSTATE only, no server message/query/password',()=>{
  const records=[],t=new CommitTracker({record:v=>records.push(v)});t.frontend(query('SELECT 1'));assert.equal(t.backend(sqlError('40P01')).forward,true);assert.deepEqual(records,[{sqlState:'40P01'}]);assert.equal(errorSqlState(sqlError('40001').body),'40001');assert.doesNotMatch(JSON.stringify(records),/secret|password|SQL body/);assert.throws(()=>errorSqlState(Buffer.from('Msecret\0\0')));
});
test('fault validators reject wrong victim, synthetic class, timeout, counts and ACK stand-ins',()=>{
  const d={mainSqlStates:['40P01'],helperSqlStates:[],errorClass:'DatabaseError',begins:2,rollbacks:1,commits:1,callbacks:2,blockedByMain:true,barrierMs:40,timeouts:['250ms','1s','1s','20ms']};assert.equal(validateDeadlockEvidence(d),d);
  for(const patch of [{errorClass:'Error'},{mainSqlStates:['40001']},{helperSqlStates:['40P01']},{barrierMs:101},{begins:3},{rollbacks:0},{blockedByMain:false},{timeouts:['1s','1s','1s','20ms']}])assert.throws(()=>validateDeadlockEvidence({...d,...patch}));
  const ack={mode:'marker',connection:'a'.repeat(24),commitCommandComplete:true,readyForQuery:'I',ackForwarded:false};validateAckReceipt(ack,'marker');for(const patch of [{commitCommandComplete:false},{readyForQuery:'T'},{ackForwarded:true},{mode:'observation'},{connection:'pid'}])assert.throws(()=>validateAckReceipt({...ack,...patch},'marker'));
});
async function pureHarness({onAdmission}={}) {
  const bindings=await fixtureBindings(),owner={runId:U,uid:process.getuid(),imageDigest:PIN.imageDigest,platform:'linux/arm64',containerId:'a'.repeat(64),networkId:'b'.repeat(64),artifactRoot:root+'/.artifacts/b1-pg-'+U,host:'127.0.0.1',port:54321};
  const receipt={...owner,storageFreeBytes:6*1024**3,dataQuotaBytes:2*1024**3,connectionLimit:12,cpuLimit:2,memoryLimitBytes:1024**3,shmBytes:128*1024**2,wallBudgetSeconds:1800,resultsQuotaBytes:128*1024**2,durabilityMountVerified:true,loopbackPublicationVerified:true,roleGrantsVerified:true,secretFileModesVerified:true};
  // Deliberate receipt double: brand construction and pool lifecycle only.
  const connections=Object.fromEntries(['b1_bootstrap','b1_migrator','b1_app'].map(user=>[user,{host:owner.host,port:owner.port,database:'b1',user,password:'pure-test-only'}]));
  const harness=await admitPostgres({owner,connections,trust:bindings.config.trust,verifyOwner:async(o,action)=>{if(onAdmission)await onAdmission(action);return receipt;},lifecycle:{restartDatabase:async()=>{throw new Error('NO_ENGINE');}}});
  return {harness,bindings};
}
test('actual frozen admitted harness accepts budgeting without replacing methods or its brand',async()=>{
  const {harness}=await pureHarness(),owner={budget:new ConnectionBudget(),signal:new AbortController().signal},original=harness.newStore;
  assert.equal(Object.isFrozen(harness),true);assertAdmitted(harness);const resources=budgetHarness(harness,owner);
  assert.equal(resources.harness,harness);assert.equal(harness.newStore,original);assert.equal(Object.isFrozen(harness),true);assertAdmitted(harness);
  assert.throws(()=>budgetHarness(Object.freeze({...harness}),owner),e=>e.code==='SQL_NOT_ADMITTED');
  assert.throws(()=>budgetHarness(harness,owner),e=>e.code==='SQL_NOT_ADMITTED');
  const a=harness.newStore(),b=harness.newStore();assert.equal(owner.budget.state.used,4);assert.throws(()=>harness.newStore(),e=>e.code==='OWNER_CLIENT_BUDGET');
  await a.close();await a.close();assert.equal(owner.budget.state.used,2);await resources.closeCase();await harness.close();assert.equal(owner.budget.state.used,0);assert.equal(resources.state().stores,0);assert.equal(resources.state().pools,0);await b.close();
});
test('plan CLI reads only supplied local configuration and reports no runtime admission',async()=>{
  const filename=path.join(testFiles,'plan-input.json');await fs.writeFile(filename,JSON.stringify({worktree:root,runId:U}));const result=await runCli(['plan',filename]);assert.equal(result.engine,'NOT_RUN');assert.match(result.status,/NO_RUNTIME_ADMISSION/);await assert.rejects(runCli(['smoke',filename]),e=>e.code==='OWNER_RUNTIME_CAPABILITY_REQUIRED');
});

test('reviewed fixed migration batch is accepted without accepting arbitrary SQL batches',async()=>{
  const sql=await fs.readFile(new URL('../../sql/001-b1.sql',import.meta.url),'utf8');
  assert.equal(classifySql(sql),'migration');const t=new CommitTracker();step(t,'BEGIN','BEGIN');t.frontend(query(sql));assert.equal(t.pending,'migration');
  for(const tag of ['CREATE SCHEMA','CREATE TABLE','CREATE FUNCTION','GRANT'])assert.equal(t.backend(complete(tag)).forward,true);assert.equal(t.backend(ready('T')).forward,true);step(t,'COMMIT','COMMIT','I');
  assert.throws(()=>classifySql(sql+' '));assert.throws(()=>classifySql('SELECT 1; SELECT 2'));
});

test('late callback inherits a closed case scope and cannot create a new pool or worker',async()=>{
  const {harness}=await pureHarness(),owner={budget:new ConnectionBudget(),signal:new AbortController().signal};let release,late;
  const resources=budgetHarness(harness,owner),gate=new Promise(r=>{release=r;});
  await resources.runCase(async()=>{late=gate.then(()=>{assert.throws(()=>harness.newStore(),e=>e.code==='OWNER_CASE_SCOPE_ENDED');return assert.rejects(harness.issuanceWorker(),e=>e.code==='OWNER_CASE_SCOPE_ENDED');});});
  release();await late;assert.equal(owner.budget.state.used,0);await harness.close();
});

test('actual harness emits the four supported lifecycle actions; unknown actions stay denied',async()=>{
  const seen=[],gate=new PhaseGate({runId:U,authorize:async()=>true});
  const {harness}=await pureHarness({onAdmission:async action=>{
    await gate.require(action);seen.push(action);
    if(action!=='connect-owned-fixture')throw new OwnerError('PURE_STOP_BEFORE_ENGINE');
  }});
  for(const call of [()=>harness.restartDatabase('clean'),()=>harness.restartDatabase('crash'),()=>harness.exhaustSerialization({maximumAttempts:3}),()=>harness.issuanceWorker()])
    await assert.rejects(call(),e=>e.code==='PURE_STOP_BEFORE_ENGINE');
  assert.deepEqual(seen,['connect-owned-fixture','restart-clean','restart-crash','serialization-fixture','start-owned-worker']);
  await assert.rejects(gate.require('restart-arbitrary'),e=>e.code==='OWNER_ACTION');
  assert.deepEqual(gate.denied,[{action:'restart-arbitrary',reason:'UNKNOWN_ACTION'}]);await harness.close();
});
test('secret materialization precedes mount creation and a failed write prevents create',async()=>{
  const secret=path.join(testFiles,'bootstrap'),events=[];
  const result=await createContainerWithSecret({writeSecret:async()=>{await fs.writeFile(secret,'fixture-only',{mode:0o600});events.push('secret');},create:async()=>{assert.equal(await fs.readFile(secret,'utf8'),'fixture-only');events.push('create');return 'container';}});
  assert.equal(result,'container');assert.deepEqual(events,['secret','create']);assert.equal((await fs.stat(secret)).mode&0o777,0o600);
  await assert.rejects(createContainerWithSecret({writeSecret:async()=>{throw new Error('write failed');},create:async()=>{events.push('unexpected');}}),/write failed/);
  assert.deepEqual(events,['secret','create']);
  const args=containerArgs(plan,'a'.repeat(64));assert.ok(args.includes('type=bind,src='+plan.guestRoot+'/secrets/bootstrap,dst=/run/secrets/bootstrap,readonly'));
  assert.doesNotMatch(JSON.stringify(args),/fixture-only/);
});
test('container bootstrap authenticates local and host access with SCRAM',()=>{
  const args=containerArgs(plan,'a'.repeat(64));
  assert.ok(args.includes('POSTGRES_USER=b1_bootstrap'));
  assert.ok(args.includes('POSTGRES_PASSWORD_FILE=/run/secrets/bootstrap'));
  assert.ok(args.includes('POSTGRES_INITDB_ARGS=--auth-host=scram-sha-256 --auth-local=scram-sha-256'));
  assert.doesNotMatch(JSON.stringify(args),/auth-local=peer|auth-host=trust/);
  // Argument construction only: no entrypoint/bootstrap or guest quota execution.
});
test('persisted fixture identity remains private, exportable, signable and stable on reload',async()=>{
  const dir=path.join(testFiles,'bindings');await fs.mkdir(dir,{mode:0o700});let fingerprint;
  const owner={plan:{artifactRoot:dir,uid:process.getuid(),runId:U},gate:{require:async action=>assert.equal(action,'create-owned-root')},setBindingsFingerprint:value=>{fingerprint=value;}};
  const first=await persistentBindings(owner),jwk=await exportJWK(first.serverKey.privateKey);
  assert.equal(jwk.d.length>0,true);assert.equal(first.serverKey.privateKey.extractable,true);
  const claims={version:1,purpose:'pure fixture regression'},signed=await first.config.signer.sign(claims);
  const verified=await compactVerify(signed,first.serverKey.publicKey);assert.deepEqual(JSON.parse(Buffer.from(verified.payload).toString()),claims);
  const family=await first.family('case-a'),request=first.grantOwnership('installation',family,'fixture-subject');
  const admin=await first.config.administration.verify(first.adminCapability),second=await persistentBindings(owner);
  assert.deepEqual(await exportJWK(second.serverKey.privateKey),jwk);assert.equal(fingerprint,digest(canonical(first.serverKey.jwk)));
  assert.equal((await second.family('case-a')).fingerprint,family.fingerprint);
  assert.deepEqual(await second.config.administration.verify(second.adminCapability),admin);
  assert.deepEqual(await second.config.ownership.verify({installationId:'installation',request,recipientFingerprint:family.fingerprint}),{installationId:'installation',profileKind:'normal',subject:'fixture-subject'});
  assert.equal((await fs.stat(dir)).mode&0o777,0o700);assert.equal((await fs.stat(path.join(dir,'fixture-bindings.json'))).mode&0o777,0o600);
  await assert.rejects(second.config.administration.verify(first.adminCapability),e=>e.code==='UNAUTHORIZED');
  await first.store.close();await second.store.close();
});
test('actual resource lifecycle bounds rejection and stalls while retaining uncertain handles',async()=>{
  const owner={budget:new ConnectionBudget(),signal:new AbortController().signal},resources=createResourceLifecycle(owner),bad={},slow={};let resolve;
  resources.hooks.track('store',bad,async()=>{throw new OwnerError('PURE_CLOSE_REJECTED');});
  resources.hooks.track('worker',slow,()=>new Promise(yes=>{resolve=yes;}));
  const began=performance.now();await assert.rejects(resources.closeCase({deadline:began+40}),e=>{
    assert.deepEqual(e.failures.map(v=>v.code),['PURE_CLOSE_REJECTED','OWNER_DEADLINE']);return e.code==='OWNER_CASE_CLEANUP_INCOMPLETE';
  });assert.ok(performance.now()-began<500);assert.equal(resources.state().stores,1);assert.equal(resources.state().workers,1);
  resolve();await flush();assert.equal(resources.state().workers,1);resources.hooks.settled(slow);assert.equal(resources.state().workers,0);
});
test('finish always invokes owner teardown after resource rejection and harness stall, retaining failures',async()=>{
  const events=[],nested=[{name:'store-0',status:'UNCERTAIN',code:'PURE_STORE_FAILED'}];let late;
  const runtime={resources:{closeCase:async()=>{events.push('resources');throw Object.assign(new OwnerError('OWNER_CASE_CLEANUP_INCOMPLETE'),{failures:nested});}},harness:{close:()=>{events.push('harness');return new Promise(resolve=>{late=resolve;});}},owner:{cleanup:async()=>{events.push('owner');return {status:'STOPPED_RETAINED'};}}};
  const began=performance.now();await assert.rejects(finishRuntime(runtime,{deadline:began+50}),error=>{
    assert.equal(error.code,'OWNER_CLEANUP_INCOMPLETE');assert.deepEqual(error.failures[0].failures,nested);assert.equal(error.failures[1].code,'OWNER_DEADLINE');return true;
  });assert.deepEqual(events,['resources','harness','owner']);assert.ok(performance.now()-began<500);late();await flush();
});
test('finish retains rejected or stalled owner teardown as incomplete',async()=>{
  for(const close of [async()=>{throw new OwnerError('PURE_OWNER_FAILED');},()=>new Promise(()=>{})]){
    let calls=0;await assert.rejects(finishRuntime({owner:{cleanup:()=>{calls++;return close();}}},{deadline:performance.now()+30}),e=>e.code==='OWNER_CLEANUP_INCOMPLETE'&&e.cleanup.status==='CLEANUP_INCOMPLETE_RETAINED');assert.equal(calls,1);
  }
});
test('fractional cleanup budgets floor without extending their absolute deadline',()=>{
  assert.equal(remainingMilliseconds(100.9,100,1.2),99);assert.equal(remainingMilliseconds(100.9,30,1.2),30);
  const timeoutMs=remainingMilliseconds(10.9,100,1.2);assert.equal(command(plan,'docker',['inspect','a'.repeat(64)],{timeoutMs}).timeoutMs,9);
  for(const [end,now] of [[1.9,1.2],[1,2],[NaN,0],[Infinity,0]])assert.throws(()=>remainingMilliseconds(end,100,now),e=>e.code==='OWNER_DEADLINE');
  assert.throws(()=>remainingMilliseconds(100,1.5,0),e=>e.code==='OWNER_INTEGER');
});
test('installed executable aliases resolve to pinned canonical targets while cache aliases remain invalid',async()=>{
  const dir=path.join(testFiles,'tools');await fs.mkdir(dir);const binary=path.join(dir,'colima'),alias=path.join(dir,'alias'),other=path.join(dir,'other');
  await fs.writeFile(binary,'pure executable fixture',{mode:0o755});await fs.writeFile(other,'changed executable',{mode:0o755});await fs.symlink(binary,alias);
  const stat=await fs.stat(binary),pin={canonicalPath:binary,bytes:stat.size,sha256:digest(await fs.readFile(binary))};
  const executable=await resolveExecutable(alias,pin),spec=command(plan,'colima',['--version'],{executable});assert.equal(spec.executable,binary);
  await fs.unlink(alias);await fs.symlink(other,alias);assert.equal(spec.executable,binary);await assert.rejects(resolveExecutable(alias,pin),e=>e.code==='OWNER_TOOL_SOURCE_PIN');
  await assert.rejects(regular(alias),e=>e.code==='OWNER_REGULAR_FILE');await fs.chmod(binary,0o644);await assert.rejects(resolveExecutable(binary,pin),e=>e.code==='OWNER_TOOL_SOURCE_PIN');
});

// Scripted transport only: actual PostgresStore guards execute, no SQL engine.
function disconnectDriver({mode,event}){
  const pool=new EventEmitter(),clients=[],ledger={pending:null,highwater:0},lost=mode==='marker'?1:2;let commits=0;
  pool.connect=async()=>{
    const client=new EventEmitter();client._txStatus='I';client.released=[];client.destroyed=0;
    client.connection={stream:{destroy(){client.destroyed++;}}};client.release=discard=>client.released.push(discard);
    client.query=async(sql,values=[])=>{
      if(sql.startsWith('BEGIN'))client._txStatus='T';
      if(sql.includes('clock_try_lock('))return {rows:[{acquired:true}]};
      if(sql.includes('clock_begin(')){ledger.pending=values[5];return {rows:[{ready:true}]};}
      if(sql.includes('clock_finish(')){assert.equal(ledger.pending,values[5]);ledger.pending=null;ledger.highwater=values[6];return {rows:[{completed:true,highwater:String(ledger.highwater),db_now:'0',rollback_detected:false}]};}
      if(sql==='COMMIT'){commits++;client._txStatus='I';if(commits===lost){client.emit(event,...(event==='error'?[new Error('scripted disconnect')]:[]));return new Promise(()=>{});}}
      if(sql.includes('clock_unlock('))return {rows:[{released:true}]};return {rows:[]};
    };clients.push(client);return client;
  };pool.end=async()=>{};return {pool,clients,ledger,commits:()=>commits};
}
for(const mode of ['marker','observation'])for(const event of ['error','end'])test('actual clock guard preserves DB_UNAVAILABLE on scripted '+mode+' COMMIT '+event,async()=>{
  const bindings=await fixtureBindings(),d=disconnectDriver({mode,event}),store=new PostgresStore({pool:d.pool});let samples=0;
  await assert.rejects(store.observeTime({trust:bindings.config.trust,deadline:performance.now()+1000,signal:new AbortController().signal},()=>{samples++;return {now:100,uncertaintySeconds:0};}),isClockDisconnect);
  assert.equal(samples,mode==='marker'?0:1);assert.equal(d.commits(),mode==='marker'?1:2);assert.equal(d.clients.length,2);
  assert.ok(d.clients[1].released.includes(true));assert.ok(d.clients[1].destroyed>0);
  assert.equal(d.ledger.pending===null,mode==='observation');assert.equal(d.ledger.highwater,mode==='marker'?0:100);
  await store.close();await bindings.store.close();
});
test('clock disconnect predicate requires the guarded ProviderError class and exact code',()=>{
  assert.equal(isClockDisconnect(new ProviderError('DB_UNAVAILABLE')),true);assert.equal(isClockDisconnect(new ProviderError('CLOCK_UNTRUSTED')),false);
  assert.equal(isClockDisconnect(Object.assign(new Error('DB_UNAVAILABLE'),{code:'DB_UNAVAILABLE'})),false);
});
function fakeTimers(){
  const timers=new Map();let id=0;return {timers,schedule:(callback,ms)=>{timers.set(++id,{callback,ms});return id;},cancel:key=>timers.delete(key),fire:()=>{const list=[...timers.values()];timers.clear();for(const t of list)t.callback();}};
}
const auth=code=>{const body=Buffer.alloc(4);body.writeUInt32BE(code);return frame('R',body);};
test('startup deadline survives Startup, auth exchanges and every split of authenticated initial Ready',()=>{
  const bytes=Buffer.concat([auth(10).raw,auth(0).raw,frame('K',Buffer.alloc(8)).raw,ready('I').raw]);
  for(let split=0;split<=bytes.length;split++){
    const timers=fakeTimers(),expired=[],lifetime=new WireLifetime({...timers,onExpire:e=>expired.push(e.code)}),frontend=new FrameDecoder({startup:true}),backend=new FrameDecoder();
    for(const f of frontend.push(startup()))lifetime.frontend(f);lifetime.transaction(false);assert.equal(timers.timers.size,1);
    lifetime.frontend(frame('p',Buffer.from('opaque SCRAM response')));
    for(const f of backend.push(bytes.subarray(0,split)))lifetime.backend(f);
    assert.equal(lifetime.startupComplete,split===bytes.length);assert.equal(timers.timers.size,split===bytes.length?0:1);
    for(const f of backend.push(bytes.subarray(split)))lifetime.backend(f);
    assert.equal(lifetime.startupComplete,true);assert.equal(timers.timers.size,0);timers.fire();assert.deepEqual(expired,[]);lifetime.close();
  }
});
test('authentication stall expires; unauthenticated Ready/query fails; transaction deadline is independent',()=>{
  const timers=fakeTimers(),expired=[],lifetime=new WireLifetime({...timers,onExpire:e=>expired.push(e.code)});
  lifetime.frontend(new FrameDecoder({startup:true}).push(startup())[0]);lifetime.backend(auth(10));lifetime.frontend(frame('p',Buffer.from('opaque')));lifetime.transaction(false);
  timers.fire();assert.deepEqual(expired,['WIRE_STARTUP_DEADLINE']);lifetime.close();
  const secondTimers=fakeTimers(),second=new WireLifetime({...secondTimers,onExpire:e=>expired.push(e.code)});
  second.frontend(new FrameDecoder({startup:true}).push(startup())[0]);assert.throws(()=>second.backend(ready('I')),e=>e.code==='WIRE_STARTUP_NOT_COMPLETE');assert.throws(()=>second.frontend(query('SELECT 1')),e=>e.code==='WIRE_AUTHENTICATION_INCOMPLETE');
  second.backend(auth(0));second.backend(ready('I'));second.transaction(true);assert.equal(secondTimers.timers.size,1);assert.equal([...secondTimers.timers.values()][0].ms,5000);
  secondTimers.fire();assert.equal(expired.at(-1),'WIRE_TRANSACTION_DEADLINE');second.close();
});

test('finish preserves all actual lifecycle failure results before its enclosing deadline',async()=>{
  const owner={budget:new ConnectionBudget(),signal:new AbortController().signal,cleanup:async()=>({status:'STOPPED_RETAINED'})},resources=createResourceLifecycle(owner);
  resources.hooks.track('store',{},async()=>{throw new OwnerError('PURE_FIRST_FAILURE');});resources.hooks.track('worker',{},()=>new Promise(()=>{}));
  await assert.rejects(finishRuntime({owner,resources},{deadline:performance.now()+70}),error=>{
    assert.equal(error.failures[0].code,'OWNER_CASE_CLEANUP_INCOMPLETE');assert.deepEqual(error.failures[0].failures.map(f=>f.code),['PURE_FIRST_FAILURE','OWNER_DEADLINE']);return true;
  });
});

// The same helper used by runDeadlock: fixture doubles have no HTTP listener.
test('deadlock registration rejection always closes the application and preserves its original error',async()=>{
  const failure=new ProviderError('DB_UNAVAILABLE'),events=[],f={close:async()=>events.push('application-close')};
  await assert.rejects(registerFixtureAndClose(f,'pure',{registerFixture:async app=>{assert.equal(app,f);events.push('register');throw failure;}}),e=>e===failure);
  assert.deepEqual(events,['register','application-close']);
});
test('deadlock non201 assertion also closes the whole application',async()=>{
  const events=[],f={close:async()=>events.push('application-close')};
  await assert.rejects(registerFixtureAndClose(f,'pure',{registerFixture:async()=>({status:503})}),e=>e.code==='ERR_ASSERTION'&&e.actual===503&&e.expected===201);
  assert.deepEqual(events,['application-close']);
});
test('deadlock successful registration retains its result after exactly one application close',async()=>{
  const result={status:201,json:{claims:{installationRef:'fixture'}}};let closes=0;
  assert.equal(await registerFixtureAndClose({close:async()=>closes++},'pure',{registerFixture:async()=>result}),result);assert.equal(closes,1);
});
test('deadlock cleanup rejection retains both registration and closure failure',async()=>{
  const failure=new ProviderError('DB_UNAVAILABLE');let closes=0;
  await assert.rejects(registerFixtureAndClose({close:async()=>{closes++;throw new OwnerError('PURE_APPLICATION_CLOSE_FAILED');}},'pure',{registerFixture:async()=>{throw failure;}}),e=>{
    assert.equal(e.cause,failure);assert.equal(e.failures[0].code,'PURE_APPLICATION_CLOSE_FAILED');return e.code==='OWNER_FAULT_APP_CLEANUP_INCOMPLETE';
  });assert.equal(closes,1);
});
test('deadlock stalled cleanup is bounded and retains the non201 assertion and uncertain state',async()=>{
  let closes=0,late;const began=performance.now();
  await assert.rejects(registerFixtureAndClose({close:()=>{closes++;return new Promise(resolve=>{late=resolve;});}},'pure',{registerFixture:async()=>({status:409}),cleanupMs:30}),e=>{
    assert.equal(e.cause.code,'ERR_ASSERTION');assert.equal(e.cause.actual,409);assert.equal(e.failures[0].code,'OWNER_DEADLINE');return e.code==='OWNER_FAULT_APP_CLEANUP_INCOMPLETE';
  });assert.equal(closes,1);assert.ok(performance.now()-began<500);late();await flush();
});
const currentWorktree=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../../../..');
async function measureReviewed(worktree){
  const files=[];
  for(const name of RUNTIME_SOURCE_FILES){const filename=path.join(worktree,name),data=await fs.readFile(filename);files.push({path:name,type:'file',bytes:data.length,sha256:digest(data)});}
  const entry=await fs.realpath(createRequire(worktree+'/prototypes/access-provider-service/package.json').resolve('@aegis-local/access-provider-contract'));
  // Deliberately fabricated review inputs for pure validator tests. These do
  // not authorize resource startup and are never real review receipts.
  return {format:'B1_REVIEWED_RUNTIME_SOURCES_V1',worktree,files,resolvedContract:{entryPath:path.relative(worktree,entry)}};
}
async function reviewedFixture(){
  const base=await fs.mkdtemp(path.join(testFiles,'reviewed-')),worktree=await fs.realpath(base);
  for(const name of RUNTIME_SOURCE_FILES){const filename=path.join(worktree,name);await fs.mkdir(path.dirname(filename),{recursive:true});await fs.copyFile(path.join(currentWorktree,name),filename);}
  const installed=path.join(worktree,'prototypes/access-provider-service/node_modules/.pnpm/pure-contract/node_modules/@aegis-local/access-provider-contract'),alias=path.join(worktree,'prototypes/access-provider-service/node_modules/@aegis-local/access-provider-contract');
  await fs.mkdir(installed,{recursive:true});await fs.mkdir(path.dirname(alias),{recursive:true});
  for(const name of ['README.md','package.json','contract.mjs','contract.test.mjs','vectors.json'])await fs.copyFile(path.join(worktree,'prototypes/access-provider-contract',name),path.join(installed,name));
  await fs.symlink(installed,alias);return {worktree,installed,alias,reviewed:await measureReviewed(worktree)};
}
const changedSource=e=>e.code==='OWNER_REVIEW_SOURCE_CHANGED';
test('complete reviewed source validator rejects missing omitted duplicate unexpected and stale manifest entries',async()=>{
  const {worktree,reviewed}=await reviewedFixture(),plan={worktree};assert.equal(reviewed.files.length,27);assert.equal(await verifyReviewedSources(plan,reviewed),true);
  const missing=structuredClone(reviewed);missing.files.pop();
  const duplicate=structuredClone(reviewed);duplicate.files[26]=duplicate.files[0];
  const unexpected=structuredClone(reviewed);unexpected.files[0].path='prototypes/access-provider-service/unreviewed.mjs';
  const stale=structuredClone(reviewed);stale.files[0].sha256='0'.repeat(64);
  const wrongSize=structuredClone(reviewed);wrongSize.files[0].bytes++;
  const wrongRoot=structuredClone(reviewed);wrongRoot.worktree+='/elsewhere';
  const wrongDependency=structuredClone(reviewed);wrongDependency.resolvedContract.entryPath='prototypes/access-provider-contract/contract.mjs';
  for(const manifest of [undefined,missing,duplicate,unexpected,stale,wrongSize,wrongRoot,wrongDependency])await assert.rejects(verifyReviewedSources(plan,manifest),changedSource);
  const extra=path.join(worktree,'prototypes/access-provider-service/unreviewed.mjs');await fs.writeFile(extra,'// unreviewed');await assert.rejects(verifyReviewedSources(plan,reviewed),changedSource);await fs.unlink(extra);
  const missingFile=path.join(worktree,'prototypes/access-provider-service/test/pg-harness.mjs'),saved=await fs.readFile(missingFile);await fs.unlink(missingFile);await assert.rejects(verifyReviewedSources(plan,reviewed),changedSource);await fs.writeFile(missingFile,saved);
  assert.equal(await verifyReviewedSources(plan,reviewed),true);
});
test('every logical service21 and contract6 input drift refuses the prior approved manifest',async()=>{
  const {worktree,reviewed}=await reviewedFixture();
  assert.equal(await verifyReviewedSources({worktree},reviewed),true);
  for(const name of RUNTIME_SOURCE_FILES){const filename=path.join(worktree,name),saved=await fs.readFile(filename);await fs.appendFile(filename,'\n');await assert.rejects(verifyReviewedSources({worktree},reviewed),changedSource,name);await fs.writeFile(filename,saved);}
  assert.equal(await verifyReviewedSources({worktree},reviewed),true);
});
test('source binding checks materialized Node dependency bytes and rejects alias retargeting',async()=>{
  const {worktree,reviewed,installed,alias}=await reviewedFixture();
  assert.equal(await verifyReviewedSources({worktree},reviewed),true);
  for(const name of ['README.md','package.json','contract.mjs','contract.test.mjs','vectors.json']){const filename=path.join(installed,name),saved=await fs.readFile(filename);await fs.appendFile(filename,'\n');await assert.rejects(verifyReviewedSources({worktree},reviewed),changedSource);await fs.writeFile(filename,saved);}
  const elsewhere=path.join(worktree,'different-contract');await fs.mkdir(elsewhere);
  for(const name of ['README.md','package.json','contract.mjs','contract.test.mjs','vectors.json'])await fs.copyFile(path.join(installed,name),path.join(elsewhere,name));
  await fs.unlink(alias);await fs.symlink(elsewhere,alias);await assert.rejects(verifyReviewedSources({worktree},reviewed),changedSource);
});
test('actual capability loading binds CLEAR receipt and immutable source reference, rechecking before each action',async()=>{
  const dir=await fs.mkdtemp(path.join(testFiles,'capability-')),manifest=await measureReviewed(currentWorktree),manifestPath=path.join(dir,'sources.json'),receiptPath=path.join(dir,'review.json'),capPath=path.join(dir,'capability.json');
  const manifestRaw=JSON.stringify(manifest),manifestHash=digest(manifestRaw),receipt={decision:'CLEAR',reviewedIdentity:{runtimeSourceManifestSha256:manifestHash}},receiptRaw=JSON.stringify(receipt);
  await fs.writeFile(manifestPath,manifestRaw);await fs.writeFile(receiptPath,receiptRaw);
  const tools={colima:{canonicalPath:'/pure/colima',bytes:1,sha256:'a'.repeat(64)}},cap={status:'SOURCE_REVIEWED_RUNTIME_PHASE_ADMITTED',runId:U,actions:['preflight'],expiresAt:new Date(Date.now()+60000).toISOString(),reviewedSources:{path:manifestPath,sha256:manifestHash},independentReview:{path:receiptPath,sha256:digest(receiptRaw)},manifest:{tools}};
  const saveCap=async value=>fs.writeFile(capPath,JSON.stringify(value));await saveCap(cap);
  const authorization=await capabilityAuthorizer(capPath,{worktree:currentWorktree,runId:U});assert.deepEqual(authorization.manifest.tools,tools);
  assert.equal(await authorization.authorize({runId:U,action:'preflight'}),true);assert.equal(await authorization.authorize({runId:U,action:'docker-start'}),false);
  assert.equal(authorization.manifest.sourceSha256['guest.sh'],manifest.files.find(f=>f.path.endsWith('/guest.sh')).sha256);
  await fs.appendFile(manifestPath,' ');await assert.rejects(authorization.authorize({runId:U,action:'preflight'}),changedSource);await fs.writeFile(manifestPath,manifestRaw);
  await fs.appendFile(receiptPath,' ');await assert.rejects(authorization.authorize({runId:U,action:'preflight'}),e=>e.code==='OWNER_REVIEW_RECEIPT');await fs.writeFile(receiptPath,receiptRaw);
  for(const changed of [{...cap,reviewedSources:undefined},{...cap,reviewedSources:{...cap.reviewedSources,path:path.join(dir,'missing')}},{...cap,reviewedSources:{...cap.reviewedSources,sha256:'0'.repeat(64)}}]){await saveCap(changed);await assert.rejects(capabilityAuthorizer(capPath,{worktree:currentWorktree,runId:U}),changedSource);}
  const notClear=JSON.stringify({...receipt,decision:'NOT_CLEAR'});await fs.writeFile(receiptPath,notClear);await saveCap({...cap,independentReview:{path:receiptPath,sha256:digest(notClear)}});await assert.rejects(capabilityAuthorizer(capPath,{worktree:currentWorktree,runId:U}),e=>e.code==='OWNER_REVIEW_RECEIPT');
  await fs.writeFile(receiptPath,receiptRaw);await fs.appendFile(manifestPath,' ');const changedHash=digest(manifestRaw+' ');await saveCap({...cap,reviewedSources:{path:manifestPath,sha256:changedHash}});await assert.rejects(capabilityAuthorizer(capPath,{worktree:currentWorktree,runId:U}),e=>e.code==='OWNER_REVIEW_RECEIPT');
});
function savePgEnvironment(){const saved=Object.fromEntries(['PGSSLMODE','PGSSLNEGOTIATION'].map(k=>[k,process.env[k]]));return ()=>{for(const [k,v] of Object.entries(saved))if(v===undefined)delete process.env[k];else process.env[k]=v;};}
test('fixture TLS admission rejects require and direct negotiation; allowed setup uses actual pinned pg Client defaults',async()=>{
  const restore=savePgEnvironment();
  try{
    delete process.env.PGSSLNEGOTIATION;
    for(const mode of ['require','prefer','verify-full','no-verify','unknown','']){process.env.PGSSLMODE=mode;let admissions=0;await assert.rejects(pureHarness({onAdmission:async()=>admissions++}),e=>e.code==='OWNER_PG_TLS_ENV');assert.equal(admissions,0);}
    process.env.PGSSLMODE='disable';process.env.PGSSLNEGOTIATION='direct';assert.throws(()=>assertFixtureEnvironment(),e=>e.code==='OWNER_PG_TLS_ENV');
    for(const mode of [undefined,'disable']){
      if(mode===undefined)delete process.env.PGSSLMODE;else process.env.PGSSLMODE=mode;process.env.PGSSLNEGOTIATION='postgres';assertFixtureEnvironment();
      const pool=createOwnedPool({host:'127.0.0.1',port:54321,user:'b1_app',database:'b1',password:'pure-only'}),client=new pool.Client(pool.options);
      assert.equal(client.ssl,false);assert.equal(client.sslNegotiation,'postgres');await pool.end();
      const {harness}=await pureHarness();assertAdmitted(harness);assert.equal(Object.isFrozen(harness),true);await harness.close();
    }
  }finally{restore();}
});
test('TLS environment drift after real harness admission refuses a new pool without changing process environment',async()=>{
  const restore=savePgEnvironment();let harness;
  try{
    delete process.env.PGSSLMODE;delete process.env.PGSSLNEGOTIATION;({harness}=await pureHarness());process.env.PGSSLMODE='require';
    assert.throws(()=>harness.newStore(),e=>e.code==='OWNER_PG_TLS_ENV');assert.equal(process.env.PGSSLMODE,'require');
    delete process.env.PGSSLMODE;const store=harness.newStore();await store.close();assertAdmitted(harness);
  }finally{restore();await harness?.close();}
});
test('owner rejects incompatible TLS before preflight authorization or connection selection',async()=>{
  const restore=savePgEnvironment();let grants=0;
  try{
    process.env.PGSSLMODE='require';delete process.env.PGSSLNEGOTIATION;
    const owner=new OwnerSupervisor({plan,manifest:{},authorize:async()=>{grants++;return true;}});
    await assert.rejects(owner.preflight(),e=>e.code==='OWNER_PG_TLS_ENV');assert.equal(grants,0);assert.throws(()=>owner.connections(),e=>e.code==='OWNER_PG_TLS_ENV');assert.equal(owner.phase,'new');
  }finally{restore();}
});

// Real pg-pool/client construction and harness/store lifetime, fake transport.
// Client.connect/query/end are mocked before any effectful connection method.
function scriptedPinnedPg(t){
  const clients=[],ended=[],ledger={pending:null,highwater:0};
  t.mock.method(Client.prototype,'connect',function(callback){
    assert.equal(this.ssl,false);assert.equal(this.sslNegotiation,'postgres');assert.equal(this.connectionParameters.ssl,false);assert.equal(this.connectionParameters.sslnegotiation,'postgres');
    this._txStatus='I';clients.push(this);queueMicrotask(()=>callback(null));
  });
  t.mock.method(Client.prototype,'query',async function(sql,values=[]){
    if(sql.startsWith('BEGIN'))this._txStatus='T';if(sql==='COMMIT'||sql==='ROLLBACK')this._txStatus='I';
    if(sql.includes('clock_try_lock('))return {rows:[{acquired:true}]};
    if(sql.includes('clock_begin(')){assert.equal(ledger.pending,null);ledger.pending=values[5];return {rows:[{ready:true}]};}
    if(sql.includes('clock_finish(')){assert.equal(ledger.pending,values[5]);ledger.pending=null;ledger.highwater=Math.max(ledger.highwater,values[6]);return {rows:[{completed:true,highwater:String(ledger.highwater),db_now:'0',rollback_detected:false}]};}
    if(sql.includes('clock_unlock('))return {rows:[{released:true}]};return {rows:[]};
  });
  t.mock.method(Client.prototype,'end',function(callback){
    this._ending=true;ended.push(this);queueMicrotask(()=>{this.emit('end');callback?.();});return Promise.resolve();
  });
  return {clients,ended,ledger};
}
const observePure=(store,harness,now)=>store.observeTime({trust:harness.trust,deadline:performance.now()+4000,signal:new AbortController().signal},()=>({now,uncertaintySeconds:0}));
for(const [setting,value] of [['PGSSLMODE','require'],['PGSSLNEGOTIATION','direct']])test('actual harness first checkout pins TLS after store creation then '+setting+' drift',async t=>{
  const restore=savePgEnvironment(),driver=scriptedPinnedPg(t);let harness,store,bindings;
  try{
    delete process.env.PGSSLMODE;delete process.env.PGSSLNEGOTIATION;({harness,bindings}=await pureHarness());store=harness.newStore();assert.equal(driver.clients.length,0);
    process.env[setting]=value;const observed=await observePure(store,harness,100);assert.equal(observed.latest,100);assert.equal(driver.clients.length,2);assert.equal(process.env[setting],value);
    // Reuse remains compatible, and restoring env allows the next operation.
    delete process.env[setting];assert.equal((await observePure(store,harness,101)).latest,101);assert.equal(driver.clients.length,2);assertAdmitted(harness);assert.equal(Object.isFrozen(harness),true);
  }finally{try{await store?.close();await harness?.close();await bindings?.store.close();}finally{restore();t.mock.restoreAll();}}
});
test('actual pg-pool idle expiry replaces harness clients with pinned TLS despite later require/direct env',async t=>{
  const restore=savePgEnvironment(),driver=scriptedPinnedPg(t);let harness,store,bindings;
  try{
    delete process.env.PGSSLMODE;delete process.env.PGSSLNEGOTIATION;({harness,bindings}=await pureHarness());store=harness.newStore();
    assert.equal((await observePure(store,harness,200)).latest,200);assert.equal(driver.clients.length,2);assert.equal(driver.ended.length,0);
    const original=driver.clients.slice();process.env.PGSSLMODE='require';process.env.PGSSLNEGOTIATION='direct';
    // Preserve actual fixture idleTimeoutMillis=1000. Its real expiry removes
    // both released clients; this is no manual/fake pool replacement.
    const deadline=performance.now()+2500;
    while(driver.ended.length<2){assert.ok(performance.now()<deadline,'pinned pg-pool idle expiry missing');await new Promise(resolve=>setTimeout(resolve,20));}
    assert.equal((await observePure(store,harness,201)).latest,201);assert.equal(driver.clients.length,4);assert.ok(driver.clients.slice(2).every(c=>!original.includes(c)));
    assert.equal(process.env.PGSSLMODE,'require');assert.equal(process.env.PGSSLNEGOTIATION,'direct');assertAdmitted(harness);
  }finally{try{await store?.close();await harness?.close();await bindings?.store.close();}finally{restore();t.mock.restoreAll();}}
});
