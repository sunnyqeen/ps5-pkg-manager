import { useState, useRef, useCallback, useEffect } from 'react';
import { initUpload, uploadStatus, cancelUpload, cancelUploadOnUnload, uploadSessionIcon, wsUploadUrl } from '../api/directInstall';
import { pollStatus } from '../api/installer';
import { createSegmentSender } from '../utils/segmentSender';

import { getQueueOwner, attachBrowserJob, cancelQueueJob, getInstallQueue } from '../api/queue';
const getOwner = getQueueOwner;

function waitForMessage(ws, timeoutMs) {
  return new Promise(function (resolve, reject) {
    const timer = setTimeout(function () {
      ws.onmessage = null;
      ws.onerror = null;
      ws.onclose = null;
      reject(new Error('Upload timed out waiting for server'));
    }, timeoutMs || 30000);
    ws.onmessage = function (ev) {
      clearTimeout(timer);
      ws.onclose = null;
      try {
        resolve(JSON.parse(ev.data));
      } catch (e) {
        reject(new Error('Bad server reply'));
      }
    };
    ws.onerror = function () {
      clearTimeout(timer);
      reject(new Error('WebSocket error'));
    };
    // A server-side close (e.g. handshake reject, protocol error) raises
    // onclose, NOT onerror. Without this every such failure masquerades
    // as a 60s timeout.
    ws.onclose = function (ev) {
      clearTimeout(timer);
      reject(new Error('Server closed connection (code ' + (ev && ev.code) + ')'));
    };
  });
}

