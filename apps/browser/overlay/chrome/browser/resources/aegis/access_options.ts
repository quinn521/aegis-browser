// Copyright 2026 GCSA
import {sendWithPromise} from 'chrome://resources/js/cr.js';

interface Preview {
  state: string;
  protocol?: string;
}
interface InboundPreferences {
  protocol: string;
  portMode: string;
  requestedPort: number;
}
interface Connection {
  state: string;
  revision?: string;
  revokeToken?: string;
  protocol?: string;
  port?: number;
}
interface OptionsState {
  state: string;
  revision?: string;
  testProfile?: InboundPreferences;
  connection?: Connection;
  pendingApplyRecovery?: boolean;
}
interface ApplyResult extends Connection {
  reason?: string;
}

function requiredElement<T extends Element>(selector: string, root: ParentNode = document): T {
  const element = root.querySelector<T>(selector);
  if (!element) throw new Error('Missing Access Options element ' + selector);
  return element;
}

const maxImageBytes = 16 * 1024 * 1024;
const form = requiredElement<HTMLFormElement>('#test-node-form');
const fields = requiredElement<HTMLFieldSetElement>('#test-node-fields');
const link = requiredElement<HTMLInputElement>('#node-share');
const image = requiredElement<HTMLInputElement>('#node-image');
const status = requiredElement<HTMLParagraphElement>('#node-status');
const discard = requiredElement<HTMLButtonElement>('#discard-preview');
const apply = requiredElement<HTMLButtonElement>('#apply-node');
const revoke = requiredElement<HTMLButtonElement>('#revoke-node');
const refresh = requiredElement<HTMLButtonElement>('#refresh-connection');
const review = requiredElement<HTMLButtonElement>('#review-revision');
const checkApply = requiredElement<HTMLButtonElement>('#check-apply-result');
const availability = requiredElement<HTMLElement>('#apply-availability');
const nodePreview = requiredElement<HTMLElement>('#node-preview');
const previewProtocol =
    requiredElement<HTMLElement>('#node-preview-protocol');
const realityProtocol = 'VLESS · TCP · REALITY · Vision';
const extendedProtocols = new Set([
  'VLESS · WebSocket · TLS',
  'VLESS · TCP · TLS · Vision',
  'VLESS · gRPC · REALITY',
]);
const draftState = requiredElement<HTMLElement>('#draft-state');
const protocol = requiredElement<HTMLSelectElement>('#inbound-protocol');
const portMode = requiredElement<HTMLSelectElement>('#inbound-port-mode');
const port = requiredElement<HTMLInputElement>('#inbound-port');
const fixedPortField = requiredElement<HTMLElement>('#fixed-port-field');
const portError = requiredElement<HTMLElement>('#port-error');
const portHint = requiredElement<HTMLElement>('#port-hint');
const fixedPortOption = requiredElement<HTMLOptionElement>(
    'option[value="fixed"]', portMode);
const messages = {
  preview: 'Preview ready · not applied.',
  notAdmitted: 'Test node preview is unavailable for this build or profile.',
  invalidShare: 'Enter one complete 3X-UI node link.',
  unsupportedOutbound: 'Use a VLESS, TCP, REALITY, Vision node link.',
  invalidImage: 'Choose one readable image of 16 MB or less.',
  noQrCode: 'No QR code was found in the image.',
  multipleQrCodes: 'Choose an image containing a single node QR code.',
  canceled: 'Preview discarded.',
  busy: 'Wait for the previous preview to finish, then try again.',
  secureStorageUnavailable: 'Secure storage is unavailable. Unlock the keychain and try again.',
};
const messagesByState: Readonly<Partial<Record<string, string>>> = messages;
const extendedImportMessages: Partial<Record<string, string>> = {
  invalidShare: 'Enter one complete node share link.',
  unsupportedOutbound:
      'Use a supported VLESS node with TLS or REALITY.',
};

let generation = 0;
let reader: FileReader|null = null;
let canApply = false;
let canActivate = false;
let canActivateAutomatically = false;
let canImportExtendedVless = false;
let working = false;
let previewReady = false;
let hasDraft = false;
let nodeDraft = false;
let revision: string|undefined;
let saved: InboundPreferences|undefined;
let uncertainOperation: string|undefined;
let requiresReview = false;
let pendingApplyRecovery = false;
let applying = false;
let lastConnection: Connection = {state: 'unavailable'};

