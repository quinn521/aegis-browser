import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {
  copyFileSync, linkSync, lstatSync, mkdirSync, mkdtempSync, readFileSync, readlinkSync, readdirSync, realpathSync, rmSync,
  symlinkSync, writeFileSync,
} from 'node:fs';
import {tmpdir} from 'node:os';
import {basename, dirname, join} from 'node:path';
import test from 'node:test';
import {fileURLToPath} from 'node:url';
import {compileGroup, matchesHost} from '../compile-geosite.mjs';

const cli = fileURLToPath(new URL('../compile-geosite.mjs', import.meta.url));
const sha256 = (data) => createHash('sha256').update(data).digest('hex');
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
  const result = spawnSync(process.execPath, [cli, '--dlc', dat, '--psl', suffixes, '--group', 'DEBIAN', '--output', output]);
  assert.notEqual(result.status, 0);
  assert.equal(readFileSync(output, 'utf8'), 'previous\n');
});

test('URL inputs use the actual network authority and valid DNS labels', () => {
  const {rules} = compileGroup(group('TRIAL', domain(2, 'good.org')), psl, 'TRIAL');
  for (const input of ['https://%67ood.org./path', 'wss://食狮.good.org:443/', 'http://other.org@good.org/',
    'https://good.org/a b', 'https://good.org/?q=C:\\docs', 'https://good.org#C:\\docs']) {
    assert.equal(matchesHost(rules, input), true, input);
  }
  for (const input of ['https://good.org@other.org/', 'https://other.org/good.org', 'ftp://good.org/',
    'https://other.org/?q=good.org#good.org', 'https://good.org\\@other.org/', 'https://good.\torg/',
    'https://good.org @other.org/', 'https://\\good.org/',
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
  assert.equal(result.stdout, '');
  assert.equal(result.stderr, '');
});

test('importing the compiler by a positional filesystem path is inert', (t) => {
  const source = `void import(require('node:url').pathToFileURL(process.argv[1]));`;
  const moduleSource = `import {pathToFileURL} from 'node:url'; await import(pathToFileURL(process.argv[1]));`;
  const dir = mkdtempSync(join(tmpdir(), 'aegis-geosite-import-test-'));
  t.after(() => rmSync(dir, {recursive: true, force: true}));
  const importer = join(dir, 'importer.mjs');
  writeFileSync(importer, moduleSource.replace('argv[1]', 'argv[2]'));
  for (const [args, stdout] of [
    [['-e', source], ''],
    [['--eval', source], ''],
    [[`--eval=${source}`], ''],
    [['--input-type=module', '-e', moduleSource], ''],
    [['--input-type=module', '--eval', moduleSource], ''],
    [['-p', source], 'undefined\n'],
    [['--print', source], 'undefined\n'],
    [['-pe', source], 'undefined\n'],
    [[importer], ''],
  ]) {
    const result = spawnSync(process.execPath, [...args, cli, '--help'], {encoding: 'utf8'});
    assert.ifError(result.error);
    assert.equal(result.status, 0, result.stderr);
    assert.equal(result.stdout, stdout, args[0]);
    assert.equal(result.stderr, '');
  }
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
    Buffer.alloc(16 * 1024 * 1024 + 1),
  ]) {
    assert.throws(() => compileGroup(bad, psl, 'TRIAL'));
  }
  assert.throws(() => compileGroup(catalogue, psl + '//'.repeat(300_000), 'TRIAL'), /PSL.*limit/i);
});

test('unbacked PSL exceptions cannot admit a public suffix', () => {
  assert.throws(() => compileGroup(group('TRIAL', domain(2, 'tenant.example')), psl + '\n!tenant.example', 'TRIAL'),
    /invalid PSL exception/i);
});

