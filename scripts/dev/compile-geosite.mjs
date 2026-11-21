#!/usr/bin/env node

import {createHash, randomUUID} from 'node:crypto';
import {
  closeSync, constants, fstatSync, fsyncSync, openSync, readSync, renameSync, rmSync, statSync, writeFileSync,
} from 'node:fs';
import {isIP} from 'node:net';
import {dirname, join, resolve} from 'node:path';
import {domainToASCII, fileURLToPath, URL} from 'node:url';
import {parseArgs} from 'node:util';

const MAX_DAT_BYTES = 4 * 1024 * 1024;
const MAX_PSL_BYTES = 512 * 1024;
const MAX_PSL_RULES = 20_000;
const MAX_GROUPS = 4_096;
const MAX_GROUP_BYTES = 512 * 1024;
const MAX_GROUP_CODE_BYTES = 64;
const MAX_GROUP_ROWS = 4_096;
const MAX_ROW_BYTES = 1_024;

const DLC = {
  project: 'v2fly/domain-list-community',
  version: '20261121064510',
  commit: 'ffb513217b9cb22835eb74a39a907d77742ce68f',
  file: 'dlc.dat',
  bytes: 2_408_868,
  sha256: 'b1e803c6b8d02def0865b1b4953ee391c562478d8ff7ee3e8b621a2d15fb7ed2',
  url: 'https://github.com/v2fly/domain-list-community/releases/download/20261121064510/dlc.dat',
  license: 'MIT',
  copyright: 'Copyright (c) 2018-2019 V2Ray',
  licenseUrl: 'https://github.com/v2fly/domain-list-community/blob/ffb513217b9cb22835eb74a39a907d77742ce68f/LICENSE',
};
const PSL = {
  project: 'Chromium',
  version: '155.0.8059.80',
  commit: '43a195105613ac83c21d3f14de279f4a045c7da0',
  file: 'effective_tld_names.dat',
  bytes: 331_000,
  sha256: '4155611645690529d1bbea0cfe653b0c45f63b028e0ba039de29a97edc3204ca',
  url: 'https://chromium.googlesource.com/chromium/src/+/43a195105613ac83c21d3f14de279f4a045c7da0/net/base/registry_controlled_domains/effective_tld_names.dat',
  license: 'MPL-2.0',
  licenseUrl: 'https://mozilla.org/MPL/2.0/',
  privateDomains: true,
};
const APPROVED_GROUP = 'DEBIAN';
const APPROVED_PAYLOAD_SHA256 = 'd865d67c829d0f13bf508e152ef402d5d9634227ed12a8b49434d231baf5a643';
const GROUP_CODE = /^[A-Z0-9][A-Z0-9_!-]*$/;
const LOCAL_SUFFIXES = ['local', 'localhost', 'localdomain', 'home.arpa', 'internal'];
const utf8 = new TextDecoder('utf-8', {fatal: true, ignoreBOM: true});

// Tags and lengths in this schema, and its supported enum values, fit uint32.
class Reader {
  constructor(data) {
    this.data = data;
    this.offset = 0;
  }

  get done() {
    return this.offset === this.data.length;
  }

  varint() {
    let value = 0;
    for (let shift = 0; shift <= 28; shift += 7) {
      if (this.done) throw new Error('Truncated protobuf varint');
      const byte = this.data[this.offset++];
      if (shift === 28 && byte > 15) throw new Error('Protobuf varint exceeds uint32');
      value += (byte & 127) * 2 ** shift;
      if (byte < 128) {
        if (shift && byte === 0) throw new Error('Non-canonical protobuf varint');
        return value;
      }
    }
    throw new Error('Protobuf varint exceeds uint32');
  }

  length(limit) {
    const length = this.varint();
    if (length > this.data.length - this.offset) throw new Error('Truncated protobuf field');
    if (length > limit) throw new Error('Protobuf field exceeds size limit');
    return length;
  }