function selectedPreferences(): InboundPreferences {
  return {protocol: protocol.value, portMode: portMode.value,
          requestedPort: portMode.value === 'fixed' ? Number(port.value) : 0};
}

function preferencesOnlyDraft() {
  if (!saved || previewReady || nodeDraft || !hasDraft) return false;
  const selected = selectedPreferences();
  return selected.protocol !== saved.protocol || selected.portMode !== saved.portMode ||
      (selected.portMode === 'fixed' && selected.requestedPort !== saved.requestedPort);
}

function syncActions() {
  const preferencesOnly = preferencesOnlyDraft();
  const autoBlocked = canActivate && !canActivateAutomatically && protocol.value === 'auto';
  apply.disabled = !canApply || !(previewReady || preferencesOnly) || !revision || working ||
      requiresReview || pendingApplyRecovery || !!uncertainOperation || !portError.hidden ||
      autoBlocked;
  discard.disabled = !hasDraft || working || !!uncertainOperation;
  revoke.disabled = !canApply || working || !lastConnection.revision ||
      !lastConnection.revokeToken ||
      !['active', 'prepared', 'publishing', 'uncertain', 'revoking', 'unavailable']
      .includes(lastConnection.state);
  review.hidden = !requiresReview;
  checkApply.hidden = !uncertainOperation;
  checkApply.disabled = working;
  refresh.disabled = !canApply || working;
  if (canApply) {
    availability.textContent = autoBlocked ?
        'Automatic activation is unavailable. Choose HTTP or SOCKS5 to apply.' :
        previewReady ?
        (canActivate ? 'Apply saves this draft and prepares the listener.' :
                       'Apply saves this draft. Connections are not available in this build.') :
        preferencesOnly ?
        'Apply saves these listener preferences for the saved test node.' :
        'Preview a node to apply your draft.';
  }
}

function showConnection(connection: Connection) {
  lastConnection = connection;
  const unavailable = 'Connection status unavailable';
  const states: Record<string, string> = {
    active: 'Active on a protected site',
    prepared: 'Listener prepared',
    pending: 'Preparing connection…',
    publishing: 'Waiting for connection confirmation…',
    inactive: 'No active connection',
    revoking: 'Revoking connection…',
    uncertain: 'Retirement could not be confirmed',
    unavailable,
  };
  const connectionState = requiredElement<HTMLElement>('#connection-state');
  connectionState.dataset['state'] = connection.state;
  connectionState.textContent = states[connection.state] ?? unavailable;
  requiredElement('#connection-hint').textContent =
      connection.state === 'prepared' ?
          'Open a website’s toolbar and choose Protect site with Test Profile.' :
          'Sites that require protection stay blocked if the listener is unavailable.';
  requiredElement('#connection-profile').textContent = saved ? 'Test node' : '—';
  requiredElement('#connection-protocol').textContent = connection.protocol ?? '—';
  requiredElement('#connection-address').textContent = connection.port ?
      '127.0.0.1:' + connection.port : '—';
  syncActions();
}

function showSavedRevision() {
  requiredElement('#saved-revision').textContent = revision ?? 'Not loaded';
}

function restorePreferences() {
  protocol.value = saved?.protocol ?? 'auto';
  portMode.value = saved?.portMode ?? 'random';
  port.value = String(saved?.requestedPort || 10806);
  refreshListenerFields();
}

async function readOptions(replaceDraft: boolean, compare = false) {
  const result = await sendWithPromise<OptionsState>('readAccessOptions');
  if (result.state !== 'ready' || !result.revision) throw new Error('Options unavailable');
  pendingApplyRecovery = result.pendingApplyRecovery === true;
  if (replaceDraft || compare || !revision) {
    revision = result.revision;
    saved = result.testProfile;
    showSavedRevision();
    if (replaceDraft) restorePreferences();
  }
  showConnection(result.connection ?? {state: 'inactive'});
  if (pendingApplyRecovery && !uncertainOperation)
    status.textContent = 'A previous Apply result is still pending. Refresh status before applying again.';
  return result;
}

