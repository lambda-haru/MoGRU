// Copyright 2026 Haruhiro Obora
// SPDX-License-Identifier: BSD-3-Clause

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

import {
  createEmptyAppInfo,
  isSameAppInfo,
  resolveFocusedWindowInfo,
} from './app_resolver.js';

const DBUS_NAME = 'io.github.lambda_haru.MoGRU.AppTracker';
const DBUS_PATH = '/io/github/lambda_haru/MoGRU/AppTracker';
const DBUS_INTERFACE = 'io.github.lambda_haru.MoGRU.AppTracker';

const DBUS_INTERFACE_XML = `
<node>
  <interface name="${DBUS_INTERFACE}">
    <method name="GetFocusedApp">
      <arg name="appName" type="s" direction="out"/>
    </method>
    <method name="GetFocusedAppInfo">
      <arg name="info" type="a{sv}" direction="out"/>
    </method>
    <signal name="FocusedAppChanged">
      <arg name="appName" type="s"/>
      <arg name="info" type="a{sv}"/>
    </signal>
  </interface>
</node>
`;

/**
 * Converts a plain app-info record into a `a{sv}` dictionary of `GLib.Variant`s.
 * Window titles are intentionally omitted for privacy.
 *
 * @param {object} info
 * @returns {Record<string, GLib.Variant>}
 */
function buildVariantDict(info) {
  return {
    app_name: GLib.Variant.new_string(info.app_name ?? ''),
    process_name: GLib.Variant.new_string(info.process_name ?? ''),
    wm_class: GLib.Variant.new_string(info.wm_class ?? ''),
    wm_class_instance: GLib.Variant.new_string(info.wm_class_instance ?? ''),
    app_id: GLib.Variant.new_string(info.app_id ?? ''),
    gtk_application_id: GLib.Variant.new_string(info.gtk_application_id ?? ''),
    sandboxed_app_id: GLib.Variant.new_string(info.sandboxed_app_id ?? ''),
    pid: GLib.Variant.new_int32(info.pid ?? 0),
    is_wayland: GLib.Variant.new_boolean(Boolean(info.is_wayland)),
  };
}

export default class MoGRUAppTrackerExtension extends Extension {
  enable() {
    this._textDecoder = new TextDecoder('utf-8');
    this._tracker = Shell.WindowTracker.get_default();
    this._trackedWindow = null;
    this._windowSignalIds = [];
    this._currentInfo = createEmptyAppInfo();

    this._procReader = {
      readExeTarget: (pid) => this._readExeTarget(pid),
      readComm: (pid) => this._readComm(pid),
    };

    this._dbusImpl = Gio.DBusExportedObject.wrapJSObject(
      DBUS_INTERFACE_XML,
      this
    );
    try {
      this._dbusImpl.export(Gio.DBus.session, DBUS_PATH);
    } catch (e) {
      console.error(`[MoGRU] Failed to export D-Bus object at ${DBUS_PATH}: ${e}`);
    }

    const ownFlags =
      Gio.BusNameOwnerFlags.ALLOW_REPLACEMENT |
      Gio.BusNameOwnerFlags.REPLACE;
    this._ownNameId = Gio.DBus.session.own_name(
      DBUS_NAME,
      ownFlags,
      () => {
        this._refreshFocusedApp(false);
      },
      () => {
        this._ownNameId = 0;
      }
    );

    this._focusWindowSignalId = global.display.connect(
      'notify::focus-window',
      () => this._onFocusChanged()
    );
    this._focusAppSignalId = this._tracker.connect(
      'notify::focus-app',
      () => this._onFocusChanged()
    );

    this._rebindWindowSignals(global.display.focus_window ?? null);
    this._refreshFocusedApp(false);
  }

  disable() {
    this._disconnectWindowSignals();

    if (this._focusWindowSignalId) {
      global.display.disconnect(this._focusWindowSignalId);
      this._focusWindowSignalId = 0;
    }
    if (this._focusAppSignalId && this._tracker) {
      this._tracker.disconnect(this._focusAppSignalId);
      this._focusAppSignalId = 0;
    }

    if (this._ownNameId) {
      Gio.DBus.session.unown_name(this._ownNameId);
      this._ownNameId = 0;
    }

    if (this._dbusImpl) {
      try {
        this._dbusImpl.unexport();
      } catch (e) {
        console.error(`[MoGRU] Failed to unexport D-Bus object: ${e}`);
      }
      this._dbusImpl.run_dispose?.();
      this._dbusImpl = null;
    }

    this._tracker = null;
    this._procReader = null;
    this._textDecoder = null;
    this._currentInfo = null;
  }

