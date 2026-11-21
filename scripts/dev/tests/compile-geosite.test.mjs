import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import test from 'node:test';
import {fileURLToPath} from 'node:url';
import {compileGroup, matchesHost} from '../compile-geosite.mjs';

const psl = [
  '// ===BEGIN ICANN DOMAINS===', 'com', 'org', 'jp', '*.kawasaki.jp',
  '!city.kawasaki.jp', '// ===END ICANN DOMAINS===',
  '// ===BEGIN PRIVATE DOMAINS===', 'tenant.example', '// ===END PRIVATE DOMAINS===',
].join('\n');

function varint(n) {
  const bytes = [];
  while (n >= 128) {
    bytes.push((n % 128) + 128);
    n = Math.floor(n / 128);
  }
  return Buffer.from([...bytes, n]);
}

function bytes(field, value) {
  const data = Buffer.from(value);
  return Buffer.concat([varint(field * 8 + 2), varint(data.length), data]);
}

function domain(type, value, suffix = Buffer.alloc(0)) {
  return bytes(2, Buffer.concat([Buffer.from([8, type]), bytes(2, value), suffix]));
}

function group(name, ...rows) {
  return bytes(1, Buffer.concat([bytes(1, name), ...rows]));
}

test('a complete group preserves exact and DNS-label suffix semantics', () => {
  const data = Buffer.concat([
    group('OTHER', domain(1, 'unused')),
    group('TRIAL', domain(3, 'news.example.org'), domain(2, 'BÜCHER.org.')),
  ]);
  const {rules} = compileGroup(data, psl, 'TRIAL');
  assert.deepEqual(rules, [
    {type: 'domain', value: 'xn--bcher-kva.org'},
    {type: 'full', value: 'news.example.org'},
  ]);
  for (const host of ['BÜCHER.org.', 'a.xn--bcher-kva.org', 'news.example.org']) {
    assert.equal(matchesHost(rules, host), true, host);
  }
  for (const host of ['notxn--bcher-kva.org', 'xn--bcher-kva.org.attacker.org', 'a.news.example.org',
    '.xn--bcher-kva.org', 'xn--bcher-kva.org..', '127.0.0.1', '[::1]', 'localhost']) {
    assert.equal(matchesHost(rules, host), false, host);
  }
});

test('registry boundaries include wildcard and private rules and honor PSL exceptions', () => {
  for (const host of ['org', 'tenant.example', 'x.kawasaki.jp']) {
    assert.throws(() => compileGroup(group('TRIAL', domain(2, 'good.org'), domain(2, host)), psl, 'TRIAL'));
  }
  assert.deepEqual(compileGroup(group('TRIAL', domain(2, 'city.kawasaki.jp')), psl, 'TRIAL').rules,
    [{type: 'domain', value: 'city.kawasaki.jp'}]);
});

test('unsupported, malformed or ambiguous inputs cannot publish a partial group', () => {
  const valid = domain(2, 'good.org');
  for (const bad of [
    group('TRIAL', valid, domain(0, 'word')),
    group('TRIAL', valid, domain(1, 'example.*')),
    group('TRIAL', valid, domain(9, 'unknown.org')),
    group('TRIAL', valid, domain(2, 'attribute.org', bytes(3, bytes(1, 'test')))),
    group('TRIAL', valid, domain(2, '127.0.0.1')),
    group('TRIAL', valid, domain(2, 'device.local')),
    group('TRIAL', valid, domain(2, 'a..example.org')),
    group('TRIAL', valid, domain(2, 'a'.repeat(64) + '.org')),
    Buffer.concat([group('TRIAL', valid), group('TRIAL', domain(2, 'other.org'))]),
    group('TRIAL', valid).subarray(0, -1),
    Buffer.from([0x0a, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02]),
  ]) {
    assert.throws(() => compileGroup(bad, psl, 'TRIAL'));
  }
});