test('a large unselected group can be skipped without relaxing the selected row limit', () => {
  const selected = group('TRIAL', domain(2, 'good.org'));
  const opaqueRow = domain(1, 'a'.repeat(64));
  const unselected = bytes(1, Buffer.concat([...Array(75_744).fill(opaqueRow), bytes(1, 'OTHER-!CN')]));
  const catalogue = Buffer.concat([selected, unselected]);
  assert.ok(catalogue.length > 4 * 1024 * 1024);
  assert.deepEqual(compileGroup(catalogue, psl, 'TRIAL').rules, [{type: 'domain', value: 'good.org'}]);
  const ordinaryRow = domain(2, 'other.org');
  assert.throws(() => compileGroup(group('TRIAL', ...Array(4_097).fill(ordinaryRow)), psl, 'TRIAL'),
    /selected group.*row count/i);
});

test('skipped records and catalogue envelopes remain bounded through EOF', () => {
  const selected = group('TRIAL', domain(2, 'good.org'));
  const overlong = group('OTHER', bytes(2, Buffer.alloc(1_025)));
  const overcount = bytes(1, Buffer.concat([bytes(1, 'OTHER'), ...Array(100_001).fill(domain(0, 'word'))]));
  for (const [tail, diagnostic] of [
    [overlong, /field exceeds size limit/i],
    [overcount, /group.*record count limit/i],
    [group('OTHER', domain(0, 'term')).subarray(0, -1), /truncated/i],
    [Buffer.from([10, 7, 10, 1, 65, 18, 3, 8, 2]), /truncated/i],
    [Buffer.from([10, 128, 128, 128, 128, 16]), /uint32/i],
  ]) {
    assert.throws(() => compileGroup(Buffer.concat([selected, tail]), psl, 'TRIAL'), diagnostic);
  }
  assert.throws(() => compileGroup(Buffer.alloc(16 * 1024 * 1024 + 1), psl, 'TRIAL'), /DAT.*size limit/i);
});

test('canonical rows reject noncanonical names, duplicates and unsupported classes as a whole', () => {
  const valid = domain(2, 'good.org');
  const compile = (...rows) => compileGroup(group('TRIAL', valid, ...rows), psl, 'TRIAL', {requireCanonical: true});
  assert.deepEqual(compile(domain(3, 'news.good.org')).rules,
    [{type: 'domain', value: 'good.org'}, {type: 'full', value: 'news.good.org'}]);
  for (const host of ['Other.org', 'other.org.', 'bücher.org']) {
    assert.throws(() => compile(domain(2, host)), /noncanonical/i);
  }
  assert.throws(() => compile(valid), /duplicate.*rule/i);
  assert.throws(() => compile(domain(4, 'other.org')), /unsupported.*type/i);
  assert.throws(() => compile(domain(2, 'tenant.example')), /public suffix/i);
});

test('source flags are exclusive and synthetic data cannot use the official CLI', (t) => {
  const dir = mkdtempSync(join(tmpdir(), 'aegis-geosite-source-test-'));
  t.after(() => rmSync(dir, {recursive: true, force: true}));
  const dat = join(dir, 'geosite.dat');
  const suffixes = join(dir, 'psl.dat');
  const output = join(dir, 'catalogue.json');
  writeFileSync(dat, group('DOUBAN', domain(2, 'douban.com')));
  writeFileSync(suffixes, psl);
  writeFileSync(output, 'previous\n');
  const run = (groupName, ...flags) => spawnSync(process.execPath, [cli, '--psl', suffixes,
    '--group', groupName, '--output', output, ...flags], {encoding: 'utf8'});
  for (const [result, diagnostic] of [
    [run('DOUBAN'), /exactly one.*--dat.*--dlc/i],
    [run('DOUBAN', '--dat', dat, '--dlc', dat), /exactly one.*--dat.*--dlc/i],
    [run('DOUBAN', '--dat=', '--dlc', dat), /exactly one.*--dat.*--dlc/i],
    [run('DOUBAN', '--dat', dat), /DAT.*regular file of 7750066 bytes/i],
    [run('WIKIMEDIA', '--dat', dat), /group.*approved/i],
    [run('constructor', '--dat', dat), /group.*approved/i],
    [run('DOUBAN', '--dlc', dat), /group.*approved/i],
  ]) {
    assert.equal(result.status, 1, result.stderr);
    assert.match(result.stderr, diagnostic);
    assert.equal(readFileSync(output, 'utf8'), 'previous\n');
  }
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
    const output = join(dir, 'catalogue.json');
    const run = ({dat = dlcPath, list = pslPath, groupName = 'DEBIAN', path = output, extra = []} = {}) =>
      spawnSync(process.execPath, [cli, '--dlc', dat, '--psl', list, '--group', groupName, '--output', path, ...extra],
        {encoding: 'utf8'});
    const success = (result) => assert.equal(result.status, 0, result.stderr);

    success(run());
    const first = readFileSync(output);
    assert.equal(sha256(first), '74889c07232426b741c021f41a994e5902fce3a6b825cc83d9fa03a739bcc7fc');
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
    const failed = run({path: occupied});
    assert.equal(failed.status, 1, failed.stderr);
    assert.match(failed.stderr, /output.*regular file/i);
    assert.deepEqual(readdirSync(dir), entries);
    assert.deepEqual(readFileSync(join(occupied, 'previous')), first);
  });

