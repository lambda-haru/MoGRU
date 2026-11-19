// Copyright 2026 Haruhiro Obora
// SPDX-License-Identifier: BSD-3-Clause

import {
  extractExeBasename,
  isCompositorOrProxyName,
  isGenericRuntimeName,
  isSameAppInfo,
  normalizeDesktopAppId,
  resolveFocusedWindowInfo,
  sanitizeAppName,
} from './app_resolver.js';

function assertEqual(actual, expected, label) {
  if (actual !== expected) {
    throw new Error(
      `Assertion failed [${label}]: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`
    );
  }
}

function assertTrue(cond, label) {
  if (!cond) {
    throw new Error(`Assertion failed [${label}]: expected truthy`);
  }
}

function assertFalse(cond, label) {
  if (cond) {
    throw new Error(`Assertion failed [${label}]: expected falsy`);
  }
}

function testBasicHelpers() {
  assertEqual(extractExeBasename('/usr/share/code/code'), 'code', 'exe path');
  assertEqual(
    extractExeBasename('/usr/bin/firefox (deleted)'),
    'firefox',
    'deleted exe path'
  );
  assertEqual(extractExeBasename(null), '', 'null exe path');

  assertEqual(
    normalizeDesktopAppId('org.gnome.Nautilus.desktop'),
    'org.gnome.Nautilus',
    'strip .desktop'
  );
  assertEqual(
    normalizeDesktopAppId('window:12'),
    '',
    'ignore synthetic window: id'
  );

  assertTrue(isCompositorOrProxyName('gnome-shell'), 'gnome-shell proxy');
  assertTrue(isCompositorOrProxyName('Xwayland'), 'Xwayland proxy');
  assertTrue(isCompositorOrProxyName('bwrap'), 'bwrap proxy');
  assertTrue(
    isCompositorOrProxyName('xdg-desktop-portal-gnome'),
    'portal proxy'
  );
  assertFalse(isCompositorOrProxyName('code'), 'code not proxy');

  assertTrue(isGenericRuntimeName('python3.12'), 'python3.12 runtime');
  assertTrue(isGenericRuntimeName('electron32'), 'electron32 runtime');
  assertTrue(isGenericRuntimeName('node'), 'node runtime');
  assertFalse(isGenericRuntimeName('slack'), 'slack not runtime');

  assertEqual(
    sanitizeAppName('code:test,app(1)[2]'),
    'code-test-app-1-2',
    'sanitize delimiters'
  );
  assertEqual(sanitizeAppName('gnome-shell'), '', 'sanitize rejects proxy');
}

function testNullWindow() {
  const info = resolveFocusedWindowInfo(null, {
    readExeTarget: () => '/usr/bin/code',
  });
  assertEqual(info.app_name, '', 'null window app_name');
  assertEqual(info.pid, 0, 'null window pid');
  assertFalse(info.is_wayland, 'null window is_wayland');
}

function testNativeProcessPriorityOverMetadata() {
  const info = resolveFocusedWindowInfo(
    {
      pid: 4242,
      wm_class: 'Code',
      wm_class_instance: 'code',
      gtk_application_id: 'com.visualstudio.code',
      sandboxed_app_id: '',
      app_id: 'code.desktop',
      is_wayland: true,
    },
    {
      readExeTarget: (pid) =>
        pid === 4242 ? '/usr/share/code/code' : null,
      readComm: () => 'code',
    }
  );
  assertEqual(info.app_name, 'code', 'native exe wins');
  assertEqual(info.process_name, 'code', 'process_name populated');
  assertEqual(info.app_id, 'code', 'app_id stripped');
  assertEqual(info.pid, 4242, 'pid preserved');
  assertTrue(info.is_wayland, 'wayland preserved');
}

function testLongExecutableNotTruncated() {
  const info = resolveFocusedWindowInfo(
    {
      pid: 777,
      wm_class: 'Gnome-terminal',
      app_id: 'org.gnome.Terminal.desktop',
      is_wayland: true,
    },
    {
      readExeTarget: () => '/usr/libexec/gnome-terminal-server',
      readComm: () => 'gnome-terminal-',
    }
  );
  assertEqual(
    info.app_name,
    'gnome-terminal-server',
    'exe avoids 15-char comm truncation'
  );
}

