// Copyright 2026 Haruhiro Obora
// SPDX-License-Identifier: BSD-3-Clause

/**
 * Pure focused-application resolution and sanitization logic for MoGRU.
 *
 * Kept free of GNOME Shell runtime imports (`Meta`, `Shell`, `Main`) so it can
 * be unit-tested standalone with `gjs`.
 */

const COMPOSITOR_OR_PROXY_NAMES = new Set([
  'gnome-shell',
  'mutter',
  'kwin_wayland',
  'weston',
  'xwayland',
  'ibus-portal',
  'xdg-desktop-portal',
  'xdg-desktop-portal-gnome',
  'xdg-desktop-portal-gtk',
  'bwrap',
  'flatpak-session-helper',
  'xdg-dbus-proxy',
  'snap-confine',
  'snap-exec',
  'chrome-sandbox',
  'apprun',
]);

const GENERIC_RUNTIME_EXACT = new Set([
  'python',
  'node',
  'nodejs',
  'deno',
  'bun',
  'electron',
  'gjs',
  'gjs-console',
  'java',
  'javaw',
  'ruby',
  'perl',
  'php',
  'lua',
  'luajit',
  'dotnet',
  'mono',
  'sh',
  'bash',
  'zsh',
  'fish',
  'dash',
  'env',
  'wine',
  'wine64',
  'wine-preloader',
  'wine64-preloader',
]);

const GENERIC_RUNTIME_PATTERNS = [
  /^python\d+(\.\d+)*$/,
  /^electron\d+$/,
  /^ruby\d+(\.\d+)*$/,
  /^php\d+(\.\d+)*$/,
  /^ld-linux/,
];

/**
 * Normalizes an executable path from `/proc/<pid>/exe` to its basename.
 * Strips the trailing ` (deleted)` marker Linux appends when the binary was
 * replaced or unlinked after launch.
 *
 * @param {string|null|undefined} targetPath
 * @returns {string}
 */
export function extractExeBasename(targetPath) {
  if (typeof targetPath !== 'string') {
    return '';
  }
  let normalized = targetPath.trim();
  if (!normalized) {
    return '';
  }
  if (normalized.endsWith(' (deleted)')) {
    normalized = normalized.slice(0, -' (deleted)'.length).trim();
  }
  const slashIdx = normalized.lastIndexOf('/');
  const base = slashIdx >= 0 ? normalized.slice(slashIdx + 1) : normalized;
  return base.trim();
}

/**
 * Strips `.desktop` suffix and ignores synthetic GNOME `window:<id>` app IDs.
 *
 * @param {string|null|undefined} rawId
 * @returns {string}
 */
export function normalizeDesktopAppId(rawId) {
  if (typeof rawId !== 'string') {
    return '';
  }
  let trimmed = rawId.trim();
  if (!trimmed || trimmed.startsWith('window:')) {
    return '';
  }
  if (trimmed.endsWith('.desktop')) {
    trimmed = trimmed.slice(0, -'.desktop'.length).trim();
  }
  return trimmed;
}

/**
 * Checks whether an identifier represents a compositor, sandbox proxy, or
 * portal rather than an end-user application.
 *
 * @param {string|null|undefined} name
 * @returns {boolean}
 */
export function isCompositorOrProxyName(name) {
  if (typeof name !== 'string') {
    return false;
  }
  const lower = name.trim().toLowerCase();
  if (!lower) {
    return false;
  }
  if (COMPOSITOR_OR_PROXY_NAMES.has(lower)) {
    return true;
  }
  return lower.startsWith('ld-linux');
}

/**
 * Checks whether an executable basename is a generic language/app runtime
 * where window metadata (`sandboxed_app_id`, `wm_class`, `.desktop` ID) gives a
 * more accurate application identity.
 *
 * @param {string|null|undefined} name
 * @returns {boolean}
 */
export function isGenericRuntimeName(name) {
  if (typeof name !== 'string') {
    return false;
  }
  const lower = name.trim().toLowerCase();
  if (!lower) {
    return false;
  }
  if (GENERIC_RUNTIME_EXACT.has(lower)) {
    return true;
  }
  return GENERIC_RUNTIME_PATTERNS.some((pattern) => pattern.test(lower));
}

/**
 * Sanitizes an application identifier so it never breaks MoGRU's
 * `app_profile_map` (`,` / `:` delimiters) or inline command parser (`)` / `]`),
 * and filters out compositor/proxy names.
 *
 * @param {string|null|undefined} rawName
 * @returns {string}
 */
export function sanitizeAppName(rawName) {
  if (typeof rawName !== 'string') {
    return '';
  }
  let cleaned = rawName.trim();
  if (!cleaned) {
    return '';
  }
  cleaned = cleaned.replace(/[,:\r\n\t()[\]]+/g, '-');
  cleaned = cleaned.replace(/^-+|-+$/g, '').trim();
  if (!cleaned || isCompositorOrProxyName(cleaned)) {
    return '';
  }
  return cleaned;
}

