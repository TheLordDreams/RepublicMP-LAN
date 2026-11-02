// SPDX-License-Identifier: GPL-3.0-only
// TCP stream shaping for disposable QA clients. CLI ports are fixed and both
// endpoints are loopback. Delay/fragmentation preserve every byte and its order.
import net from 'node:net';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const local = path.join(repo, 'local');
const MAX_CHUNK = 256, HIGH = 256 * 1024, LOW = 64 * 1024, LIMIT = 1024 * 1024;
const MAX_CONNECTIONS = 8, MAX_TOTAL_CONNECTIONS = 128;
const CONNECT_TIMEOUT_MS = 5000, CONNECT_RETRY_MS = 100;
const inside = (root, candidate) => {
  const r = path.relative(root, candidate);
  return r === '' || (!r.startsWith('..' + path.sep) && r !== '..' && !path.isAbsolute(r));
};
function privateDirectory(requested) {
  if (!requested) throw Error('--run-directory is required');
  const directory = path.resolve(requested);
  if (!inside(local, directory) || directory === local) throw Error('Run directory must be a child of repo/local');
  fs.mkdirSync(local, { recursive: true });
  const realLocal = fs.realpathSync(local);
  if (!inside(fs.realpathSync(repo), realLocal)) throw Error('local resolves outside repository');
  // Check the nearest existing ancestor before creating anything beneath it.
  let ancestor = directory;
  while (!fs.existsSync(ancestor)) ancestor = path.dirname(ancestor);
  if (!inside(realLocal, fs.realpathSync(ancestor))) throw Error('Run directory ancestor escapes local');
  fs.mkdirSync(directory, { recursive: true });
  const real = fs.realpathSync(directory);
  if (!inside(realLocal, real)) throw Error('Run directory escapes local');
  return real;
}
function port(value, allowZero = false) {
  if (!Number.isInteger(value) || value < (allowZero ? 0 : 1) || value > 65535) throw Error('Invalid loopback port');
  return value;
}

