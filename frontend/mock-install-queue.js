// Process-owned queue mock used by the UI smoke test and npm run mock.
export function createMockInstallQueue({ packages, start, status, stop, upload, offline }) {
  let jobs = [], activeId = 0, sequence = 0, runId = 0;
  const pending = (job) => ['queued', 'preparing', 'installing', 'blocked'].includes(job.state);
  const finishStates = ['completed', 'submitted', 'failed', 'canceled', 'aborted'];
  const find = (id) => jobs.find((job) => job.id === id);
  const cancel = (job) => {
    if (!job || !pending(job)) throw new Error('Job is no longer pending');
    if (job.id === activeId) { stop(); activeId = 0; upload.active = false; }
    job.state = 'canceled'; job.error = 'Canceled';
  };
  const isDependent = (job) => ['update', 'dlc'].includes(job.kind);
  const hasBase = (job) => packages.some((pkg) => pkg.title_id === job.title_id && pkg.pkg_type === 'base' && pkg.is_installed) ||
    jobs.some((base) => base.order < job.order && base.title_id === job.title_id && base.kind === 'base' &&
      ['queued', 'preparing', 'installing', 'completed', 'submitted'].includes(base.state));
  const browserRefusal = (job) => {
    const installed = packages.find((pkg) => pkg.title_id === job.title_id && pkg.is_installed);
    if (!installed) return '';
    if (job.kind === 'dlc') return packages.some((pkg) => pkg.content_id === job.content_id && pkg.is_dlc_installed) ? 'DLC is already installed' : '';
    if (!job.version || !installed.installed_version) return job.kind === 'base' ? 'Application is already installed' : '';
    const parts = (value) => String(value).replace(/^v/i, '').split('.').map(Number);
    const before = parts(installed.installed_version), next = parts(job.version);
    for (let i = 0; i < Math.max(before.length, next.length); i++) {
      if ((before[i] || 0) !== (next[i] || 0)) return (before[i] || 0) > (next[i] || 0) ? 'Installed version is same or newer' : '';
    }
    return 'Installed version is same or newer';
  };
  const tick = () => {
    for (const job of jobs) {
      if (job.sourceOwner && pending(job) && Date.now() - job.seen > 20000) cancel(job);
      if (isDependent(job) && job.state === 'queued' && !hasBase(job)) {
        job.state = 'blocked'; job.error = 'Base is not installed or queued before this package';
      }
    }
    const active = find(activeId);
    if (active) {
      if (active.state === 'preparing') return;
      const current = status();
      Object.assign(active, { downloaded_bytes: current.downloaded_bytes, progress: current.progress,
        is_direct_storage: current.is_direct_storage, waiting_for_disc: current.waiting_for_disc, prompt: current.prompt_message });
      if (!current.is_installing) {
        active.state = current.status === 'submitted' ? 'submitted' : current.completed ? 'completed' : 'failed';
        if (active.state === 'completed') {
          for (const pkg of packages.filter((pkg) => pkg.title_id === active.title_id)) {
            pkg.is_installed = true;
            if (active.kind === 'base' && pkg.pkg_type !== 'base') { pkg.can_install = true; pkg.install_disabled_reason = ''; }
            if (pkg.path === active.path) { pkg.can_install = false; pkg.is_dlc_installed = active.kind === 'dlc'; }
          }
        }
        activeId = 0;
        if (active.sourceOwner) upload.active = false;
      }
      return;
    }
    if (status().is_installing) return;
    const next = [...jobs].sort((a, b) => a.order - b.order).find((job) => job.state === 'queued');
    if (!next) return;
    const passthrough = !next.sourceOwner && String(next.path || '').startsWith('pass:');
    const pkg = passthrough ? {
      path: next.path, title_id: next.title_id, title_name: next.title_name || 'Package',
      content_id: next.content_id || '', total_pkg_size: next.total_bytes || 0, file_size: next.total_bytes || 0,
      can_install: true,
    } : packages.find((pkg) => pkg.path === next.path);
    const refusal = next.sourceOwner ? browserRefusal(next) : '';
    if (refusal) { next.state = 'aborted'; next.error = refusal; return; }
    if (!next.sourceOwner && !pkg) { next.state = 'failed'; next.error = 'Package source unavailable. Reconnect and retry.'; return; }
    if (pkg?.can_install === false && !/Base.*(not installed|aborted)/i.test(pkg.install_disabled_reason || '')) {
      next.state = 'blocked'; next.error = pkg.install_disabled_reason || 'Already installed'; return;
    }
    activeId = next.id;
    next.state = next.sourceOwner ? 'preparing' : 'installing';
    if (!next.sourceOwner) start(pkg);
  };
  setInterval(tick, 100).unref();
  const publicJobs = () => jobs.map(({ sourceOwner, seen, ...job }) => ({ ...job, source_id: sourceOwner?.slice(0, 16) || '' }));

  return {
    selected(id, owner) { const job = find(id); return job && id === activeId && job.state === 'preparing' && job.sourceOwner === owner; },
    async handle(req, res, pathname) {
      if (pathname !== '/api/queue' && !pathname.startsWith('/api/queue/')) return false;
      let body = '';
      for await (const chunk of req) body += chunk;
      try {
        const data = JSON.parse(body || '{}');
        let result = { success: true };
        const job = find(data.id);
        if (req.method === 'GET' && pathname === '/api/queue') result = { active_id: activeId, jobs: publicJobs() };
        else if (pathname === '/api/queue') {
          if (!Array.isArray(data.jobs) || !data.jobs.length) throw new Error('Packages required');
          if (!jobs.some(pending)) runId++;
          const ids = data.jobs.map((request) => {
            const duplicate = jobs.find((job) => pending(job) && job.sourceOwner === request.owner &&
              (request.owner ? job.file_key === request.file_key : job.path === request.path));
            if (duplicate) return duplicate.id;
            const pkg = packages.find((pkg) => pkg.path === request.path);
            const job = { ...request, sourceOwner: request.owner, id: ++sequence, order: sequence, run_id: runId,
              path: request.path || '', title_name: request.title_name || pkg?.title_name || 'Package',
              title_id: request.title_id || pkg?.title_id || '', kind: request.kind || pkg?.pkg_type || 'base',
              total_bytes: request.total_bytes || pkg?.total_pkg_size || pkg?.file_size || 0,
              downloaded_bytes: 0, progress: 0, state: 'queued', error: '', seen: Date.now() };
            delete job.owner;
            jobs.push(job);
            if (job.kind === 'base') for (const dependent of jobs.filter((other) => other.state === 'blocked' && other.title_id === job.title_id)) {
              dependent.state = 'queued'; dependent.error = ''; dependent.order = ++sequence;
            }
            return job.id;
          });
          result = { success: true, ids };
        } else if (pathname.endsWith('/cancel')) {
          cancel(job);
          const delay = Number(process.env.PKG_MOCK_CANCEL_DELAY_MS) || 0;
          if (delay > 0) await new Promise((resolve) => setTimeout(resolve, delay));
        }
        else if (pathname.endsWith('/retry')) {
          if (!job || !['failed', 'canceled', 'blocked'].includes(job.state)) throw new Error('Job cannot be retried');
          if (!jobs.some(pending)) runId++;
          Object.assign(job, { state: 'queued', error: '', order: ++sequence, run_id: runId, downloaded_bytes: 0, progress: 0, seen: Date.now() });
        } else if (pathname.endsWith('/clear')) jobs = jobs.filter((job) => !finishStates.includes(job.state));
        else if (pathname.endsWith('/heartbeat')) jobs.filter((job) => job.sourceOwner === data.owner).forEach((job) => { job.seen = Date.now(); });
        else if (pathname.endsWith('/disconnect')) jobs.filter((job) => job.sourceOwner === data.owner && pending(job)).forEach(cancel);
        else if (pathname.endsWith('/attach')) {
          if (!this.selected(data.id, data.owner) || data.path !== 'live:' + upload.id) throw new Error('Job is not selected');
          job.state = 'installing';
          start({ path: data.path, title_id: job.title_id, title_name: job.title_name, content_id: job.content_id, file_size: job.total_bytes });
        } else throw new Error('Unknown queue action');
        res.writeHead(200, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(result));
      } catch (e) {
        res.writeHead(400, { 'Content-Type': 'application/json' }); res.end(JSON.stringify({ success: false, error: e.message }));
      }
      return true;
    },
  };
}