const xrayInputs = process.env.AEGIS_XRAY_GEOSITE_INPUT_DIR;
test('authentic standard XTLS DAT reproduces the three certified groups and rejects WIKIMEDIA',
  {skip: !(xrayInputs && pinnedInputs) && 'set AEGIS_XRAY_GEOSITE_INPUT_DIR and AEGIS_GEOSITE_INPUT_DIR'}, (t) => {
    const dir = mkdtempSync(join(tmpdir(), 'aegis-geosite-xray-test-'));
    t.after(() => rmSync(dir, {recursive: true, force: true}));
    const datPath = join(xrayInputs, 'geosite.dat');
    const pslPath = join(pinnedInputs, 'effective_tld_names.dat');
    const legacyPath = join(pinnedInputs, 'dlc.dat');
    const output = join(dir, 'catalogue.json');
    const run = ({script = cli, source = '--dat', dat = datPath, list = pslPath,
      groupName = 'DEBIAN', path = output, extra = []} = {}) =>
      spawnSync(process.execPath, [script, source, dat, '--psl', list, '--group', groupName, '--output', path, ...extra],
        {encoding: 'utf8'});
    const cliDirectoryLink = join(dir, 'dev');
    symlinkSync(dirname(cli), cliDirectoryLink, 'dir');
    const cliAlias = join(cliDirectoryLink, basename(cli));
    const missing = run({script: cliAlias, extra: ['--verify']});
    assert.equal(missing.status, 1, missing.stderr);
    assert.match(missing.stderr, /ENOENT/);
    assert.equal(lstatSync(output, {throwIfNoEntry: false}), undefined);

    const raw = readFileSync(datPath);
    const suffixes = readFileSync(pslPath);
    const vectors = JSON.parse(readFileSync(new URL('./fixtures/beta-groups.json', import.meta.url), 'utf8'));
    assert.deepEqual(Object.keys(vectors).sort(), ['DEBIAN', 'DOUBAN', 'MEITUAN']);
    for (const [groupName, {count, rules, sha256: digest}] of Object.entries(vectors)) {
      const result = run({groupName});
      assert.equal(result.status, 0, result.stderr);
      const generated = readFileSync(output);
      const catalogue = JSON.parse(generated);
      assert.equal(catalogue.schemaVersion, 1);
      assert.equal(catalogue.group, groupName);
      assert.equal(catalogue.ruleCount, count);
      assert.deepEqual(catalogue.rules, rules.map(([type, value]) => ({type, value})));
      assert.equal(catalogue.payloadSha256, digest);
      assert.equal(catalogue.payloadSha256, sha256(catalogue.rules.map(({type, value}) => type + ':' + value + '\n').join('')));
      assert.deepEqual(Object.keys(catalogue.inputs), ['dat', 'psl']);
      assert.equal(catalogue.inputs.dat.project, 'XTLS/Xray-rules-dat');
      assert.equal(catalogue.inputs.dat.file, 'geosite.dat');
      assert.equal(catalogue.inputs.dat.version, '20261120235103');
      assert.equal(catalogue.inputs.dat.commit, 'd74acca73490cb57a937674ce8a8709d2a3021ba');
      assert.equal(catalogue.inputs.dat.tagCommit, '0f8082e3b11bbe8ee65f26de86cbd2e0d510c983');
      assert.match(catalogue.inputs.dat.licenseUrl, /^https:\/\/github\.com\/XTLS\/Xray-rules-dat\/blob\/d74acca[0-9a-f]+\/LICENSE-inherited$/);
      assert.equal(catalogue.inputs.dat.sha256, sha256(raw));
      assert.equal(catalogue.inputs.psl.sha256, sha256(suffixes));
      assert.equal(catalogue.inputs.psl.privateDomains, true);
      for (const extra of [[], ['--verify']]) {
        const repeat = run({groupName, extra});
        assert.equal(repeat.status, 0, repeat.stderr);
        assert.deepEqual(readFileSync(output), generated);
      }
    }
    assert.throws(() => compileGroup(raw, suffixes.toString('utf8'), 'WIKIMEDIA', {requireCanonical: true}),
      /public suffix.*(?:toolforge|wmcloud|wmflabs)\.org/i);

    const previous = readFileSync(output);
    const wrongDat = join(dir, 'wrong.dat');
    const wrongPsl = join(dir, 'wrong-psl.dat');
    const datAlias = join(dir, 'geosite-alias.dat');
    const pslAlias = join(dir, 'psl-alias.dat');
    const outputAlias = join(dir, 'catalogue-alias.json');
    // Use scratch copies so alias tests work across volumes and preserve the captured inputs.
    const datCopy = join(dir, 'pinned-geosite.dat');
    writeFileSync(datCopy, raw);
    linkSync(datCopy, datAlias);
    symlinkSync(realpathSync.native(pslPath), pslAlias);
    symlinkSync(output, outputAlias);
    raw[raw.length - 1] ^= 1;
    suffixes[0] ^= 1;
    writeFileSync(wrongDat, raw);
    writeFileSync(wrongPsl, suffixes);
    for (const [args, diagnostic] of [
      [{dat: wrongDat}, /DAT.*SHA-256/i],
      [{list: wrongPsl}, /PSL.*SHA-256/i],
      [{dat: legacyPath}, /DAT.*regular file/i],
      [{dat: datPath, source: '--dlc'}, /DLC.*regular file/i],
      [{groupName: 'MEITUAN', dat: legacyPath, source: '--dlc'}, /group.*approved/i],
      [{groupName: 'WIKIMEDIA'}, /group.*approved/i],
      [{dat: datCopy, path: datAlias}, /output.*separate/i],
      [{path: pslAlias}, /output.*regular file/i],
      [{path: outputAlias}, /output.*regular file/i],
    ]) {
      const result = run(args);
      assert.equal(result.status, 1, result.stderr);
      assert.match(result.stderr, diagnostic);
      assert.deepEqual(readFileSync(output), previous);
    }
    assert.equal(sha256(readFileSync(datAlias)), sha256(readFileSync(datPath)));
    assert.equal(sha256(readFileSync(pslAlias)), sha256(readFileSync(pslPath)));
    assert.equal(readlinkSync(outputAlias), output);

    const tampered = Buffer.from(previous);
    tampered[0] ^= 1;
    writeFileSync(output, tampered);
    const verify = run({groupName: JSON.parse(previous).group, extra: ['--verify']});
    assert.equal(verify.status, 1);
    assert.match(verify.stderr, /catalogue.*match/i);
    assert.deepEqual(readFileSync(output), tampered);
  });