// Library port overrides permit isolated tests on ephemeral ports; the CLI
// deliberately exposes no host/port overrides. No non-loopback host is accepted.
export async function startProxy({ runDirectory, profile = 'clean', disconnectAfterMs = 0,
  listenPort = 5561, targetPort = 5560 } = {}) {
  if (!['clean', 'lag-jitter'].includes(profile)) throw Error('Profile must be clean or lag-jitter');
  if (!Number.isInteger(disconnectAfterMs) || disconnectAfterMs < 0 || disconnectAfterMs > 3600000)
    throw Error('disconnect-after-ms must be 0..3600000');
  port(listenPort, true); port(targetPort);
  const directory = privateDirectory(runDirectory);
  const readyFile = path.join(directory, 'tcp-chaos-proxy.ready.json');
  const summaryFile = path.join(directory, 'tcp-chaos-proxy.summary.json');
  for (const file of [readyFile, summaryFile]) if (fs.existsSync(file)) throw Error('Use a fresh proxy run directory');
  const startedAt = new Date().toISOString(), pairs = new Set(), history = [];
  let stopping = false, reason = '', serverError = '', rejectedConnections = 0, nextId = 0, injectedDisconnects = 0;
  let finishResolve;
  const closed = new Promise(resolve => { finishResolve = resolve; });
  let finished = false;
  function finish() {
    if (finished) return;
    finished = true;
    const summary = { schema: 1, pid: process.pid, profile, startedAt, finishedAt: new Date().toISOString(),
      reason, serverError, rejectedConnections, disconnectAfterMs, injectedDisconnects, maxChunk: MAX_CHUNK,
      bufferLimitPerDirection: LIMIT, connections: history };
    try {
      fs.writeFileSync(summaryFile + '.tmp', JSON.stringify(summary, null, 2) + '\n', { flag: 'wx' });
      fs.renameSync(summaryFile + '.tmp', summaryFile);
    } catch (error) { summary.summaryWriteError = String(error); }
    finishResolve(summary);
  }
  const server = net.createServer({ allowHalfOpen: true }, source => {
    if (stopping || pairs.size >= MAX_CONNECTIONS || history.length >= MAX_TOTAL_CONNECTIONS) {
      ++rejectedConnections; source.destroy(); return;
    }
    const id = ++nextId;
    // Native rejoin can finish loading before the host starts listening again.
    // Hold the client stream until one actual upstream connection succeeds.
    source.pause(); source.setNoDelay(true);
    let target = null;
    const record = { id, startedAt: new Date().toISOString(), connected: false, endedAt: null,
      endReason: '', errors: [], connectionAttempts: 0, upstreamRefusals: 0,
      clientToHost: {}, hostToClient: {} };
    history.push(record);
    let ended = false, disconnectTimer = null, retryTimer = null, connectTimer = null;
    let sourceEnded = false, targetEnded = false, sourceClosed = false, targetClosed = false;
    const pair = { stop };
    pairs.add(pair);
    let forward = null, reverse = null;
    function stop(why) {
      if (ended) return;
      ended = true;
      clearTimeout(disconnectTimer);
      clearTimeout(retryTimer); clearTimeout(connectTimer);
      forward?.dispose(); reverse?.dispose();
      record.endReason = why; record.endedAt = new Date().toISOString();
      source.destroy(); target?.destroy(); pairs.delete(pair);
    }
    source.on('error', e => { record.errors.push('client: ' + e.code); stop('client-error'); });
    source.on('end', () => { sourceEnded = true; });
    // A normal socket close after EOF must not discard its delayed reverse
    // queue. Its bytes may still be waiting to drain to the other socket.
    source.on('close', hadError => {
      sourceClosed = true;
      if (hadError || !sourceEnded) stop('client-close');
      else if (targetClosed) stop('complete');
    });
    connectTimer = setTimeout(() => {
      record.errors.push('target: connect-timeout'); stop('target-connect-timeout');
    }, CONNECT_TIMEOUT_MS);
    function connectTarget() {
      if (ended) return;
      ++record.connectionAttempts;
      const attempt = net.createConnection({ host: '127.0.0.1', port: targetPort, allowHalfOpen: true });
      target = attempt; attempt.setNoDelay(true);
      let connected = false;
      attempt.on('error', e => {
        if (ended) return;
        if (!connected && e.code === 'ECONNREFUSED') {
          ++record.upstreamRefusals;
          attempt.destroy();
          retryTimer = setTimeout(connectTarget, CONNECT_RETRY_MS);
          return;
        }
        record.errors.push('target: ' + e.code); stop('target-error');
      });
      attempt.on('end', () => { if (connected) targetEnded = true; });
      attempt.on('close', hadError => {
        // Refused attempts are replaced; they never owned a forwarding stream.
        if (!connected || ended) return;
        targetClosed = true;
        if (hadError || !targetEnded) stop('target-close');
        else if (sourceClosed) stop('complete');
      });
      attempt.on('connect', () => {
        if (ended) { attempt.destroy(); return; }
        connected = true;
        clearTimeout(connectTimer); clearTimeout(retryTimer);
        record.connected = true;
        forward = direction(source, attempt, record.clientToHost, id * 2);
        reverse = direction(attempt, source, record.hostToClient, id * 2 + 1);
        source.resume();
        // One fault per run: a reconnect must remain available for recovery.
        if (disconnectAfterMs && !injectedDisconnects) disconnectTimer = setTimeout(() => {
          if (!ended && !injectedDisconnects) {
            ++injectedDisconnects;
            stop('injected-disconnect');
          }
        }, disconnectAfterMs);
      });
    }
    connectTarget();

    function direction(input, output, metrics, seed) {
      Object.assign(metrics, { readBytes: 0, forwardedBytes: 0, fragments: 0, maxQueuedBytes: 0,
        pendingBytesAtClose: 0, pauses: 0 });
      let queue = [], head = 0, queued = 0, waitingDrain = false, inputEnded = input.readableEnded;
      let disposed = false, timer = null, lastDue = 0, state = seed >>> 0;
      function jitter() { state = (Math.imul(state, 1664525) + 1013904223) >>> 0; return state % 101; }
      function flow() {
        if (disposed) return;
        if (queued >= HIGH || waitingDrain) {
          if (!input.isPaused()) { input.pause(); ++metrics.pauses; }
        } else if (queued <= LOW && !inputEnded) input.resume();
      }
      function pump() {
        clearTimeout(timer); timer = null;
        if (disposed || waitingDrain) return;
        while (head < queue.length) {
          const next = queue[head], remaining = next.due - performance.now();
          if (remaining > 0) { timer = setTimeout(pump, remaining); flow(); return; }
          ++head; queued -= next.data.length;
          // write(false) still accepts these bytes. Wait for drain before the
          // next write, keeping the socket's own writable buffer bounded too.
          const writable = output.write(next.data);
          metrics.forwardedBytes += next.data.length; ++metrics.fragments;
          if (!writable) { waitingDrain = true; break; }
        }
        if (head === queue.length) { queue = []; head = 0; }
        else if (head > 1024) { queue = queue.slice(head); head = 0; }
        flow();
        if (inputEnded && queued === 0 && !waitingDrain) output.end();
      }
      input.on('data', data => {
        if (disposed) return;
        metrics.readBytes += data.length;
        if (queued + data.length > LIMIT) { record.errors.push('bounded-buffer-limit'); stop('buffer-limit'); return; }
        const arrived = performance.now();
        for (let offset = 0; offset < data.length; offset += MAX_CHUNK) {
          const due = profile === 'clean' ? arrived : arrived + 120 + jitter();
          lastDue = Math.max(lastDue, due); // FIFO even when later jitter is lower.
          const chunk = data.subarray(offset, Math.min(offset + MAX_CHUNK, data.length));
          queue.push({ data: chunk, due: lastDue }); queued += chunk.length;
        }
        metrics.maxQueuedBytes = Math.max(metrics.maxQueuedBytes, queued);
        flow(); pump();
      });
      input.on('end', () => { inputEnded = true; pump(); });
      output.on('drain', () => { waitingDrain = false; pump(); });
      if (inputEnded) pump();
      return { dispose() {
        disposed = true; clearTimeout(timer);
        metrics.pendingBytesAtClose = queued;
        queue = []; queued = 0;
      } };
    }
  });
  function stop(why = 'requested-stop') {
    if (stopping) return closed;
    stopping = true; reason = why;
    // An explicit stop is a connection failure, not simulated packet loss.
    // Account queued bytes, close our sockets, then write the private summary.
    for (const pair of [...pairs]) pair.stop('proxy-stop');
    server.close(finish);
    return closed;
  }
  server.on('error', error => {
    serverError = error.code ?? String(error);
    if (!stopping) { stopping = true; reason = 'server-error'; for (const pair of [...pairs]) pair.stop('server-error'); }
    if (server.listening) server.close(finish); else finish();
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen({ host: '127.0.0.1', port: listenPort }, () => { server.removeListener('error', reject); resolve(); });
  });
  const ready = { schema: 1, pid: process.pid, profile, startedAt,
    listen: { host: '127.0.0.1', port: server.address().port }, target: { host: '127.0.0.1', port: targetPort },
    maxChunk: MAX_CHUNK, baseDelayMs: profile === 'clean' ? 0 : 120,
    jitterMs: profile === 'clean' ? 0 : [0, 100], fifo: true, disconnectAfterMs,
    maxConnections: MAX_CONNECTIONS, maxTotalConnections: MAX_TOTAL_CONNECTIONS, bufferLimitPerDirection: LIMIT,
    targetConnectTimeoutMs: CONNECT_TIMEOUT_MS, targetConnectRetryMs: CONNECT_RETRY_MS };
  try {
    fs.writeFileSync(readyFile + '.tmp', JSON.stringify(ready, null, 2) + '\n', { flag: 'wx' });
    fs.renameSync(readyFile + '.tmp', readyFile);
  } catch (error) { await stop('ready-file-error'); throw error; }
  return { ready, closed, stop };
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const args = process.argv.slice(2), options = {};
  try {
    for (let i = 0; i < args.length; ++i) {
      const key = args[i], value = args[++i];
      if (!value) throw Error('Missing option value: ' + key);
      if (key === '--run-directory') options.runDirectory = value;
      else if (key === '--profile') options.profile = value;
      else if (key === '--disconnect-after-ms' && /^\d+$/.test(value)) options.disconnectAfterMs = Number(value);
      else throw Error('Unknown option: ' + key);
    }
    const proxy = await startProxy(options);
    console.log(JSON.stringify({ event: 'ready', ...proxy.ready }));
    for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, () => proxy.stop(signal));
    process.on('message', message => { if (message === 'stop' || message?.type === 'stop') proxy.stop('ipc-stop'); });
    process.on('disconnect', () => proxy.stop('parent-disconnected'));
    const summary = await proxy.closed;
    if (process.connected) process.disconnect();
    console.log(JSON.stringify({ event: 'stopped', reason: summary.reason }));
    if (summary.serverError || summary.summaryWriteError) process.exitCode = 1;
  } catch (error) { console.error(String(error.stack ?? error)); process.exitCode = 1; }
}