test('unverified CLI inputs preserve an existing catalogue', (t) => {
  const dir = mkdtempSync(join(tmpdir(), 'aegis-geosite-test-'));
  t.after(() => rmSync(dir, {recursive: true, force: true}));
  const dat = join(dir, 'dlc.dat');
  const suffixes = join(dir, 'psl.dat');
  const output = join(dir, 'catalogue.json');
  writeFileSync(dat, group('DEBIAN', domain(2, 'altered.org')));
  writeFileSync(suffixes, psl);
  writeFileSync(output, 'previous\n');
  const cli = fileURLToPath(new URL('../compile-geosite.mjs', import.meta.url));
  const result = spawnSync(process.execPath, [cli, '--dlc', dat, '--psl', suffixes, '--group', 'DEBIAN', '--output', output]);
  assert.notEqual(result.status, 0);
  assert.equal(readFileSync(output, 'utf8'), 'previous\n');
});

test('URL inputs use the actual network authority and valid DNS labels', () => {
  const {rules} = compileGroup(group('TRIAL', domain(2, 'good.org')), psl, 'TRIAL');
  for (const input of ['https://%67ood.org./path', 'wss://食狮.good.org:443/', 'http://other.org@good.org/']) {
    assert.equal(matchesHost(rules, input), true, input);
  }
  for (const input of ['https://good.org@other.org/', 'https://other.org/good.org', 'ftp://good.org/',
    'https:///good.org/', 'https://-bad.good.org/', 'https://_bad.good.org/', 'https://good.org../',
    'good.org/path', '%67ood.org']) {
    assert.equal(matchesHost(rules, input), false, input);
  }
  assert.ok(Object.isFrozen(rules));
  assert.ok(Object.isFrozen(rules[0]));
});

test('network URL matching works without URL.parse', () => {
  const compiler = new URL('../compile-geosite.mjs', import.meta.url);
  const result = spawnSync(process.execPath, ['--input-type=module', '--eval', `
    import assert from 'node:assert/strict';
    import {URL} from 'node:url';

    delete URL.parse;
    assert.equal(typeof URL.parse, 'undefined');
    const {matchesHost} = await import(process.argv[1]);
    const rules = [{type: 'full', value: 'good.org'}];
    assert.equal(matchesHost(rules, 'ws://user@GOOD.org:8080/path'), true);
    assert.equal(matchesHost(rules, 'https://good.org:65536/'), false);
    assert.equal(matchesHost(rules, 'https://%zz.good.org/'), false);
  `, compiler.href], {encoding: 'utf8'});
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stderr);
});

test('domain rules cannot cover a nested registry or wildcard tenants', () => {
  const list = psl.replace('tenant.example', 'tenant.example\ntenants.example.org');
  for (const host of ['kawasaki.jp', 'example.org', '*.good.org', 'appliance.home.arpa']) {
    assert.throws(() => compileGroup(group('TRIAL', domain(2, host)), list, 'TRIAL'));
  }
  assert.deepEqual(compileGroup(group('TRIAL', domain(2, 'person.tenant.example')), psl, 'TRIAL').rules,
    [{type: 'domain', value: 'person.tenant.example'}]);
});

test('decoder rejects invalid fields, invalid UTF-8 and oversized input', () => {
  const valid = domain(2, 'good.org');
  const catalogue = group('TRIAL', valid);
  for (const bad of [
    group('TRIAL', domain(2, 'good.org', Buffer.from([8, 3]))),
    group('TRIAL', domain(2, 'good.org', bytes(2, 'other.org'))),
    group('TRIAL', domain(2, 'good.org', bytes(4, 'unsupported'))),
    group('TRIAL', bytes(2, Buffer.from([10, 1, 2]))),
    group('TRIAL', domain(2, Buffer.from([0xc3, 0x28]))),
    group('TRIAL', domain(2, '\uFEFFgood.org')),
    group('TRIAL', domain(2, '\u00ad'.repeat(1024) + 'good.org')),
    group('TRIAL', ...Array(12_000).fill(valid)),
    group('TRIAL'),
    Buffer.concat([catalogue, Buffer.from([0x0a, 0xff, 0xff, 0xff, 0xff, 0x0f])]),
    Buffer.alloc(5 * 1024 * 1024),
  ]) {
    assert.throws(() => compileGroup(bad, psl, 'TRIAL'));
  }
  assert.throws(() => compileGroup(catalogue, psl + '//'.repeat(300_000), 'TRIAL'), /PSL.*limit/i);
});

