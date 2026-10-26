import assert from 'node:assert/strict';
import test from 'node:test';
import { passthroughJobPath, remoteHttpsFilename } from '../src/utils/remoteHttpsFile.js';

test('passthrough keeps the link for the PS5 installer', () => {
  const link = passthroughJobPath(' HTTPS://cdn.example/dir/Game.pkg#part ');
  assert.equal(link.url, 'https://cdn.example/dir/Game.pkg');
  assert.equal(link.path, 'pass:https://cdn.example/dir/Game.pkg');
  assert.equal(link.filename, 'Game.pkg');
  assert.throws(() => passthroughJobPath('ftp://cdn.example/a.pkg'), /Passthrough/);
  assert.throws(() => passthroughJobPath('https://user:secret@cdn.example/a.pkg'), /username/);
});

test('remote https filename uses the last path segment', () => {
  assert.equal(remoteHttpsFilename('https://cdn.example/games/My%20Game.pkg?token=1'), 'My Game.pkg');
  assert.equal(remoteHttpsFilename('https://cdn.example/'), 'package.pkg');
});
