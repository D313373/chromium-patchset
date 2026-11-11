// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {EventTracker} from '//resources/js/event_tracker.js';
import {loadTimeData} from '//resources/js/load_time_data.js';
import {GlicRequestHeaderInjector} from '/shared/glic_request_headers.js';
import type {ChromeEvent} from '/tools/typescript/definitions/chrome_event.js';

import type {BrowserProxyImpl} from './browser_proxy.js';
import type {Subscriber} from './glic_api/glic_api.js';
import {DetailedWebClientState, GlicApiCommunicator, GlicApiHost, WebClientState} from './glic_api_impl/host/glic_api_host.js';
import type {ApiHostEmbedder} from './glic_api_impl/host/glic_api_host.js';
import {ObservableValue} from './observable.js';
import type {ObservableValueReadOnly} from './observable.js';
import {OneShotTimer} from './timer.js';

// LINT.IfChange(WebviewExitReason)
enum WebviewExitReason {
  NORMAL = 0,
  ABNORMAL = 1,
  CRASHED = 2,
  KILLED = 3,
  OOM_KILLED = 4,
  OOM = 5,
  FAILED_TO_LAUNCH = 6,
  INTEGRITY_FAILURE = 7,
  UNKNOWN = 8,
}
// LINT.ThenChange(//tools/metrics/histograms/metadata/glic/enums.xml:GlicWebviewExitReason)

const WEBVIEW_EXIT_REASON_MAP = {
  'normal': WebviewExitReason.NORMAL,
  'abnormal': WebviewExitReason.ABNORMAL,
  'crashed': WebviewExitReason.CRASHED,
  'killed': WebviewExitReason.KILLED,
  'oom killed': WebviewExitReason.OOM_KILLED,
  'oom': WebviewExitReason.OOM,
  'failed to launch': WebviewExitReason.FAILED_TO_LAUNCH,
  'integrity failure': WebviewExitReason.INTEGRITY_FAILURE,
};

function webviewExitReasonStringToEnum(reason: chrome.webviewTag.ExitReason):
    WebviewExitReason {
  return WEBVIEW_EXIT_REASON_MAP[reason] ?? WebviewExitReason.UNKNOWN;
}

function isSimulachromeAgentGuest(): boolean {
  try {
    if (loadTimeData.valueExists('simulachromeAgentGuest') &&
        loadTimeData.getBoolean('simulachromeAgentGuest')) {
      return true;
    }
  } catch (_) {
  }
  try {
    const guest = String(loadTimeData.getString('glicGuestURL') || '');
    return guest.startsWith('chrome://simulachrome-agent') ||
        guest.includes('chrome://glic/agent/');
  } catch (_) {
    return false;
  }
}

export type PageType =
    // A login page.
    'login'
    // A page that should be displayed.
    |'regular'
    // A error page that should be displayed.
    |'guestError'
    // An error page that indicates access loss.
    |'guestCaaError'
    // The page could not be loaded.
    |'loadError';

// Calls from the webview to its owner.
export interface WebviewDelegate {
  // Called when there is an error during page load.
  webviewError(reason: string): void;
  // Called when the embedded web page is unresponsive.
  webviewUnresponsive(): void;
  // Called when a page commits inside the webview.
  webviewPageCommit(pageType: PageType): void;
  // Called when the webview redirects to an access error page.
  webviewDeniedByAdmin(): void;
}

// To match needed pieces of tools/typescript/definitions/web_request.d.ts,
// because this enum isn't actually available in this context.
enum ResourceType {
  MAIN_FRAME = 'main_frame',
}

// State for the WebviewController which lives as long as the WebUI content.
// This is necessary because we may destroy and rebuild the WebviewController
// multiple times.
export class WebviewPersistentState {
  // Normally, we load only the glicGuestURL. However, if that guest decides to
  // navigate to a different URL after the client connects, we will remember
  // that URL for loading later. To avoid getting stuck on a bad URL, we will
  // allow using `loadUrl` only once unless a client successfully connects.
  // Note that this supports internal development.
  private loadUrl: string|undefined;
  private loadUrlUsed = false;

  useLoadUrl(): string {
    if (this.loadUrl && !this.loadUrlUsed) {
      this.loadUrlUsed = true;
      return this.loadUrl;
    } else {
      return loadTimeData.getString('glicGuestURL');
    }
  }

  onCommitAfterConnect(newUrl: string) {
    this.loadUrl = newUrl;
    this.loadUrlUsed = false;
  }

  onClientReady() {
    // Web client became ready, allow loadUrl to be used again.
    this.loadUrlUsed = false;
  }
}