function cancel() {
  ++generation;
  if (reader?.readyState === FileReader.LOADING) {
    reader.abort();
  }
  reader = null;
  previewReady = false;
  nodePreview.hidden = true;
  apply.disabled = true;
  chrome.send('cancelAccessTestNodePreview');
}

function preview(source: 'text'|'image', data: string, request: number) {
  return sendWithPromise<Preview>('previewAccessTestNode', source, data).then(result => {
    if (request !== generation) {
      return;
    }
    nodePreview.hidden = result.state !== 'preview';
    previewReady = result.state === 'preview';
    const label = result.protocol ?? '';
    previewProtocol.textContent = previewReady ?
        (canImportExtendedVless && extendedProtocols.has(label) ?
             label : realityProtocol) : '';
    syncActions();
    status.dataset['error'] = String(
        result.state !== 'preview' && result.state !== 'canceled');
    const importMessage = canImportExtendedVless ?
        extendedImportMessages[result.state] : undefined;
    status.textContent =
        importMessage ?? messagesByState[result.state] ?? 'Preview unavailable.';
  }).catch(() => {
    if (request === generation) {
      status.textContent = 'Preview unavailable.';
      status.dataset['error'] = 'true';
    }
  });
}

form.addEventListener('submit', event => {
  event.preventDefault();
  if (working || fields.disabled || uncertainOperation) return;
  cancel();
  const input = link.value;
  link.value = '';
  hasDraft = true;
  nodeDraft = true;
  discard.disabled = false;
  draftState.textContent = 'Unsaved draft';
  status.textContent = 'Checking node link…';
  void preview('text', input, generation);
});

link.addEventListener('input', () => {
  if (!nodeDraft || fields.disabled) return;
  cancel();
  status.dataset['error'] = 'false';
  status.textContent = 'Preview the edited link before applying.';
  syncActions();
});

image.addEventListener('change', () => {
  const selected = image.files?.[0];
  image.value = '';
  if (!selected || working || fields.disabled || uncertainOperation) {
    return;
  }
  cancel();
  hasDraft = true;
  nodeDraft = true;
  discard.disabled = false;
  draftState.textContent = 'Unsaved draft';
  if (selected.size === 0 || selected.size > maxImageBytes) {
    status.textContent = messages.invalidImage;
    return;
  }
  const request = generation;
  const current = new FileReader();
  reader = current;
  status.textContent = 'Reading QR code…';
  current.addEventListener('loadend', () => {
    if (request !== generation) {
      return;
    }
    reader = null;
    const encoded = current.result;
    if (typeof encoded !== 'string' || !encoded.includes(',')) {
      status.textContent = messages.invalidImage;
      return;
    }
    void preview('image', encoded.slice(encoded.indexOf(',') + 1), request);
  }, {once: true});
  current.readAsDataURL(selected);
});

function onActionClick(button: HTMLButtonElement, action: () => Promise<void>) {
  button.addEventListener('click', () => {
    void action().catch(() => {
      status.dataset['error'] = 'true';
      status.textContent = 'Access Options is unavailable.';
    });
  });
}

onActionClick(discard, async () => {
  cancel();
  hasDraft = false;
  nodeDraft = false;
  const request = generation;
  link.value = '';
  restorePreferences();
  requiresReview = false;
  review.hidden = true;
  draftState.textContent = saved ? 'Saved preferences' : 'No changes saved';
  status.textContent = 'Discarding preview…';
  discard.disabled = true;
  link.focus();
  try {
    const result = await sendWithPromise<Preview>('discardAccessTestNodeDraft');
    if (request !== generation) {
      return;
    }
    if (finishDiscard(result)) await reloadCommitted(request);
  } catch {
    if (request === generation) {
      status.textContent = messages.secureStorageUnavailable;
      status.dataset['error'] = 'true';
      discard.disabled = false;
      hasDraft = true;
    }
  }
});

function finishDiscard(result: Preview): boolean {
  const cleared = result.state === 'canceled';
  const edited = hasDraft;
  status.textContent = messagesByState[result.state] ?? 'Discard unavailable.';
  status.dataset['error'] = String(!cleared);
  discard.disabled = cleared && !edited;
  hasDraft = edited || !cleared;
  return cleared && canApply && !edited;
}