test('publication protects a pinned pathname after the opened input is replaced',
  {skip: !(xrayInputs && pinnedInputs) && 'set AEGIS_XRAY_GEOSITE_INPUT_DIR and AEGIS_GEOSITE_INPUT_DIR'}, (t) => {
    const dir = mkdtempSync(join(tmpdir(), 'aegis-geosite-refresh-test-'));
    t.after(() => rmSync(dir, {recursive: true, force: true}));
    const dat = join(xrayInputs, 'geosite.dat');
    const psl = join(pinnedInputs, 'effective_tld_names.dat');
    const preload = join(dir, 'refresh.cjs');
    for (const [kind, pinned] of [['dat', dat], ['psl', psl]]) {
      const input = join(dir, kind);
      const alias = input + '-alias';
      const replacement = input + '-replacement';
      copyFileSync(pinned, input);
      symlinkSync(input, alias);
      writeFileSync(replacement, 'refreshed input\n');
      // Replace the directory entry while the compiler still holds the original fd.
      writeFileSync(preload, `
        const fs = require('node:fs');
        const {syncBuiltinESMExports} = require('node:module');
        const input = ${JSON.stringify(input)};
        const info = fs.statSync(input, {bigint: true});
        const fstatSync = fs.fstatSync;
        fs.fstatSync = (...args) => {
          const opened = fstatSync(...args);
          if (opened.dev === info.dev && opened.ino === info.ino) {
            fs.renameSync(${JSON.stringify(replacement)}, input);
          }
          return opened;
        };
        syncBuiltinESMExports();
      `);
      const result = spawnSync(process.execPath, ['--require', preload, cli,
        '--dat', kind === 'dat' ? alias : dat, '--psl', kind === 'psl' ? alias : psl,
        '--group', 'DEBIAN', '--output', input], {encoding: 'utf8'});
      assert.equal(result.status, 1, result.stderr);
      assert.match(result.stderr, /output.*separate/i);
      assert.equal(readFileSync(input, 'utf8'), 'refreshed input\n');
      assert.equal(readlinkSync(alias), input);
      assert.ok(readdirSync(dir).every((entry) => !entry.startsWith('.geosite-')));
    }
  });

