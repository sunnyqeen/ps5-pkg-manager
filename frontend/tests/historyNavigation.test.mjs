import assert from 'node:assert/strict';
import test from 'node:test';
import {
  getRouteFromHash,
  formatHash,
  getHistoryChain,
  writeHistory,
  resolveDrive,
  getSmbShareFromStorage,
} from '../src/hooks/useHistoryNavigation.js';
import { ALL_SOURCES_DRIVE } from '../src/constants/config.js';
import { getSourceInfo } from '../src/utils/sourceInfo.js';

test('getRouteFromHash parses drive and title hashes correctly', () => {
  assert.deepEqual(getRouteFromHash(''), { type: 'drives' });
  assert.deepEqual(getRouteFromHash('#/'), { type: 'drives' });
  assert.deepEqual(getRouteFromHash('#/queue'), { type: 'queue' });
  assert.deepEqual(getRouteFromHash('#/settings'), { type: 'settings' });
  assert.deepEqual(getRouteFromHash('#/smb'), { type: 'smb' });
  assert.deepEqual(getRouteFromHash('#/direct-install'), { type: 'direct-install' });
  assert.deepEqual(getRouteFromHash('#/drive/smb_muh6rn5i'), { type: 'drive', driveId: 'smb_muh6rn5i' });
  assert.deepEqual(getRouteFromHash('#/drive/%2Fmnt%2Fusb0'), { type: 'drive', driveId: '/mnt/usb0' });
  assert.deepEqual(getRouteFromHash('#/title/CUSA00123'), { type: 'title', titleId: 'CUSA00123' });
});

test('resolveDrive returns ALL_SOURCES_DRIVE for __all__', () => {
  const result = resolveDrive('__all__', []);
  assert.equal(result.id, '__all__');
  assert.equal(result.label, ALL_SOURCES_DRIVE.label);
});

test('resolveDrive matches drive from drives list when available', () => {
  const drives = [
    { id: 'usb0', label: 'USB Drive 0', path: '/mnt/usb0', type: 'usb', mounted: true, pkg_count: 3 },
    { id: 'smb_muh6rn5i', label: 'Home NAS', path: 'smb://192.168.1.100/games', type: 'smb', mounted: true, pkg_count: 12 },
  ];
  const foundUsb = resolveDrive('usb0', drives);
  assert.equal(foundUsb.label, 'USB Drive 0');
  assert.equal(foundUsb.pkg_count, 3);

  const foundSmb = resolveDrive('smb_muh6rn5i', drives);
  assert.equal(foundSmb.label, 'Home NAS');
  assert.equal(foundSmb.path, 'smb://192.168.1.100/games');
  assert.equal(foundSmb.pkg_count, 12);
});

test('resolveDrive falls back to localStorage SMB shares before drives list is loaded', () => {
  const mockStorage = {
    pkgmgr_settings: JSON.stringify({
      smb_shares: [
        {
          id: 'smb_muh6rn5i',
          label: 'My TrueNAS',
          server: '192.168.1.50',
          port: 445,
          share: 'PS5_PKGS',
          browse_only: false,
        },
        {
          id: 'smb_custom_port',
          label: '',
          server: 'nas.local',
          port: 4445,
          share: 'games',
          browse_only: true,
        },
      ],
    }),
  };

  const originalWindow = globalThis.window;
  globalThis.window = {
    localStorage: {
      getItem: (key) => mockStorage[key] || null,
    },
  };

  try {
    // Empty drives array represents reload before /api/drives resolves
    const resolved = resolveDrive('smb_muh6rn5i', []);
    assert.equal(resolved.id, 'smb_muh6rn5i');
    assert.equal(resolved.label, 'My TrueNAS');
    assert.equal(resolved.path, 'smb://192.168.1.50/PS5_PKGS');
    assert.equal(resolved.type, 'smb');
    assert.equal(resolved.browse_only, false);

    // Share without custom label falls back to server/share
    const resolvedPort = resolveDrive('smb_custom_port', []);
    assert.equal(resolvedPort.id, 'smb_custom_port');
    assert.equal(resolvedPort.label, 'nas.local/games');
    assert.equal(resolvedPort.path, 'smb://nas.local:4445/games');
    assert.equal(resolvedPort.browse_only, true);

    // Unknown drive falls back to drive ID
    const unknown = resolveDrive('unknown_drive', []);
    assert.equal(unknown.id, 'unknown_drive');
    assert.equal(unknown.label, 'unknown_drive');
  } finally {
    globalThis.window = originalWindow;
  }
});

test('drive reconciliation matches drive by id or path and updates friendly metadata', () => {
  const fallbackDrive = { id: 'smb_muh6rn5i', path: 'smb_muh6rn5i', label: 'smb_muh6rn5i', clickable: true };
  const loadedDrives = [
    { id: 'smb_muh6rn5i', label: 'My NAS Share', path: 'smb://192.168.1.100/games', type: 'smb', pkg_count: 8, mounted: true, clickable: true },
  ];

  const match = loadedDrives.find((d) =>
    (fallbackDrive.id && d.id === fallbackDrive.id) ||
    (fallbackDrive.path && d.path === fallbackDrive.path) ||
    ((d.id || d.path) === (fallbackDrive.id || fallbackDrive.path))
  );

  assert.ok(match);
  assert.equal(match.label, 'My NAS Share');
  assert.equal(match.path, 'smb://192.168.1.100/games');
  assert.equal(match.pkg_count, 8);
});