/**
 * Returns an empty focused-app info structure.
 *
 * @returns {object}
 */
export function createEmptyAppInfo() {
  return {
    app_name: '',
    process_name: '',
    wm_class: '',
    wm_class_instance: '',
    app_id: '',
    gtk_application_id: '',
    sandboxed_app_id: '',
    pid: 0,
    is_wayland: false,
  };
}

/**
 * Resolves the primary application name and structured metadata from a focused
 * window snapshot and procfs reader callbacks.
 *
 * @param {object|null|undefined} snapshot
 * @param {object} [procReader]
 * @param {function(number): (string|null)} [procReader.readExeTarget]
 * @param {function(number): (string|null)} [procReader.readComm]
 * @returns {object}
 */
export function resolveFocusedWindowInfo(snapshot, procReader = {}) {
  if (!snapshot) {
    return createEmptyAppInfo();
  }

  const pid =
    Number.isInteger(snapshot.pid) && snapshot.pid > 0 ? snapshot.pid : 0;
  const wmClass =
    typeof snapshot.wm_class === 'string' ? snapshot.wm_class.trim() : '';
  const wmClassInstance =
    typeof snapshot.wm_class_instance === 'string'
      ? snapshot.wm_class_instance.trim()
      : '';
  const gtkApplicationId = normalizeDesktopAppId(snapshot.gtk_application_id);
  const sandboxedAppId = normalizeDesktopAppId(snapshot.sandboxed_app_id);
  const appId = normalizeDesktopAppId(snapshot.app_id);
  const isWayland = Boolean(snapshot.is_wayland);

  let exeBase = '';
  let commBase = '';
  if (pid > 0) {
    if (typeof procReader.readExeTarget === 'function') {
      exeBase = extractExeBasename(procReader.readExeTarget(pid));
    }
    if (typeof procReader.readComm === 'function') {
      const rawComm = procReader.readComm(pid);
      commBase = typeof rawComm === 'string' ? rawComm.trim() : '';
    }
  }

  const processName = exeBase || commBase || '';

  const pickUsable = (candidate) => {
    const sanitized = sanitizeAppName(candidate);
    if (!sanitized) {
      return '';
    }
    return sanitized;
  };

  let appName = '';

  // 1. Prefer exact executable basename from `/proc/<pid>/exe` when it is not a
  //    compositor, sandbox proxy, or generic runtime.
  if (
    exeBase &&
    !isCompositorOrProxyName(exeBase) &&
    !isGenericRuntimeName(exeBase)
  ) {
    appName = pickUsable(exeBase);
  }

  // 2. Fallback chain for sandboxed apps (Flatpak/Snap), unreadable `/proc/<pid>/exe`
  //    (EACCES), generic runtimes (Electron/Python/Node), or missing PID.
  if (!appName) {
    const metadataCandidates = [
      sandboxedAppId,
      gtkApplicationId,
      wmClass,
      wmClassInstance,
      appId,
    ];
    for (const candidate of metadataCandidates) {
      const resolved = pickUsable(candidate);
      if (resolved) {
        appName = resolved;
        break;
      }
    }
  }

  // 3. Fallback to `/proc/<pid>/comm` if `/proc/<pid>/exe` was unreadable and
  //    window metadata yielded nothing.
  if (
    !appName &&
    commBase &&
    !isCompositorOrProxyName(commBase) &&
    !isGenericRuntimeName(commBase)
  ) {
    appName = pickUsable(commBase);
  }

  // 4. Last resort: if a generic runtime had zero metadata, report the runtime
  //    name rather than returning empty when a real non-compositor PID exists.
  if (!appName && processName && !isCompositorOrProxyName(processName)) {
    appName = pickUsable(processName);
  }

  return {
    app_name: appName,
    process_name: processName,
    wm_class: wmClass,
    wm_class_instance: wmClassInstance,
    app_id: appId,
    gtk_application_id: gtkApplicationId,
    sandboxed_app_id: sandboxedAppId,
    pid,
    is_wayland: isWayland,
  };
}

/**
 * Checks whether two resolved app-info objects are identical.
 *
 * @param {object|null|undefined} a
 * @param {object|null|undefined} b
 * @returns {boolean}
 */
export function isSameAppInfo(a, b) {
  if (!a || !b) {
    return a === b;
  }
  return (
    a.app_name === b.app_name &&
    a.process_name === b.process_name &&
    a.wm_class === b.wm_class &&
    a.wm_class_instance === b.wm_class_instance &&
    a.app_id === b.app_id &&
    a.gtk_application_id === b.gtk_application_id &&
    a.sandboxed_app_id === b.sandboxed_app_id &&
    a.pid === b.pid &&
    a.is_wayland === b.is_wayland
  );
}