function testFlatpakBwrapFallbackToSandboxedAppId() {
  const info = resolveFocusedWindowInfo(
    {
      pid: 888,
      sandboxed_app_id: 'com.visualstudio.code',
      gtk_application_id: 'com.visualstudio.code',
      wm_class: 'Code',
      app_id: 'com.visualstudio.code.desktop',
      is_wayland: true,
    },
    {
      readExeTarget: () => '/usr/bin/bwrap',
      readComm: () => 'bwrap',
    }
  );
  assertEqual(
    info.app_name,
    'com.visualstudio.code',
    'bwrap falls back to sandboxed_app_id'
  );
}

function testSnapEaccesFallbackToMetadataThenComm() {
  const withMetadata = resolveFocusedWindowInfo(
    {
      pid: 999,
      sandboxed_app_id: '',
      gtk_application_id: '',
      wm_class: 'firefox',
      app_id: 'firefox_firefox.desktop',
      is_wayland: true,
    },
    {
      readExeTarget: () => null, // EACCES
      readComm: () => 'firefox',
    }
  );
  assertEqual(
    withMetadata.app_name,
    'firefox',
    'EACCES uses wm_class metadata'
  );
  assertEqual(withMetadata.process_name, 'firefox', 'process_name uses comm');

  const withoutMetadata = resolveFocusedWindowInfo(
    {
      pid: 1000,
      sandboxed_app_id: '',
      gtk_application_id: '',
      wm_class: '',
      app_id: 'window:5',
      is_wayland: false,
    },
    {
      readExeTarget: () => null,
      readComm: () => 'custom-x11-app',
    }
  );
  assertEqual(
    withoutMetadata.app_name,
    'custom-x11-app',
    'fallback to comm when exe unreadable and metadata absent'
  );
}

function testGenericRuntimeFallback() {
  const electronApp = resolveFocusedWindowInfo(
    {
      pid: 2020,
      wm_class: 'obsidian',
      app_id: 'obsidian.desktop',
      is_wayland: true,
    },
    {
      readExeTarget: () => '/usr/lib/electron32/electron',
      readComm: () => 'electron',
    }
  );
  assertEqual(
    electronApp.app_name,
    'obsidian',
    'generic electron runtime falls back to wm_class'
  );

  const rawPythonScript = resolveFocusedWindowInfo(
    {
      pid: 2021,
      is_wayland: true,
    },
    {
      readExeTarget: () => '/usr/bin/python3.14',
      readComm: () => 'python3',
    }
  );
  assertEqual(
    rawPythonScript.app_name,
    'python3.14',
    'raw runtime without metadata uses runtime name as last resort'
  );
}

function testXWaylandWithoutPid() {
  const info = resolveFocusedWindowInfo(
    {
      pid: 0,
      wm_class: 'Slack',
      wm_class_instance: 'slack',
      app_id: 'slack.desktop',
      is_wayland: false,
    },
    {
      readExeTarget: () => {
        throw new Error('should not read pid 0');
      },
    }
  );
  assertEqual(info.app_name, 'Slack', 'missing pid uses wm_class');
}

function testIsSameAppInfo() {
  const a = resolveFocusedWindowInfo(
    {pid: 10, window_id: 1001, wm_class: 'Code', is_wayland: true},
    {readExeTarget: () => '/usr/bin/code'}
  );
  const b = resolveFocusedWindowInfo(
    {pid: 10, window_id: 1001, wm_class: 'Code', is_wayland: true},
    {readExeTarget: () => '/usr/bin/code'}
  );
  const c = resolveFocusedWindowInfo(
    {pid: 11, window_id: 1001, wm_class: 'Code', is_wayland: true},
    {readExeTarget: () => '/usr/bin/code'}
  );
  const d = resolveFocusedWindowInfo(
    {pid: 10, window_id: 1002, wm_class: 'Code', is_wayland: true},
    {readExeTarget: () => '/usr/bin/code'}
  );
  assertEqual(a.window_id, 1001, 'window_id preserved');
  assertTrue(isSameAppInfo(a, b), 'identical app infos match');
  assertFalse(isSameAppInfo(a, c), 'different pids differ');
  assertFalse(isSameAppInfo(a, d), 'different window_ids differ');
}

function main() {
  testBasicHelpers();
  testNullWindow();
  testNativeProcessPriorityOverMetadata();
  testLongExecutableNotTruncated();
  testFlatpakBwrapFallbackToSandboxedAppId();
  testSnapEaccesFallbackToMetadataThenComm();
  testGenericRuntimeFallback();
  testXWaylandWithoutPid();
  testIsSameAppInfo();
  print('ALL app_resolver_test checks passed!');
}

main();