  bytes(limit) {
    const size = this.length(limit);
    const start = this.offset;
    this.offset += size;
    return this.data.subarray(start, this.offset);
  }

  skipBytes() {
    const size = this.length(MAX_DAT_BYTES);
    this.offset += size;
  }
}

function findGroup(data, requested) {
  const catalogue = new Reader(data);
  const codes = new Set();
  let selected;
  while (!catalogue.done) {
    if (codes.size >= MAX_GROUPS) throw new Error('DAT exceeds group count limit');
    if (catalogue.varint() !== 10) throw new Error('Invalid GeoSiteList entry field');
    const group = catalogue.bytes(MAX_DAT_BYTES);
    const fields = new Reader(group);
    let code;
    while (!fields.done) {
      switch (fields.varint()) {
        case 10: // GeoSite.country_code
          if (code !== undefined) throw new Error('Duplicate GeoSite group code field');
          code = utf8.decode(fields.bytes(MAX_GROUP_CODE_BYTES));
          break;
        case 18: // GeoSite.domain
          fields.skipBytes();
          break;
        default:
          throw new Error('Unsupported GeoSite group field');
      }
    }
    if (!code || !GROUP_CODE.test(code)) throw new Error('Invalid GeoSite group code');
    if (codes.has(code)) throw new Error('Duplicate GeoSite group: ' + code);
    codes.add(code);
    if (code === requested) {
      if (group.length > MAX_GROUP_BYTES) throw new Error('Selected group exceeds size limit');
      selected = group;
    }
  }
  if (!selected) throw new Error('GeoSite group not found: ' + requested);
  return selected;
}

