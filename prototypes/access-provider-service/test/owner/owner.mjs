// External fixture supervisor. Imports never spawn, write, connect or listen.
import { spawn } from 'node:child_process';
import { createHash, randomBytes } from 'node:crypto';
import { promises as fs, constants as FC, createReadStream } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
import { performance } from 'node:perf_hooks';
import { createOwnedPool, closeOwnedPool } from '../../store.mjs';
import { WireProxy } from './wire-proxy.mjs';

const GiB=1024**3,MiB=1024**2;
export const PIN=Object.freeze({node:'22.23.1',pnpm:'9.15.0',colima:'0.10.3',docker:'29.6.1',
  vmBytes:3758096384,vmSha256:'7ab64981326b7c34f4822d58764e83d2b4cb097472fa1c23975f3bb7b53790d0',
  imageDigest:'sha256:a85953d6f830fd55a12929df3b7a4fa94dc96d652de62d2d714e0867ccb3670d',
  configDigest:'sha256:bb96c2f03a0b490277250ef12f9fd17e43244db37811cca90711e135bd98ece2',
  image:'docker.io/library/postgres@sha256:a85953d6f830fd55a12929df3b7a4fa94dc96d652de62d2d714e0867ccb3670d',
  platform:'linux/arm64',serverVersion:'170011'});
export const CAPS=Object.freeze({cpu:2,vmMemory:3*GiB,rootDisk:12*GiB,vmDataDisk:16*GiB,
  data:2*GiB,memory:GiB,shm:128*MiB,results:128*MiB,pids:128,clients:12,
  appPools:2,appConnections:2,residual:6*GiB,setupMs:900000,suiteMs:1800000,cleanupMs:30000});
export const ACTIONS=Object.freeze(['preflight','create-owned-root','hdiutil-create','hdiutil-attach',
  'provision','colima-start','colima-status','guest-preflight','guest-prepare','guest-quota-test',
  'guest-secrets','guest-inspect','guest-sentinel','guest-verify-sentinel','docker-inspect','docker-image-inspect',
  'docker-image-load','docker-network-create','docker-network-inspect','docker-create','docker-cp-passwd',
  'docker-start','docker-stop','docker-kill','colima-stop','verify-forwarder','provision-roles',
  'start-owned-wire-proxy','connect-owned-fixture','verify-health','create-fresh-schema','lock-owned-domain',
  'lock-owned-credential','inspect-owned-fixture','restart-owned-database','restart-owned-vm',
  'start-sql-slice','run-clock-smoke','run-faults','run-suite','cleanup','verify-stopped',
  'restart-clean','restart-crash','serialization-fixture','start-owned-worker']);