export function useDirectUpload() {
  const [state, setState] = useState('idle');
  const [uploadSpeed, setUploadSpeed] = useState(0);
  const [offset, setOffset] = useState(0);
  const [total, setTotal] = useState(0);
  const [sessionId, setSessionId] = useState('');
  const [iconUrl, setIconUrl] = useState('');
  const [installing, setInstalling] = useState(false);
  const sessionIdRef = useRef(sessionStorage.getItem('directInstallSession') || '');
  const installCalledRef = useRef(false);
  const installStartedRef = useRef(false);
  const cancelRef = useRef(false);
  // Set only by cancel(), unlike an internal abort such as a failed install start.
  const userCanceledRef = useRef(false);
  const startErrorRef = useRef('');
  // Numbers each upload; cancel() and reset() retire the one in progress so it
  // cannot touch the session of an upload started after it.
  const attemptRef = useRef(0);
  const activeJobRef = useRef(0);
  const wsRef = useRef(null);
  const statusTimerRef = useRef(null);
  const checkingRef = useRef(false);
  const uploadStatusInFlightRef = useRef(false);
  const unloadCancelSentRef = useRef(false);
  const speedTimerRef = useRef(null);
  const uploadRateRef = useRef({ samples: [], lastAt: 0 });

  useEffect(function () {
    const cancelOnUnload = function () {
      if (!installStartedRef.current || cancelRef.current || unloadCancelSentRef.current) return;
      const sid = sessionIdRef.current;
      if (!sid) return;
      unloadCancelSentRef.current = true;
      cancelUploadOnUnload(getOwner(), sid);
    };
    window.addEventListener('pagehide', cancelOnUnload);
    return function () {
      window.removeEventListener('pagehide', cancelOnUnload);
      if (speedTimerRef.current) clearInterval(speedTimerRef.current);
    };
  }, []);

  const stopStatusPoll = useCallback(function () {
    if (statusTimerRef.current) {
      clearInterval(statusTimerRef.current);
      statusTimerRef.current = null;
    }
  }, []);

  const reset = useCallback(function () {
    attemptRef.current++;
    activeJobRef.current = 0;
    checkingRef.current = false;
    installCalledRef.current = false;
    cancelRef.current = false;
    unloadCancelSentRef.current = false;
    stopStatusPoll();
    if (speedTimerRef.current) clearInterval(speedTimerRef.current);
    speedTimerRef.current = null;
    setUploadSpeed(0);
    if (wsRef.current) {
      try { wsRef.current.close(); } catch (e) {}
      wsRef.current = null;
    }
    setState('idle');
    setOffset(0);
    setTotal(0);
    setSessionId('');
    setInstalling(false);
    installStartedRef.current = false;
    sessionIdRef.current = '';
    sessionStorage.removeItem('directInstallSession');
    setIconUrl('');
  }, [stopStatusPoll]);

  const cancel = useCallback(async function () {
    // Retire and detach this attempt before any network await. Late cancel
    // replies must never clear a newer attempt's socket or session.
    const jobId = activeJobRef.current;
    const sid = sessionIdRef.current;
    const socket = wsRef.current;
    attemptRef.current++;
    activeJobRef.current = 0;
    sessionIdRef.current = '';
    wsRef.current = null;
    checkingRef.current = false;
    cancelRef.current = true;
    userCanceledRef.current = true;
    stopStatusPoll();
    if (socket) { try { socket.close(); } catch (e) {} }
    if (speedTimerRef.current) clearInterval(speedTimerRef.current);
    speedTimerRef.current = null;
    sessionStorage.removeItem('directInstallSession');
    setSessionId('');
    setInstalling(false);
    setUploadSpeed(0);
    setState('canceled');
    await Promise.allSettled([
      jobId ? cancelQueueJob(jobId) : Promise.resolve(),
      sid ? cancelUpload(getOwner(), sid) : Promise.resolve(),
    ]);
  }, [stopStatusPoll]);

  // Polls server upload status for header_ready (install can start as soon
  // as the header is parsed, mid-upload) and keeps offset fresh.
  const pollHeader = useCallback(function (sid, isStale, jobId) {
    stopStatusPoll();
    // Ends the upload with `message`; upload() reports it as a failure.
    const fail = function (message) {
      if (isStale()) return;
      stopStatusPoll();
      if (cancelRef.current) return;
      startErrorRef.current = message;
      cancelRef.current = true;
      if (wsRef.current) wsRef.current.close();
    };
    const tick = async function () {
      if (uploadStatusInFlightRef.current) return;
      uploadStatusInFlightRef.current = true;
      try {
        const st = await uploadStatus();
        if (isStale()) return;
        // The session can vanish, e.g. torn down by a previous install's cleanup.
        if (st && sid && (!st.active || (st.session_id && st.session_id !== sid))) {
          fail('The console dropped this upload before installing it. Try it again.');
          return;
        }
        if (st && typeof st.received === 'number') {
          setOffset(st.received);
        }
        if (st && st.header_ready) {
          stopStatusPoll();
          if (!installCalledRef.current && !cancelRef.current) {
            installCalledRef.current = true;
            try {
              const current = await pollStatus();
              if (isStale()) return;
              if (!(current?.is_installing && current.pkg_path === 'live:' + sid)) {
                await attachBrowserJob(jobId, getOwner(), 'live:' + sid);
              }
              if (isStale()) return;
              setInstalling(true);
              installStartedRef.current = true;
            } catch (e) {
              fail(e.message || 'Could not start installation');
              await cancelUpload(getOwner(), sid).catch(function () {});
            }
          }
        }
      } catch (e) {} finally {
        uploadStatusInFlightRef.current = false;
      }
    };
    tick();
    statusTimerRef.current = setInterval(tick, 2000);
  }, [stopStatusPoll]);

  // Streams one parsed package ({ file, details, iconUrl }; the caller owns iconUrl).
  // Resolves to { outcome: 'complete' | 'skipped' | 'failed' | 'canceled', error }.
  const upload = useCallback(async function (item) {
    if (!item || !item.file || !item.details || checkingRef.current) return { outcome: 'skipped', error: '' };
    activeJobRef.current = item.jobId;
    const file = item.file;
    const pkg = item.details;
    const canceled = { outcome: 'canceled', error: '' };
    const attempt = ++attemptRef.current;
    const isStale = function () { return attemptRef.current !== attempt; };
    const halted = function () { return isStale() || cancelRef.current; };
    cancelRef.current = false;
    userCanceledRef.current = false;
    startErrorRef.current = '';
    setIconUrl(item.iconUrl || '');
    setTotal(file.size);
    setState('uploading');
    setUploadSpeed(0);
    uploadRateRef.current = { samples: [], lastAt: 0 };
    if (speedTimerRef.current) clearInterval(speedTimerRef.current);
    speedTimerRef.current = setInterval(function () {
      const now = Date.now();
      const rate = uploadRateRef.current;
      const recent = rate.samples.filter((sample) => now - sample.time <= 2000);
      rate.samples = recent;
      const activeUs = recent.reduce((sum, sample) => sum + sample.receiveUs, 0);
      const receivedBytes = recent.reduce((sum, sample) => sum + sample.bytes, 0);
      setUploadSpeed(rate.lastAt && now - rate.lastAt <= 1500 && activeUs > 0
        ? receivedBytes * 1000000 / activeUs : 0);
    }, 500);
    installCalledRef.current = false;
    installStartedRef.current = false;
    const SEG = 1024 * 1024;
    const NSEGS = Math.max(1, Math.ceil(file.size / SEG));
    // Coordinate bounded read-ahead with seek requests from installer
    // workers. Keep a bounded number in flight, retry busy writes, and leave the
    // socket open to serve seeks until the installation finalizes.
    let sender = null;
    let demandMode = false;
    let baselineSeg = 0;
    const ackedSet = new Set();
    let transportEnded = false;
    let done = false;     // terminal: completed / failed / canceled
    let finishTimer = null;
    let watchdogTimer = null;
    let lastActivityAt = 0;
    let ws = null;
    let completionResolve = null;
    let completionReject = null;
    let terminalCheck = null;
    const completion = new Promise(function (res, rej) {
      completionResolve = res;
      completionReject = rej;
    });
    // A socket can reject before the initial sender pump has returned.
    completion.catch(() => {});
    const fail = function (err) {
      if (!done) {
        done = true;
        completionReject(err instanceof Error ? err : new Error(String(err)));
      }
    };

    // Installer cleanup can close the RAM session before the next queue poll.
    // Check this job's outcome before treating that close as a failed upload.
    const transportEnd = function (error) {
      transportEnded = true;
      if (done) return;
      if (halted()) { fail(new Error('Canceled')); return; }
      if (terminalCheck) return;
      terminalCheck = (async () => {
        for (let n = 0; n < 10 && !done && !halted(); n++) {
          try {
            const queue = await getInstallQueue();
            const job = queue.jobs.find((entry) => entry.id === item.jobId);
            if (job?.state === 'completed') { done = true; completionResolve(''); return; }
            if (!job || ['failed', 'canceled', 'blocked', 'aborted'].includes(job.state)) { fail(new Error(job?.error || error.message)); return; }
          } catch (e) { /* A real disconnection eventually aborts this job. */ }
          await new Promise((resolve) => setTimeout(resolve, 200));
        }
        if (!done) fail(error);
      })();
    };

    const recordReceiveRate = function (msg) {
      const bytes = Number(msg.rx_bytes);
      const receiveUs = Number(msg.rx_us);
      if (!Number.isFinite(bytes) || !Number.isFinite(receiveUs) || bytes <= 0 || receiveUs <= 0) return;
      const now = Date.now();
      const rate = uploadRateRef.current;
      rate.samples.push({ bytes, receiveUs, time: now });
      rate.samples = rate.samples.filter((sample) => now - sample.time <= 2000);
      rate.lastAt = now;
    };

    const installDispatcher = function () {
      lastActivityAt = Date.now();
      ws.onmessage = function (ev) {
        if (typeof ev.data !== 'string' || done || halted()) return;
        lastActivityAt = Date.now();
        let msg = null;
        try {
          msg = JSON.parse(ev.data);
        } catch (e) {
          fail(new Error('Bad server reply'));
          return;
        }
        if (!msg || typeof msg.op !== 'string') {
          fail(new Error('Bad server reply'));
          return;
        }
        if (msg.op === 'ack' && typeof msg.seg === 'number') {
          recordReceiveRate(msg);
          if (!ackedSet.has(msg.seg)) {
            ackedSet.add(msg.seg);
            setOffset(Math.min(file.size, ackedSet.size * SEG));
          }
          sender.ack(msg.seg);
        } else if (msg.op === 'busy' && typeof msg.seg === 'number') {
          recordReceiveRate(msg);
          sender.busy(msg.seg);
        } else if (msg.op === 'seek' && typeof msg.seg === 'number') {
          sender.request(msg.seg);
        } else if (msg.op === 'pong' && demandMode) {
          // Keep the socket alive while the installer is not requesting data.
        } else if (msg.op === 'complete') {
          // Upload completion alone is not the install outcome. Await this job.
        } else if (msg.op === 'error') {
          transportEnd(new Error(msg.error || 'Server rejected chunk'));
        } else {
          fail(new Error('Bad server reply'));
        }
      };
      ws.onerror = function () {
        transportEnd(new Error('WebSocket error'));
      };
      ws.onclose = function (ev) {
        // Fires on unexpected drops AND on our own post-complete close;
        // fail() no-ops once done.
        transportEnd(new Error('Server closed connection (code ' + (ev && ev.code) + ')'));
      };
    };

    // Silent-socket watchdog: without it a dead connection (no TCP
    // close, e.g. dropped Wi-Fi) hangs the UI forever, since the
    // dispatcher itself has no timeouts. Demand mode uses a heartbeat
    // during installer pauses; legacy sequential mode exempts pure standby.
    // Pending work or a heartbeat must receive a reply within 60 s.
    const watchTraffic = function () {
      watchdogTimer = setInterval(function () {
        if (done || halted()) {
          if (watchdogTimer) {
            clearInterval(watchdogTimer);
            watchdogTimer = null;
          }
          return;
        }
        const pending = demandMode || sender.hasPending();
        if (demandMode && Date.now() - lastActivityAt > 15000 && ws.readyState === WebSocket.OPEN) {
          try { ws.send(JSON.stringify({ op: 'ping' })); } catch (e) { fail(e); }
        }
        if (pending && Date.now() - lastActivityAt > 60000) {
          fail(new Error('Upload stalled: no server reply for 60s'));
        }
      }, 5000);
    };

    // The backend job remains active through all installer cleanup. Observe
    // this job's terminal outcome, never another installation's global status.
    const watchFinish = function () {
      finishTimer = setInterval(async function () {
        if (done || halted()) return;
        let queue;
        try { queue = await getInstallQueue(); } catch (e) { return; }
        if (done || halted()) return;
        const job = queue.jobs.find((entry) => entry.id === item.jobId);
        if (!job || ['failed', 'blocked', 'canceled', 'aborted'].includes(job.state)) {
          fail(new Error(job?.error || 'Browser installation was canceled'));
        } else if (job.state === 'completed') {
          done = true;
          sender.flushStats();
          setInstalling(false);
          completionResolve('');
        }
      }, 1000);
    };

    // Stops this upload's own timers and socket. The shared status poll, speed
    // meter and socket ref are left alone once a newer upload owns them.
    const cleanup = function () {
      if (sender) sender.stop();
      if (finishTimer) clearInterval(finishTimer);
      if (watchdogTimer) clearInterval(watchdogTimer);
      finishTimer = null;
      watchdogTimer = null;
      if (ws) {
        try { ws.close(); } catch (e) {}
      }
      if (isStale()) return;
      stopStatusPoll();
      if (speedTimerRef.current) clearInterval(speedTimerRef.current);
      speedTimerRef.current = null;
      setUploadSpeed(0);
      wsRef.current = null;
    };

    try {
      const init = await initUpload(file.name, file.size, getOwner(), '', { ...pkg, queue_id: item.jobId });
      if (isStale() || userCanceledRef.current) {
        // Canceled while the session was being created: release it now.
        if (init.session_id) await cancelUpload(getOwner(), init.session_id).catch(function () {});
        throw new Error('Canceled');
      }
      baselineSeg = Math.ceil((init.offset || 0) / SEG);
      for (let s = 0; s < baselineSeg; s++) ackedSet.add(s);
      sessionIdRef.current = init.session_id || '';
      sessionStorage.setItem('directInstallSession', sessionIdRef.current);
      setSessionId(init.session_id || '');
      setOffset(Math.min(file.size, baselineSeg * SEG));
      if (init.session_id && pkg.icon_size) {
        const slice = file.slice(pkg.icon_offset, pkg.icon_offset + pkg.icon_size, 'image/png');
        try { await uploadSessionIcon(getOwner(), init.session_id, slice); } catch (e) {}
      }
      if (isStale() || userCanceledRef.current || cancelRef.current) throw new Error('Canceled');
      if (init.session_id) pollHeader(init.session_id, isStale, item.jobId);

      ws = new WebSocket(wsUploadUrl(init.ws_port || 18842));
      wsRef.current = ws;
      await new Promise(function (resolve, reject) {
        ws.onopen = resolve;
        ws.onerror = function () { reject(new Error('Cannot reach upload socket')); };
        ws.onclose = function (ev) { reject(new Error('Server closed connection (code ' + (ev && ev.code) + ')')); };
      });

      // Confirm over the socket (idempotent: same file+total resumes).
      ws.send(JSON.stringify({ op: 'init', filename: file.name, total: file.size,
        owner: getOwner(), session_id: init.session_id }));
      const ready = await waitForMessage(ws, 30000);
      if (ready.op === 'error') throw new Error(ready.error || 'Server refused upload');
      if (ready.op !== 'ready') throw new Error('Bad server reply to init');

      demandMode = Number.isInteger(ready.demand_window) && ready.demand_window > 0;
      sender = createSegmentSender({
        totalSegments: NSEGS,
        startSegment: baselineSeg,
        maxInFlight: ready.upload_window === 2 ? 2 : 1,
        onStats: (stats) => {
          if (ready.upload_window === 2 && ws.readyState === WebSocket.OPEN) {
            try { ws.send(JSON.stringify({ op: 'sender_stats', ...stats })); } catch (e) { fail(e); }
          }
        },
        demandWindow: Number.isInteger(ready.demand_window) ? Math.min(8, Math.max(0, ready.demand_window)) : 0,
        acknowledged: ackedSet,
        readSegment: (segment) => file.slice(segment * SEG, Math.min((segment + 1) * SEG, file.size)).arrayBuffer(),
        sendSegment: (segment, buffer) => {
          ws.send(JSON.stringify({ op: 'seg', seg: segment }));
          ws.send(buffer);
        },
        shouldStop: () => done || transportEnded || halted(),
        onError: transportEnd
      });
      installDispatcher();
      watchTraffic();
      watchFinish();
      await sender.pump();
      await completion;
      cleanup();
      if (isStale()) return canceled;
      setState('complete');
      return { outcome: 'complete', error: '' };
    } catch (e) {
      cleanup();
      if (isStale() || userCanceledRef.current) return canceled;
      setState('error');
      return { outcome: 'failed', error: startErrorRef.current || e.message || 'Upload failed' };
    }
  }, [pollHeader]);

  return { state, offset, total, uploadSpeed, sessionId, iconUrl, installing, upload, cancel, reset };
}