function reloadCommitted(request: number) {
  return sendWithPromise<OptionsState>('readAccessOptions').then(latest => {
    if (request !== generation || hasDraft) return;
    if (latest.state !== 'ready' || !latest.revision) throw new Error('Options unavailable');
    pendingApplyRecovery = latest.pendingApplyRecovery === true;
    revision = latest.revision;
    saved = latest.testProfile;
    showSavedRevision();
    restorePreferences();
    draftState.textContent = saved ? 'Saved preferences' : 'No changes saved';
    showConnection(latest.connection ?? {state: 'inactive'});
  }).catch(() => {
    if (request === generation) {
      status.textContent = 'Saved settings are unavailable. Try again.';
      status.dataset['error'] = 'true';
    }
  });
}

function refreshListenerFields() {
  const http = protocol.value === 'http';
  fixedPortOption.textContent = http ?
      'Fixed port (internal)' : 'Fixed · 10806 (internal)';
  portHint.textContent = http ?
      'Enter a decimal port from 1 to 65535.' :
      'Fixed Auto and SOCKS5 listeners use 10806.';
  portError.textContent = http ?
      'Enter a decimal port from 1 to 65535.' :
      'Use 10806, or choose a system-allocated port.';
  fixedPortField.hidden = portMode.value !== 'fixed';
  const validHttpPort = /^[0-9]{1,5}$/.test(port.value) &&
      Number(port.value) >= 1 && Number(port.value) <= 65535;
  const invalid = portMode.value === 'fixed' &&
      (http ? !validHttpPort : port.value !== '10806');
  portError.hidden = !invalid;
  if (invalid) {
    port.setAttribute('aria-invalid', 'true');
  } else {
    port.removeAttribute('aria-invalid');
  }
}

function updateListenerDraft() {
  refreshListenerFields();
  draftState.textContent = 'Unsaved draft';
  hasDraft = true;
  syncActions();
}
protocol.addEventListener('change', updateListenerDraft);
portMode.addEventListener('change', updateListenerDraft);
port.addEventListener('input', updateListenerDraft);
port.addEventListener('keydown', event => {
  if (event.key === 'Enter') {
    event.preventDefault();
    updateListenerDraft();
  }
});

async function handleApplyResult(result: ApplyResult, operation: string) {
  status.dataset['error'] = 'false';
  if (result.state === 'uncertain') {
    uncertainOperation = operation;
    status.textContent = 'The save result is unknown. Check the same Apply operation.';
    return;
  }
  uncertainOperation = undefined;
  if (['saved', 'prepared', 'savedUnavailable'].includes(result.state)) {
    revision = result.revision ?? revision;
    if (applying) {
      saved = selectedPreferences();
    }
    showSavedRevision();
    previewReady = false;
    hasDraft = false;
    nodeDraft = false;
    nodePreview.hidden = true;
    draftState.textContent = 'Saved preferences';
    if (result.state === 'prepared') {
      status.textContent = 'Saved. The listener is ready to select in the website toolbar.';
      showConnection(result);
    } else {
      status.textContent = result.state === 'savedUnavailable' ?
          'Saved. ' + (result.reason ?? 'The listener could not be prepared.') :
          'The save was confirmed. Review the saved settings before editing again.';
      if (result.state === 'saved') await readOptions(true);
      else showConnection({state: 'inactive'});
    }
    return;
  }
  status.dataset['error'] = 'true';
  if (result.state === 'automaticProtocolNotAdmitted') {
    status.textContent = 'Automatic activation is unavailable. Choose HTTP or SOCKS5 to apply.';
  } else if (result.reason === 'configuration_revision_conflict') {
    requiresReview = true;
    status.textContent = 'Another window saved changes. Review the saved settings before applying this draft.';
  } else if (result.reason === 'options_unavailable') {
    status.textContent = 'Saved settings could not be read. Try applying this draft again.';
  } else if (result.reason === 'profile_runtime_in_use') {
    status.textContent = 'Revoke the current test connection before replacing its settings.';
  } else if (result.reason === 'active_profile_switch_requires_deactivation') {
    status.textContent =
        'Deactivate the current Profile before activating another. The current connection was not changed.';
  } else {
    status.textContent = 'The draft was not saved. Check the preview and listener preferences.';
  }
}

