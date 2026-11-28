import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {createRequire} from 'node:module';
import test from 'node:test';
import vm from 'node:vm';
import {fileURLToPath} from 'node:url';

const requireWorkspace = createRequire(new URL('../../../package.json', import.meta.url));
const {JSDOM} = requireWorkspace('jsdom');
const ts = requireWorkspace('typescript');
const resources = fileURLToPath(new URL('../overlay/chrome/browser/resources/aegis/', import.meta.url));
const page = readFileSync(resources + 'access_options.html', 'utf8');
const {outputText} = ts.transpileModule(
    readFileSync(resources + 'access_options.ts', 'utf8'), {
      compilerOptions: {target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS},
    });
const tick = () => new Promise(resolve => setImmediate(resolve));

function openOptions(t, capabilities = {enabled: true, private: false}) {
  const dom = new JSDOM(page, {runScripts: 'outside-only'});
  t.after(() => dom.window.close());
  const previews = [];
  const discards = [];
  const cancellations = [];
  const applies = [];
  const preferenceApplies = [];
  const applyQueries = [];
  const revocations = [];
  let saved = {state: 'ready', revision: '0'};
  let readFails = false;
  const {document, Event} = dom.window;
  const context = dom.getInternalVMContext();
  context.exports = {};
  context.chrome = {send: message => cancellations.push(message)};
  context.require = id => {
    assert.equal(id, 'chrome://resources/js/cr.js');
    return {
      sendWithPromise(message, ...args) {
        if (message === 'getAccessOptionsCapabilities') {
          return Promise.resolve(capabilities);
        }
        if (message === 'discardAccessTestNodeDraft') {
          return new Promise((resolve, reject) => discards.push({resolve, reject}));
        }
        if (message === 'readAccessOptions') {
          return readFails ? Promise.reject(new Error('read unavailable')) :
                             Promise.resolve(saved);
        }
        if (message === 'applyAccessTestNode') {
          return new Promise(resolve => applies.push({args, resolve}));
        }
        if (message === 'applyAccessTestNodePreferences') {
          return new Promise(resolve => preferenceApplies.push({args, resolve}));
        }
        if (message === 'queryAccessTestNodeApply') {
          return new Promise(resolve => applyQueries.push({args, resolve}));
        }
        if (message === 'revokeAccessTestNode') {
          return new Promise(resolve => revocations.push({args, resolve}));
        }
        assert.equal(message, 'previewAccessTestNode');
        const [source, payload] = args;
        return new Promise(resolve => previews.push({source, payload, resolve}));
      },
    };
  };
  vm.runInContext(outputText, context);
  return {
    document,
    Event,
    form: document.querySelector('form'),
    fields: document.querySelector('fieldset'),
    input: document.querySelector('#node-share'),
    status: document.querySelector('[role=status]'),
    discard: document.querySelector('#discard-preview'),
    image: document.querySelector('#node-image'),
    previewText(value) {
      this.input.value = value;
      this.form.dispatchEvent(new Event('submit', {cancelable: true}));
    },
    selectImage(file) {
      Object.defineProperty(this.image, 'files', {configurable: true, value: file ? [file] : []});
      this.image.dispatchEvent(new Event('change'));
    },
    makeImage(bytes) {
      return new dom.window.File([Uint8Array.from(bytes)], 'image.png', {type: 'image/png'});
    },
    previews,
    discards,
    cancellations,
    applies,
    preferenceApplies,
    applyQueries,
    revocations,
    setSaved(snapshot) {
      saved = snapshot;
    },
    failReads() {
      readFails = true;
    },
  };
}

test('connection details associate each value with a named row header', t => {
  const {document} = openOptions(t);
  const summary = document.querySelector('table[aria-labelledby="connection-heading"]');
  assert.ok(summary);
  assert.deepEqual(Array.from(summary.rows, row => [
    row.querySelector('th[scope="row"]').textContent,
    row.querySelector('td').id,
  ]), [
    ['Test Profile', 'connection-profile'],
    ['Protocol', 'connection-protocol'],
    ['Local address', 'connection-address'],
    ['Saved revision', 'saved-revision'],
  ]);
});