export class OwnerError extends Error {constructor(code){super(code);this.code=code;}}
const refuse=code=>{throw new OwnerError(code);};
const integer=(n,min=0,max=Number.MAX_SAFE_INTEGER)=>{if(!Number.isSafeInteger(n)||n<min||n>max)refuse('OWNER_INTEGER');return n;};
const uuid=u=>{if(typeof u!=='string'||!/^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/.test(u))refuse('OWNER_UUID');return u;};
export const digest=bytes=>createHash('sha256').update(bytes).digest('hex');
// This protocol deliberately has no TLS negotiation. Refuse incompatible
// ambient settings instead of changing production Pool/TLS behavior or env.
export function assertFixtureEnvironment(env=process.env){
  if(env.PGSSLMODE!==undefined&&env.PGSSLMODE!=='disable'||env.PGSSLNEGOTIATION!==undefined&&env.PGSSLNEGOTIATION!=='postgres')refuse('OWNER_PG_TLS_ENV');
}
export const RUNTIME_SOURCE_FILES=Object.freeze([
  ...['README.md','app.mjs','authority.mjs','http.mjs','migrate.mjs','package.json','pnpm-lock.yaml','proof.mjs','sql/001-b1.sql','start.mjs','store.mjs','test/fixtures.mjs','test/pg-harness.mjs','test/postgres.test.mjs','test/validation.test.mjs','test/owner/owner.mjs','test/owner/guest.sh','test/owner/runner.mjs','test/owner/wire-proxy.mjs','test/owner/fault-cases.mjs','test/owner/owner.test.mjs'].map(n=>'prototypes/access-provider-service/'+n),
  ...['README.md','package.json','pnpm-lock.yaml','contract.mjs','contract.test.mjs','vectors.json'].map(n=>'prototypes/access-provider-contract/'+n),
]);
const sourceFailure=()=>refuse('OWNER_REVIEW_SOURCE_CHANGED');
async function scopedFiles(root){
  const files=[];
  async function walk(dir){
    for(const item of await fs.readdir(dir,{withFileTypes:true})){
      if(dir===root&&item.name==='node_modules')continue;
      const filename=path.join(dir,item.name);
      if(item.isDirectory())await walk(filename);
      else if(item.isFile())files.push(path.relative(root,filename));
      else sourceFailure();
    }
  }
  await walk(root);return files.sort();
}
// Compare against the exact manifest approved by an external review receipt.
// This function never creates a new manifest or blesses the current bytes.
export async function verifyReviewedSources(plan,reviewed){
  try{
    const worktree=plan.worktree,expected=new Set(RUNTIME_SOURCE_FILES);
    if(await fs.realpath(worktree)!==worktree||reviewed?.format!=='B1_REVIEWED_RUNTIME_SOURCES_V1'||reviewed.worktree!==worktree||!Array.isArray(reviewed.files)||reviewed.files.length!==expected.size)sourceFailure();
    const entries=new Map();
    for(const f of reviewed.files){
      if(!expected.has(f?.path)||entries.has(f.path)||f.type!=='file'||!Number.isSafeInteger(f.bytes)||f.bytes<0||! /^[0-9a-f]{64}$/.test(f.sha256??''))sourceFailure();
      entries.set(f.path,f);
    }
    for(const packageName of ['access-provider-service','access-provider-contract']){
      const prefix='prototypes/'+packageName+'/',root=path.resolve(worktree,prefix);
      if(await fs.realpath(root)!==root)sourceFailure();
      const actual=(await scopedFiles(root)).map(n=>prefix+n).sort(),wanted=[...expected].filter(n=>n.startsWith(prefix)).sort();
      if(JSON.stringify(actual)!==JSON.stringify(wanted))sourceFailure();
    }
    for(const [name,f] of entries){const filename=path.join(worktree,name),s=await regular(filename);if(s.size!==f.bytes||await fileHash(filename)!==f.sha256)sourceFailure();}
    const serviceRoot=path.join(worktree,'prototypes/access-provider-service'),contractRoot=path.join(worktree,'prototypes/access-provider-contract');
    const service=await jsonFile(serviceRoot+'/package.json'),contract=await jsonFile(contractRoot+'/package.json');
    if(service.dependencies?.['@aegis-local/access-provider-contract']!=='file:../access-provider-contract'||contract.name!=='@aegis-local/access-provider-contract'||contract.exports!=='./contract.mjs')sourceFailure();
    const entry=await fs.realpath(createRequire(serviceRoot+'/package.json').resolve('@aegis-local/access-provider-contract'));
    if(!entry.startsWith(worktree+'/')||path.basename(entry)!=='contract.mjs'||reviewed.resolvedContract?.entryPath!==path.relative(worktree,entry))sourceFailure();
    // pnpm file dependencies can be a materialized copy. Bind the module Node
    // actually resolves, checking its bytes against the reviewed local package.
    const installedRoot=path.dirname(entry),actual=await scopedFiles(installedRoot),wanted=['README.md','package.json','contract.mjs','contract.test.mjs','vectors.json'];
    if(await fs.realpath(serviceRoot+'/node_modules/@aegis-local/access-provider-contract')!==installedRoot)sourceFailure();
    if(wanted.some(n=>!actual.includes(n))||actual.some(n=>!wanted.includes(n)&&n!=='pnpm-lock.yaml'))sourceFailure();
    for(const name of actual){const f=entries.get('prototypes/access-provider-contract/'+name),filename=path.join(installedRoot,name),s=await regular(filename);if(!f||s.size!==f.bytes||await fileHash(filename)!==f.sha256)sourceFailure();}
    return true;
  }catch{sourceFailure();}
}
export function createPlan({worktree,runId,uid=process.getuid(),vmImage,ociLayout,importArchive}) {
  uuid(runId);integer(uid);if(typeof worktree!=='string'||!worktree.startsWith('/Volumes/ExternalSSD/repositories/')||path.resolve(worktree)!==worktree)refuse('OWNER_WORKTREE');
  for(const p of [vmImage,ociLayout,importArchive].filter(Boolean))if(typeof p!=='string'||!p.startsWith('/Volumes/ExternalSSD/')||path.resolve(p)!==p)refuse('OWNER_CACHE_PATH');
  const root=worktree+'/.artifacts/b1-pg-'+runId,home='/Volumes/ExternalSSD/b1pg-'+runId+'/c',profile='b1-owner',socket=home+'/'+profile+'/docker.sock';
  if(Buffer.byteLength(socket)>=104)refuse('OWNER_SOCKET_LENGTH');
  return Object.freeze({worktree,runId,uid,artifactRoot:root,colimaHome:home,profile,socket,
    dockerConfig:root+'/docker-config',results:root+'/results',logImage:root+'/logs.img',
    guestRoot:'/var/lib/b1-owner/'+runId,containerName:'aegis-b1-pg-'+runId,
    networkName:'aegis-b1-net-'+runId,vmImage,ociLayout,importArchive});
}
// Remaining worst-case allocations, not previously observed free space.
export function residualCapacity(hostFree,guestFree,{hostReserve=CAPS.rootDisk+CAPS.vmDataDisk+CAPS.results+8*GiB,guestReserve=CAPS.data+GiB}={}) {
  for(const n of [hostFree,guestFree,hostReserve,guestReserve])integer(n);
  const remaining=Math.min(hostFree-hostReserve,guestFree-guestReserve);if(!Number.isSafeInteger(remaining)||remaining<CAPS.residual)refuse('OWNER_CAPACITY');return remaining;
}
export class ConnectionBudget {
  #used=0;#apps=0;#closed=false;
  get state(){return {used:this.#used,apps:this.#apps,limit:CAPS.clients};}
  reserve(kind,count=2){
    integer(count,1,CAPS.clients);if(this.#closed||this.#used+count>CAPS.clients||kind==='app'&&(this.#apps>=2||count!==2))refuse('OWNER_CLIENT_BUDGET');
    this.#used+=count;if(kind==='app')this.#apps++;let live=true;
    return ()=>{if(!live)return;live=false;this.#used-=count;if(kind==='app')this.#apps--;};
  }
  close(){this.#closed=true;}
}
export function command(plan,tool,args,{stdin=null,secret=false,timeoutMs=30000,executable:canonical=null}={}) {
  if(!Array.isArray(args)||args.some(x=>typeof x!=='string'||x.includes('\0')))refuse('OWNER_ARGV');integer(timeoutMs,1,CAPS.setupMs);
  const env={PATH:'/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin',HOME:process.env.HOME,
    LANG:'C',LC_ALL:'C',COLIMA_HOME:plan.colimaHome,DOCKER_CONFIG:plan.dockerConfig,
    LIMA_HOME:plan.colimaHome+'/_lima',TMPDIR:plan.results};
  const executable={colima:'/opt/homebrew/bin/colima',docker:'/opt/homebrew/bin/docker',hdiutil:'/usr/bin/hdiutil',lsof:'/usr/sbin/lsof',ps:'/bin/ps',pnpm:'/opt/homebrew/bin/pnpm'}[tool];
  if(!executable)refuse('OWNER_TOOL');
  if(tool==='docker'&&(args.some(a=>a==='--context'||a.startsWith('--host')||a==='-H'||a==='prune')||!['inspect','image','network','create','cp','start','stop','kill','--version'].includes(args[0])))refuse('OWNER_DOCKER_COMMAND');
  if(tool==='docker'&&args[0]==='image'&&!['inspect','load'].includes(args[1])||tool==='docker'&&args[0]==='network'&&!['inspect','create'].includes(args[1]))refuse('OWNER_DOCKER_COMMAND');
  if(canonical!==null&&(typeof canonical!=='string'||path.resolve(canonical)!==canonical))refuse('OWNER_EXECUTABLE_PATH');
  return {executable:canonical??executable,args:tool==='colima'?['--profile',plan.profile,...args]:tool==='docker'?['-H','unix://'+plan.socket,...args]:args,env,stdin,secret,timeoutMs};
}
export function colimaStartArgs(plan){return ['start','--activate=false','--ssh-config=false','--mount','none','--template=false','--ssh-agent=false','--binfmt=false','--vz-rosetta=false','--runtime','docker','--arch','aarch64','--vm-type','vz','--cpus','2','--memory','3','--root-disk','12','--disk','16','--disk-image',plan.vmImage];}
export async function createContainerWithSecret({writeSecret,create}) {
  await writeSecret();return create();
}
export function containerArgs(plan,networkId){
  if(!/^[0-9a-f]{64}$/.test(networkId))refuse('OWNER_NETWORK');
  return ['create','--pull=never','--name',plan.containerName,'--label','b1.owner='+plan.runId,
    '--platform',PIN.platform,'--network',networkId,'--publish','127.0.0.1:0:5432',
    '--cpus','2','--memory','1g','--memory-swap','1g','--shm-size','128m','--pids-limit','128',
    '--read-only','--log-driver','none','--tmpfs','/tmp:rw,noexec,nosuid,size=67108864',
    '--tmpfs','/var/run/postgresql:rw,noexec,nosuid,size=16777216',
    '--mount','type=bind,src='+plan.guestRoot+'/data,dst=/var/lib/postgresql/data',
    '--mount','type=bind,src='+plan.guestRoot+'/secrets/bootstrap,dst=/run/secrets/bootstrap,readonly',
    '--env','PGDATA=/var/lib/postgresql/data/pgdata','--env','POSTGRES_USER=b1_bootstrap',
    '--env','POSTGRES_DB=b1','--env','POSTGRES_PASSWORD_FILE=/run/secrets/bootstrap',
    '--env','POSTGRES_INITDB_ARGS=--auth-host=scram-sha-256 --auth-local=scram-sha-256',
    PIN.image,'postgres','-c','max_connections=12','-c','superuser_reserved_connections=0',
    '-c','reserved_connections=0','-c','fsync=on','-c','synchronous_commit=on','-c','full_page_writes=on',
    '-c','deadlock_timeout=20ms','-c','log_statement=none','-c','log_min_error_statement=panic',
    '-c','log_min_messages=panic','-c','logging_collector=off'];
}
export async function ownedRace(operation,{signal,deadline,onLate=()=>{}}) {
  let timer,abort,cancelled=false,produced=false,value,disposed=false;
  const dispose=()=>{if(disposed||!produced)return;disposed=true;Promise.resolve().then(()=>onLate(value)).catch(()=>{});};
  const work=Promise.resolve().then(operation);work.then(v=>{produced=true;value=v;if(cancelled)dispose();},()=>{});
  try {
    const v=await Promise.race([work,new Promise((_,reject)=>{
      abort=()=>reject(new OwnerError('OWNER_CANCELLED'));signal?.addEventListener('abort',abort,{once:true});
      timer=setTimeout(()=>reject(new OwnerError('OWNER_DEADLINE')),Math.max(0,deadline-performance.now()));if(signal?.aborted)abort();
    })]);if(signal?.aborted||performance.now()>=deadline)refuse('OWNER_DEADLINE');return v;
  }catch(error){cancelled=true;dispose();throw error;}finally{clearTimeout(timer);signal?.removeEventListener('abort',abort);}
}
// Never extend a deadline by rounding a positive fraction upward.
export function remainingMilliseconds(deadline,ceiling=CAPS.cleanupMs,now=performance.now()) {
  if(!Number.isFinite(deadline)||!Number.isFinite(now))refuse('OWNER_DEADLINE');integer(ceiling,1,CAPS.setupMs);
  const n=Math.floor(Math.min(ceiling,deadline-now));if(n<1)refuse('OWNER_DEADLINE');return n;
}
export async function settleClosures(closures,deadline) {
  return Promise.all(closures.map(async({name,close})=>{
    try{await ownedRace(close,{deadline});return {name,status:'CLOSED'};}
    catch(error){return {name,status:'UNCERTAIN',code:error instanceof OwnerError?error.code:'OWNER_CLOSE_FAILED',...(Array.isArray(error.failures)?{failures:error.failures}:{})};}
  }));
}
export class PhaseGate {
  denied=[];
  constructor({runId,authorize}){uuid(runId);if(typeof authorize!=='function')refuse('OWNER_AUTHORITY_REQUIRED');this.runId=runId;this.authorize=authorize;}
  async require(action,context={}){
    if(!ACTIONS.includes(action)){this.denied.push({action,reason:'UNKNOWN_ACTION'});refuse('OWNER_ACTION');}
    try{if(await this.authorize({runId:this.runId,action,...context})!==true)refuse('OWNER_NOT_AUTHORIZED');}
    catch(error){this.denied.push({action,reason:error instanceof OwnerError?error.code:'AUTHORITY_REJECTED'});throw error instanceof OwnerError?error:new OwnerError('OWNER_NOT_AUTHORIZED');}
  }
}
// Both the phase deadline and this command's cap begin before authorization.
// The synchronous start callback is also the inert seam used by pure tests.
export async function runAuthorizedCommand(owner,action,spec,{deadline,cleanup=false},start){
  integer(spec.timeoutMs,1,CAPS.setupMs);
  if(!Number.isFinite(deadline))refuse('OWNER_DEADLINE');
  const end=Math.min(deadline,performance.now()+spec.timeoutMs);remainingMilliseconds(end,spec.timeoutMs);
  await owner.gate.require(action);if(owner.signal.aborted&&!cleanup)refuse('OWNER_CANCELLED');
  const timeoutMs=remainingMilliseconds(end,spec.timeoutMs);
  return start({deadline:end,timeoutMs});
}
class Commands {
  children=new Set();sink=null;
  constructor(owner){this.owner=owner;}
  async run(action,spec,{deadline,cleanup=false}){
    return runAuthorizedCommand(this.owner,action,spec,{deadline,cleanup},({deadline:end})=>new Promise((resolve,reject)=>{
      const child=spawn(spec.executable,spec.args,{env:spec.env,shell:false,stdio:['pipe','pipe','pipe']});
      const identity={child,action,spawnedAt:performance.now(),done:false};this.children.add(identity);
      let bytes=0,out=[],err=[],failure=null,killTimer,forceTimer;const maximum=2*MiB;
      const stop=code=>{failure??=new OwnerError(code);if(!identity.done){child.kill('SIGTERM');killTimer??=setTimeout(()=>{if(!identity.done)child.kill('SIGKILL');forceTimer=setTimeout(()=>{if(!identity.done)reject(new OwnerError('OWNER_CHILD_STOP_UNCONFIRMED'));},1000);},1000);}};
      const abort=()=>stop('OWNER_CANCELLED'),timer=setTimeout(()=>stop('OWNER_COMMAND_DEADLINE'),Math.max(0,Math.floor(end-performance.now())));
      if(!cleanup)this.owner.signal.addEventListener('abort',abort,{once:true});
      const capture=(target,chunk)=>{bytes+=chunk.length;if(bytes>maximum){stop('OWNER_OUTPUT_LIMIT');return;}if(!spec.secret)target.push(chunk);};
      child.stdout.on('data',c=>capture(out,c));child.stderr.on('data',c=>capture(err,c));
      child.stdin.on('error',()=>stop('OWNER_STDIN'));child.on('error',()=>stop('OWNER_SPAWN'));
      child.on('close',code=>{
        identity.done=true;this.children.delete(identity);clearTimeout(timer);clearTimeout(killTimer);clearTimeout(forceTimer);this.owner.signal.removeEventListener('abort',abort);
        this.owner.addEvent({action,exitCode:code,outputBytes:bytes,secret:spec.secret});
        if(failure||code!==0)reject(failure??new OwnerError('OWNER_COMMAND_FAILED'));
        else resolve({stdout:Buffer.concat(out).toString('utf8'),stderr:Buffer.concat(err).toString('utf8')});
      });
      child.stdin.end(spec.stdin??undefined);if(this.owner.signal.aborted&&!cleanup)abort();
    }));
  }
  stop(){for(const id of this.children)if(!id.done)id.child.kill('SIGTERM');}
}
async function fileHash(p){const h=createHash('sha256');for await(const chunk of createReadStream(p))h.update(chunk);return h.digest('hex');}
async function privateFile(p,bytes,{exclusive=false,beforeWrite=()=>{}}={}){
  beforeWrite();const handle=await fs.open(p,FC.O_WRONLY|FC.O_CREAT|FC.O_NOFOLLOW|(exclusive?FC.O_EXCL:FC.O_TRUNC),0o600);
  try{beforeWrite();await handle.writeFile(bytes);beforeWrite();await handle.sync();}finally{await handle.close();}
}
export async function regular(p){const s=await fs.lstat(p);if(!s.isFile()||s.isSymbolicLink())refuse('OWNER_REGULAR_FILE');return s;}
// Installed aliases may be Homebrew symlinks; immutable caches may not.
export async function resolveExecutable(alias,pin) {
  const executable=await fs.realpath(alias),s=await regular(executable);
  if(!pin||pin.canonicalPath!==executable||pin.bytes!==s.size||pin.sha256!==await fileHash(executable)||(s.mode&0o111)===0)refuse('OWNER_TOOL_SOURCE_PIN');
  return executable;
}
async function jsonFile(p){const s=await regular(p);if(s.size>2*MiB)refuse('OWNER_JSON_SIZE');return JSON.parse(await fs.readFile(p,'utf8'));}
const safeCode=e=>e instanceof OwnerError?e.code:'OWNER_OPERATION_FAILED';
export class OwnerSupervisor {
  events=[];budget=new ConnectionBudget();owner=null;phase='new';proxy=null;
  #controller=new AbortController();#setupDeadline=null;#suiteTimer=null;#suiteDeadline=null;#secrets=null;#logDevice=null;#guestIdentity=null;#defaultState=null;#plan;#manifest;#commands;#bindingsFingerprint=null;#quotaProof=null;#executables={};
  constructor({plan,manifest,authorize}){this.#plan=plan;this.#manifest=manifest;this.gate=new PhaseGate({runId:plan.runId,authorize});this.#commands=new Commands(this);}
  get plan(){return this.#plan;}get signal(){return this.#controller.signal;}
  get denied(){return [...this.gate.denied];}
  addEvent(event){if(this.events.length>=2048)this.events.shift();this.events.push(event);}
  #deadline(){const end=this.#suiteDeadline??this.#setupDeadline;if(end===null)refuse('OWNER_SETUP_DEADLINE');return end;}
  #remaining(){const n=Math.floor(this.#deadline()-performance.now());if(n<1)refuse('OWNER_SETUP_DEADLINE');return n;}
  async #run(action,tool,args,options={}){return this.#commands.run(action,command(this.#plan,tool,args,{...options,executable:this.#executables[tool]??null}),{deadline:this.#deadline()});}
  async #hostFree(){const p=this.#plan;const s=await fs.stat('/Volumes/ExternalSSD');if(!s.isDirectory()||await fs.realpath('/Volumes/ExternalSSD')!=='/Volumes/ExternalSSD'||await fs.realpath(p.worktree)!==p.worktree)refuse('OWNER_VOLUME');await fs.access(p.worktree,FC.W_OK);const v=await fs.statfs('/Volumes/ExternalSSD');return integer(v.bavail*v.bsize);}
  async #defaults(){
    const home=process.env.HOME;const paths=[home+'/.docker/config.json',home+'/.colima/default/colima.yaml',home+'/.ssh/config'];const state={};
    for(const p of paths){try{state[p]={sha256:await fileHash(p),realpath:await fs.realpath(p)};}catch(e){if(e.code!=='ENOENT')throw e;state[p]={absent:true};}}
    return state;
  }
  async preflight(){
    assertFixtureEnvironment();
    const deadline=performance.now()+CAPS.setupMs;
    await this.gate.require('preflight');if(this.phase!=='new')refuse('OWNER_PHASE');this.#setupDeadline=deadline;this.#remaining();
    if(process.versions.node!==PIN.node)refuse('OWNER_NODE_PIN');
    const free=await this.#hostFree();residualCapacity(free,100*GiB);
    const m=this.#manifest;if(!m||m.runId!==this.plan.runId)refuse('OWNER_MANIFEST');
    for(const [name,version] of [['colima',PIN.colima],['docker',PIN.docker],['pnpm',PIN.pnpm]]){
      const alias=command(this.plan,name,['--version']).executable;const executable=await resolveExecutable(alias,m.tools?.[name]);this.#executables[name]=executable;
      const r=await this.#run('preflight',name,['--version']);if(!r.stdout.includes(version))refuse('OWNER_TOOL_VERSION');
    }
    const vm=await regular(this.plan.vmImage);if(vm.size!==PIN.vmBytes||await fileHash(this.plan.vmImage)!==PIN.vmSha256)refuse('OWNER_VM_PIN');
    const layout=this.plan.ociLayout;if(!layout)refuse('OWNER_OCI_CACHE');
    const manifest=await jsonFile(layout+'/blobs/sha256/'+PIN.imageDigest.slice(7));
    if(await fileHash(layout+'/blobs/sha256/'+PIN.imageDigest.slice(7))!==PIN.imageDigest.slice(7)||manifest.config.digest!==PIN.configDigest)refuse('OWNER_OCI_MANIFEST');
    const config=await jsonFile(layout+'/blobs/sha256/'+PIN.configDigest.slice(7));
    if(await fileHash(layout+'/blobs/sha256/'+PIN.configDigest.slice(7))!==PIN.configDigest.slice(7)||config.os!=='linux'||config.architecture!=='arm64')refuse('OWNER_OCI_PLATFORM');
    let compressed=0;for(const layer of manifest.layers){if(!/^sha256:[0-9a-f]{64}$/.test(layer.digest))refuse('OWNER_OCI_LAYER');const p=layout+'/blobs/sha256/'+layer.digest.slice(7),s=await regular(p);if(s.size!==layer.size||await fileHash(p)!==layer.digest.slice(7))refuse('OWNER_OCI_LAYER');compressed+=s.size;this.#remaining();}
    if(compressed!==156463634||manifest.layers.length!==14)refuse('OWNER_OCI_SIZE');
    // Docker support is independently demonstrated for a supplied cache archive.
    // No implicit OCI conversion/load/pull and no startup-triggered download.
    if(!m.imageImport||m.imageImport.protocol!=='REVIEWED_DOCKER_LOAD_ARCHIVE'||m.imageImport.imageDigest!==PIN.imageDigest||!this.plan.importArchive||m.imageImport.sha256!==await fileHash(this.plan.importArchive))refuse('OWNER_OCI_IMPORT_UNVERIFIED');
    if((await regular(this.plan.importArchive)).size>8*GiB)refuse('OWNER_IMAGE_ARCHIVE_LIMIT');
    // A source-bound exhaustive sink contract is an external prerequisite. A
    // boolean supplied by the candidate cannot substitute for its review receipt.
    if(!m.logRouting?.sourceReceipt||!m.logRouting.routes?.length||m.logRouting.vmImageSha256!==PIN.vmSha256||m.logRouting.guestBootPolicy!=='NO_PERSISTENT_LOG_SINKS_BEFORE_OWNED_SETUP')refuse('OWNER_LOG_ROUTING_UNVERIFIED');
    const proof=await jsonFile(m.logRouting.sourceReceipt.path);if(await fileHash(m.logRouting.sourceReceipt.path)!==m.logRouting.sourceReceipt.sha256||proof.status!=='EXHAUSTIVE_LOG_SINKS_REVIEWED'||proof.runId!==this.plan.runId||proof.vmImageSha256!==PIN.vmSha256||JSON.stringify(proof.routes)!==JSON.stringify(m.logRouting.routes))refuse('OWNER_LOG_SOURCE_RECEIPT');
    this.#defaultState=await this.#defaults();this.phase='preflight';return {status:'PREFLIGHT_INPUTS_VERIFIED_RUNTIME_NOT_STARTED',hostFree:free,compressedLayerBytes:compressed};
  }
  async #initRoots(){
    const deadline=this.#setupDeadline,check=()=>{if(this.signal.aborted)refuse('OWNER_CANCELLED');if(deadline===null||performance.now()>=deadline)refuse('OWNER_SETUP_DEADLINE');};
    await this.gate.require('create-owned-root');check();const p=this.plan;
    for(const dir of [p.artifactRoot,path.dirname(p.colimaHome)]){
      try{await fs.lstat(dir);check();refuse('OWNER_EXISTING_ROOT');}catch(e){if(e.code!=='ENOENT')throw e;}
      check();await fs.mkdir(dir,{mode:0o700});check();
    }
    for(const dir of [p.colimaHome,p.dockerConfig,p.results]){check();await fs.mkdir(dir,{mode:0o700});check();}
    this.#secrets=Object.fromEntries(['b1_bootstrap','b1_migrator','b1_app'].map(role=>[role,randomBytes(32).toString('hex')]));
    await privateFile(p.artifactRoot+'/database-secrets.json',JSON.stringify(this.#secrets),{exclusive:true,beforeWrite:check});
    await privateFile(p.artifactRoot+'/identity.json',JSON.stringify({runId:p.runId,uid:p.uid,colimaHome:p.colimaHome,profile:p.profile,socket:p.socket,containerName:p.containerName,networkName:p.networkName}),{exclusive:true,beforeWrite:check});check();
  }
  async #logVolume(){
    const p=this.plan;
    await this.#run('hdiutil-create','hdiutil',['create','-size','128m','-layout','NONE','-fs','HFS+','-volname','B1-'+p.runId,'-type','UDRW',p.logImage]);
    if((await regular(p.logImage)).size>CAPS.results)refuse('OWNER_LOG_IMAGE_LIMIT');
    await this.#run('hdiutil-attach','hdiutil',['attach','-nobrowse','-noautoopen','-mountpoint',p.results,p.logImage]);
    const stat=await fs.stat(p.results),parent=await fs.stat(p.artifactRoot),v=await fs.statfs(p.results);
    if(stat.dev===parent.dev||v.blocks*v.bsize>CAPS.results)refuse('OWNER_LOG_FILESYSTEM_CAP');this.#logDevice=stat.dev;
    const routes=this.#manifest.logRouting.routes;
    for(const r of routes){
      if(typeof r.path!=='string'||!r.path.startsWith(p.colimaHome+'/')||path.resolve(r.path)!==r.path||typeof r.sink!=='string'||!r.sink.startsWith(p.results+'/')||path.resolve(r.sink)!==r.sink||!['file','directory'].includes(r.kind))refuse('OWNER_LOG_ROUTE_PATH');
      await fs.mkdir(path.dirname(r.path),{recursive:true,mode:0o700});await fs.mkdir(path.dirname(r.sink),{recursive:true,mode:0o700});
      if(r.kind==='directory')await fs.mkdir(r.sink,{mode:0o700});else await privateFile(r.sink,'',{exclusive:true});
      await fs.symlink(r.sink,r.path);if(!(await fs.lstat(r.path)).isSymbolicLink()||await fs.realpath(r.path)!==r.sink||(await fs.stat(r.path)).dev!==this.#logDevice)refuse('OWNER_LOG_ROUTE');
    }
    await this.#assertLogRoutes();
  }
  async #assertLogRoutes(){if(this.#logDevice===null)refuse('OWNER_LOG_NOT_MOUNTED');for(const r of this.#manifest.logRouting.routes){if(!(await fs.lstat(r.path)).isSymbolicLink()||await fs.realpath(r.path)!==r.sink||(await fs.stat(r.path)).dev!==this.#logDevice)refuse('OWNER_LOG_ROUTE_CHANGED');}const v=await fs.statfs(this.plan.results);if(v.blocks*v.bsize>CAPS.results)refuse('OWNER_LOG_FILESYSTEM_CAP');}
  async record(name,measurement){
    if(!/^[a-z0-9-]{1,80}$/.test(name))refuse('OWNER_RECEIPT_NAME');await this.#assertLogRoutes();
    const value={at:new Date().toISOString(),runId:this.plan.runId,measurement};const out=JSON.stringify(value)+'\n';if(Buffer.byteLength(out)>MiB)refuse('OWNER_RECEIPT_LIMIT');
    await privateFile(this.plan.results+'/'+name+'.json',out,{exclusive:true});return this.plan.results+'/'+name+'.json';
  }
  async #guest(action,args=[],stdin=null){
    const source=await fs.readFile(new URL('./guest.sh',import.meta.url),'utf8');
    if(this.#manifest.sourceSha256?.['guest.sh']!==digest(source))refuse('OWNER_GUEST_SOURCE_CHANGED');
    // No host sudo. Script and arguments execute inside the exact profile only.
    const r=await this.#run('guest-'+action,'colima',['ssh','--','sudo','-n','bash','-c',source,'b1-owner',this.plan.runId,action,...args],{stdin,secret:action==='secrets'});
    if(action==='secrets')return;return JSON.parse(r.stdout);
  }
  async #dockerJson(action,args){const r=await this.#run(action,'docker',args);return JSON.parse(r.stdout);}
  async provision(){
    await this.gate.require('provision');if(this.phase!=='preflight')refuse('OWNER_PHASE');await this.#initRoots();this.phase='allocating';await this.#logVolume();
    // Routing exists and is measured before the first VM command.
    await this.#assertLogRoutes();await this.#run('colima-start','colima',colimaStartArgs(this.plan),{timeoutMs:600000});
    const vm=await this.#dockerJsonColima();await this.#validateVm(vm);await this.#guest('preflight');await this.#guest('prepare');
    await this.#run('docker-image-load','docker',['image','load','--input',this.plan.importArchive],{timeoutMs:300000});
    const [image]=await this.#dockerJson('docker-image-inspect',['image','inspect',PIN.image]);
    if(image.Os!=='linux'||image.Architecture!=='arm64'||!image.RepoDigests?.some(s=>s.endsWith('@'+PIN.imageDigest)))refuse('OWNER_LOADED_IMAGE');
    const net=await this.#run('docker-network-create','docker',['network','create','--internal','--label','b1.owner='+this.plan.runId,this.plan.networkName]);const networkId=net.stdout.trim();
    if(!/^[0-9a-f]{64}$/.test(networkId))refuse('OWNER_NETWORK_ID');
    // Bind sources must exist before Docker validates the create request.
    const result=await createContainerWithSecret({writeSecret:()=>this.#guest('secrets',[],Object.values(this.#secrets).join('\n')+'\n'),create:()=>this.#run('docker-create','docker',containerArgs(this.plan,networkId))});const containerId=result.stdout.trim();if(!/^[0-9a-f]{64}$/.test(containerId))refuse('OWNER_CONTAINER_ID');
    // Capture identities immediately so cancellation can stop only these IDs.
    this.owner=Object.freeze({runId:this.plan.runId,uid:this.plan.uid,artifactRoot:this.plan.artifactRoot,
      containerId,networkId,imageDigest:PIN.imageDigest,platform:PIN.platform,host:'127.0.0.1',port:0});
    await privateFile(this.plan.artifactRoot+'/resource-identity.json',JSON.stringify(this.owner),{exclusive:true});
    await this.#run('docker-cp-passwd','docker',['cp',containerId+':/etc/passwd',this.plan.results+'/container-passwd']);
    const passwd=await fs.readFile(this.plan.results+'/container-passwd','utf8'),line=passwd.split('\n').find(s=>s.startsWith('postgres:'));if(!line)refuse('OWNER_POSTGRES_UID');
    const [,,pgUid,pgGid]=line.split(':');integer(Number(pgUid),1);integer(Number(pgGid),1);
    this.#quotaProof=await this.#guest('quota-test',[pgUid,pgGid]);if(this.#quotaProof.enospcObserved!==true||this.#quotaProof.furtherAllocationFailed!==true||this.#quotaProof.uid!==Number(pgUid)||this.#quotaProof.gid!==Number(pgGid))refuse('OWNER_ENOSPC_NOT_PROVEN');await this.record('durable-cap-negative-write',this.#quotaProof);
    this.#guestIdentity=await this.#guest('inspect');await this.#guest('sentinel');
    await this.#run('docker-start','docker',['start',containerId]);this.phase='provisioned';
    const observed=await this.#resources();this.backendPort=observed.backendPort;
    await this.#forwarder(this.backendPort);await this.#roles();
    assertFixtureEnvironment();this.proxy=new WireProxy({backendPort:this.backendPort,authorize:a=>this.gate.require(a),onFailure:e=>{this.addEvent({wireFailure:safeCode(e)});}});
    const front=await this.proxy.start({deadline:this.#deadline(),signal:this.signal});this.owner=Object.freeze({...this.owner,port:front});
    await privateFile(this.plan.artifactRoot+'/resource-identity.json',JSON.stringify(this.owner));
    await this.verifyOwner(this.owner,'connect-owned-fixture');return this.owner;
  }
  async #dockerJsonColima(){const r=await this.#run('colima-status','colima',['status','--json']);return JSON.parse(r.stdout);}
  async #validateVm(vm){
    // Version-bound observation schema is supplied by the independent collector.
    // Colima status alone is insufficient: generated config and guest facts agree.
    const config=await fs.readFile(this.plan.colimaHome+'/b1-owner/colima.yaml','utf8');
    const r=await this.#guest('preflight');
    if(r.cpu!==2||r.memoryBytes>CAPS.vmMemory||r.memoryBytes<2*GiB||r.swapBytes!==0||r.hostMounts.length!==0||r.rootFilesystemBytes>CAPS.rootDisk||r.rootFilesystemBytes<11*GiB||r.dockerFilesystemBytes>CAPS.vmDataDisk||r.dockerFilesystemBytes<15*GiB||r.rootDevice===r.dockerDevice)refuse('OWNER_VM_ISOLATION');
    const fields=this.#manifest.vmConfigFields;if(!fields)refuse('OWNER_VM_CONFIG_SCHEMA');
    for(const [kind,expected] of [['cpu',2],['memory',3],['rootDisk',12],['dataDisk',16]]){const field=fields[kind];if(typeof field!=='string'||!/^[A-Za-z][A-Za-z0-9]*$/.test(field)||!new RegExp('^'+field+':\\s*'+expected+'\\s*$','m').test(config))refuse('OWNER_VM_CONFIG_CAP');}
    const schema=this.#manifest.vmStatusFields;if(!schema||vm[schema.profile]!==this.plan.profile||vm[schema.runtime]!=='docker'||vm[schema.arch]!=='aarch64'||vm[schema.vmType]!=='vz')refuse('OWNER_VM_STATUS_SCHEMA');
    // No host mount and no change to ambient default configuration.
    if(!/mounts:\s*\[\]/.test(config)||JSON.stringify(await this.#defaults())!==JSON.stringify(this.#defaultState))refuse('OWNER_DEFAULT_OR_MOUNTS_CHANGED');
    await this.record('vm-isolation',{status:vm,guest:r,configurationSha256:digest(config)});
  }
  async #resources(){
    if(!this.owner)refuse('OWNER_RESOURCE_ID_REQUIRED');await this.#assertLogRoutes();
    const p=this.plan,o=this.owner,[c]=await this.#dockerJson('docker-inspect',['inspect',o.containerId]);
    const [n]=await this.#dockerJson('docker-network-inspect',['network','inspect',o.networkId]);
    if(c.Id!==o.containerId||c.Name!=='/'+p.containerName||c.Config?.Labels?.['b1.owner']!==p.runId||n.Id!==o.networkId||n.Name!==p.networkName||n.Labels?.['b1.owner']!==p.runId||n.Internal!==true)refuse('OWNER_RESOURCE_IDENTITY');
    const h=c.HostConfig;if(h.NanoCpus!==2e9||h.Memory!==GiB||h.MemorySwap!==GiB||h.ShmSize!==CAPS.shm||h.PidsLimit!==128||h.ReadonlyRootfs!==true||h.LogConfig?.Type!=='none'||h.Privileged===true||h.NetworkMode!==o.networkId)refuse('OWNER_CONTAINER_CAPS');
    if(c.Config.Image!==PIN.image||c.Platform!=='linux'||c.State?.Running!==true)refuse('OWNER_CONTAINER_STATE');
    const data=c.Mounts?.find(m=>m.Destination==='/var/lib/postgresql/data');
    const secret=c.Mounts?.find(m=>m.Destination==='/run/secrets/bootstrap');
    if(c.Mounts.length!==2||data?.Type!=='bind'||data.Source!==p.guestRoot+'/data'||data.RW!==true||secret?.Source!==p.guestRoot+'/secrets/bootstrap'||secret.RW!==false||!c.Config.Env.includes('PGDATA=/var/lib/postgresql/data/pgdata'))refuse('OWNER_DATA_MOUNT');
    const publication=c.NetworkSettings.Ports?.['5432/tcp'];if(!publication||publication.length!==1||publication[0].HostIp!=='127.0.0.1')refuse('OWNER_PUBLICATION');
    const backendPort=Number(publication[0].HostPort);integer(backendPort,1,65535);
    if(Object.keys(n.Containers??{}).some(id=>id!==o.containerId))refuse('OWNER_FOREIGN_NETWORK_MEMBER');
    const socket=await fs.lstat(p.socket);if(!socket.isSocket()||socket.uid!==p.uid)refuse('OWNER_SOCKET_IDENTITY');
    const guest=await this.#guest('inspect');if(guest.quotaBytes!==CAPS.data||guest.blockSize!==4096||guest.blockCount!==524288||guest.mountType!=='ext4'||guest.backingFile!==p.guestRoot+'/data.ext4'||guest.swapBytes!==0||guest.secretModes!==true||guest.enospcProofExists!==true)refuse('OWNER_GUEST_CAP_OR_MODES');
    if(this.#guestIdentity&&['filesystemUuid','backingInode','loopDevice','mountDevice'].some(k=>guest[k]!==this.#guestIdentity[k]))refuse('OWNER_DATA_IDENTITY_CHANGED');
    const remaining=residualCapacity(await this.#hostFree(),guest.capacityParentFreeBytes,{hostReserve:GiB,guestReserve:0});
    return {backendPort,guest,remaining,container:{id:c.Id,image:c.Image,caps:{cpu:h.NanoCpus,memory:h.Memory,swap:h.MemorySwap,pids:h.PidsLimit,shm:h.ShmSize},mounts:c.Mounts,publication},network:{id:n.Id,internal:n.Internal},socket:{dev:socket.dev,ino:socket.ino,uid:socket.uid}};
  }
  async #forwarder(port){
    const r=await this.#run('verify-forwarder','lsof',['-nP','-iTCP:'+port,'-sTCP:LISTEN','-FpuLn']);
    const lines=r.stdout.trim().split('\n'),pids=lines.filter(s=>/^p[0-9]+$/.test(s)).map(s=>Number(s.slice(1)));
    if(pids.length!==1||lines.filter(s=>s.startsWith('n')).some(s=>s!=='n127.0.0.1:'+port))refuse('OWNER_FORWARDER_BIND');
    const identity=await this.#run('verify-forwarder','ps',['-p',String(pids[0]),'-o','uid=,lstart=,command=']);
    const raw=identity.stdout.trim();if(!raw.startsWith(String(this.plan.uid)+' ')||!raw.includes(this.plan.colimaHome))refuse('OWNER_FORWARDER_IDENTITY');
    return {pid:pids[0],birthAndCommandHash:digest(raw),bind:'127.0.0.1:'+port};
  }
  connections({backend=false}={}){
    assertFixtureEnvironment();
    if(!this.owner||!this.#secrets)refuse('OWNER_NOT_PROVISIONED');const port=backend?this.backendPort:this.owner.port;integer(port,1,65535);
    return Object.fromEntries(Object.entries(this.#secrets).map(([role,password])=>[role,{host:'127.0.0.1',port,database:'b1',user:role,password}]));
  }
  async withClient(role,callback,{backend=true}={}){
    const phaseEnd=this.#deadline();
    await this.gate.require('inspect-owned-fixture');if(this.signal.aborted)refuse('OWNER_CANCELLED');remainingMilliseconds(phaseEnd);
    const release=this.budget.reserve('helper',2);let pool,client;
    try{
      pool=createOwnedPool(this.connections({backend})[role]);pool.on('error',()=>{});
      const connectEnd=Math.min(phaseEnd,performance.now()+1500);
      client=await ownedRace(()=>{if(this.signal.aborted)refuse('OWNER_CANCELLED');remainingMilliseconds(connectEnd,1500);return pool.connect();},
        {signal:this.signal,deadline:connectEnd,onLate:c=>{c.connection?.stream?.destroy();c.release(true);}});
      client.on('error',()=>{});let active=true;const deadline=Math.min(phaseEnd,performance.now()+5000),raw=client;
      const check=()=>{if(!active||this.signal.aborted||performance.now()>=deadline)refuse('OWNER_CLIENT_SCOPE_ENDED');};
      const guarded={query:(...args)=>{check();return raw.query(...args);}};
      try{return await ownedRace(()=>{check();return callback(guarded);},{signal:this.signal,deadline});}finally{active=false;}
    }finally{
      try{client?.connection?.stream?.destroy();client?.release(true);if(pool)await closeOwnedPool(pool,1000);}finally{release();}
    }
  }
  async #roles(){
    await this.gate.require('provision-roles');
    // Readiness is bounded. Only the verified loopback forwarder is contacted.
    const deadline=performance.now()+30000;let ready=false;
    while(performance.now()<deadline){try{await this.withClient('b1_bootstrap',c=>c.query('SELECT 1'));ready=true;break;}catch{await new Promise(r=>setTimeout(r,50));if(this.signal.aborted)refuse('OWNER_CANCELLED');}}
    if(!ready)refuse('OWNER_DATABASE_NOT_READY');
    await this.withClient('b1_bootstrap',async c=>{
      const s=this.#secrets;if(Object.values(s).some(p=>!/^[0-9a-f]{64}$/.test(p)))refuse('OWNER_SECRET_FORMAT');
      // Password literals exist only in this private protocol buffer, never argv,
      // traces or server statement logs. Fixed roles prevent SQL interpolation.
      await c.query("CREATE ROLE b1_migrator LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION PASSWORD '"+s.b1_migrator+"'");
      await c.query("CREATE ROLE b1_app LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION PASSWORD '"+s.b1_app+"'");
      await c.query('REVOKE ALL ON DATABASE b1 FROM PUBLIC');await c.query('GRANT CONNECT ON DATABASE b1 TO b1_migrator,b1_app');await c.query('GRANT CREATE ON DATABASE b1 TO b1_migrator');await c.query('REVOKE ALL ON SCHEMA public FROM PUBLIC');
    });
  }
  async #roleFacts(){
    return this.withClient('b1_bootstrap',async c=>{
      const {rows}=await c.query("SELECT rolname,rolsuper,rolcreatedb,rolcreaterole,rolreplication,has_database_privilege(rolname,'b1','CREATE') AS ddl,has_database_privilege(rolname,'b1','TEMP') AS temp,has_database_privilege(rolname,'b1','CONNECT') AS connect FROM pg_roles WHERE rolname IN ('b1_bootstrap','b1_migrator','b1_app') ORDER BY rolname");
      if(rows.length!==3)refuse('OWNER_ROLE_COUNT');for(const r of rows)if(r.rolname!=='b1_bootstrap'&&(r.rolsuper||r.rolcreatedb||r.rolcreaterole||r.rolreplication||r.temp||!r.connect||r.ddl!==(r.rolname==='b1_migrator')))refuse('OWNER_ROLE_GRANTS');
      if(rows.find(r=>r.rolname==='b1_bootstrap').rolsuper!==true)refuse('OWNER_BOOTSTRAP_ROLE');
      const {rows:[settings]}=await c.query("SELECT current_setting('server_version_num') AS version,current_setting('max_connections') AS max_connections,current_setting('fsync') AS fsync,current_setting('synchronous_commit') AS synchronous_commit,current_setting('full_page_writes') AS full_page_writes,current_setting('deadlock_timeout') AS deadlock_timeout,current_setting('data_directory') AS data_directory,(SELECT system_identifier::text FROM pg_control_system()) AS system_identifier,to_regnamespace('provider_b1')::text AS schema");
      if(settings.version!==PIN.serverVersion||settings.max_connections!=='12'||['fsync','synchronous_commit','full_page_writes'].some(k=>settings[k]!=='on')||settings.deadlock_timeout!=='20ms'||settings.data_directory!=='/var/lib/postgresql/data/pgdata')refuse('OWNER_PG_SETTINGS');
      const {rows:escapes}=await c.query("SELECT spcname,pg_tablespace_location(oid) AS location FROM pg_tablespace WHERE pg_tablespace_location(oid)<>''");if(escapes.length)refuse('OWNER_TABLESPACE_ESCAPE');
      let catalog=null;
      if(settings.schema){
        const {rows:functions}=await c.query("SELECT p.proname,p.prosecdef,p.proconfig,r.rolname,pg_get_functiondef(p.oid) AS definition,has_function_privilege('b1_app',p.oid,'EXECUTE') AS app_execute FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace JOIN pg_roles r ON r.oid=p.proowner WHERE n.nspname='provider_b1' AND p.proname IN ('clock_begin','clock_finish','clock_try_lock','clock_unlock','clock_key','clock_owned','clock_assert_owned','seed_clock') ORDER BY p.proname");
        if(functions.length!==8||functions.some(f=>!f.prosecdef||f.rolname!=='b1_migrator'||JSON.stringify(f.proconfig)!=='["search_path=pg_catalog"]'||f.app_execute!==['clock_begin','clock_finish','clock_try_lock','clock_unlock'].includes(f.proname)))refuse('OWNER_CLOCK_CATALOG');
        const {rows:[grants]}=await c.query("SELECT has_schema_privilege('b1_app','provider_b1','CREATE') AS schema_ddl,has_table_privilege('b1_app','provider_b1.domain_clocks','UPDATE') AS clock_update,has_table_privilege('b1_app','provider_b1.domain_clocks','INSERT') AS clock_insert");if(Object.values(grants).some(Boolean))refuse('OWNER_APP_DDL_CLOCK_GRANTS');
        catalog={functions:functions.map(({definition,...f})=>({...f,definitionSha256:digest(definition)})),grants};
      }
      return {roles:rows,settings,catalog};
    });
  }
  async verifyOwner(candidate,action){
    await this.gate.require(action);if(!this.owner||candidate!==this.owner)refuse('OWNER_CANDIDATE_IDENTITY');
    const measured=await this.#resources();if(measured.backendPort!==this.backendPort)refuse('OWNER_BACKEND_PORT_CHANGED');const forwarder=await this.#forwarder(measured.backendPort),roles=await this.#roleFacts();
    if(action==='create-fresh-schema'&&roles.settings.schema!==null)refuse('OWNER_SCHEMA_NOT_FRESH');
    if(action==='verify-health'&&!roles.catalog)refuse('OWNER_SCHEMA_NOT_MIGRATED');
    const files=['database-secrets.json','identity.json','resource-identity.json'];
    for(const f of files){const s=await regular(this.plan.artifactRoot+'/'+f);if(s.uid!==this.plan.uid||(s.mode&0o777)!==0o600)refuse('OWNER_SECRET_MODE');}
    for(const d of [this.plan.artifactRoot,this.plan.dockerConfig]){const s=await fs.lstat(d);if(!s.isDirectory()||s.isSymbolicLink()||(s.mode&0o777)!==0o700)refuse('OWNER_PRIVATE_DIRECTORY');}
    if(JSON.stringify(await this.#defaults())!==JSON.stringify(this.#defaultState))refuse('OWNER_DEFAULT_CHANGED');
    const rawRef=await this.record('admission-'+randomBytes(8).toString('hex'),{resources:measured,forwarder,roles,rolePhase:roles.catalog?'migrated-catalog':'provisioned-database'});
    return {...this.owner,storageFreeBytes:measured.remaining,dataQuotaBytes:measured.guest.quotaBytes,connectionLimit:Number(roles.settings.max_connections),cpuLimit:2,memoryLimitBytes:GiB,shmBytes:CAPS.shm,wallBudgetSeconds:1800,resultsQuotaBytes:CAPS.results,durabilityMountVerified:true,loopbackPublicationVerified:true,roleGrantsVerified:true,secretFileModesVerified:true,rolePhase:roles.catalog?'migrated-catalog':'provisioned-database',rawMeasurement:rawRef};
  }
  beginSql(){
    if(this.#suiteTimer)refuse('OWNER_SUITE_ALREADY_STARTED');if(this.signal.aborted)refuse('OWNER_CANCELLED');
    const now=performance.now();if(this.#setupDeadline===null||now>=this.#setupDeadline)refuse('OWNER_SETUP_DEADLINE');
    this.#suiteDeadline=now+CAPS.suiteMs;this.#suiteTimer=setTimeout(()=>this.cancel('OWNER_SUITE_DEADLINE'),CAPS.suiteMs);
  }
  setBindingsFingerprint(fingerprint){if(!/^[0-9a-f]{64}$/.test(fingerprint)||this.#bindingsFingerprint&&this.#bindingsFingerprint!==fingerprint)refuse('OWNER_SIGNING_IDENTITY_CHANGED');this.#bindingsFingerprint=fingerprint;}
  async #durableSnapshot(){return this.withClient('b1_bootstrap',async c=>{
    const {rows:[control]}=await c.query('SELECT system_identifier::text FROM pg_control_system()');const {rows:[schema]}=await c.query("SELECT to_regnamespace('provider_b1')::text AS schema");const hashes={};
    if(schema.schema)for(const table of ['domains','domain_clocks','installations','pools','profiles','credentials','challenges','operations']){const {rows}=await c.query('SELECT to_jsonb(t)::text AS value FROM provider_b1.'+table+' t ORDER BY to_jsonb(t)::text');hashes[table]=digest(JSON.stringify(rows.map(r=>r.value)));}
    return {systemIdentifier:control.system_identifier,rowHashes:hashes,signingFingerprint:this.#bindingsFingerprint};
  });}
  async restartDatabase(candidate,kind){
    if(candidate!==this.owner||!['clean','crash'].includes(kind))refuse('OWNER_RESTART_KIND');await this.verifyOwner(candidate,'restart-owned-database');
    if(this.budget.state.used!==0)refuse('OWNER_ACTIVE_CLIENTS');const before=await this.#durableSnapshot();await this.#guest('verify-sentinel');
    await this.#run(kind==='clean'?'docker-stop':'docker-kill','docker',kind==='clean'?['stop','--time','5',this.owner.containerId]:['kill','--signal','KILL',this.owner.containerId]);
    await this.#run('docker-start','docker',['start',this.owner.containerId]);
    const after=await this.#waitSnapshot();if(JSON.stringify(before)!==JSON.stringify(after))refuse('OWNER_RESTART_DURABILITY');await this.#guest('verify-sentinel');await this.verifyOwner(this.owner,'verify-health');
    return this.record('restart-'+randomBytes(6).toString('hex'),{kind,before,after});
  }
  async #waitSnapshot(){const end=performance.now()+30000;while(performance.now()<end){try{return await this.#durableSnapshot();}catch{if(this.signal.aborted)refuse('OWNER_CANCELLED');await new Promise(r=>setTimeout(r,50));}}refuse('OWNER_RESTART_NOT_READY');}
  async restartVm(){
    await this.verifyOwner(this.owner,'restart-owned-vm');if(this.budget.state.used!==0)refuse('OWNER_ACTIVE_CLIENTS');const before=await this.#durableSnapshot();
    await this.#run('colima-stop','colima',['stop']);await this.#run('colima-start','colima',colimaStartArgs(this.plan),{timeoutMs:600000});
    await this.#guest('prepare');await this.#run('docker-start','docker',['start',this.owner.containerId]);const after=await this.#waitSnapshot();
    if(JSON.stringify(before)!==JSON.stringify(after))refuse('OWNER_VM_RESTART_DURABILITY');await this.#guest('verify-sentinel');return this.record('vm-restart-'+randomBytes(6).toString('hex'),{before,after});
  }
  cancel(code='OWNER_CANCELLED'){this.#controller.abort(new OwnerError(code));this.#commands.stop();}
  async cleanup({deadline=performance.now()+CAPS.cleanupMs}={}){
    const end=Math.min(deadline,performance.now()+CAPS.cleanupMs);
    clearTimeout(this.#suiteTimer);const failures=[];let cleanupCode,authorized=false;
    // Terminal local retirement cannot depend on an external phase capability.
    try{this.cancel();}catch(e){failures.push({action:'cancel',code:safeCode(e)});}
    try{this.budget.close();}catch(e){failures.push({action:'budget-close',code:safeCode(e)});}
    if(this.proxy){
      const now=performance.now(),closeEnd=Number.isFinite(end)?Math.min(end,now+1000):now;
      try{
        // Call close even with zero wait budget: it retires the listener and
        // transports synchronously before its bounded completion wait.
        const closing=Promise.resolve(this.proxy.close(Math.max(0,Math.floor(closeEnd-now))));closing.catch(()=>{});
        await ownedRace(()=>closing,{deadline:closeEnd});
      }catch(e){failures.push({action:'proxy-close',code:safeCode(e)});}
    }
    try{remainingMilliseconds(end);await ownedRace(()=>this.gate.require('cleanup'),{deadline:end});remainingMilliseconds(end);authorized=true;}
    catch(e){cleanupCode=safeCode(e);failures.push({action:'cleanup',code:cleanupCode});}
    const attempt=async(action,tool,args)=>{
      try{await this.#commands.run(action,command(this.plan,tool,args,{executable:this.#executables[tool]??null,timeoutMs:10000}),{deadline:end,cleanup:true});}
      catch(e){failures.push({action,code:safeCode(e)});}
    };
    if(authorized&&this.owner)await attempt('docker-stop','docker',['stop','--time','5',this.owner.containerId]);
    if(authorized&&this.phase!=='new'&&this.phase!=='preflight')await attempt('colima-stop','colima',['stop']);
    let confirmed=false;
    if(authorized&&this.owner)try{
      await this.gate.require('verify-stopped');const r=await this.#commands.run('docker-inspect',command(this.plan,'docker',['inspect',this.owner.containerId],{executable:this.#executables.docker??null}),{deadline:end,cleanup:true});
      confirmed=JSON.parse(r.stdout)[0].State.Running===false;
    }catch{ // A stopped VM makes Docker unavailable; exact profile stopped proof is required.
      try{const r=await this.#commands.run('colima-status',command(this.plan,'colima',['status','--json'],{executable:this.#executables.colima??null}),{deadline:end,cleanup:true});const v=JSON.parse(r.stdout);confirmed=v[this.#manifest.vmStatusFields?.status]==='Stopped';}catch{}
    }
    if(this.#commands.children.size)failures.push({action:'children',code:'OWNER_CHILDREN_REMAIN'});
    if(!confirmed)failures.push({action:'verify-stopped',code:'OWNER_STOP_UNCONFIRMED'});
    const receipt={status:failures.length?'CLEANUP_INCOMPLETE_RETAINED':'STOPPED_RETAINED',failures,denied:this.denied,owner:this.owner,plan:{colimaHome:this.plan.colimaHome,artifactRoot:this.plan.artifactRoot},dataRemoved:false,secretsRetained:true,...(cleanupCode?{code:cleanupCode}:{})};
    // Always retain failure identities; never remove a container/network/profile,
    // detach a foreign volume, or prune. A later same-owner continuation admits it.
    if(this.#logDevice!==null)try{await this.record('cleanup-'+randomBytes(6).toString('hex'),receipt);}catch{receipt.receiptWriteFailed=true;}
    return receipt;
  }
}
// The CLI supplies an external, reviewed phase capability. It cannot be generated
// by a candidate "verified" flag or ambient environment variable.
export async function capabilityAuthorizer(capabilityPath,plan){
  const cap=await jsonFile(capabilityPath);const sourceDir=path.dirname(fileURLToPath(import.meta.url));
  if(cap.status!=='SOURCE_REVIEWED_RUNTIME_PHASE_ADMITTED'||cap.runId!==plan.runId||!Array.isArray(cap.actions)||cap.actions.some(a=>!ACTIONS.includes(a))||!cap.independentReview?.path)refuse('OWNER_RUNTIME_CAPABILITY');
  if(sourceDir!==plan.worktree+'/prototypes/access-provider-service/test/owner'||!cap.reviewedSources?.path||! /^[0-9a-f]{64}$/.test(cap.reviewedSources.sha256??''))sourceFailure();
  const pinnedJson=async(reference,code)=>{
    try{
      if(! /^[0-9a-f]{64}$/.test(reference?.sha256??'')||(await regular(reference.path)).size>2*MiB)refuse(code);
      const raw=await fs.readFile(reference.path);if(raw.length>2*MiB||digest(raw)!==reference.sha256)refuse(code);return JSON.parse(raw.toString('utf8'));
    }catch{refuse(code);}
  };
  const checkReferences=async()=>({receipt:await pinnedJson(cap.independentReview,'OWNER_REVIEW_RECEIPT'),reviewed:await pinnedJson(cap.reviewedSources,'OWNER_REVIEW_SOURCE_CHANGED')});
  const {receipt,reviewed}=await checkReferences();
  if(receipt.decision!=='CLEAR'||receipt.reviewedIdentity?.runtimeSourceManifestSha256!==cap.reviewedSources.sha256)refuse('OWNER_REVIEW_RECEIPT');
  await verifyReviewedSources(plan,reviewed);assertFixtureEnvironment();
  const end=Date.parse(cap.expiresAt);if(!Number.isFinite(end)||end<=Date.now())refuse('OWNER_CAPABILITY_EXPIRED');
  const sourceSha256=Object.fromEntries(reviewed.files.filter(f=>f.path.startsWith('prototypes/access-provider-service/test/owner/')).map(f=>[path.basename(f.path),f.sha256])),actions=new Set(cap.actions);
  return {manifest:{...cap.manifest,sourceSha256},authorize:async({runId,action})=>{
    if(runId!==cap.runId||Date.now()>=end||!actions.has(action))return false;
    await checkReferences();await verifyReviewedSources(plan,reviewed);return Date.now()<end;
  }};
}
