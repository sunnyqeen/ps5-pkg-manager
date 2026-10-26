// Passthrough stores an http(s) link for the PS5 installer. The browser does not fetch it.

export function remoteHttpsFilename(url) {
  let segment = '';
  try {
    segment = new URL(url).pathname.split('/').filter(Boolean).pop() || '';
  } catch (e) {
    segment = '';
  }
  let name = segment;
  try { name = decodeURIComponent(segment); } catch (e) { /* Keep the raw segment. */ }
  name = name.replace(/[\u0000-\u001f\\/]/g, '').trim();
  return name || 'package.pkg';
}

export function passthroughJobPath(url) {
  const trimmed = String(url || '').trim();
  let parsed;
  try { parsed = new URL(trimmed); } catch (e) {
    throw new Error('That link is not a valid http:// or https:// address');
  }
  if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') {
    throw new Error('Passthrough accepts an http:// or https:// link');
  }
  if (parsed.username || parsed.password) throw new Error('Links with a username or password are not supported');
  parsed.hash = '';
  const link = parsed.href;
  const path = `pass:${link}`;
  if (path.length >= 512) throw new Error('That link is too long');
  return { path, url: link, filename: remoteHttpsFilename(link) };
}