onActionClick(apply, async () => {
  if (apply.disabled || !revision) return;
  const message = previewReady ? 'applyAccessTestNode' : 'applyAccessTestNodePreferences';
  const operation = crypto.randomUUID();
  working = applying = true;
  fields.disabled = true;
  status.textContent = 'Applying draft…';
  syncActions();
  try {
    const result = await sendWithPromise<ApplyResult>(message,
        revision, operation, protocol.value, portMode.value,
        portMode.value === 'fixed' ? port.value : '');
    await handleApplyResult(result, operation);
  } catch {
    await handleApplyResult({state: 'uncertain'}, operation);
  } finally {
    applying = working = false;
    fields.disabled = !!uncertainOperation;
    syncActions();
  }
});

onActionClick(checkApply, async () => {
  if (!uncertainOperation || working) return;
  const operation = uncertainOperation;
  working = true;
  syncActions();
  try {
    await handleApplyResult(
        await sendWithPromise<ApplyResult>('queryAccessTestNodeApply', operation), operation);
  } catch {
    status.textContent = 'The save result is still unavailable.';
  } finally {
    working = false;
    fields.disabled = !!uncertainOperation;
    syncActions();
  }
});

onActionClick(review, async () => {
  if (working) return;
  try {
    await readOptions(false, true);
    requiresReview = false;
    const label = saved?.protocol.toUpperCase() ?? 'No test node';
    const listener = saved?.portMode === 'fixed' ?
        ' · fixed port ' + saved.requestedPort : ' · system allocated';
    status.textContent = 'Saved: ' + label + (saved ? listener : '') +
        '. Your preview and listener draft are still in this window.';
    syncActions();
  } catch {
    status.textContent = 'Saved settings are unavailable. Try again.';
  }
});

refresh.addEventListener('click', () => {
  if (refresh.disabled) return;
  void readOptions(false).catch(() => {
    showConnection({state: 'unavailable'});
  });
});

onActionClick(revoke, async () => {
  const {revision: connectionRevision, revokeToken} = lastConnection;
  if (revoke.disabled || !connectionRevision || !revokeToken) return;
  working = true;
  showConnection({state: 'revoking', revision: connectionRevision, revokeToken});
  try {
    const result = await sendWithPromise<Connection>(
        'revokeAccessTestNode', connectionRevision, revokeToken);
    if (result.state === 'connectionChanged') {
      showConnection({state: 'unavailable'});
      try {
        await readOptions(false);
        status.textContent = 'The test connection changed. Review its current status before revoking.';
      } catch {
        status.textContent = 'The test connection changed. Refresh its status before revoking.';
      }
      return;
    }
    showConnection(result.state === 'revoked' ? {state: 'inactive'} :
        {state: 'uncertain', revision: connectionRevision, revokeToken});
    status.textContent = result.state === 'revoked' ?
        'Test connection revoked.' : 'Retirement is unconfirmed. Protected sites stay blocked.';
  } catch {
    showConnection({state: 'uncertain', revision: connectionRevision, revokeToken});
  } finally {
    working = false;
    syncActions();
  }
});

window.addEventListener('pagehide', cancel);

void sendWithPromise<{
  enabled: boolean,
  private: boolean,
  testApply?: boolean,
  testActivate?: boolean,
  testAutoProtocol?: boolean,
  testExtendedVless?: boolean,
}>(
    'getAccessOptionsCapabilities').then(async result => {
  if (!result.enabled || result.private) {
    status.textContent = messages.notAdmitted;
    return;
  }
  canApply = result.testApply === true;
  canActivate = canApply && result.testActivate === true;
  canActivateAutomatically = canActivate && result.testAutoProtocol === true;
  canImportExtendedVless = result.testExtendedVless === true;
  if (canImportExtendedVless) {
    requiredElement('#share-label').textContent = 'Node share link';
    requiredElement('#share-hint').textContent =
        'Paste one VLESS link or choose its QR image. Supported modes are ' +
        'TCP/REALITY/Vision, WebSocket/TLS, TCP/TLS/Vision and gRPC/REALITY.';
  }
  if (!canApply) availability.textContent = 'This build supports preview only.';
  if (canApply) {
    await readOptions(true);
  }
  fields.disabled = false;
  syncActions();
}).catch(() => {
  canApply = false;
  fields.disabled = false;
  syncActions();
  status.textContent = 'Access Options is unavailable.';
});