test('publication protects DAT and PSL names when a parent symlink is retargeted',
  {skip: !(xrayInputs && pinnedInputs) && 'set AEGIS_XRAY_GEOSITE_INPUT_DIR and AEGIS_GEOSITE_INPUT_DIR'}, async (t) => {
    const dat = join(xrayInputs, 'geosite.dat');
    const psl = join(pinnedInputs, 'effective_tld_names.dat');
    for (const [kind, pinned] of [['dat', dat], ['psl', psl]]) {
      for (const alias of ['identical name', 'canonical path', 'hard link']) {
        await t.test(`${kind}: ${alias}`, (t) => {
          const dir = mkdtempSync(join(tmpdir(), 'aegis-geosite-retarget-test-'));
          t.after(() => rmSync(dir, {recursive: true, force: true}));
          const before = join(dir, 'before', 'nested');
          const after = join(dir, 'after', 'nested');
          mkdirSync(before, {recursive: true});
          mkdirSync(after, {recursive: true});
          const current = join(dir, 'current');
          const next = join(dir, 'next');
          symlinkSync(before, current, 'dir');
          symlinkSync(after, next, 'dir');
          const name = `${current}/../${kind}`;
          const original = join(dirname(before), kind);
          const refreshed = join(dirname(after), kind);
          copyFileSync(pinned, original);
          writeFileSync(refreshed, 'refreshed input\n');
          writeFileSync(join(dir, kind), 'lexical decoy\n');
          const catalogue = join(dir, 'catalogue.json');
          if (alias === 'hard link') linkSync(refreshed, catalogue);
          else writeFileSync(catalogue, 'previous catalogue\n');
          const previousCatalogue = readFileSync(catalogue);
          const output = alias === 'identical name' ? name : alias === 'canonical path' ? refreshed : catalogue;

          // Move the parent after opening the input, or after flushing the temporary output.
          const method = alias === 'identical name' ? 'fstatSync' : 'fsyncSync';
          const preload = join(dir, 'retarget.cjs');
          writeFileSync(preload, `
            const fs = require('node:fs');
            const {dev, ino} = fs.statSync(${JSON.stringify(original)}, {bigint: true});
            const method = ${JSON.stringify(method)};
            const call = fs[method];
            fs[method] = (...args) => {
              const result = call(...args);
              if (method === 'fsyncSync' || (result.dev === dev && result.ino === ino)) {
                fs.renameSync(${JSON.stringify(next)}, ${JSON.stringify(current)});
              }
              return result;
            };
            require('node:module').syncBuiltinESMExports();
          `);
          const result = spawnSync(process.execPath, ['--require', preload, cli,
            '--dat', kind === 'dat' ? name : dat, '--psl', kind === 'psl' ? name : psl,
            '--group', 'DEBIAN', '--output', output], {encoding: 'utf8'});
          assert.equal(readlinkSync(current), after);
          assert.equal(result.status, 1, result.stderr);
          assert.match(result.stderr, /output.*separate/i);
          assert.deepEqual(readFileSync(original), readFileSync(pinned));
          assert.equal(readFileSync(refreshed, 'utf8'), 'refreshed input\n');
          assert.deepEqual(readFileSync(catalogue), previousCatalogue);
          assert.ok([dir, dirname(after)].every((path) =>
            readdirSync(path).every((entry) => !entry.startsWith('.geosite-'))));
        });
      }
    }
  });