test('manual preview starts disabled and remains disabled for restricted profiles', async t => {
  for (const identity of [
    {enabled: false, private: false},
    {enabled: true, private: true},
  ]) {
    const page = openOptions(t, identity);
    assert.equal(page.fields.disabled, true);
    await tick();
    assert.equal(page.fields.disabled, true);
    assert.match(page.status.textContent, /unavailable/);
    assert.equal(page.previews.length, 0);
  }
});

test('Apply binds the preview to its revision and the selected port before any site activation', async t => {
  const page = openOptions(t, {enabled: true, private: false, testApply: true});
  await tick();
  const byId = id => page.document.getElementById(id);
  const apply = byId('apply-node');
  assert.equal(apply.disabled, true);
  page.previewText('synthetic-test-profile');
  page.previews[0].resolve({state: 'preview'});
  await tick();
  const protocol = byId('inbound-protocol');
  const portMode = byId('inbound-port-mode');
  protocol.value = 'socks5';
  protocol.dispatchEvent(new page.Event('change'));
  portMode.value = 'fixed';
  portMode.dispatchEvent(new page.Event('change'));
  assert.equal(apply.disabled, false);
  assert.equal(page.applies.length, 0);
  apply.click();
  assert.equal(page.applies.length, 1);
  const [revision, operation, ...preferences] = page.applies[0].args;
  assert.equal(revision, '0');
  assert.match(operation, /^[a-f0-9-]{36}$/);
  assert.deepEqual(preferences, ['socks5', 'fixed', '10806']);
  page.applies[0].resolve({state: 'prepared', revision: '1', revokeToken: 'listener-a',
                           protocol: 'SOCKS5', port: 10806});
  await tick();
  assert.equal(byId('connection-state').textContent, 'Listener prepared');
  assert.equal(byId('connection-address').textContent, '127.0.0.1:10806');
  assert.equal(byId('saved-revision').textContent, '1');
  assert.doesNotMatch(page.document.body.textContent, /synthetic-test-profile/);
  byId('revoke-node').click();
  assert.deepEqual(page.revocations[0].args, ['1', 'listener-a']);
  page.revocations[0].resolve({state: 'revoked'});
  await tick();
  assert.equal(byId('connection-state').textContent, 'No active connection');
  assert.equal(byId('connection-address').textContent, '—');
});

test('DEV text and QR previews display only the fixed transport labels', {timeout: 1500}, async t => {
  const page = openOptions(t, {
    enabled: true, private: false, testApply: true, testExtendedVless: true,
  });
  await tick();
  assert.equal(page.input.maxLength, 8192);
  assert.match(page.document.querySelector('#share-hint').textContent, /WebSocket\/TLS/);
  page.previewText('private-synthetic-share');
  page.previews[0].resolve({state: 'preview', protocol: 'VLESS · WebSocket · TLS'});
  await tick();
  assert.equal(page.document.querySelector('#node-preview-protocol').textContent, 'VLESS · WebSocket · TLS');
  assert.equal(page.input.value, '');
  assert.doesNotMatch(page.document.body.textContent, /private-synthetic-share/);
  assert.equal(page.applies.length, 0);
  for (const label of ['VLESS · TCP · TLS · Vision', 'VLESS · gRPC · REALITY']) {
    const count = page.previews.length;
    page.selectImage(page.makeImage([0, 1, 250]));
    while (page.previews.length === count) {
      await tick();
    }
    assert.equal(page.previews.at(-1).source, 'image');
    page.previews.at(-1).resolve({state: 'preview', protocol: label});
    await tick();
    assert.equal(page.document.querySelector('#node-preview-protocol').textContent, label);
  }
});

