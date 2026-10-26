import React, { useState, useEffect } from 'react';
import DebugSpeedOverlay from '../screens/DebugSpeedOverlay';
import { formatBytes, formatEta } from '../../utils/formatters';
import { pendingStates, terminalStates, queueStateLabel, queuePrompt, queueSourceLabel } from '../../utils/installQueue';
import { iconUrlFor } from '../../BlurIcon';
import PackageThumbnail from '../PackageThumbnail';
import { isPlayStation } from '../../constants/config';

export default function InstallQueueView({ queue, onBack, debugEnabled = false }) {
  const [selectedJobId, setSelectedJobId] = useState(null);

  useEffect(() => {
    const handleKeyDown = (event) => {
      if (event.key === 'Escape') {
        if (isPlayStation) return;
        onBack();
      }
    };
    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [onBack]);

  const { overview } = queue;
  const rank = (job) => ['checking', 'preparing', 'installing', 'canceling'].includes(job.state) ? 0 : pendingStates.has(job.state) ? 1 : 2;
  const ordered = [...(queue.jobs || [])]
    .filter((job) => job.state !== 'canceled')
    .sort((a, b) => rank(a) - rank(b) || a.order - b.order);

  const hasPending = ordered.some((job) => pendingStates.has(job.state));
  const hasFailures = ordered.some((job) => ['failed', 'aborted', 'blocked'].includes(job.state));

  // Identify active or selected hero job
  const activeJob = ordered.find((job) => ['checking', 'preparing', 'installing', 'canceling'].includes(job.state)) ||
    ordered.find((job) => pendingStates.has(job.state)) ||
    ordered[0] ||
    null;

  const heroJob = (selectedJobId && ordered.find((job) => job.id === selectedJobId)) || activeJob;

  const heroActive = heroJob && ['checking', 'preparing', 'installing', 'canceling'].includes(heroJob.state);
  const heroRetryable = heroJob && ['failed', 'canceled', 'blocked'].includes(heroJob.state);
  const heroCancellable = heroJob && pendingStates.has(heroJob.state) && heroJob.state !== 'canceling' && !(heroActive && heroJob.is_direct_storage);
  const heroTitle = heroJob ? (heroJob.title_name || heroJob.path || 'Package') : '';
  const heroLocal = heroJob && heroJob.source_id === queue.sourceId ? queue.files?.find((file) => file.id === heroJob.file_key) : null;
  const heroThumbnail = heroJob ? (heroLocal?.iconUrl || ((!heroJob.source_id || heroJob.state === 'installing') ? iconUrlFor(heroJob.path) : null)) : null;
  const heroPrompt = heroActive ? queuePrompt(heroJob) : '';
  const heroStateLabel = heroJob ? queueStateLabel(heroJob) : '';
  const heroError = heroJob?.error && heroJob.error.trim().toLowerCase() !== heroStateLabel.toLowerCase() ? heroJob.error : '';

  let heroStatusChip = 'Queued';
  if (heroJob) {
    if (heroJob.is_multipart) {
      heroStatusChip = `Installing Part ${heroJob.current_part || 1} of ${heroJob.total_parts || 1}`;
    } else if (heroJob.is_direct_storage) {
      heroStatusChip = 'Direct Storage Install';
    } else if (heroJob.state === 'installing') {
      heroStatusChip = 'Installing to PS5';
    } else if (heroJob.state === 'checking') {
      heroStatusChip = 'Checking Package';
    } else if (heroJob.state === 'preparing') {
      heroStatusChip = 'Preparing Installation';
    } else if (heroJob.state === 'canceling') {
      heroStatusChip = 'Canceling Installation';
    } else if (heroJob.state === 'completed') {
      heroStatusChip = (ordered.length > 0 && !hasPending && !hasFailures) ? 'All Done!' : 'Installation Complete';
    } else if (heroJob.state === 'submitted') {
      heroStatusChip = 'Submitted to PS5';
    } else if (heroJob.state === 'failed') {
      heroStatusChip = 'Installation Failed';
    } else if (heroJob.state === 'blocked') {
      heroStatusChip = 'Installation Blocked';
    } else if (heroJob.state === 'aborted') {
      heroStatusChip = 'Installation Aborted';
    } else {
      heroStatusChip = heroStateLabel || 'Queued';
    }
  }

  const etaText = queue.installSpeed > 0 && heroJob && heroJob.total_bytes > heroJob.downloaded_bytes
    ? formatEta((heroJob.total_bytes - heroJob.downloaded_bytes) / queue.installSpeed)
    : null;

  // Batch and queue calculations
  const pendingJobs = ordered.filter((job) => pendingStates.has(job.state));
  const hasMultiplePending = pendingJobs.length > 1;

  const currentRun = Math.max(0, ...ordered.map((job) => job.run_id || 0));
  const batchJobs = ordered.filter((job) => (job.run_id || 0) === currentRun);
  const resolvedInBatch = batchJobs.filter((job) => terminalStates.has(job.state));
  const isMultiItemBatch = (overview?.runCount || batchJobs.length) > 1;

  // The right queue is visible if:
  // - During active installs (hasPending): only if more than one item is active/pending, or in a multi-item batch
  // - When all installs are finished (!hasPending): if there is more than 1 item in total in the queue
  const showRightQueue = hasPending
    ? (hasMultiplePending || (isMultiItemBatch && ordered.length > 1))
    : (ordered.length > 1);

  // Total queue progress calculations (e.g. "1/2") and total remaining time
  const totalJobCount = isMultiItemBatch
    ? (overview?.runCount || batchJobs.length)
    : (overview?.count || queue.jobs?.length || 1);
  const currentJobIndex = isMultiItemBatch
    ? Math.min(totalJobCount, (overview?.runResolved ?? resolvedInBatch.length) + 1)
    : Math.min(overview?.count || 1, (overview?.resolved || 0) + 1);

  const totalRemainingBytes = (queue.jobs || [])
    .filter((j) => pendingStates.has(j.state))
    .reduce((sum, j) => sum + Math.max(0, (j.total_bytes || 0) - (j.downloaded_bytes || 0)), 0);

  const totalEtaText = queue.installSpeed > 0 && totalRemainingBytes > 0
    ? formatEta(totalRemainingBytes / queue.installSpeed)
    : null;

  return (
    <div id="install-queue-panel" aria-label="Installation queue" className="relative flex flex-col flex-1 min-h-0 w-full">
      {/* Overlayed Back button in top-left (does not take vertical space, does not stretch) */}
      <div className="absolute top-0 left-0 z-20">
        <button
          type="button"
          onClick={onBack}
          className="px-3.5 py-1.5 rounded-[2px] ps5-focus-item bg-white/10 hover:bg-white/15 border border-white/15 text-sm font-semibold transition-all flex items-center space-x-1.5 text-zinc-200 cursor-pointer shadow-md backdrop-blur-sm"
        >
          <span>&larr;</span>
          <span>Back</span>
        </button>
      </div>

      {!queue.connected && (
        <div className="p-3 bg-amber-500/10 border border-amber-500/30 rounded-[2px] text-sm text-amber-300 shrink-0 mb-3">
          Connection lost. Queue status will refresh when the app reconnects.
        </div>
      )}

      {overview?.submitted > 0 && (
        <div className="p-3 bg-blue-500/10 border border-blue-500/30 rounded-[2px] text-xs text-blue-300 shrink-0 mb-3">
          {overview.submitted} submitted to PS5. Check PS5 Notifications for installation progress and cancellation.
        </div>
      )}

      {/* Main content: Hero (black background) taking full width or 2-column if >1 active/pending items */}
      <div className="flex flex-col lg:flex-row gap-6 items-start flex-1 min-h-0">
        {/* Left Column: Active Install Hero (no card background, pure black) */}
        <div className={`w-full ${showRightQueue ? 'lg:w-[60%] shrink-0 lg:sticky lg:top-0' : 'max-w-3xl mx-auto'}`}>
          {heroJob ? (
            <article aria-label={heroTitle} className="px-4 py-2 sm:py-3 flex flex-col items-center text-center">
              {/* Status Chip */}
              <div className={`px-3.5 py-1 rounded-[2px] text-xs uppercase font-bold tracking-wider mb-4 border ${
                ['failed', 'blocked', 'aborted', 'canceled'].includes(heroJob.state)
                  ? 'bg-amber-500/20 text-amber-300 border-amber-500/30'
                  : ['completed', 'submitted'].includes(heroJob.state)
                  ? 'bg-emerald-500/20 text-emerald-300 border-emerald-500/30'
                  : 'bg-blue-500/20 text-blue-300 border-blue-500/30'
              }`}>
                {heroStatusChip}
              </div>

              {/* Big Package Picture */}
              <div className="relative w-48 h-48 sm:w-60 sm:h-60 rounded-[2px] overflow-hidden bg-black/50 border border-white/20 flex items-center justify-center shrink-0 shadow-2xl">
                <div className="absolute inset-0 flex items-center justify-center text-zinc-600 pointer-events-none">
                  <svg className="w-16 h-16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5">
                    <rect x="2" y="3" width="20" height="14" rx="2" />
                    <line x1="8" y1="21" x2="16" y2="21" />
                    <line x1="12" y1="17" x2="12" y2="21" />
                  </svg>
                </div>
                <PackageThumbnail src={heroThumbnail} title={heroTitle} small={false} loading="eager" />
              </div>

              {/* Big Title below Package Picture */}
              <h2 className="text-xl sm:text-2xl font-black text-white mt-5 max-w-xl truncate text-center" title={heroTitle}>
                {heroTitle}
              </h2>

              {/* Metadata Subtitle */}
              <p className="text-xs sm:text-sm font-mono text-zinc-400 mt-1 text-center">
                {heroJob.title_id ? `${heroJob.title_id} • ` : ''}
                <span className="uppercase">{heroJob.kind}</span>
                {` • ${formatBytes(heroJob.total_bytes)}`}
                {` • ${queueSourceLabel(heroJob)}`}
              </p>

              {/* Prompt or Error */}
              {heroPrompt ? (
                <p className="text-xs text-blue-300 font-medium mt-1.5 text-center max-w-lg">
                  {heroPrompt}
                </p>
              ) : null}
              {heroError ? (
                <p className="text-xs text-amber-300 font-medium mt-1.5 text-center max-w-lg">
                  {heroError}
                </p>
              ) : null}

              {/* Direct Storage Banner (if direct storage) */}
              {heroJob.is_direct_storage ? (
                <div className="w-full max-w-xl lg:max-w-2xl mx-auto mt-6 p-4 bg-white/[0.04] border border-blue-500/30 rounded-[2px] text-left shadow-lg">
                  <div className="flex items-start space-x-3.5">
                    <div className="w-7 h-7 rounded-full bg-blue-500/20 border border-blue-400/40 flex items-center justify-center shrink-0 mt-0.5 text-blue-300">
                      <svg className="w-4 h-4" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                        <path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z" />
                      </svg>
                    </div>
                    <div className="flex-1 space-y-1">
                      <h3 className="text-xs font-semibold text-white">Direct Storage Installation in Progress</h3>
                      <p className="text-xs text-zinc-300 leading-relaxed">
                        Track installation progress directly in the PS5 Notifications downloads.
                      </p>
                    </div>
                  </div>
                </div>
              ) : (
                <>
                  {/* Current item progress bar: percent & bytes on top, speed & ETA on new line */}
                  <div className="w-full max-w-xl lg:max-w-2xl mx-auto space-y-2 mt-6">
                    <div className="flex justify-between items-center text-xs sm:text-sm px-0.5">
                      <span className="text-zinc-300 font-medium">Current item progress</span>
                      <span className="text-zinc-400 font-mono">
                        {heroJob.state === 'installing' ? (
                          `${Math.max(0, heroJob.progress).toFixed(1)}% · ${formatBytes(heroJob.downloaded_bytes)} / ${formatBytes(heroJob.total_bytes)}`
                        ) : heroJob.state === 'completed' ? (
                          <span className="text-emerald-400 font-semibold">100% · Complete</span>
                        ) : (
                          <span>{heroStateLabel}</span>
                        )}
                      </span>
                    </div>
                    <div
                      role="progressbar"
                      aria-label={`${heroTitle} installation progress`}
                      aria-valuenow={heroJob.state === 'completed' ? 100 : Math.floor(Math.max(0, heroJob.progress))}
                      aria-valuemin={0}
                      aria-valuemax={100}
                      className="w-full bg-white/10 rounded-[2px] h-4 sm:h-5 overflow-hidden border border-white/20 p-0.5"
                    >
                      <div
                        className={`h-full rounded-[2px] transition-all duration-300 ${
                          heroJob.state === 'completed' ? 'bg-emerald-500' : 'bg-[#0070d1]'
                        }`}
                        style={{
                          width: `${heroJob.state === 'completed' ? 100 : Math.min(100, Math.max(0, heroJob.progress))}%`
                        }}
                      />
                    </div>
                    {/* Speed and remaining time on a new line */}
                    {heroJob.state === 'installing' && (queue.installSpeed > 0 || etaText) && (
                      <div className="flex justify-between items-center text-xs font-mono text-zinc-400 px-0.5">
                        <span>{queue.installSpeed > 0 ? `${formatBytes(queue.installSpeed)}/s` : ''}</span>
                        <span>{etaText ? `Remaining: ${etaText}` : ''}</span>
                      </div>
                    )}
                  </div>

                  {/* Option A: Total queue progress bar directly underneath current item progress bar */}
                  {showRightQueue && isMultiItemBatch && totalJobCount > 1 && (
                    <div className="w-full max-w-xl lg:max-w-2xl mx-auto space-y-2 mt-4 pt-4 border-t border-white/10">
                      <div className="flex justify-between items-center text-xs sm:text-sm px-0.5">
                        <span className="text-zinc-300 font-medium">Total queue progress</span>
                        <span className="text-zinc-400 font-mono">
                          {overview.percent === null ? 'Working…' : `${Math.floor(overview.percent)}%`} · {currentJobIndex}/{totalJobCount}
                        </span>
                      </div>
                      <div
                        role="progressbar"
                        aria-label="Overall queue progress"
                        aria-valuenow={overview.percent !== null ? Math.floor(overview.percent) : 0}
                        aria-valuemin={0}
                        aria-valuemax={100}
                        className="w-full bg-white/10 rounded-[2px] h-3.5 sm:h-4 overflow-hidden border border-white/20 p-0.5"
                      >
                        <div
                          className="h-full bg-cyan-500 rounded-[2px] transition-all duration-300"
                          style={{ width: `${overview.percent !== null ? Math.min(100, Math.max(0, overview.percent)) : 0}%` }}
                        />
                      </div>
                      {totalEtaText && (
                        <div className="flex justify-end items-center text-xs font-mono text-zinc-400 px-0.5">
                          <span>Total remaining: {totalEtaText}</span>
                        </div>
                      )}
                    </div>
                  )}
                </>
              )}

              {debugEnabled && heroJob.source_id === queue.sourceId && (
                <div className="w-full max-w-xl lg:max-w-2xl mx-auto mt-3 pt-2 border-t border-white/10">
                  <DebugSpeedOverlay inline uploadSpeed={queue.uploadSpeed} installSpeed={queue.installSpeed} />
                </div>
              )}

              {/* Actions for Hero Item */}
              {(heroCancellable || heroRetryable || heroJob.state === 'completed') && (
                <div className="flex justify-center gap-3 mt-6">
                  {heroJob.state === 'completed' && !heroCancellable && !heroRetryable && (
                    <button
                      type="button"
                      onClick={onBack}
                      className="px-6 py-2.5 rounded-[2px] ps5-focus-item bg-blue-600 hover:bg-blue-500 text-white text-sm font-bold transition-colors cursor-pointer shadow-md"
                    >
                      Browse Packages
                    </button>
                  )}
                  {heroCancellable && (
                    <button
                      type="button"
                      disabled={!queue.connected}
                      onClick={() => queue.cancel(heroJob)}
                      className="px-6 py-2.5 rounded-[2px] ps5-focus-item bg-white/10 hover:bg-rose-600/80 border border-white/20 text-sm font-semibold text-zinc-200 hover:text-white transition-all cursor-pointer disabled:opacity-50"
                    >
                      {heroActive ? 'Cancel install' : 'Remove from queue'}
                    </button>
                  )}
                  {heroRetryable && (
                    <button
                      type="button"
                      disabled={!queue.connected}
                      onClick={() => queue.retry(heroJob)}
                      className="px-6 py-2.5 rounded-[2px] ps5-focus-item bg-blue-600 hover:bg-blue-500 text-white text-sm font-semibold rounded-[2px] transition-colors cursor-pointer disabled:opacity-50"
                    >
                      Retry
                    </button>
                  )}
                </div>
              )}
            </article>
          ) : (
            <div className="p-12 text-center space-y-4 flex flex-col items-center justify-center min-h-[460px]">
              <div className="w-16 h-16 mx-auto rounded-full bg-blue-500/10 border border-blue-500/20 flex items-center justify-center text-blue-400">
                <svg className="w-8 h-8" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                  <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
                  <polyline points="7 10 12 15 17 10" />
                  <line x1="12" y1="15" x2="12" y2="3" />
                </svg>
              </div>
              <h2 className="text-xl font-bold text-white">Install Queue is Empty</h2>
              <p className="text-sm text-zinc-400 max-w-md mx-auto">
                No installations yet. Install a package to add it here.
              </p>
              <button
                type="button"
                onClick={onBack}
                className="px-5 py-2.5 rounded-[2px] ps5-focus-item bg-blue-600 hover:bg-blue-500 text-white text-sm font-bold transition-colors cursor-pointer"
              >
                Browse Packages
              </button>
            </div>
          )}
        </div>

        {/* Right Column: Queue List Cards (Only visible if >1 active/pending items) */}
        {showRightQueue && (
          <div className="w-full lg:w-[40%] flex flex-col min-h-0 rounded-[2px] bg-[#141520] border border-white/10 p-5 shadow-xl">
            <div className="flex items-center justify-between pb-3 border-b border-white/10 shrink-0">
              <h3 className="text-base font-bold text-white">Install Queue</h3>
              {overview?.resolved > 0 && (
                <button
                  type="button"
                  disabled={!queue.connected}
                  onClick={queue.clearFinished}
                  className="px-3 py-1 rounded-[2px] ps5-focus-item bg-white/10 hover:bg-white/15 border border-white/10 text-zinc-200 text-xs font-semibold transition-colors disabled:opacity-50 cursor-pointer"
                >
                  Clear completed
                </button>
              )}
            </div>

            <div className="flex-1 min-h-0 overflow-y-auto space-y-3.5 pr-1.5 mt-3 max-h-[calc(100vh-80px)] ps5-scrollbar">
              {ordered.map((job) => {
                const isHero = heroJob?.id === job.id;
                const active = ['checking', 'preparing', 'installing', 'canceling'].includes(job.state);
                const retryable = ['failed', 'canceled', 'blocked'].includes(job.state);
                const cancellable = pendingStates.has(job.state) && job.state !== 'canceling' && !(active && job.is_direct_storage);
                const isInstalled = ['completed', 'submitted'].includes(job.state);
                const title = job.title_name || job.path || 'Package';
                const local = job.source_id === queue.sourceId ? queue.files?.find((file) => file.id === job.file_key) : null;
                const thumbnail = local?.iconUrl || ((!job.source_id || job.state === 'installing') ? iconUrlFor(job.path) : null);
                const prompt = active ? queuePrompt(job) : '';
                const stateLabel = queueStateLabel(job);
                const error = job.error && job.error.trim().toLowerCase() !== stateLabel.toLowerCase() ? job.error : '';

                return (
                  <article
                    key={job.id}
                    onClick={() => setSelectedJobId(job.id)}
                    className={`p-4 border rounded-[2px] cursor-pointer transition-all ${
                      isHero
                        ? 'border-blue-500 bg-blue-500/15 shadow-md ring-1 ring-blue-500/50'
                        : active
                        ? 'bg-blue-500/5 border-blue-500/30 hover:border-blue-500/60'
                        : 'bg-white/[0.02] border-white/10 hover:border-white/20'
                    }`}
                  >
                    <div className="flex items-center gap-3.5">
                      {/* Bigger Thumbnail */}
                      <div className="relative w-16 h-16 sm:w-20 sm:h-20 shrink-0 overflow-hidden rounded-[2px] bg-black/50 border border-white/15 shadow-md">
                        <PackageThumbnail src={thumbnail} title={title} small={false} loading={active ? 'eager' : 'lazy'} />
                      </div>

                      {/* Item Details */}
                      <div className="flex-1 min-w-0">
                        <div className="flex items-center gap-2">
                          <h4 className="font-bold text-white truncate text-sm sm:text-base leading-tight" title={title}>
                            {title}
                          </h4>
                          <span className="text-[11px] text-zinc-400 shrink-0 uppercase font-mono bg-white/5 px-1.5 py-0.5 rounded-[2px] border border-white/10">
                            {job.kind}
                          </span>
                        </div>

                        <p className={`text-xs mt-1 font-medium ${
                          ['failed', 'blocked', 'aborted'].includes(job.state)
                            ? 'text-amber-300'
                            : ['completed', 'submitted'].includes(job.state)
                            ? 'text-emerald-400'
                            : 'text-blue-300'
                        }`}>
                          {stateLabel}
                        </p>

                        <p className="text-xs text-zinc-400 mt-0.5 truncate font-mono">
                          {job.title_id ? `${job.title_id} · ` : ''}{formatBytes(job.total_bytes)}
                        </p>

                        {prompt && <p className="text-xs text-zinc-300 mt-1 break-words">{prompt}</p>}

                        {job.state === 'installing' && !job.is_direct_storage && !job.waiting_for_disc && (
                          <div className="mt-2 text-xs text-zinc-300">
                            <div className="flex justify-between font-mono text-[11px] text-zinc-400">
                              <span>{Math.max(0, job.progress).toFixed(1)}%</span>
                              <span>{formatBytes(job.downloaded_bytes)} / {formatBytes(job.total_bytes)}</span>
                            </div>
                            <div className="h-1.5 bg-white/10 mt-1 overflow-hidden rounded-[1px]">
                              <div className="h-full bg-blue-500 rounded-[1px] transition-all duration-300" style={{ width: `${Math.min(100, Math.max(0, job.progress))}%` }} />
                            </div>
                          </div>
                        )}

                        {error && <p className="text-xs text-amber-300 mt-1 break-words">{error}</p>}
                      </div>

                      {/* Square Action / Status on the Right to Cancel / Clear / Retry / Checkmark */}
                      {(cancellable || retryable || isInstalled) && (
                        <div className="shrink-0 flex items-center justify-center pl-1" onClick={(e) => (cancellable || retryable ? e.stopPropagation() : undefined)}>
                          {cancellable && (
                            <button
                              type="button"
                              disabled={!queue.connected}
                              onClick={() => queue.cancel(job)}
                              title={active ? 'Cancel install' : 'Remove from queue'}
                              aria-label={active ? 'Cancel install' : 'Remove from queue'}
                              className="w-10 h-10 rounded-[2px] ps5-focus-item bg-white/5 hover:bg-rose-600/30 border border-white/15 hover:border-rose-500/50 text-zinc-400 hover:text-white flex items-center justify-center transition-all cursor-pointer disabled:opacity-50"
                            >
                              <span className="sr-only">{active ? 'Cancel install' : 'Remove from queue'}</span>
                              <svg className="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" strokeLinejoin="round">
                                <line x1="18" y1="6" x2="6" y2="18" />
                                <line x1="6" y1="6" x2="18" y2="18" />
                              </svg>
                            </button>
                          )}
                          {retryable && (
                            <button
                              type="button"
                              disabled={!queue.connected}
                              onClick={() => queue.retry(job)}
                              title="Retry"
                              aria-label="Retry installation"
                              className="w-10 h-10 rounded-[2px] ps5-focus-item bg-blue-600/20 hover:bg-blue-600 border border-blue-500/40 text-blue-300 hover:text-white flex items-center justify-center transition-all cursor-pointer disabled:opacity-50"
                            >
                              <span className="sr-only">Retry</span>
                              <svg className="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" strokeLinejoin="round">
                                <polyline points="23 4 23 10 17 10" />
                                <path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10" />
                              </svg>
                            </button>
                          )}
                          {isInstalled && (
                            <div
                              title={job.state === 'submitted' ? 'Submitted' : 'Installed'}
                              aria-label={job.state === 'submitted' ? 'Submitted' : 'Installed'}
                              className="w-10 h-10 rounded-[2px] bg-emerald-500/15 border border-emerald-500/30 text-emerald-400 flex items-center justify-center pointer-events-none"
                            >
                              <svg className="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" strokeLinejoin="round">
                                <polyline points="20 6 9 17 4 12" />
                              </svg>
                            </div>
                          )}
                        </div>
                      )}
                    </div>
                  </article>
                );
              })}
            </div>
          </div>
        )}
      </div>
    </div>
  );
}