test('publication creates its temporary file beside the filesystem-resolved destination',
  {skip: !pinnedInputs && 'set AEGIS_GEOSITE_INPUT_DIR to the two captured pinned files'}, (t) => {
    const dir = mkdtempSync(join(tmpdir(), 'aegis-geosite-directory-test-'));
    t.after(() => rmSync(dir, {recursive: true, force: true}));
    const actual = join(dir, 'actual');
    const nested = join(actual, 'nested');
    const link = join(dir, 'link');
    mkdirSync(nested, {recursive: true});
    symlinkSync(nested, link, 'dir');
    const output = `${link}/../catalogue.json`;
    const lexical = join(dir, 'catalogue.json');
    writeFileSync(lexical, 'lexical destination\n');

    // Assert temp placement even on hosts where both directories use the same filesystem.
    const preload = join(dir, 'directory.cjs');
    writeFileSync(preload, `
      const assert = require('node:assert/strict');
      const fs = require('node:fs');
      const {syncBuiltinESMExports} = require('node:module');
      const {dirname} = require('node:path');
      const openSync = fs.openSync;
      fs.openSync = (path, flags, ...args) => {
        if (flags === 'wx') assert.equal(dirname(path), ${JSON.stringify(realpathSync.native(actual))});
        return openSync(path, flags, ...args);
      };
      syncBuiltinESMExports();
    `);
    const args = [cli, '--dlc', join(pinnedInputs, 'dlc.dat'),
      '--psl', join(pinnedInputs, 'effective_tld_names.dat'), '--group', 'DEBIAN', '--output', output];
    const result = spawnSync(process.execPath, ['--require', preload, ...args], {encoding: 'utf8'});
    assert.equal(result.status, 0, result.stderr);
    assert.equal(JSON.parse(readFileSync(join(actual, 'catalogue.json'), 'utf8')).group, 'DEBIAN');
    assert.equal(readFileSync(lexical, 'utf8'), 'lexical destination\n');
    assert.deepEqual(readdirSync(actual).sort(), ['catalogue.json', 'nested']);
    const verify = spawnSync(process.execPath, [...args, '--verify'], {encoding: 'utf8'});
    assert.equal(verify.status, 0, verify.stderr);
  });