test('a conflicting draft keeps its preview and compares the newer committed revision before retry', async t => {
  const page = openOptions(t, {enabled: true, private: false, testApply: true});
  await tick();
  const byId = id => page.document.getElementById(id);
  page.previewText('synthetic-conflicting-draft');
  page.previews[0].resolve({state: 'preview'});
  await tick();
  byId('apply-node').click();
  page.applies[0].resolve({state: 'notSaved', revision: '1', reason: 'configuration_revision_conflict'});
  await tick();
  assert.equal(byId('apply-node').disabled, true);
  assert.equal(byId('node-preview').hidden, false);
  page.setSaved({state: 'ready', revision: '1',
    testProfile: {protocol: 'http', portMode: 'fixed', requestedPort: 9990}});
  byId('review-revision').click();
  await tick();
  assert.match(page.status.textContent, /HTTP · fixed port 9990/);
  assert.equal(byId('node-preview').hidden, false);
  assert.equal(byId('inbound-protocol').value, 'auto');
  byId('apply-node').click();
  assert.equal(page.applies[1].args[0], '1');
  assert.notEqual(page.applies[1].args[1], page.applies[0].args[1]);
  page.applies[1].resolve({state: 'savedUnavailable', revision: '2', reason: 'Listener unavailable'});
  await tick();
  assert.equal(byId('connection-state').textContent, 'No active connection');
  assert.match(page.status.textContent, /^Saved\. Listener unavailable/);
});

test('an unavailable options read keeps the preview and allows a retry', async t => {
  const page = openOptions(t, {enabled: true, private: false, testApply: true});
  await tick();
  const byId = id => page.document.getElementById(id);
  page.previewText('synthetic-node');
  page.previews[0].resolve({state: 'preview'});
  await tick();
  const apply = byId('apply-node');
  apply.click();
  page.applies[0].resolve({state: 'notSaved', reason: 'options_unavailable'});
  await tick();
  assert.match(page.status.textContent, /saved settings could not be read/i);
  assert.equal(byId('review-revision').hidden, true);
  assert.equal(byId('node-preview').hidden, false);
  assert.equal(apply.disabled, false);
});

test('activating another Profile while one runs is rejected and keeps the connection', async t => {
  const page = openOptions(t, {enabled: true, private: false, testApply: true});
  await tick();
  const byId = id => page.document.getElementById(id);
  page.previewText('synthetic-second-profile');
  page.previews[0].resolve({state: 'preview'});
  await tick();
  byId('apply-node').click();
  page.applies[0].resolve({state: 'notSaved', revision: '1',
                           reason: 'active_profile_switch_requires_deactivation'});
  await tick();
  assert.equal(page.status.dataset['error'], 'true');
  assert.match(page.status.textContent,
               /^Deactivate the current Profile before activating another\./);
  assert.match(page.status.textContent, /current connection was not changed/);
  assert.doesNotMatch(page.status.textContent, /Another window saved changes/);
  assert.equal(page.applies.length, 1);
});

test('an uncertain Apply queries the same operation without sending a second commit', async t => {
  const page = openOptions(t, {enabled: true, private: false, testApply: true});
  await tick();
  const byId = id => page.document.getElementById(id);
  page.previewText('synthetic-unknown-draft');
  page.previews[0].resolve({state: 'preview'});
  await tick();
  byId('apply-node').click();
  const operation = page.applies[0].args[1];
  page.applies[0].resolve({state: 'uncertain'});
  await tick();
  assert.equal(page.fields.disabled, true);
  assert.equal(byId('apply-node').disabled, true);
  byId('check-apply-result').click();
  assert.equal(page.applies.length, 1);
  assert.equal(page.applyQueries[0].args[0], operation);
  page.setSaved({state: 'ready', revision: '1',
    testProfile: {protocol: 'socks5', portMode: 'random', requestedPort: 0}});
  page.applyQueries[0].resolve({state: 'saved', revision: '1'});
  await tick();
  assert.equal(byId('saved-revision').textContent, '1');
  assert.equal(byId('node-preview').hidden, true);
  assert.equal(page.fields.disabled, false);
});