  /**
   * D-Bus method: returns the resolved primary focused application name.
   * Refreshes synchronously from current window state to guarantee freshness.
   *
   * @returns {string}
   */
  GetFocusedApp() {
    this._refreshFocusedApp(false);
    return this._currentInfo?.app_name ?? '';
  }

  /**
   * D-Bus method: returns structured metadata about the focused application.
   *
   * @returns {Record<string, GLib.Variant>}
   */
  GetFocusedAppInfo() {
    this._refreshFocusedApp(false);
    return buildVariantDict(this._currentInfo ?? createEmptyAppInfo());
  }

  _readExeTarget(pid) {
    try {
      return GLib.file_read_link(`/proc/${pid}/exe`);
    } catch (_e) {
      return null;
    }
  }

  _readComm(pid) {
    try {
      const [ok, bytes] = GLib.file_get_contents(`/proc/${pid}/comm`);
      if (!ok || !bytes || !this._textDecoder) {
        return null;
      }
      return this._textDecoder.decode(bytes);
    } catch (_e) {
      return null;
    }
  }

  _captureWindowSnapshot(window) {
    if (!window) {
      return null;
    }
    let appId = '';
    const shellApp = this._tracker ? this._tracker.get_window_app(window) : null;
    if (shellApp && !(shellApp.is_window_backed && shellApp.is_window_backed())) {
      appId = shellApp.get_id?.() ?? '';
    }
    const isWayland =
      typeof window.get_client_type === 'function'
        ? window.get_client_type() === Meta.WindowClientType.WAYLAND
        : true;

    return {
      pid: typeof window.get_pid === 'function' ? window.get_pid() : 0,
      wm_class: typeof window.get_wm_class === 'function' ? (window.get_wm_class() ?? '') : '',
      wm_class_instance:
        typeof window.get_wm_class_instance === 'function'
          ? (window.get_wm_class_instance() ?? '')
          : '',
      gtk_application_id:
        typeof window.get_gtk_application_id === 'function'
          ? (window.get_gtk_application_id() ?? '')
          : '',
      sandboxed_app_id:
        typeof window.get_sandboxed_app_id === 'function'
          ? (window.get_sandboxed_app_id() ?? '')
          : '',
      app_id: appId,
      is_wayland: isWayland,
    };
  }

  _disconnectWindowSignals() {
    if (this._trackedWindow && this._windowSignalIds.length > 0) {
      for (const id of this._windowSignalIds) {
        this._trackedWindow.disconnect(id);
      }
    }
    this._windowSignalIds = [];
    this._trackedWindow = null;
  }

  _rebindWindowSignals(window) {
    if (window === this._trackedWindow) {
      return;
    }
    this._disconnectWindowSignals();
    if (!window) {
      return;
    }
    this._trackedWindow = window;
    this._windowSignalIds.push(
      window.connect('notify::wm-class', () => this._refreshFocusedApp(true)),
      window.connect('notify::gtk-application-id', () =>
        this._refreshFocusedApp(true)
      )
    );
  }

  _onFocusChanged() {
    const focusWindow = global.display.focus_window ?? null;
    this._rebindWindowSignals(focusWindow);
    this._refreshFocusedApp(true);
  }

  _refreshFocusedApp(emitSignal) {
    const focusWindow = global.display.focus_window ?? null;
    const snapshot = this._captureWindowSnapshot(focusWindow);
    const nextInfo = resolveFocusedWindowInfo(snapshot, this._procReader);

    if (isSameAppInfo(this._currentInfo, nextInfo)) {
      return;
    }

    this._currentInfo = nextInfo;

    if (emitSignal && this._dbusImpl) {
      try {
        this._dbusImpl.emit_signal(
          'FocusedAppChanged',
          new GLib.Variant('(sa{sv})', [
            nextInfo.app_name,
            buildVariantDict(nextInfo),
          ])
        );
      } catch (e) {
        console.error(`[MoGRU] Failed to emit FocusedAppChanged signal: ${e}`);
      }
    }
  }
}
