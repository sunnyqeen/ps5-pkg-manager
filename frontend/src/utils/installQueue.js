export const pendingStates = new Set(['queued', 'checking', 'preparing', 'installing', 'canceling', 'blocked']);
export const terminalStates = new Set(['completed', 'submitted', 'failed', 'canceled', 'aborted']);
export const fileKey = ({ file }) => [file.name, file.size, file.lastModified].join('|');

// Preserve bulk-selection order except where a selected base must precede
// its own update/DLC. Do not move unrelated games ahead of earlier packages.
export function orderBrowserFiles(files) {
  const ordered = [];
  for (const file of files) {
    if (ordered.includes(file)) continue;
    if (['update', 'dlc'].includes(file.details.pkg_type)) {
      const base = files.find((other) => other.details.pkg_type === 'base' &&
        other.details.title_id === file.details.title_id && !ordered.includes(other));
      if (base) ordered.push(base);
    }
    ordered.push(file);
  }
  return ordered;
}

export function queueOverview(jobs) {
  const visible = jobs.filter((job) => job.state !== 'canceled');
  const pending = visible.filter((job) => pendingStates.has(job.state));
  const active = pending.find((job) => ['checking', 'preparing', 'installing', 'canceling'].includes(job.state));
  const run = Math.max(0, ...visible.map((job) => job.run_id || 0));
  const members = visible.filter((job) => (job.run_id || 0) === run);
  const total = members.reduce((sum, job) => sum + job.total_bytes, 0);
  const resolved = visible.filter((job) => terminalStates.has(job.state));
  const bytes = members.reduce((sum, job) => sum + (terminalStates.has(job.state) ? job.total_bytes :
    job.state === 'installing' ? Math.min(job.total_bytes, job.downloaded_bytes) : 0), 0);
  const indeterminate = Boolean(active?.is_direct_storage || active?.state === 'preparing' || active?.waiting_for_disc);
  return { pending, active, resolved: resolved.length, count: visible.length,
    runCount: members.length,
    runResolved: members.filter((job) => terminalStates.has(job.state)).length,
    submitted: visible.filter((job) => job.state === 'submitted').length,
    percent: !indeterminate && total > 0 ? Math.min(100, bytes / total * 100) : null };
}

export function canQueuePackage(pkg, jobs) {
  if (jobs.some((job) => job.path === pkg.path && pendingStates.has(job.state))) return false;
  if (pkg.can_install !== false) return true;
  const missingBase = /Base (package|game).*(not installed|aborted)/i.test(pkg.install_disabled_reason || '');
  return missingBase && jobs.some((job) => job.title_id === pkg.title_id && job.kind === 'base' &&
    (['queued', 'checking', 'preparing', 'installing', 'completed', 'submitted'].includes(job.state)));
}

export function canQueueRemote(remote, jobs, selected = []) {
  if (!remote?.details || remote.status !== 'ready') return false;
  const queuedPath = remote.path || remote.url;
  if ((jobs || []).some((job) => job.path === queuedPath && !job.source_id &&
    (pendingStates.has(job.state) || job.state === 'completed'))) return false;
  const pkg = {
    path: queuedPath,
    title_id: remote.details.title_id,
    can_install: remote.eligibility?.can_install,
    install_disabled_reason: remote.eligibility?.install_disabled_reason,
  };
  if (canQueuePackage(pkg, jobs || [])) return true;
  const missingBase = /Base (package|game).*(not installed|aborted)/i.test(pkg.install_disabled_reason || '');
  return missingBase && selected.some((base) => base !== remote && base.details?.pkg_type === 'base' &&
    base.details.title_id === remote.details.title_id && canQueueRemote(base, jobs, selected));
}

export function queueSourceLabel(job) {
  if (job?.source_id) return 'Browser';
  const path = job?.path || '';
  if (path.startsWith('pass:')) return 'Passthrough';
  if (path.startsWith('smb:')) return 'SMB';
  if (/^https?:\/\//i.test(path)) return 'Route';
  return 'USB / Disc';
}

export function canQueueBrowserFile(local, jobs, sourceId, selected = []) {
  if (local.status !== 'ready' || !local.details) return false;
  if (jobs.some((job) => job.file_key === local.id && job.source_id === sourceId &&
    (pendingStates.has(job.state) || job.state === 'completed'))) return false;
  const pkg = { ...local.details, path: `browser:${local.id}`, ...local.eligibility };
  if (canQueuePackage(pkg, jobs)) return true;
  const missingBase = /Base (package|game).*(not installed|aborted)/i.test(pkg.install_disabled_reason || '');
  return missingBase && selected.some((base) => base.details?.pkg_type === 'base' && base.details.title_id === pkg.title_id &&
    canQueueBrowserFile(base, jobs, sourceId));
}

export function queueStateLabel(job) {
  if (job.waiting_for_disc && job.state === 'installing') return 'Waiting for next disc / USB part';
  if (job.is_direct_storage && job.state === 'installing') return 'Submitting to PS5';
  if (job.state === 'installing' && /^Finishing installation of /i.test(job.prompt || '')) return 'Finishing installation';
  return { queued: 'Queued', checking: 'Checking package', preparing: 'Preparing', installing: 'Installing', canceling: 'Canceling',
    completed: 'Installed', submitted: 'Submitted to PS5', failed: 'Failed', blocked: 'Blocked', canceled: 'Canceled', aborted: 'Aborted' }[job.state] || job.state;
}

export function queuePrompt(job) {
  const prompt = (job.prompt || '').trim();
  // The row already identifies the package and its current state. Keep prompts
  // about media and other actionable details, rather than repeating the title.
  if (!job.waiting_for_disc && !job.is_direct_storage &&
    /^(?:Installing(?: package)?(?: .+)?|Finishing installation of .+)(?:\.{3}|…)$/i.test(prompt)) return '';
  if (prompt.replace(/[.…]+$/, '').toLowerCase() === queueStateLabel(job).toLowerCase()) return '';
  if (!prompt && job.source_id && job.state === 'preparing') return 'Waiting for the source browser';
  return prompt;
}