test('a stale revoke at the same revision requires a fresh connection token', async t => {
  const page = openOptions(t, {enabled: true, private: false, testApply: true});
  page.setSaved({state: 'ready', revision: '1',
    testProfile: {protocol: 'socks5', portMode: 'random', requestedPort: 0},
    connection: {state: 'active', revision: '1', revokeToken: 'listener-a',
                 protocol: 'SOCKS5', port: 45122}});
  await tick();
  const revoke = page.document.getElementById('revoke-node');
  revoke.click();
  assert.deepEqual(page.revocations[0].args, ['1', 'listener-a']);
  page.setSaved({state: 'ready', revision: '1',
    testProfile: {protocol: 'socks5', portMode: 'random', requestedPort: 0},
    connection: {state: 'prepared', revision: '1', revokeToken: 'listener-b',
                 protocol: 'SOCKS5', port: 45123}});
  page.revocations[0].resolve({state: 'connectionChanged'});
  await tick();
  assert.equal(page.revocations.length, 1);
  assert.equal(page.document.getElementById('connection-address').textContent, '127.0.0.1:45123');
  assert.match(page.status.textContent, /review its current status/i);
  revoke.click();
  assert.deepEqual(page.revocations[1].args, ['1', 'listener-b']);
  page.revocations[1].resolve({state: 'revoked'});
  await tick();
});

test('revoke uses the owned revision while publishing or unavailable', async t => {
  const page = openOptions(t, {enabled: true, private: false, testApply: true});
  const snapshot = {state: 'ready', revision: '7',
    testProfile: {protocol: 'socks5', portMode: 'random', requestedPort: 0}};
  page.setSaved({...snapshot, connection: {state: 'pending'}});
  await tick();
  const byId = id => page.document.getElementById(id);
  const revoke = byId('revoke-node');
  assert.equal(revoke.disabled, true);
  revoke.click();
  assert.equal(page.revocations.length, 0);

  for (const [state, label] of [
    ['publishing', 'Waiting for connection confirmation…'],
    ['unavailable', 'Connection status unavailable'],
  ]) {
    page.setSaved({...snapshot, connection: {state, revision: '7', revokeToken: state}});
    byId('refresh-connection').click();
    await tick();
    assert.equal(byId('connection-state').textContent, label);
    assert.equal(byId('connection-address').textContent, '—');
    assert.equal(revoke.disabled, false);
    revoke.click();
    assert.deepEqual(page.revocations.at(-1).args, ['7', state]);
    page.revocations.at(-1).resolve({state: 'revoked'});
    await tick();
  }
  assert.equal(page.revocations.length, 2);
});

test('a failed refresh never restores a token rejected as stale', async t => {
  const page = openOptions(t, {enabled: true, private: false, testApply: true});
  page.setSaved({state: 'ready', revision: '1',
    connection: {state: 'active', revision: '1', revokeToken: 'listener-a'}});
  await tick();
  const revoke = page.document.getElementById('revoke-node');
  revoke.click();
  page.failReads();
  page.revocations[0].resolve({state: 'connectionChanged'});
  await tick();
  assert.equal(revoke.disabled, true);
  assert.equal(page.document.getElementById('connection-state').dataset.state, 'unavailable');
  revoke.click();
  assert.equal(page.revocations.length, 1);
});

test('editing the link while revoke is pending still discards the staged node', async t => {
  const page = openOptions(t, {enabled: true, private: false, testApply: true});
  page.setSaved({state: 'ready', revision: '1',
    connection: {state: 'active', revision: '1', revokeToken: 'listener-a'}});
  await tick();
  page.previewText('synthetic-node-a');
  page.previews[0].resolve({state: 'preview'});
  await tick();
  page.document.getElementById('revoke-node').click();
  page.input.value = 'synthetic-node-b';
  page.input.dispatchEvent(new page.Event('input'));
  page.revocations[0].resolve({state: 'revoked'});
  await tick();
  const apply = page.document.getElementById('apply-node');
  assert.equal(apply.disabled, true);
  apply.click();
  assert.equal(page.applies.length, 0);
});

