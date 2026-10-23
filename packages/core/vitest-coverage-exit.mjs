import {realpathSync} from 'node:fs';

// Vitest 4 acknowledges fork teardown over IPC before its parent sends SIGTERM.
// Preload before its process.exit interception, only in that exact worker.
let isVitestFork = false;
if (process.env.NODE_V8_COVERAGE && typeof process.send === 'function' && process.argv[1]) {
  isVitestFork = realpathSync(process.argv[1]) ===
    realpathSync(new URL('./node_modules/vitest/dist/workers/forks.js', import.meta.url));
}
if (isVitestFork) {
  const exit = process.exit.bind(process);
  const send = process.send;
  let stopRequested = false;
  let stopped = false;
  process.on('message', message => {
    if (message?.__vitest_worker_request__ !== true) return;
    stopRequested = message.type === 'stop';
    stopped = false;
  });
  process.send = function (message, ...args) {
    if (stopRequested && message?.__vitest_worker_response__ === true && message.type === 'stopped') {
      // The pool normally ignores the exit status after its stop acknowledgement.
      // Report an already-failing worker through the existing teardown error path.
      if (Number(process.exitCode ?? 0) !== 0 && message.error == null) {
        message = {...message, error: {
          name: 'Error',
          message: `Vitest worker requested nonzero exit code ${process.exitCode}`,
        }};
      }
      stopped = message.error == null;
    }
    try {
      return Reflect.apply(send, this, [message, ...args]);
    } catch (error) {
      stopped = false;
      throw error;
    }
  };
  process.on('SIGTERM', () => {
    // Preserve failure; an early signal cannot accept unfinished tests/teardown.
    // Vitest's original 500 ms SIGKILL fallback remains in the parent.
    const code = process.exitCode ?? 0;
    exit(Number(code) !== 0 ? code : stopped ? 0 : 143);
  });
}
