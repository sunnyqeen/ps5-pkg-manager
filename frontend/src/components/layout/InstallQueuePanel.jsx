import React, { useEffect } from 'react';
import DebugSpeedOverlay from '../screens/DebugSpeedOverlay';
import { formatBytes } from '../../utils/formatters';
import { pendingStates, queueStateLabel, queuePrompt, queueSourceLabel } from '../../utils/installQueue';
import { iconUrlFor } from '../../BlurIcon';
import PackageThumbnail from '../PackageThumbnail';

export default function InstallQueuePanel({ queue, onClose, debugEnabled = false }) {
  useEffect(() => {
    const previousOverflow = document.body.style.overflow;
    document.body.style.overflow = 'hidden';
    const close = (event) => { if (event.key === 'Escape') onClose(); };
    window.addEventListener('keydown', close);
    return () => { document.body.style.overflow = previousOverflow; window.removeEventListener('keydown', close); };
  }, [onClose]);
  const rank = (job) => ['checking', 'preparing', 'installing', 'canceling'].includes(job.state) ? 0 : pendingStates.has(job.state) ? 1 : 2;
  const ordered = [...queue.jobs].sort((a, b) => rank(a) - rank(b) || a.order - b.order);
  const { overview } = queue;
  return (
    <div className="fixed inset-0 z-[60] bg-black/60" data-queue-backdrop="true" onClick={(event) => { if (event.target === event.currentTarget) onClose(); }}>
    <aside id="install-queue-panel" role="dialog" aria-modal="true" aria-label="Installation queue" data-modal-dialog="true"
      className="absolute top-0 right-0 bottom-0 w-full sm:w-[430px] max-w-full bg-[#12131a] border-l border-white/20 shadow-2xl flex flex-col">
      <div className="p-4 border-b border-white/10 space-y-3">
        <div className="flex items-center justify-between gap-3">
          <h2 className="text-lg font-bold">Install queue</h2>
          <button type="button" onClick={onClose} aria-label="Close install queue" className="ps5-focus-item px-3 py-2 bg-white/10 rounded-[2px]">Close</button>
        </div>
        {!queue.connected && <p className="text-sm text-amber-300">Connection lost. Queue status will refresh when the app reconnects.</p>}
        <p className="text-sm text-zinc-300">{overview.pending.length} active / pending · {overview.resolved} finished</p>
        {overview.pending.length > 0 && overview.percent !== null && (
          <div aria-label="Overall queue progress" role="progressbar" aria-valuenow={Math.floor(overview.percent)} aria-valuemin={0} aria-valuemax={100} className="bg-white/10 h-1.5 overflow-hidden">
            <div className="h-full bg-blue-500" style={{ width: `${overview.percent}%` }} />
          </div>
        )}
        {overview.submitted > 0 && <p className="text-xs text-blue-300">{overview.submitted} submitted to PS5. Check PS5 Notifications for installation progress and cancellation.</p>}
      </div>
      <div className="p-4 space-y-3 overflow-y-auto flex-1">
        {debugEnabled && overview.active?.source_id === queue.sourceId && <DebugSpeedOverlay inline uploadSpeed={queue.uploadSpeed} installSpeed={queue.installSpeed} />}
        {!ordered.length && <p className="text-sm text-zinc-400 py-6">No installations yet. Install a package to add it here.</p>}
        {ordered.map((job) => {
          const active = ['checking', 'preparing', 'installing', 'canceling'].includes(job.state);
          const retryable = ['failed', 'canceled', 'blocked'].includes(job.state);
          const cancellable = pendingStates.has(job.state) && job.state !== 'canceling' && !(active && job.is_direct_storage);
          const title = job.title_name || job.path || 'Package';
          const local = job.source_id === queue.sourceId ? queue.files?.find((file) => file.id === job.file_key) : null;
          const thumbnail = local?.iconUrl || ((!job.source_id || job.state === 'installing') ? iconUrlFor(job.path) : null);
          const prompt = active ? queuePrompt(job) : '';
          const stateLabel = queueStateLabel(job);
          const error = job.error && job.error.trim().toLowerCase() !== stateLabel.toLowerCase() ? job.error : '';
          return (
            <article key={job.id} className={`p-3 border rounded-[2px] ${active ? 'bg-blue-500/10 border-blue-500/40' : 'bg-white/5 border-white/10'}`}>
              <div className="flex gap-3">
                <div className="relative w-16 h-16 shrink-0 overflow-hidden rounded-[2px] bg-black/50 border border-white/10">
                  <PackageThumbnail src={thumbnail} title={title} small loading={active ? 'eager' : 'lazy'} />
                </div>
                <div className="flex-1 min-w-0">
                  <div className="flex justify-between gap-3 text-sm">
                    <h3 className="font-semibold break-words min-w-0">{title}</h3>
                    <span className="text-xs text-zinc-400 shrink-0 uppercase">{job.kind}</span>
                  </div>
                  <p className={`text-xs mt-1 ${['failed', 'blocked', 'aborted'].includes(job.state) ? 'text-amber-300' : 'text-blue-300'}`}>{stateLabel}</p>
                  <p className="text-xs text-zinc-400 mt-1">{job.title_id} · {formatBytes(job.total_bytes)} · {queueSourceLabel(job)}</p>
                  {prompt && <p className="text-xs text-zinc-300 mt-2 break-words">{prompt}</p>}
                  {job.state === 'installing' && !job.is_direct_storage && !job.waiting_for_disc && (
                    <div className="mt-2 text-xs text-zinc-300">
                      <span>{Math.max(0, job.progress).toFixed(1)}% · {formatBytes(job.downloaded_bytes)} / {formatBytes(job.total_bytes)}</span>
                      <div className="h-1 bg-white/10 mt-1"><div className="h-full bg-blue-500" style={{ width: `${Math.min(100, Math.max(0, job.progress))}%` }} /></div>
                    </div>
                  )}
                  {error && <p className="text-xs text-amber-300 mt-2 break-words">{error}</p>}
                  {(cancellable || retryable) && <div className="flex gap-2 mt-3">
                    {cancellable && <button type="button" disabled={!queue.connected} onClick={() => queue.cancel(job)} className="ps5-focus-item px-3 py-1.5 border border-white/20 rounded-[2px] text-xs hover:bg-red-500/20 disabled:opacity-50">{active ? 'Cancel install' : 'Remove from queue'}</button>}
                    {retryable && <button type="button" disabled={!queue.connected} onClick={() => queue.retry(job)} className="ps5-focus-item px-3 py-1.5 bg-blue-600 rounded-[2px] text-xs disabled:opacity-50">Retry</button>}
                  </div>}
                </div>
              </div>
            </article>
          );
        })}
      </div>
      <div className="border-t border-white/10 p-4">
        <button type="button" disabled={!queue.connected} onClick={queue.clearFinished} className="ps5-focus-item text-sm px-3 py-2 bg-white/10 rounded-[2px] disabled:opacity-50">Clear finished entries</button>
      </div>
    </aside>
    </div>
  );
}