test('late replies after discard stay in their originating page and cannot restore a preview', async t => {
  const first = openOptions(t);
  const second = openOptions(t);
  await tick();
  assert.equal(first.fields.disabled, false);
  first.previewText('synthetic-first-input');
  second.previewText('synthetic-second-input');
  assert.equal(first.input.value, '');
  assert.equal(second.input.value, '');
  assert.deepEqual(first.previews.map(p => p.payload), ['synthetic-first-input']);
  assert.deepEqual(second.previews.map(p => p.payload), ['synthetic-second-input']);
  first.discard.click();
  assert.equal(first.status.textContent, 'Discarding preview…');
  first.discards[0].resolve({state: 'canceled'});
  first.previews[0].resolve({state: 'preview', protocol: 'late-first'});
  second.previews[0].resolve({state: 'preview', protocol: 'VLESS · TCP · REALITY · Vision'});
  await tick();
  assert.equal(first.status.textContent, 'Preview discarded.');
  assert.equal(second.status.textContent, 'Preview ready · not applied.');
  assert.equal(second.document.querySelector('#node-preview').hidden, false);
  assert.equal(second.cancellations.length, 1);
  assert.deepEqual(first.cancellations, ['cancelAccessTestNodePreview',
    'cancelAccessTestNodePreview']);
});

test('the image picker forwards only selected bytes and ignores a stale decode', {timeout: 1500}, async t => {
  const page = openOptions(t);
  await tick();
  page.selectImage(null);
  await tick();
  assert.equal(page.previews.length, 0);
  page.selectImage(page.makeImage([0, 1, 250]));
  while (page.previews.length === 0) {
    await tick();
  }
  assert.equal(page.previews[0].source, 'image');
  assert.equal(page.previews[0].payload, 'AAH6');

  page.previewText('synthetic-replacement');
  page.previews[1].resolve({state: 'invalidShare'});
  page.previews[0].resolve({state: 'preview', protocol: 'late-image'});
  await tick();
  assert.equal(page.status.textContent, 'Enter one complete 3X-UI node link.');
});

test('listener preferences are a draft; 1080 is rejected and discard clears the draft', async t => {
  const page = openOptions(t);
  await tick();
  const byId = id => page.document.getElementById(id);
  const mode = byId('inbound-port-mode');
  const port = byId('inbound-port');
  mode.value = 'fixed';
  mode.dispatchEvent(new page.Event('change'));
  assert.equal(byId('fixed-port-field').hidden, false);
  assert.equal(byId('port-error').hidden, true);
  port.value = '1080';
  port.dispatchEvent(new page.Event('input'));
  assert.equal(port.getAttribute('aria-invalid'), 'true');
  assert.equal(byId('port-error').hidden, false);
  assert.equal(byId('apply-node').disabled, true);
  port.value = '10806';
  port.dispatchEvent(new page.Event('input'));
  assert.equal(byId('port-error').hidden, true);
  assert.equal(byId('apply-node').disabled, true);
  assert.equal(page.previews.length, 0);
  page.discard.click();
  assert.equal(mode.value, 'random');
  assert.equal(byId('fixed-port-field').hidden, true);
  assert.equal(byId('draft-state').textContent, 'No changes saved');
  assert.equal(page.document.activeElement, page.input);
  page.discards[0].resolve({state: 'canceled'});
  await tick();
  assert.equal(page.status.textContent, 'Preview discarded.');
});

test('pasted input is masked, cleared on preview, and cannot enable Apply', async t => {
  const page = openOptions(t);
  await tick();
  assert.equal(page.input.type, 'password');
  page.previewText('synthetic-single-node');
  assert.equal(page.input.value, '');
  assert.equal(page.previews[0].payload, 'synthetic-single-node');
  page.previews[0].resolve({state:'preview',protocol:'VLESS · TCP · REALITY · Vision'});
  await tick();
  assert.equal(page.document.querySelector('#node-preview').hidden, false);
  assert.equal(page.document.querySelector('#apply-node').disabled, true);
  assert.match(page.document.querySelector('.connection-state').textContent, /unavailable/);
  assert.doesNotMatch(page.document.body.textContent, /synthetic-single-node/);
  page.discard.click();
  assert.equal(page.document.querySelector('#node-preview').hidden, true);
  page.discards[0].resolve({state: 'canceled'});
  await tick();
});