test('unbacked PSL exceptions cannot admit a public suffix', () => {
  assert.throws(() => compileGroup(group('TRIAL', domain(2, 'tenant.example')), psl + '\n!tenant.example', 'TRIAL'),
    /invalid PSL exception/i);
});

const pinnedInputs = process.env.AEGIS_GEOSITE_INPUT_DIR;
test('pinned inputs reproduce the DEBIAN catalogue and reject all of WIKIMEDIA',
  {skip: !pinnedInputs && 'set AEGIS_GEOSITE_INPUT_DIR to the two captured pinned files'}, (t) => {
    const dir = mkdtempSync(join(tmpdir(), 'aegis-geosite-pinned-test-'));
    t.after(() => rmSync(dir, {recursive: true, force: true}));
    const dlcPath = join(pinnedInputs, 'dlc.dat');
    const pslPath = join(pinnedInputs, 'effective_tld_names.dat');
    const dlc = readFileSync(dlcPath);
    const suffixes = readFileSync(pslPath, 'utf8');
    assert.throws(() => compileGroup(dlc, suffixes, 'WIKIMEDIA'), /public suffix.*(?:toolforge|wmcloud|wmflabs)\.org/i);
    const cli = fileURLToPath(new URL('../compile-geosite.mjs', import.meta.url));
    const output = join(dir, 'catalogue.json');
    const run = ({dat = dlcPath, list = pslPath, groupName = 'DEBIAN', path = output, extra = []} = {}) =>
      spawnSync(process.execPath, [cli, '--dlc', dat, '--psl', list, '--group', groupName, '--output', path, ...extra],
        {encoding: 'utf8'});
    const success = (result) => assert.equal(result.status, 0, result.stderr);
    const sha256 = (data) => createHash('sha256').update(data).digest('hex');

    success(run());
    const first = readFileSync(output);
    const catalogue = JSON.parse(first);
    assert.equal(catalogue.schemaVersion, 1);
    assert.equal(catalogue.group, 'DEBIAN');
    assert.equal(catalogue.ruleCount, 2);
    assert.deepEqual(catalogue.rules, [{type: 'domain', value: 'debian.org'}, {type: 'domain', value: 'dotdeb.org'}]);
    assert.equal(catalogue.payloadSha256, 'd865d67c829d0f13bf508e152ef402d5d9634227ed12a8b49434d231baf5a643');
    assert.equal(catalogue.payloadSha256, sha256(catalogue.rules.map(({type, value}) => type + ':' + value + '\n').join('')));
    assert.equal(catalogue.inputs.dlc.sha256, sha256(dlc));
    assert.equal(catalogue.inputs.psl.sha256, sha256(suffixes));
    success(run());
    assert.deepEqual(readFileSync(output), first);
    success(run({extra: ['--verify']}));

    const modifiedDlc = join(dir, 'modified.dat');
    const modifiedPsl = join(dir, 'modified-psl.dat');
    dlc[dlc.length - 1] ^= 1;
    writeFileSync(modifiedDlc, dlc);
    writeFileSync(modifiedPsl, 'X' + suffixes.slice(1));
    for (const [args, diagnostic] of [
      [{groupName: 'WIKIMEDIA'}, /group.*approved/i],
      [{dat: modifiedDlc}, /dlc.*sha-256/i],
      [{list: modifiedPsl}, /psl.*sha-256/i],
    ]) {
      const result = run(args);
      assert.notEqual(result.status, 0);
      assert.match(result.stderr, diagnostic);
      assert.deepEqual(readFileSync(output), first);
    }

    writeFileSync(output, Buffer.from(first).fill(' ', first.length - 2, first.length - 1));
    const tampered = readFileSync(output);
    assert.notDeepEqual(tampered, first);
    const verify = run({extra: ['--verify']});
    assert.notEqual(verify.status, 0);
    assert.match(verify.stderr, /catalogue.*match/i);
    assert.deepEqual(readFileSync(output), tampered);

    const occupied = join(dir, 'occupied');
    mkdirSync(occupied);
    writeFileSync(join(occupied, 'previous'), first);
    const entries = readdirSync(dir);
    assert.notEqual(run({path: occupied}).status, 0);
    assert.deepEqual(readdirSync(dir), entries);
    assert.deepEqual(readFileSync(join(occupied, 'previous')), first);
  });
