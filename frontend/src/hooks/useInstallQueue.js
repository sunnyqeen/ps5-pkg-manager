import { useCallback, useEffect, useRef, useState } from 'react';
import { enqueueJobs, getInstallQueue, getQueueOwner, cancelQueueJob, retryQueueJob,
  clearFinishedJobs, heartbeatQueue, disconnectQueueSource, packageJob } from '../api/queue';
import { checkUploadEligibility, probeRemotePackage } from '../api/directInstall';
import { parseLocalPkg } from '../utils/parseLocalPkg';
import { passthroughJobPath } from '../utils/remoteHttpsFile';
import { fileKey, pendingStates, terminalStates, queueOverview, orderBrowserFiles, canQueueBrowserFile, canQueueRemote } from '../utils/installQueue';
import { useWakeLock } from './useWakeLock';
import { getBrowserTitle, formatInstallTitle } from '../utils/title';

export function useInstallQueue(upload, { showToast, onResolved, appVersion }) {
  const [snapshot, setSnapshot] = useState({ jobs: [], active_id: 0 });
  const [connected, setConnected] = useState(true);
  const [files, setFiles] = useState([]);
  const [remotes, setRemotes] = useState([]);
  const [skipped, setSkipped] = useState([]);
  const latest = useRef({ upload, showToast, onResolved, files, snapshot });
  latest.current = { upload, showToast, onResolved, files, snapshot };
  const filesRef = useRef(new Map());
  const remotesRef = useRef(new Map());
  const enqueuingFiles = useRef(new Map());
  const uploading = useRef(0);
  const started = useRef(new Map());
  const refreshSerial = useRef(0);
  const seen = useRef(new Map());
  const rate = useRef({ id: 0, bytes: 0, time: 0, speed: 0 });
  const owner = getQueueOwner();
  const overview = queueOverview(snapshot.jobs);
  useWakeLock(snapshot.jobs.some((job) => job.source_id === owner.slice(0, 16) && pendingStates.has(job.state)));

  const refresh = useCallback(async () => {
    const serial = ++refreshSerial.current;
    const admissions = new Map(enqueuingFiles.current);
    try {
      const data = await getInstallQueue();
      if (serial !== refreshSerial.current) return;
      if (!Array.isArray(data.jobs)) throw new Error('Queue unavailable');
      data.jobs.sort((a, b) => a.order - b.order);
      latest.current.snapshot = data;
      // Release reservations only after a successful poll started after admission.
      // A lost or superseded refresh must not make an accepted source clearable.
      admissions.forEach((jobId, id) => {
        if (jobId && enqueuingFiles.current.get(id) === jobId) enqueuingFiles.current.delete(id);
      });
      setSnapshot(data);
      setConnected(true);
      let filesChanged = false;
      for (const job of data.jobs) {
        const before = seen.current.get(job.id);
        if (before !== job.state && job.state === 'aborted' && job.source_id === owner.slice(0, 16)) {
          const local = filesRef.current.get(job.file_key);
          if (local) {
            local.eligibility = { ...local.eligibility, can_install: false, install_disabled_reason: job.error };
            filesChanged = true;
          }
        }
        if (before && before !== job.state && terminalStates.has(job.state)) {
          latest.current.showToast(`${job.title_name || job.path || 'Package'}: ${job.state === 'submitted' ? 'submitted to PS5' : job.state}`, job.state === 'failed' ? 'error' : 'info');
          latest.current.onResolved?.();
        }
        seen.current.set(job.id, job.state);
      }
      if (filesChanged) setFiles(Array.from(filesRef.current.values()));
      const active = data.jobs.find((job) => job.id === data.active_id);
      const now = Date.now();
      const before = rate.current;
      const speed = active && before.id === active.id && now > before.time
        ? Math.max(0, active.downloaded_bytes - before.bytes) * 1000 / (now - before.time) : 0;
      rate.current = { id: active?.id || 0, bytes: active?.downloaded_bytes || 0, time: now, speed };
      if (active?.source_id === owner.slice(0, 16) && active.state === 'preparing' && !uploading.current && started.current.get(active.id) !== active.order) {
        const local = filesRef.current.get(active.file_key);
        if (!local?.details) { await cancelQueueJob(active.id); return; }
        uploading.current = active.id;
        started.current.set(active.id, active.order);
        latest.current.upload.reset();
        latest.current.upload.upload({ ...local, jobId: active.id }).then(async (result) => {
          if (result.outcome !== 'complete') {
            await cancelQueueJob(active.id).catch(() => {});
            if (result.error) latest.current.showToast(result.error, 'error');
          }
        }).finally(() => { if (uploading.current === active.id) uploading.current = 0; });
      }
      if (uploading.current) {
        const job = data.jobs.find((entry) => entry.id === uploading.current);
        if (!job || ['canceled', 'failed', 'blocked', 'canceling', 'aborted'].includes(job.state)) latest.current.upload.cancel();
      }
    } catch (e) { if (serial === refreshSerial.current) setConnected(false); }
  }, [owner]);

  useEffect(() => {
    let stopped = false;
    let timer;
    const poll = async () => {
      await refresh();
      if (!stopped) timer = setTimeout(poll, 1000);
    };
    poll();
    const heartbeat = setInterval(() => heartbeatQueue().catch(() => {}), 3000);
    const unload = () => disconnectQueueSource();
    window.addEventListener('pagehide', unload);
    return () => { stopped = true; clearTimeout(timer); clearInterval(heartbeat); window.removeEventListener('pagehide', unload); };
  }, [refresh]);

  useEffect(() => {
    const active = snapshot.jobs.find((job) => job.id === snapshot.active_id) ||
      snapshot.jobs.find((job) => ['installing', 'preparing'].includes(job.state));
    const isInstalling = Boolean(active && (active.state === 'installing' || active.state === 'preparing'));

    if (!isInstalling) {
      document.title = getBrowserTitle(appVersion);
      return undefined;
    }

    const currentRun = Math.max(0, ...snapshot.jobs.map((j) => j.run_id || 0));
    const batchJobs = snapshot.jobs.filter((j) => (j.run_id || 0) === currentRun && j.state !== 'canceled');
    const resolvedInBatch = batchJobs.filter((j) => terminalStates.has(j.state)).length;
    const totalCount = batchJobs.length;
    const currentIndex = Math.min(totalCount, resolvedInBatch + 1);

    document.title = formatInstallTitle({
      activeJob: active,
      totalCount,
      currentIndex,
      speed: rate.current.speed,
      appVersion,
    });

    return () => { document.title = getBrowserTitle(appVersion); };
  }, [snapshot, appVersion]);

  useEffect(() => () => {
    filesRef.current.forEach((file) => { if (file.iconUrl) URL.revokeObjectURL(file.iconUrl); });
    filesRef.current.clear();
  }, []);

  const perform = useCallback(async (action) => {
    try { const result = await action(); await refresh(); return result; }
    catch (e) { latest.current.showToast(e.message, 'error'); return null; }
  }, [refresh]);

  const enqueuePackages = useCallback((packages) => perform(async () => {
    const result = await enqueueJobs(packages.map(packageJob));
    latest.current.showToast(`${result.ids.length} package${result.ids.length === 1 ? '' : 's'} added to install queue`, 'success');
    return result;
  }), [perform]);

  const addFiles = useCallback(async (found, unreadable = []) => {
    if (unreadable.length) setSkipped((list) => [...list, ...unreadable]);
    const added = [];
    for (const entry of found) {
      const id = fileKey(entry);
      if (filesRef.current.has(id)) continue;
      const local = { ...entry, id, status: 'reading' };
      filesRef.current.set(id, local);
      added.push(local);
    }
    setFiles(Array.from(filesRef.current.values()));
    // Bound package reads while preserving the selected-file order.
    const pending = [...added];
    const inspect = async () => {
      while (pending.length) {
        const local = pending.shift();
        if (filesRef.current.get(local.id) !== local) continue;
        try {
          local.details = await parseLocalPkg(local.file);
          if (filesRef.current.get(local.id) !== local) continue;
          if (local.details.icon_size > 0) {
            const start = local.details.icon_offset;
            const end = start + local.details.icon_size;
            const slice = local.file.slice(start, end, 'image/png');
            local.iconUrl = URL.createObjectURL(slice);
          }
          local.eligibility = await checkUploadEligibility(local.details);
          if (filesRef.current.get(local.id) !== local) continue;
          local.status = 'ready';
        } catch (e) {
          if (filesRef.current.get(local.id) !== local) continue;
          local.status = 'error'; local.error = e.message;
        }
        setFiles(Array.from(filesRef.current.values()));
      }
    };
    await Promise.all(Array.from({ length: Math.min(3, pending.length) }, inspect));
  }, []);

  const enqueueFiles = useCallback(async (ids) => {
    const selected = ids.map((id) => filesRef.current.get(id)).filter((local) => local?.details && !enqueuingFiles.current.has(local.id));
    if (!selected.length) return null;
    // Retain upload sources while admission is in flight, before jobs appear in a poll.
    selected.forEach((local) => enqueuingFiles.current.set(local.id, 0));
    const result = await perform(async () => {
      const pending = [...selected];
      const inspect = async () => {
        while (pending.length) {
          const local = pending.shift();
          local.eligibility = await checkUploadEligibility(local.details);
        }
      };
      await Promise.all(Array.from({ length: Math.min(3, pending.length) }, inspect));
      setFiles(Array.from(filesRef.current.values()));
      const addable = selected.filter((local) => filesRef.current.get(local.id) === local &&
        canQueueBrowserFile(local, latest.current.snapshot.jobs, owner.slice(0, 16), selected));
      if (addable.length < selected.length) latest.current.showToast(
        `${selected.length - addable.length} package(s) cannot be queued: ${selected.find((local) => !addable.includes(local))?.eligibility?.install_disabled_reason || 'already installed or queued'}`, 'info');
      if (!addable.length) return null;
      const ordered = orderBrowserFiles(addable);
      const response = await enqueueJobs(ordered.map((local) => ({ owner, file_key: local.id,
        title_id: local.details.title_id, title_name: local.details.title_name,
        content_id: local.details.content_id, kind: local.details.pkg_type,
        version: local.details.app_version, total_bytes: local.file.size })));
      ordered.forEach((local, index) => enqueuingFiles.current.set(local.id, response.ids[index]));
      return response;
    });
    selected.forEach((local) => {
      if (!result || enqueuingFiles.current.get(local.id) === 0) enqueuingFiles.current.delete(local.id);
    });
    return result;
  }, [owner, perform]);

  const retry = useCallback((job) => {
    if (job.source_id && (job.source_id !== owner.slice(0, 16) || !filesRef.current.has(job.file_key))) {
      latest.current.showToast('Select this package again in Direct Install to retry it.', 'info');
      return;
    }
    return perform(() => retryQueueJob(job.id));
  }, [owner, perform]);

  const filePending = useCallback((id) => enqueuingFiles.current.has(id) || latest.current.snapshot.jobs.some((job) =>
    job.file_key === id && job.source_id === owner.slice(0, 16) && pendingStates.has(job.state)), [owner]);

  const addRemote = useCallback(async (url, mode = 'route') => {
    const trimmed = (url || '').trim();
    if (!trimmed) return false;
    try {
      if (mode === 'passthrough') {
        const link = passthroughJobPath(trimmed);
        if (remotesRef.current.has(link.path)) {
          latest.current.showToast('That link is already listed', 'info');
          return false;
        }
        remotesRef.current.set(link.path, {
          id: link.path,
          url: link.url,
          path: link.path,
          passthrough: true,
          filename: link.filename,
          fileSize: 0,
          hasIcon: false,
          status: 'ready',
          details: {
            title_id: '',
            title_name: link.filename,
            content_id: '',
            app_version: '',
            pkg_type: 'base',
            sdk_version: 0,
            file_size: 0,
          },
          eligibility: { can_install: true, install_disabled_reason: '', is_installed: false, installed_version: '' },
        });
        setRemotes(Array.from(remotesRef.current.values()));
        return true;
      }
      const data = await probeRemotePackage(trimmed);
      const key = data.url || trimmed;
      if (remotesRef.current.has(key)) {
        latest.current.showToast('That link is already listed', 'info');
        return false;
      }
      remotesRef.current.set(key, {
        id: key,
        url: key,
        filename: data.filename || 'package.pkg',
        fileSize: Number(data.file_size) || 0,
        hasIcon: Boolean(data.has_icon),
        status: 'ready',
        details: {
          title_id: data.title_id || '',
          title_name: data.title_name || data.filename || 'Package',
          content_id: data.content_id || '',
          app_version: data.app_version || '',
          pkg_type: data.pkg_type || 'base',
          sdk_version: Number(data.sdk_version) || 0,
          file_size: Number(data.file_size) || 0,
        },
        eligibility: {
          can_install: data.can_install !== false,
          install_disabled_reason: data.install_disabled_reason || '',
          is_installed: Boolean(data.is_installed),
          installed_version: data.installed_version || '',
        },
      });
      setRemotes(Array.from(remotesRef.current.values()));
      return true;
    } catch (e) {
      latest.current.showToast(e.message || 'Could not read the remote package', 'error');
      return false;
    }
  }, []);

  const enqueueRemotes = useCallback(async (urls) => {
    const selected = urls.map((url) => remotesRef.current.get(url)).filter(Boolean);
    if (!selected.length) return null;
    const addable = selected.filter((remote) => canQueueRemote(remote, latest.current.snapshot.jobs, selected));
    if (addable.length < selected.length) {
      const blocked = selected.find((remote) => !addable.includes(remote));
      latest.current.showToast(`${selected.length - addable.length} package(s) cannot be queued: ${blocked?.eligibility?.install_disabled_reason || 'already installed or queued'}`, 'info');
    }
    if (!addable.length) return null;
    return perform(async () => {
      const ordered = orderBrowserFiles(addable);
      const response = await enqueueJobs(ordered.map((remote) => packageJob({
        path: remote.path || remote.url,
        title_id: remote.details.title_id,
        title_name: remote.details.title_name,
        content_id: remote.details.content_id,
        pkg_type: remote.details.pkg_type,
        app_version: remote.details.app_version,
        total_pkg_size: remote.fileSize,
      })));
      latest.current.showToast(`${response.ids.length} package${response.ids.length === 1 ? '' : 's'} added to install queue`, 'success');
      return response;
    });
  }, [perform]);

  const remotePending = useCallback((url) => latest.current.snapshot.jobs.some((job) =>
    (job.path === url || job.path === `pass:${url}`) && !job.source_id && pendingStates.has(job.state)), []);

  const removeRemote = useCallback((url) => {
    if (remotePending(url)) return;
    remotesRef.current.delete(url);
    setRemotes(Array.from(remotesRef.current.values()));
  }, [remotePending]);

  const removeFile = useCallback((id) => {
    if (filePending(id)) return;
    const local = filesRef.current.get(id);
    if (local?.iconUrl) URL.revokeObjectURL(local.iconUrl);
    filesRef.current.delete(id);
    setFiles(Array.from(filesRef.current.values()));
  }, [filePending]);

  const clearFiles = useCallback(() => {
    filesRef.current.forEach((local, id) => {
      if (filePending(id)) return;
      if (local.iconUrl) URL.revokeObjectURL(local.iconUrl);
      filesRef.current.delete(id);
    });
    setFiles(Array.from(filesRef.current.values()));
    setSkipped([]);
  }, [filePending]);

  const cancel = useCallback((job) => {
    if (!job) return;
    const active = ['checking', 'preparing', 'installing', 'canceling'].includes(job.state);
    if (!active) {
      setSnapshot((prev) => ({
        ...prev,
        jobs: prev.jobs.filter((j) => j.id !== job.id)
      }));
    }
    return perform(() => cancelQueueJob(job.id));
  }, [perform]);

  return { ...snapshot, connected, overview, files, remotes, skipped, uploadSpeed: upload.uploadSpeed, installSpeed: rate.current.speed, addFiles, addRemote, removeFile, removeRemote, clearFiles, enqueueFiles, enqueueRemotes, enqueuePackages,
    cancel, retry,
    clearFinished: () => perform(clearFinishedJobs), refresh, sourceId: owner.slice(0, 16) };
}