test('fixed HTTP ports use the complete decimal range without enabling Apply', async t => {
  const page = openOptions(t);
  await tick();
  const byId = id => page.document.getElementById(id);
  const protocol = byId('inbound-protocol');
  const mode = byId('inbound-port-mode');
  const port = byId('inbound-port');
  protocol.value = 'http';
  protocol.dispatchEvent(new page.Event('change'));
  mode.value = 'fixed';
  mode.dispatchEvent(new page.Event('change'));
  assert.equal(mode.selectedOptions[0].textContent, 'Fixed port (internal)');
  for (const value of ['1', '80', '1080', '8080', '10806', '65535', '00001']) {
    port.value = value;
    port.dispatchEvent(new page.Event('input'));
    assert.equal(byId('port-error').hidden, true, value);
    assert.equal(port.hasAttribute('aria-invalid'), false, value);
  }
  for (const value of ['', '0', '65536', '1e3', '1.0', '-1', ' 80', '80 ', '+80', '000080', '٨٠']) {
    port.value = value;
    port.dispatchEvent(new page.Event('input'));
    assert.equal(byId('port-error').hidden, false, value);
    assert.equal(port.getAttribute('aria-invalid'), 'true', value);
  }
  assert.match(byId('port-error').textContent, /1 to 65535/);
  assert.equal(byId('apply-node').disabled, true);
  assert.equal(page.previews.length, 0);
});

test('switching from HTTP does not admit its fixed port for Auto or SOCKS5', async t => {
  const page = openOptions(t);
  await tick();
  const byId = id => page.document.getElementById(id);
  const protocol = byId('inbound-protocol');
  const mode = byId('inbound-port-mode');
  const port = byId('inbound-port');
  protocol.value = 'http';
  mode.value = 'fixed';
  port.value = '1080';
  port.dispatchEvent(new page.Event('input'));
  assert.equal(byId('port-error').hidden, true);
  for (const value of ['socks5', 'auto']) {
    protocol.value = value;
    protocol.dispatchEvent(new page.Event('change'));
    assert.equal(port.value, '1080');
    assert.equal(byId('port-error').hidden, false);
    assert.match(mode.selectedOptions[0].textContent, /10806/);
  }
  port.value = '10806';
  port.dispatchEvent(new page.Event('input'));
  assert.equal(byId('port-error').hidden, true);
  protocol.value = 'http';
  protocol.dispatchEvent(new page.Event('change'));
  page.discard.click();
  page.discards[0].resolve({state: 'canceled'});
  await tick();
  assert.equal(protocol.value, 'auto');
  assert.match(mode.querySelector('option[value=fixed]').textContent, /10806/);
  assert.match(byId('port-hint').textContent, /SOCKS5/);
  assert.equal(byId('port-error').hidden, true);
});

test('discard reports secure storage failures and permits a successful retry', async t => {
  const page = openOptions(t);
  await tick();
  page.previewText('synthetic-single-node');
  page.previews[0].resolve({state: 'preview', protocol: 'VLESS'});
  await tick();
  page.discard.click();
  assert.equal(page.discard.disabled, true);
  page.discards[0].resolve({state: 'secureStorageUnavailable'});
  await tick();
  assert.equal(page.status.dataset.error, 'true');
  assert.match(page.status.textContent, /Secure storage is unavailable/);
  assert.equal(page.discard.disabled, false);
  assert.equal(page.document.querySelector('#node-preview').hidden, true);
  page.discard.click();
  page.discards[1].resolve({state: 'canceled'});
  await tick();
  assert.equal(page.status.textContent, 'Preview discarded.');
  assert.equal(page.status.dataset.error, 'false');
  assert.equal(page.discard.disabled, true);
});

test('a late discard cannot replace a newer draft or a closed page status', async t => {
  const page = openOptions(t);
  await tick();
  page.previewText('synthetic-first');
  page.discard.click();
  page.previewText('synthetic-second');
  page.previews[1].resolve({state: 'preview', protocol: 'VLESS'});
  await tick();
  page.discards[0].resolve({state: 'canceled'});
  page.previews[0].resolve({state: 'invalidShare'});
  await tick();
  assert.equal(page.status.textContent, 'Preview ready · not applied.');
  assert.equal(page.document.querySelector('#node-preview').hidden, false);
  page.discard.click();
  page.document.defaultView.dispatchEvent(new page.Event('pagehide'));
  page.discards[1].reject(new Error('synthetic transport failure'));
  await tick();
  assert.equal(page.status.textContent, 'Discarding preview…');
  assert.equal(page.document.querySelector('#node-preview').hidden, true);
});