function dnsName(input) {
  // Guard the raw DNS path against authority, URL delimiters, and escaped input.
  if (!input || /[\s\p{Cc}%:/\\@[\]?#]/u.test(input)) return '';
  const ascii = domainToASCII(input).replace(/\.$/, '');
  if (!ascii || ascii.length > 253 || isIP(ascii)) return '';
  return ascii.split('.').every((label) => /^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(label)) ? ascii : '';
}

function isLocal(name) {
  return LOCAL_SUFFIXES.some(
    (suffix) => name === suffix || name.endsWith('.' + suffix)
  );
}

function parentName(name) {
  const dot = name.indexOf('.');
  return dot < 0 ? '' : name.slice(dot + 1);
}

function readRegistry(text) {
  if (typeof text !== 'string' || Buffer.byteLength(text) > MAX_PSL_BYTES) {
    throw new Error('PSL exceeds input size limit or is not text');
  }
  const exact = new Set();
  const wildcards = new Set();
  const exceptions = new Set();
  const registryAncestors = new Set();
  let count = 0;

  // The ICANN and PRIVATE sections use the same PSL matching algorithm.
  for (const [line] of text.matchAll(/[^\n]+/g)) {
    const entry = line.trim().split(/\s/u, 1)[0];
    if (!entry || entry.startsWith('//')) continue;
    if (++count > MAX_PSL_RULES) throw new Error('PSL exceeds rule count limit');
    const wildcard = entry.startsWith('*.');
    const exception = entry.startsWith('!');
    const name = dnsName(entry.replace(/^(?:\*\.|!)/, ''));
    if (!name) throw new Error('Invalid PSL rule: ' + entry);
    if (exception) {
      exceptions.add(name);
      continue;
    }
    (wildcard ? wildcards : exact).add(name);
    // A domain rule on an ancestor would also match every registrant under this boundary.
    for (let ancestor = wildcard ? name : parentName(name); ancestor; ancestor = parentName(ancestor)) {
      registryAncestors.add(ancestor);
    }
  }
  if (!count) throw new Error('Empty PSL');
  for (const exception of exceptions) {
    if (!wildcards.has(parentName(exception))) throw new Error('Invalid PSL exception: ' + exception);
  }
  return {exact, wildcards, exceptions, registryAncestors};
}

function isPublicSuffix(name, {exact, wildcards, exceptions}) {
  // Only a rule matching the entire name can make it a public suffix. A matching
  // exception, even on an ancestor, takes priority and shortens the suffix.
  for (let suffix = name; suffix; suffix = parentName(suffix)) {
    if (exceptions.has(suffix)) return false;
  }
  const parent = parentName(name);
  return !parent || exact.has(name) || wildcards.has(parent);
}

function readRule(bytes, registry) {
  const domain = new Reader(bytes);
  let type;
  let value;
  while (!domain.done) {
    switch (domain.varint()) {
      case 8: // Domain.type
        if (type !== undefined) throw new Error('Duplicate GeoSite domain type field');
        type = domain.varint();
        break;
      case 18: // Domain.value
        if (value !== undefined) throw new Error('Duplicate GeoSite domain value field');
        value = utf8.decode(domain.bytes(MAX_ROW_BYTES));
        break;
      case 26: // Domain.attribute
        throw new Error('GeoSite domain attributes are unsupported');
      default:
        throw new Error('Unsupported GeoSite domain field');
    }
  }
  // Omitted type is the protobuf default, Plain (keyword).
  if (type !== 2 && type !== 3) throw new Error('Unsupported GeoSite domain type: ' + (type ?? 0));
  const name = dnsName(value);
  if (!name || isLocal(name)) throw new Error('Invalid GeoSite DNS rule: ' + value);
  if (isPublicSuffix(name, registry)) throw new Error('GeoSite rule is a public suffix: ' + name);
  if (type === 2 && registry.registryAncestors.has(name)) {
    throw new Error('GeoSite domain rule covers a registry boundary: ' + name);
  }
  return Object.freeze({type: type === 2 ? 'domain' : 'full', value: name});
}

// The decoder can inspect synthetic and rejected groups. Only the CLI admits a
// group for the macOS DEV build, after verifying both immutable inputs.
export function compileGroup(buffer, pslText, group) {
  if (!Buffer.isBuffer(buffer) || buffer.length > MAX_DAT_BYTES) throw new Error('Invalid DAT input or size limit exceeded');
  if (typeof group !== 'string' || group.length > MAX_GROUP_CODE_BYTES || !GROUP_CODE.test(group)) {
    throw new Error('Invalid requested GeoSite group code');
  }
  const selected = new Reader(findGroup(buffer, group));
  const registry = readRegistry(pslText);
  const rules = [];
  while (!selected.done) {
    // findGroup verified that the only other field here is the group code.
    if (selected.varint() === 10) {
      selected.skipBytes();
      continue;
    }
    if (rules.length >= MAX_GROUP_ROWS) throw new Error('Selected group exceeds row count limit');
    rules.push(readRule(selected.bytes(MAX_ROW_BYTES), registry));
  }
  if (!rules.length) throw new Error('Selected GeoSite group is empty');
  rules.sort((left, right) => {
    if (left.type !== right.type) return left.type < right.type ? -1 : 1;
    if (left.value === right.value) return 0;
    return left.value < right.value ? -1 : 1;
  });
  return {rules: Object.freeze(rules)};
}

export function matchesHost(rules, host) {
  if (typeof host !== 'string') return false;
  let hostname = host;
  if (/^(?:https?|wss?):\/\/[^/?#\\]/i.test(host)) {
    if (/[\s\p{Cc}\\]/u.test(host)) return false;
    try {
      hostname = new URL(host).hostname;
    } catch {
      return false;
    }
  }
  const name = dnsName(hostname);
  if (!name.includes('.') || isLocal(name)) return false;

  return rules.some(({type, value}) =>
    (type === 'full' && name === value) ||
    (type === 'domain' && (name === value || name.endsWith('.' + value))));
}

function sha256(data) {
  return createHash('sha256').update(data).digest('hex');
}

function readExactFile(path, size, label) {
  const fd = openSync(path, constants.O_RDONLY | constants.O_NONBLOCK);
  try {
    const info = fstatSync(fd, {bigint: true});
    if (!info.isFile() || info.size !== BigInt(size)) throw new Error(label + ' must be a regular file of ' + size + ' bytes');
    // Bound the read even if a file changes after fstat. The extra byte detects growth.
    const data = Buffer.allocUnsafe(size + 1);
    let offset = 0;
    while (offset < data.length) {
      const length = readSync(fd, data, offset, data.length - offset, null);
      if (length === 0) break;
      offset += length;
    }
    if (offset !== size) throw new Error(label + ' changed size while reading');
    return {data: data.subarray(0, size), info};
  } finally {
    closeSync(fd);
  }
}

function readPinned(path, pin, label) {
  const input = readExactFile(path, pin.bytes, label);
  if (sha256(input.data) !== pin.sha256) throw new Error(label + ' SHA-256 does not match the pin');
  return input;
}

function writeAtomically(path, data) {
  const temporary = join(dirname(path), '.geosite-' + randomUUID() + '.tmp');
  const fd = openSync(temporary, 'wx', 0o644);
  let renamed = false;
  try {
    try {
      writeFileSync(fd, data);
      fsyncSync(fd);
    } finally {
      closeSync(fd);
    }
    renameSync(temporary, path);
    renamed = true;
  } finally {
    if (!renamed) rmSync(temporary, {force: true});
  }
}

function main() {
  const {values} = parseArgs({
    options: {
      dlc: {type: 'string'},
      psl: {type: 'string'},
      group: {type: 'string'},
      output: {type: 'string'},
      verify: {type: 'boolean'},
      help: {type: 'boolean'},
    },
  });
  const {dlc, psl, group, output, verify, help} = values;
  if (help) {
    console.log('Usage: node scripts/dev/compile-geosite.mjs --dlc <dlc.dat> --psl <effective_tld_names.dat> --group DEBIAN --output <catalogue.json> [--verify]');
    console.log('Use the decoded Chromium 155.0.8059.80 PSL and V2Fly 20261121064510 dlc.dat. Both inputs are verified locally.');
    console.log('--verify rebuilds in memory and compares the full catalogue byte for byte.');
    return;
  }
  if (!dlc || !psl || !group || !output) throw new Error('Required options: --dlc, --psl, --group, --output (see --help)');
  if (group !== APPROVED_GROUP) throw new Error('Only the DEBIAN group is approved');
  const datInput = readPinned(dlc, DLC, 'DLC');
  const pslInput = readPinned(psl, PSL, 'PSL');
  const {rules} = compileGroup(datInput.data, utf8.decode(pslInput.data), group);
  const payload = rules.map(({type, value}) => type + ':' + value + '\n').join('');
  const payloadSha256 = sha256(payload);
  if (payloadSha256 !== APPROVED_PAYLOAD_SHA256) throw new Error('Compiled rule payload SHA-256 does not match the pin');
  const catalogue = {
    schemaVersion: 1,
    compiler: {name: 'compile-geosite', version: 1},
    group,
    inputs: {dlc: DLC, psl: PSL},
    ruleCount: rules.length,
    payloadSha256,
    rules,
  };
  const expected = Buffer.from(JSON.stringify(catalogue, null, 2) + '\n');
  if (verify) {
    const {data} = readExactFile(output, expected.length, 'Catalogue to verify');
    if (!data.equals(expected)) throw new Error('Catalogue does not match the pinned build');
  } else {
    const destination = statSync(output, {bigint: true, throwIfNoEntry: false});
    if (destination && [datInput.info, pslInput.info].some(
      (input) => destination.dev === input.dev && destination.ino === input.ino
    )) {
      throw new Error('Output must be separate from the pinned input files');
    }
    writeAtomically(output, expected);
  }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    main();
  } catch (error) {
    console.error('compile-geosite: ' + error.message);
    process.exitCode = 1;
  }
}