type ChromeEventFunctionType<T> =
    T extends ChromeEvent<infer ListenerType>? ListenerType : never;

// Creates and manages the <webview> element, and the GlicApiHost which
// communicates with it. For the packaged Simulachrome agent guest, uses an
// <iframe> loaded from a blob: document whose module graph is also blob:
// URLs (chrome:// cannot be a <webview> guest, chrome://glic cannot iframe
// itself, and chrome://glic/agent scripts are not served to blob documents).
export class WebviewController {
  webview?: chrome.webviewTag.WebView;
  private agentFrame?: HTMLIFrameElement;
  private host?: GlicApiHost;
  private communicator?: GlicApiCommunicator;
  private hostSubscriber?: Subscriber;
  private onDestroy: Array<() => void> = [];
  private eventTracker = new EventTracker();
  private webClientState =
      ObservableValue.withValue(WebClientState.UNINITIALIZED);
  private oneMinuteTimer = new OneShotTimer(1000 * 60);
  private glicRequestHeaderInjector?: GlicRequestHeaderInjector;

  constructor(
      private readonly container: HTMLElement,
      private browserProxy: BrowserProxyImpl,
      private delegate: WebviewDelegate,
      private hostEmbedder: ApiHostEmbedder,
      private persistentState: WebviewPersistentState,
  ) {
    if (isSimulachromeAgentGuest()) {
      this.installAgentIframe();
      return;
    }

    this.webview =
        document.createElement('webview') as chrome.webviewTag.WebView;

    this.glicRequestHeaderInjector = new GlicRequestHeaderInjector(
        this.webview, loadTimeData.getString('chromeVersion'),
        loadTimeData.getString('chromeChannel'),
        loadTimeData.getString('glicHeaderRequestTypes'));

    // Intercept all main frame requests, and block them if they are not allowed
    // origins.
    const onBeforeRequest = this.onBeforeRequest.bind(this);
    this.webview.request.onBeforeRequest.addListener(
        onBeforeRequest, {
          types: [ResourceType.MAIN_FRAME],
          urls: ['<all_urls>'],
        },
        ['blocking']);
    this.onDestroy.push(() => {
      this.webview?.request.onBeforeRequest.removeListener(onBeforeRequest);
    });

    this.webview.id = 'guestFrame';
    this.webview.setAttribute('partition', 'persist:glicpart');
    this.container.appendChild(this.webview);

    this.eventTracker.add(
        this.webview, 'loadcommit', this.onLoadCommit.bind(this));
    this.eventTracker.add(
        this.webview, 'contentload', this.contentLoaded.bind(this));
    this.eventTracker.add(this.webview, 'loadstop', this.onLoadStop.bind(this));
    this.eventTracker.add(
        this.webview, 'newwindow', this.onNewWindow.bind(this));
    this.eventTracker.add(
        this.webview, 'permissionrequest', this.onPermissionRequest.bind(this));
    this.eventTracker.add(
        this.webview, 'unresponsive', this.onUnresponsive.bind(this));
    this.eventTracker.add(this.webview, 'exit', this.onExit.bind(this) as any);

    this.webview.src = this.persistentState.useLoadUrl();

    this.oneMinuteTimer.start(() => {
      if (this.host) {
        chrome.metricsPrivate.recordEnumerationValue(
            'Glic.Host.WebClientState.AtOneMinute',
            this.host.getDetailedWebClientState(),
            DetailedWebClientState.MAX_VALUE + 1);
      }
    });
  }