test('an invalid reply allows correction and pagehide invalidates pending work', async t => {
  const page = openOptions(t);
  await tick();
  page.previewText('invalid');
  page.previews[0].resolve({state:'invalidShare'});
  await tick();
  assert.equal(page.status.dataset.error, 'true');
  page.previewText('correction');
  page.document.defaultView.dispatchEvent(new page.Event('pagehide'));
  page.previews[1].resolve({state:'preview',protocol:'VLESS'});
  await tick();
  assert.equal(page.document.querySelector('#node-preview').hidden, true);
  assert.ok(page.cancellations.length >= 3);
});

test('Return in the port field preserves the validated node preview', async t => {
  const page = openOptions(t);
  await tick();
  page.previewText('synthetic-single-node');
  page.previews[0].resolve({state:'preview',protocol:'VLESS · TCP · REALITY · Vision'});
  await tick();
  const port = page.document.getElementById('inbound-port');
  port.focus();
  const event = new page.document.defaultView.KeyboardEvent('keydown', {
    key:'Enter', bubbles:true, cancelable:true,
  });
  port.dispatchEvent(event);
  assert.equal(event.defaultPrevented, true);
  assert.equal(page.previews.length, 1);
  assert.equal(page.document.querySelector('#node-preview').hidden, false);
  assert.equal(page.input.value, '');
  assert.equal(page.status.textContent, 'Preview ready · not applied.');
});

test('editing the share field invalidates the staged preview before Apply', async t => {
  const page = openOptions(t, {enabled: true, private: false, testApply: true});
  await tick();
  const apply = page.document.getElementById('apply-node');
  page.previewText('synthetic-node-a');
  page.previews[0].resolve({state: 'preview'});
  await tick();
  assert.equal(apply.disabled, false);
  const canceled = page.cancellations.length;
  page.input.value = 'synthetic-node-b';
  page.input.dispatchEvent(new page.Event('input'));
  assert.equal(apply.disabled, true);
  assert.equal(page.document.querySelector('#node-preview').hidden, true);
  assert.equal(page.cancellations.length, canceled + 1);
  assert.equal(page.status.textContent, 'Preview the edited link before applying.');
  apply.click();
  assert.equal(page.applies.length, 0);
  assert.equal(page.preferenceApplies.length, 0);
});

test('editing the link during preview prevents an older reply from enabling Apply', async t => {
  const page = openOptions(t, {enabled: true, private: false, testApply: true});
  await tick();
  const apply = page.document.getElementById('apply-node');
  page.previewText('synthetic-node-a');
  const canceled = page.cancellations.length;
  page.input.value = 'synthetic-node-b';
  page.input.dispatchEvent(new page.Event('input'));
  page.previews[0].resolve({state: 'preview'});
  await tick();
  assert.equal(apply.disabled, true);
  assert.equal(page.document.querySelector('#node-preview').hidden, true);
  assert.equal(page.cancellations.length, canceled + 1);
  apply.click();
  assert.equal(page.applies.length, 0);

  page.previewText(page.input.value);
  assert.equal(page.previews[1].payload, 'synthetic-node-b');
  page.previews[1].resolve({state: 'preview'});
  await tick();
  assert.equal(apply.disabled, false);
});