test('resolveDrive falls back to localStorage SMB share with subpath', () => {
  const mockStorage = {
    pkgmgr_settings: JSON.stringify({
      smb_shares: [
        {
          id: 'smb_ps5',
          label: 'PS5 Games',
          server: '192.168.1.50',
          port: 445,
          share: 'games',
          path: 'PS5',
          browse_only: false,
        },
        {
          id: 'smb_ps4',
          label: 'PS4 Games',
          server: '192.168.1.50',
          port: 445,
          share: 'games',
          path: 'PS4',
          browse_only: false,
        },
      ],
    }),
  };

  const originalWindow = globalThis.window;
  globalThis.window = {
    localStorage: {
      getItem: (key) => mockStorage[key] || null,
    },
  };

  try {
    const ps5Drive = resolveDrive('smb_ps5', []);
    assert.equal(ps5Drive.id, 'smb_ps5');
    assert.equal(ps5Drive.path, 'smb://192.168.1.50/games/PS5');
    assert.equal(ps5Drive.label, 'PS5 Games');

    const ps4Drive = resolveDrive('smb_ps4', []);
    assert.equal(ps4Drive.id, 'smb_ps4');
    assert.equal(ps4Drive.path, 'smb://192.168.1.50/games/PS4');
    assert.equal(ps4Drive.label, 'PS4 Games');
  } finally {
    globalThis.window = originalWindow;
  }
});

test('getSourceInfo labels http and https package links as URL', () => {
  const httpPkg = getSourceInfo('http://192.168.1.20:8080/Astraea_Veil_of_Cygnus.pkg');
  assert.equal(httpPkg.name, 'URL');
  assert.equal(httpPkg.type, 'url');
  const httpsPkg = getSourceInfo('pass:https://cdn.example/games/Solvanna_Sands_of_Aeon.pkg');
  assert.equal(httpsPkg.name, 'URL');
  assert.equal(httpsPkg.id, 'url');
  assert.equal(getSourceInfo('/mnt/usb0/game.pkg').name, 'USB0');
});

test('getSourceInfo matches most specific SMB drive when multiple shares use same server and share', () => {
  const drives = [
    { id: 'smb_ps5', label: 'PS5 Games', path: 'smb://192.168.1.50/games/PS5', type: 'smb' },
    { id: 'smb_ps4', label: 'PS4 Games', path: 'smb://192.168.1.50/games/PS4', type: 'smb' },
  ];

  const ps5Pkg = getSourceInfo('smb://192.168.1.50/games/PS5/title1.pkg', drives);
  assert.equal(ps5Pkg.id, 'smb_ps5');
  assert.equal(ps5Pkg.name, 'PS5 Games');

  const ps4Pkg = getSourceInfo('smb://192.168.1.50/games/PS4/title2.pkg', drives);
  assert.equal(ps4Pkg.id, 'smb_ps4');
  assert.equal(ps4Pkg.name, 'PS4 Games');
});

test('resolveDrive exact match takes precedence when subpath share appears earlier in settings', () => {
  const mockStorage = {
    pkgmgr_settings: JSON.stringify({
      smb_shares: [
        {
          id: 'smb_sub',
          label: 'PS5 Subfolder',
          server: '192.168.1.50',
          port: 445,
          share: 'games',
          path: 'PS5',
        },
        {
          id: 'smb_root',
          label: 'Games Root',
          server: '192.168.1.50',
          port: 445,
          share: 'games',
          path: '',
        },
      ],
    }),
  };

  const originalWindow = globalThis.window;
  globalThis.window = {
    localStorage: {
      getItem: (key) => mockStorage[key] || null,
    },
  };

  try {
    const rootDrive = resolveDrive('smb://192.168.1.50/games', []);
    assert.equal(rootDrive.id, 'smb_root');
    assert.equal(rootDrive.label, 'Games Root');
    assert.equal(rootDrive.path, 'smb://192.168.1.50/games');

    const subDrive = resolveDrive('smb://192.168.1.50/games/PS5', []);
    assert.equal(subDrive.id, 'smb_sub');
    assert.equal(subDrive.label, 'PS5 Subfolder');
  } finally {
    globalThis.window = originalWindow;
  }
});

test('formatHash and getHistoryChain handle queue route correctly', () => {
  assert.equal(formatHash({ type: 'queue' }), '#/queue');
  assert.deepEqual(getHistoryChain({ type: 'queue' }), [{ type: 'drives' }, { type: 'queue' }]);
});

test('writeHistory re-pushes route when history hash changes during back navigation', () => {
  const pushed = [];
  const originalWindow = globalThis.window;
  globalThis.window = {
    location: { hash: '#/' },
    history: {
      pushState: (state, title, url) => pushed.push({ state, title, url }),
    },
  };

  try {
    // Current route was drive/usb0, but user pressed back (popping hash to #/)
    const currentRoute = { type: 'drive', driveId: 'usb0' };
    writeHistory(currentRoute, false);
    assert.equal(pushed.length, 1);
    assert.equal(pushed[0].url, '#/drive/usb0');
    assert.equal(pushed[0].state.type, 'drive');
  } finally {
    globalThis.window = originalWindow;
  }
});

test('writeHistory is idempotent when location hash matches target hash', () => {
  const pushed = [];
  const originalWindow = globalThis.window;
  globalThis.window = {
    location: { hash: '#/drive/usb0' },
    history: {
      pushState: (state, title, url) => pushed.push({ state, title, url }),
    },
  };

  try {
    const currentRoute = { type: 'drive', driveId: 'usb0' };
    writeHistory(currentRoute, false);
    assert.equal(pushed.length, 0);
  } finally {
    globalThis.window = originalWindow;
  }
});
