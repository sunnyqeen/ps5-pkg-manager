import React, { useRef, useState } from 'react';
import SdkBadge from '../SdkBadge';
import { collectDroppedPkgFiles, collectInputPkgFiles } from '../../utils/collectPkgFiles';
import { pendingStates, queueStateLabel, canQueueBrowserFile, canQueueRemote } from '../../utils/installQueue';
import { formatBytes, formatVersion } from '../../utils/formatters';
import { getPlatform } from '../../utils/platform';
import PackageThumbnail from '../PackageThumbnail';

export default function DirectInstallView({ queue, onBack, onOpenQueue }) {
  const input = useRef(null);
  const [reading, setReading] = useState(false);
  const [link, setLink] = useState('');
  const [route, setRoute] = useState('route');
  const [adding, setAdding] = useState(false);
  const [tab, setTab] = useState('local');
  const localTab = useRef(null);
  const remoteTab = useRef(null);
  const selectTab = (next) => {
    setTab(next);
    (next === 'local' ? localTab : remoteTab).current?.focus();
  };
  const mode = route;
  const modeHelp = {
    route: 'The console downloads the link, then streams it to the PS5 installer.',
    passthrough: 'The console checks the link and follows redirects. The PS5 installer then downloads that final link directly.',
  }[mode];
  const remotes = queue.remotes || [];
  const ownJob = (local) => [...queue.jobs].reverse().find((job) => job.file_key === local.id && job.source_id === queue.sourceId);
  const remoteJob = (remote) => [...queue.jobs].reverse().find((job) => job.path === (remote.path || remote.url) && !job.source_id);
  const ready = queue.files.filter((local) => canQueueBrowserFile(local, queue.jobs, queue.sourceId, queue.files));
  const readyRemotes = remotes.filter((remote) => canQueueRemote(remote, queue.jobs, remotes));
  const addLink = async (event) => {
    event.preventDefault();
    const url = link.trim();
    if (!url || adding) return;
    setAdding(true);
    try {
      if (await queue.addRemote(url, mode)) setLink('');
    } finally { setAdding(false); }
  };
  const collect = async (selection, dropped) => {
    setReading(true);
    try {
      const result = dropped ? await collectDroppedPkgFiles(selection) : { files: collectInputPkgFiles(selection), unreadable: [] };
      await queue.addFiles(result.files, result.unreadable);
    } finally { setReading(false); }
  };
  return (
    <div className="max-w-6xl mx-auto space-y-5" onDragOver={(event) => event.preventDefault()} onDrop={(event) => event.preventDefault()}>
      <div className="flex items-center justify-between gap-3">
        <button type="button" onClick={onBack} className="ps5-focus-item px-4 py-2 bg-white/10 rounded-[2px]">← Back</button>
        <button type="button" onClick={onOpenQueue} className="ps5-focus-item px-4 py-2 bg-blue-600 rounded-[2px]">Open install queue</button>
      </div>

      <div role="tablist" aria-label="Package source" className="flex gap-1 border-b border-white/10" onKeyDown={(event) => {
        if (event.key !== 'ArrowLeft' && event.key !== 'ArrowRight') return;
        event.preventDefault();
        selectTab(tab === 'local' ? 'remote' : 'local');
      }}>
        <button ref={localTab} type="button" role="tab" id="local-storage-tab" aria-selected={tab === 'local'} aria-controls="local-storage-panel" tabIndex={tab === 'local' ? 0 : -1} onClick={() => setTab('local')} className={`ps5-focus-item px-4 py-2.5 text-sm font-semibold border-b-2 -mb-px ${tab === 'local' ? 'border-blue-500 text-white' : 'border-transparent text-zinc-400'}`}>
          Local Storage
          <span className="ml-2 text-xs font-normal text-zinc-400">{queue.files.length}</span>
        </button>
        <button ref={remoteTab} type="button" role="tab" id="remote-uri-tab" aria-selected={tab === 'remote'} aria-controls="remote-uri-panel" tabIndex={tab === 'remote' ? 0 : -1} onClick={() => setTab('remote')} className={`ps5-focus-item px-4 py-2.5 text-sm font-semibold border-b-2 -mb-px ${tab === 'remote' ? 'border-blue-500 text-white' : 'border-transparent text-zinc-400'}`}>
          Remote URL
          <span className="ml-2 text-xs font-normal text-zinc-400">{remotes.length}</span>
        </button>
      </div>

      <section id="local-storage-panel" role="tabpanel" aria-labelledby="local-storage-tab" hidden={tab !== 'local'} className="border border-dashed border-white/20 bg-white/5 p-6 space-y-3 rounded-[2px]" onDragOver={(event) => event.preventDefault()} onDrop={(event) => {
        event.preventDefault(); event.stopPropagation(); event.__pkgManagerDropHandled = true;
        collect(event.dataTransfer, true);
      }}>
        <p className="text-xs text-zinc-400">{queue.files.length} package{queue.files.length === 1 ? '' : 's'} · {ready.length} ready</p>
        <p className="text-sm text-zinc-300">Choose or drop PKG files from this computer.</p>
        <div className="flex flex-wrap gap-3">
          <button type="button" onClick={() => input.current.click()} className="ps5-focus-item px-4 py-2 bg-white/10 rounded-[2px]">Choose packages</button>
          <button type="button" disabled={!ready.length || !queue.connected} onClick={() => queue.enqueueFiles(ready.map((local) => local.id))} className="ps5-focus-item px-4 py-2 bg-blue-600 disabled:opacity-40 rounded-[2px]">Install all ({ready.length})</button>
        </div>
        <input ref={input} type="file" multiple accept=".pkg" className="hidden" onChange={(event) => { collect(event.target.files, false); event.target.value = ''; }} />
        {reading && <p className="text-sm text-blue-300">Reading packages…</p>}
        {queue.skipped.length > 0 && <p className="text-sm text-amber-300">{queue.skipped.length} unreadable file(s) skipped.</p>}
        <div aria-label="Local packages" className="grid grid-cols-1 sm:grid-cols-2 md:grid-cols-3 lg:grid-cols-4 gap-4">
          {queue.files.map((local) => {
            const job = ownJob(local);
            const pending = job && pendingStates.has(job.state);
            const canCancel = pending && job.state !== 'canceling' && !job.is_direct_storage;
            const canQueue = canQueueBrowserFile(local, queue.jobs, queue.sourceId);
            const platform = getPlatform(local.details?.title_id);
            const kind = local.details?.pkg_type;
            const title = local.details?.title_name || local.file.name;
            const problem = local.error || job?.error || (!pending && job?.state !== 'completed' && local.eligibility?.can_install === false ? local.eligibility.install_disabled_reason : '');
            return (
              <article key={local.id} className="min-w-0 flex flex-col p-2.5 bg-[#141520] border border-white/10 rounded-[2px]">
                <div className="aspect-square-box rounded-[2px] overflow-hidden bg-black/50 border border-white/10">
                  <div className="aspect-square-content overflow-hidden">
                    <PackageThumbnail src={local.iconUrl} title={title} />
                    {platform && <span className={`absolute top-2 right-2 z-20 px-2 py-0.5 rounded-[2px] text-[10px] font-bold border pointer-events-none ${platform === 'PS5' ? 'bg-white text-black border-white' : 'bg-zinc-900 text-zinc-200 border-zinc-600'}`}>{platform}</span>}
                    {kind && <span className={`absolute bottom-2 right-2 z-20 px-2 py-0.5 rounded-[2px] text-[10px] font-bold border uppercase pointer-events-none ${kind === 'update' ? 'bg-purple-600 text-white border-purple-400/60' : kind === 'dlc' ? 'bg-emerald-600 text-white border-emerald-400/60' : 'bg-blue-600 text-white border-blue-400/60'}`}>{kind}</span>}
                  </div>
                </div>
                <div className="flex-1 min-w-0 pt-3 pb-3 space-y-1.5">
                  <h3 className="text-sm font-semibold break-words">{title}</h3>
                  <p className="text-xs text-zinc-400 truncate" title={local.path || local.file.name}>{local.path || local.file.name}</p>
                  <p className="text-xs text-zinc-400">{[local.details?.title_id, formatVersion(local.details?.app_version), formatBytes(local.file.size)].filter(Boolean).join(' · ')}</p>
                  <SdkBadge pkg={local.details} />
                  <p className={`text-xs ${problem ? 'text-amber-300' : job?.state === 'completed' ? 'text-emerald-300' : 'text-blue-300'}`}>{job ? queueStateLabel(job) : local.status === 'reading' ? 'Reading package…' : local.status === 'error' ? 'Unable to read package' : canQueue ? 'Ready' : 'Not installable'}</p>
                  {job?.state === 'installing' && <div role="progressbar" aria-label={`${title} installation progress`} aria-valuenow={Math.floor(Math.max(0, job.progress))} aria-valuemin={0} aria-valuemax={100} className="h-1 bg-white/10">
                    <div className="h-full bg-blue-500" style={{ width: `${Math.min(100, Math.max(0, job.progress))}%` }} />
                  </div>}
                  {problem && <p className="text-xs text-amber-300 break-words">{problem}</p>}
                </div>
                <div className="flex flex-wrap gap-2">
                  <button type="button" disabled={!queue.connected || (pending ? !canCancel : !canQueue)} onClick={() => pending ? queue.cancel(job) : queue.enqueueFiles([local.id])} className="ps5-focus-item flex-1 text-xs px-3 py-2 bg-blue-600 hover:bg-blue-500 disabled:opacity-40 rounded-[2px]">{job?.state === 'completed' ? 'Installed' : pending ? ['queued', 'blocked'].includes(job.state) ? 'Unqueue' : 'Cancel install' : 'Install'}</button>
                  <button type="button" disabled={Boolean(pending)} onClick={() => queue.removeFile(local.id)} className="ps5-focus-item text-xs px-3 py-2 bg-white/10 hover:bg-white/20 disabled:opacity-40 rounded-[2px]">Cancel</button>
                </div>
              </article>
            );
          })}
        </div>
      </section>

      <section id="remote-uri-panel" role="tabpanel" aria-labelledby="remote-uri-tab" hidden={tab !== 'remote'} className="border border-white/15 bg-white/5 p-6 space-y-3 rounded-[2px]">
        <p className="text-xs text-zinc-400">{remotes.length} link{remotes.length === 1 ? '' : 's'} · {readyRemotes.length} ready</p>
        <p className="text-sm text-zinc-300">Paste a package link and choose how the PS5 installs it.</p>
        <form onSubmit={addLink} className="space-y-2">
          <div role="radiogroup" aria-label="Link install method" className="flex flex-wrap gap-2">
            <button type="button" role="radio" aria-checked={mode === 'route'} disabled={adding} title="The console downloads the link, then streams it" onClick={() => setRoute('route')} className={`ps5-focus-item px-4 py-2 rounded-[2px] disabled:opacity-40 ${mode === 'route' ? 'bg-blue-600 text-white' : 'bg-white/10 text-zinc-200'}`}>Route</button>
            <button type="button" role="radio" aria-checked={mode === 'passthrough'} disabled={adding} title="Give the link to the PS5 installer" onClick={() => setRoute('passthrough')} className={`ps5-focus-item px-4 py-2 rounded-[2px] disabled:opacity-40 ${mode === 'passthrough' ? 'bg-blue-600 text-white' : 'bg-white/10 text-zinc-200'}`}>Passthrough</button>
          </div>
          <p className="text-xs text-zinc-400">{modeHelp}</p>
          <div className="flex flex-wrap gap-2">
            <label className="sr-only" htmlFor="remote-pkg-link">Remote package link</label>
            <input id="remote-pkg-link" type="url" inputMode="url" autoCapitalize="off" autoCorrect="off" spellCheck="false" value={link} onChange={(event) => setLink(event.target.value)} placeholder="http://192.168.1.20:8080/game.pkg" className="min-w-[16rem] flex-1 bg-black/50 border border-white/15 rounded-[2px] px-3.5 py-2.5 text-sm text-white placeholder-zinc-500 focus:outline-none focus:border-white/40" />
            <button type="submit" disabled={adding || !link.trim()} className="ps5-focus-item px-4 py-2 bg-white/10 disabled:opacity-40 rounded-[2px]">{adding ? 'Adding…' : 'Add link'}</button>
          </div>
        </form>
        <button type="button" disabled={!readyRemotes.length || !queue.connected} onClick={() => queue.enqueueRemotes(readyRemotes.map((remote) => remote.path || remote.url))} className="ps5-focus-item px-4 py-2 bg-blue-600 disabled:opacity-40 rounded-[2px]">Install all ({readyRemotes.length})</button>
        <div aria-label="Remote packages" className="grid grid-cols-1 sm:grid-cols-2 md:grid-cols-3 lg:grid-cols-4 gap-4">
          {remotes.map((remote) => {
            const job = remoteJob(remote);
            const pending = job && pendingStates.has(job.state);
            const canCancel = pending && job.state !== 'canceling' && !job.is_direct_storage;
            const canQueue = canQueueRemote(remote, queue.jobs, remotes);
            const platform = getPlatform(remote.details?.title_id);
            const kind = remote.details?.pkg_type;
            const title = remote.details?.title_name || remote.filename;
            const problem = job?.error || (!pending && job?.state !== 'completed' && remote.eligibility?.can_install === false ? remote.eligibility.install_disabled_reason : '');
            const icon = remote.hasIcon ? `/api/icon?path=${encodeURIComponent(remote.url)}` : '';
            return (
              <article key={remote.id} className="min-w-0 flex flex-col p-2.5 bg-[#141520] border border-white/10 rounded-[2px]">
                <div className="aspect-square-box rounded-[2px] overflow-hidden bg-black/50 border border-white/10">
                  <div className="aspect-square-content overflow-hidden">
                    <PackageThumbnail src={icon} title={title} />
                    {platform && <span className={`absolute top-2 right-2 z-20 px-2 py-0.5 rounded-[2px] text-[10px] font-bold border pointer-events-none ${platform === 'PS5' ? 'bg-white text-black border-white' : 'bg-zinc-900 text-zinc-200 border-zinc-600'}`}>{platform}</span>}
                    {kind && <span className={`absolute bottom-2 right-2 z-20 px-2 py-0.5 rounded-[2px] text-[10px] font-bold border uppercase pointer-events-none ${kind === 'update' ? 'bg-purple-600 text-white border-purple-400/60' : kind === 'dlc' ? 'bg-emerald-600 text-white border-emerald-400/60' : 'bg-blue-600 text-white border-blue-400/60'}`}>{kind}</span>}
                  </div>
                </div>
                <div className="flex-1 min-w-0 pt-3 pb-3 space-y-1.5">
                  <h3 className="text-sm font-semibold break-words">{title}</h3>
                  <p className="text-xs text-zinc-400 truncate" title={remote.url}>{remote.url}</p>
                  <p className="text-xs text-zinc-400">{[remote.details?.title_id, formatVersion(remote.details?.app_version), remote.fileSize ? formatBytes(remote.fileSize) : ''].filter(Boolean).join(' · ')}</p>
                  <SdkBadge pkg={remote.details} />
                  <p className={`text-xs ${problem ? 'text-amber-300' : job?.state === 'completed' ? 'text-emerald-300' : 'text-blue-300'}`}>{`${remote.passthrough ? 'Passthrough' : 'Route'} · ${job ? queueStateLabel(job) : canQueue ? 'Ready' : 'Not installable'}`}</p>
                  {job?.state === 'installing' && <div role="progressbar" aria-label={`${title} installation progress`} aria-valuenow={Math.floor(Math.max(0, job.progress))} aria-valuemin={0} aria-valuemax={100} className="h-1 bg-white/10">
                    <div className="h-full bg-blue-500" style={{ width: `${Math.min(100, Math.max(0, job.progress))}%` }} />
                  </div>}
                  {problem && <p className="text-xs text-amber-300 break-words">{problem}</p>}
                </div>
                <div className="flex flex-wrap gap-2">
                  <button type="button" disabled={!queue.connected || (pending ? !canCancel : !canQueue)} onClick={() => pending ? queue.cancel(job) : queue.enqueueRemotes([remote.path || remote.url])} className="ps5-focus-item flex-1 text-xs px-3 py-2 bg-blue-600 hover:bg-blue-500 disabled:opacity-40 rounded-[2px]">{job?.state === 'completed' ? 'Installed' : pending ? ['queued', 'blocked'].includes(job.state) ? 'Unqueue' : 'Cancel install' : 'Install'}</button>
                  <button type="button" disabled={Boolean(pending)} onClick={() => queue.removeRemote(remote.path || remote.url)} className="ps5-focus-item text-xs px-3 py-2 bg-white/10 hover:bg-white/20 disabled:opacity-40 rounded-[2px]">Remove</button>
                </div>
              </article>
            );
          })}
        </div>
      </section>
    </div>
  );
}