test('listener preferences can be applied for the saved node without a new import', async t => {
  const page = openOptions(t, {enabled: true, private: false, testApply: true});
  page.setSaved({state: 'ready', revision: '3',
                 testProfile: {protocol: 'auto', portMode: 'random', requestedPort: 0}});
  await tick();
  await tick();
  const byId = id => page.document.getElementById(id);
  const apply = byId('apply-node');
  const protocol = byId('inbound-protocol');
  assert.equal(apply.disabled, true);
  protocol.value = 'http';
  protocol.dispatchEvent(new page.Event('change'));
  assert.equal(apply.disabled, false);
  apply.click();
  assert.equal(page.applies.length, 0);
  assert.equal(page.preferenceApplies.length, 1);
  const [revision, operation, ...preferences] = page.preferenceApplies[0].args;
  assert.equal(revision, '3');
  assert.match(operation, /^[a-f0-9-]{36}$/);
  assert.deepEqual(preferences, ['http', 'random', '']);
  page.preferenceApplies[0].resolve({state: 'savedUnavailable', revision: '4'});
  await tick();
  assert.equal(byId('saved-revision').textContent, '4');
  assert.equal(apply.disabled, true);
  protocol.value = 'auto';
  protocol.dispatchEvent(new page.Event('change'));
  assert.equal(apply.disabled, false);
  protocol.value = 'http';
  protocol.dispatchEvent(new page.Event('change'));
  assert.equal(apply.disabled, true);
});

test('discarding a stale draft reloads the latest committed revision', async t => {
  const page = openOptions(t, {enabled: true, private: false, testApply: true});
  page.setSaved({state: 'ready', revision: '1',
                 testProfile: {protocol: 'auto', portMode: 'random', requestedPort: 0}});
  await tick();
  await tick();
  const byId = id => page.document.getElementById(id);
  const protocol = byId('inbound-protocol');
  protocol.value = 'socks5';
  protocol.dispatchEvent(new page.Event('change'));
  page.setSaved({state: 'ready', revision: '5',
                 testProfile: {protocol: 'http', portMode: 'fixed', requestedPort: 8080}});
  page.discard.click();
  assert.equal(protocol.value, 'auto');
  page.discards[0].resolve({state: 'canceled'});
  await tick();
  await tick();
  assert.equal(byId('saved-revision').textContent, '5');
  assert.equal(protocol.value, 'http');
  assert.equal(byId('inbound-port-mode').value, 'fixed');
  assert.equal(byId('inbound-port').value, '8080');
  assert.equal(byId('draft-state').textContent, 'Saved preferences');
  assert.equal(byId('apply-node').disabled, true);
});

test('DEV requires an explicit protocol until automatic activation is admitted', async t => {
  const page = openOptions(t, {
    enabled: true, private: false, testApply: true,
    testActivate: true, testAutoProtocol: false,
  });
  await tick();
  const byId = id => page.document.getElementById(id);
  page.previewText('synthetic-dev-node');
  page.previews[0].resolve({state: 'preview'});
  await tick();
  const apply = byId('apply-node');
  assert.equal(byId('inbound-protocol').value, 'auto');
  assert.equal(apply.disabled, true);
  assert.match(byId('apply-availability').textContent, /choose HTTP or SOCKS5/i);
  apply.click();
  assert.equal(page.applies.length, 0);
  byId('inbound-protocol').value = 'http';
  byId('inbound-protocol').dispatchEvent(new page.Event('change'));
  assert.equal(apply.disabled, false);
  apply.click();
  assert.equal(page.applies[0].args[2], 'http');
});

test('an internal Alpha build can save a draft while activation stays unavailable', async t => {
  const page = openOptions(t, {enabled: true, private: false, testApply: true, testActivate: false});
  await tick();
  const byId = id => page.document.getElementById(id);
  page.previewText('synthetic-alpha-node');
  page.previews[0].resolve({state: 'preview'});
  await tick();
  assert.equal(byId('apply-node').disabled, false);
  assert.equal(byId('apply-availability').textContent,
               'Apply saves this draft. Connections are not available in this build.');
  byId('apply-node').click();
  assert.equal(page.applies.length, 1);
  page.applies[0].resolve({state: 'savedUnavailable', revision: '1',
                           reason: 'Connections are not available in this build.'});
  await tick();
  assert.equal(page.status.textContent, 'Saved. Connections are not available in this build.');
  assert.equal(byId('connection-state').textContent, 'No active connection');
  assert.equal(byId('saved-revision').textContent, '1');
});