  private installAgentIframe() {
    const url = this.persistentState.useLoadUrl();
    const iframe = document.createElement('iframe');
    iframe.id = 'guestFrame';
    iframe.title = 'Simulachrome Agent';
    iframe.setAttribute(
        'allow', 'clipboard-read; clipboard-write; microphone; camera');
    iframe.style.border = '0';
    iframe.style.width = '100%';
    iframe.style.height = '100%';
    iframe.style.display = 'block';
    iframe.style.background = '#0b1020';
    this.agentFrame = iframe;
    this.container.appendChild(iframe);

    const markReady = () => {
      this.browserProxy.handler.webviewCommitted({url});
      this.webClientState.assignAndSignal(WebClientState.RESPONSIVE);
      this.persistentState.onClientReady();
      this.hostEmbedder.webClientReady();
      this.delegate.webviewPageCommit('login');
    };

    // Do not navigate the iframe to chrome://glic/agent/* (frame-ancestors
    // 'none' + nested GlicUI crash) or chrome://simulachrome-agent (blank
    // gray host panel). Fetch packaged files in the parent and embed a blob:
    // document whose scripts are also blob: URLs (script-src allows blob:
    // for this guest). chrome://glic/agent/*.js is not served to blob
    // documents, so the module graph cannot use those URLs.
    const requestAgentFile = (relative: string): Promise<string> => {
      return new Promise((resolve, reject) => {
        const cb = `__sc_agent_file_${Date.now()}_${
            Math.random().toString(36).slice(2)}`;
        const timeout = window.setTimeout(() => {
          try {
            delete (window as unknown as Record<string, unknown>)[cb];
          } catch (_) {
          }
          reject(new Error('dashboardGetAgentHtml timeout'));
        }, 8000);
        (window as unknown as Record<string, unknown>)[cb] = (result: {
          ok?: boolean, html?: string, error?: string
        }) => {
          window.clearTimeout(timeout);
          try {
            delete (window as unknown as Record<string, unknown>)[cb];
          } catch (_) {
          }
          if (result && result.ok && typeof result.html === 'string') {
            resolve(result.html);
          } else {
            reject(new Error(result?.error || 'dashboardGetAgentHtml failed'));
          }
        };
        try {
          (chrome as unknown as {send: (m: string, a: unknown[]) => void})
              .send('dashboardGetAgentHtml', [cb, relative]);
        } catch (e) {
          window.clearTimeout(timeout);
          reject(e);
        }
      });
    };

    const blobUrl = (text: string, type: string): string => {
      const url = URL.createObjectURL(new Blob([text], {type}));
      this.onDestroy.push(() => URL.revokeObjectURL(url));
      return url;
    };

    const parentBridgeJs =
        `(function(){try{if(!window.chrome)window.chrome={};` +
        `var _send=window.chrome.send;window.chrome.send=function(m,a){` +
        `try{if(window.parent&&window.parent!==window&&` +
        `window.parent.chrome&&typeof window.parent.chrome.send==='function'){` +
        `var args=Array.prototype.slice.call(a||[]);` +
        `var cb=args[0];if(typeof cb==='string'){` +
        `var parentCb='__sc_bridge_'+cb;` +
        `window.parent[parentCb]=function(){` +
        `var fn=window[cb];if(typeof fn==='function')` +
        `fn.apply(window,arguments);` +
        `try{delete window.parent[parentCb];}catch(e){}};` +
        `args[0]=parentCb;}` +
        `return window.parent.chrome.send(m,args);}}catch(e){}` +
        `if(typeof _send==='function')return _send.apply(this,arguments);` +
        `};}catch(e){}})();`;

    void (async () => {
      try {
        const [htmlRaw, clientJsRaw, apiBoot, glicApi] = await Promise.all([
          requestAgentFile('ollama.html'),
          requestAgentFile('ollama_client.js'),
          requestAgentFile('api_boot.js'),
          requestAgentFile('glic_api_stub.js'),
        ]);
        const apiBootUrl = blobUrl(apiBoot, 'text/javascript');
        const glicApiUrl = blobUrl(glicApi, 'text/javascript');
        let clientJs = clientJsRaw;
        const importRewrites: Array<[string, string]> = [
          ["from '/glic/glic_api/glic_api.js'", `from '${glicApiUrl}'`],
          ['from "/glic/glic_api/glic_api.js"', `from '${glicApiUrl}'`],
          ["from '../api_boot.js'", `from '${apiBootUrl}'`],
          ['from "../api_boot.js"', `from '${apiBootUrl}'`],
          ["from './glic_api_stub.js'", `from '${glicApiUrl}'`],
          ["from './api_boot.js'", `from '${apiBootUrl}'`],
        ];
        for (const [from, to] of importRewrites) {
          clientJs = clientJs.split(from).join(to);
        }
        const clientUrl = blobUrl(clientJs, 'text/javascript');
        const bridgeUrl = blobUrl(parentBridgeJs, 'text/javascript');
        let html = htmlRaw;
        html = html.replace(
            /<script src="chrome:\/\/glic\/agent\/parent_bridge\.js"><\/script>\s*/g,
            '');
        html = html.replace(
            /\b(src)=["'](?:\.\/)?ollama_client\.js["']/g,
            `src="${clientUrl}"`);
        html = html.replace(
            /\b(src)=["']chrome:\/\/glic\/agent\/ollama_client\.js["']/g,
            `src="${clientUrl}"`);
        html = `<script src="${bridgeUrl}"></script>\n` + html;
        const pageUrl = blobUrl(html, 'text/html');
        iframe.addEventListener('load', markReady, {once: true});
        iframe.src = pageUrl;
      } catch (err) {
        console.warn('Failed to embed packaged agent UI', err);
        this.delegate.webviewError('agent_blob_error');
      }
    })();
  }

  getWebClientState(): ObservableValueReadOnly<WebClientState> {
    return this.webClientState;
  }

  destroy() {
    this.glicRequestHeaderInjector?.destroy();
    this.oneMinuteTimer.reset();
    if (this.host) {
      chrome.metricsPrivate.recordEnumerationValue(
          'Glic.Host.WebClientState.OnDestroy',
          this.host.getDetailedWebClientState(),
          DetailedWebClientState.MAX_VALUE + 1);
    }
    this.destroyHost(
        this.webClientState.getCurrentValue() === WebClientState.ERROR ?
            WebClientState.ERROR :
            WebClientState.UNINITIALIZED);
    this.eventTracker.removeAll();
    this.onDestroy.forEach(f => f());
    this.onDestroy = [];
    this.agentFrame?.remove();
    this.agentFrame = undefined;
    this.webview?.remove();
    this.webview = undefined;
  }

  private destroyHost(webClientState: WebClientState) {
    if (this.hostSubscriber) {
      this.hostSubscriber.unsubscribe();
      this.hostSubscriber = undefined;
    }
    if (this.host) {
      this.host.destroy();
      this.host = undefined;
    }
    if (this.communicator) {
      this.communicator.destroy();
      this.communicator = undefined;
    }
    this.webClientState.assignAndSignal(webClientState);
  }

  waitingOnPanelWillOpen(): boolean {
    return this.host?.waitingOnPanelWillOpen() ?? false;
  }

  onLoadTimeOut(): void {
    if (this.host) {
      chrome.metricsPrivate.recordEnumerationValue(
          'Glic.Host.WebClientState.OnLoadTimeOut',
          this.host.getDetailedWebClientState(),
          DetailedWebClientState.MAX_VALUE + 1);
    }
  }

  private onLoadCommit(e: any): void {
    this.loadCommit(e.url, e.isTopLevel);
  }

  private onLoadStop(): void {
    this.webview?.focus();
  }

  private onNewWindow(e: Event): void {
    this.onNewWindowEvent(e as chrome.webviewTag.NewWindowEvent);
  }

  private async onPermissionRequest(e: any): Promise<void> {
    e.preventDefault();
    if (!this.host) {
      e.request.deny();
      return;
    }
    switch (e.permission) {
      case 'media': {
        // TODO(b/416092165): Block mic requests if the mic permission was not
        // granted.
        e.request.allow();
        return;
      }
      case 'geolocation': {
        const isGeolocationAllowed =
            await this.host.shouldAllowGeolocationPermissionRequest();
        if (isGeolocationAllowed) {
          e.request.allow();
        } else {
          e.request.deny();
        }
        return;
      }
    }
    console.warn(`Webview permission request was denied: ${e.permission}`);
    e.request.deny();
  }

  private onUnresponsive(): void {
    this.delegate.webviewUnresponsive();
  }

  private onExit: ChromeEventFunctionType<typeof chrome.webviewTag.exit> =
      (event) => {
        chrome.metricsPrivate.recordEnumerationValue(
            'Glic.Session.WebClientCrash.ExitReason',
            webviewExitReasonStringToEnum(event.reason),
            Object.keys(WEBVIEW_EXIT_REASON_MAP).length);
        if (event.reason !== 'normal') {
          this.destroyHost(WebClientState.ERROR);
          chrome.metricsPrivate.recordUserAction('GlicSessionWebClientCrash');
          console.warn(`webview exit. processID: ${event.processID}, reason: ${
              event.reason}`);
        }
      };

  private loadCommit(url: string, isTopLevel: boolean) {
    if (!isTopLevel) {
      return;
    }
    if (this.host) {
      chrome.metricsPrivate.recordEnumerationValue(
          'Glic.Host.WebClientState.OnCommit',
          this.host.getDetailedWebClientState(),
          DetailedWebClientState.MAX_VALUE + 1);
    }
    const wasResponsive = this.getWebClientState().getCurrentValue() ===
        WebClientState.RESPONSIVE;

    this.destroyHost(WebClientState.UNINITIALIZED);

    // Local Simulachrome agent UI does not speak Gemini's Glic web-client
    // protocol. Any top-level commit while configured for the packaged agent
    // (including chrome-error:// after a WebUI hiccup) must not fall through
    // to the Gemini "isn't available" panel.
    if (isSimulachromeAgentGuest()) {
      this.browserProxy.handler.webviewCommitted({url});
      this.webClientState.assignAndSignal(WebClientState.RESPONSIVE);
      this.persistentState.onClientReady();
      this.hostEmbedder.webClientReady();
      this.delegate.webviewPageCommit('login');
      return;
    }

    const origin = new URL(url).origin;
    if (this.webview?.contentWindow && origin !== 'null') {
      this.communicator =
          new GlicApiCommunicator(origin, this.webview.contentWindow);
      this.host = new GlicApiHost(
          this.browserProxy, this.communicator, this.hostEmbedder);
      this.hostSubscriber = this.host.getWebClientState().subscribe(state => {
        if (state === WebClientState.RESPONSIVE) {
          this.persistentState.onClientReady();
        }
        this.webClientState.assignAndSignal(state);
      });
    }
    this.browserProxy.handler.webviewCommitted({url});

    if (!this.host) {
      this.delegate.webviewPageCommit('loadError');
      return;
    }

    // TODO(https://crbug.com/388328847): Remove when login issues are
    // resolved.
    if (url.startsWith('https://login.corp.google.com/') ||
        url.startsWith('https://accounts.google.com/') ||
        url.startsWith('https://accounts.googlers.com/') ||
        url.startsWith('https://gaiastaging.corp.google.com/')) {
      this.delegate.webviewPageCommit('login');
    } else if (new URL(url).pathname.startsWith('/sorry/')) {
      this.delegate.webviewPageCommit('guestError');
    } else {
      if (wasResponsive) {
        this.persistentState.onCommitAfterConnect(url);
      }

      // This forces the page to reload after navigation.
      // TODO(b/439718538): revisit overall logic, this may be buggy.
      if (loadTimeData.getBoolean('reloadAfterNavigation')) {
        this.delegate.webviewPageCommit('regular');
      }
    }
  }

  private contentLoaded() {
    this.communicator?.contentLoaded();
  }

  private onNewWindowEvent(event: chrome.webviewTag.NewWindowEvent) {
    if (!this.host) {
      return;
    }

    event.preventDefault();
    if (loadTimeData.getBoolean('glicPopupWindowsEnabled') &&
        event.windowOpenDisposition === 'new_popup') {
      this.host.openLinkInPopup(
          event.targetUrl, event.initialWidth, event.initialHeight);
    } else {
      this.host.openLinkInNewTab(event.targetUrl);
    }
    event.stopPropagation();
  }

  private urlMatchesAdminBlockedUrl(url: string) {
    const adminBlockedRedirectPatterns =
        loadTimeData.getString('adminBlockedRedirectPatterns');
    if (!adminBlockedRedirectPatterns) {
      return false;
    }
    if (adminBlockedRedirectPatterns.split(' ').some(
            pattern => new URLPattern(pattern.trim()).test(url))) {
      console.warn(`Admin blocked error page detected.`);
      return true;
    }
    return false;
  }

  private onBeforeRequest:
      ChromeEventFunctionType<typeof chrome.webRequest.onBeforeRequest> =
          (details) => {
            // Allow subframe requests.
            if (details.frameId !== 0) {
              return {};
            }
            if (this.urlMatchesAdminBlockedUrl(details.url)) {
              this.delegate.webviewDeniedByAdmin();
              return {cancel: true};
            }

            return {cancel: !urlMatchesAllowedOrigin(details.url)};
          };
}

/**
 * Returns a URLPattern given an origin pattern string that has the syntax:
 * <protocol>://<hostname>[:<port>]
 * where <protocol>, <hostname> and <port> are inserted into URLPattern.
 */
export function matcherForOrigin(originPattern: string): URLPattern|null {
  // This regex is overly permissive in what characters can exist in protocol
  // or hostname. This isn't a problem because we're just passing data to
  // URLPattern.
  const match = originPattern.match(/([^:]+):\/\/([^:]*)(?::(\d+))?[/]?/);
  if (!match) {
    return null;
  }

  const [protocol, hostname, port] = [match[1], match[2], match[3] ?? '*'];
  try {
    return new URLPattern({protocol, hostname, port});
  } catch (_) {
    return null;
  }
}

export function urlMatchesAllowedOrigin(url: string) {
  // For development.
  if (loadTimeData.getBoolean('devMode')) {
    return true;
  }

  // A URL is allowed if it either matches glicGuestURL's origin, or it matches
  // any of the approved origins.
  const defaultUrl = new URL(loadTimeData.getString('glicGuestURL'));
  if (matcherForOrigin(defaultUrl.origin)?.test(url)) {
    return true;
  }

  return loadTimeData.getString('glicAllowedOrigins')
      .split(' ')
      .some(origin => matcherForOrigin(origin.trim())?.test(url));
}
