// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {loadTimeData} from '//resources/js/load_time_data.js';
import {getRequiredElement} from 'chrome://resources/js/util.js';

import type {BrowserProxyImpl} from './browser_proxy.js';
import {AutomationRunTarget} from './glic.mojom-webui.js';
import {setForceStockGeminiGuest, stockGeminiGuestSelected} from './webview.js';

interface ScriptInfo {
  id: string;
  name: string;
}

interface LocatorLike {
  type?: string;
  value?: string;
}

interface GuidedScrapeTarget {
  locators: LocatorLike[];
  relativeLocators?: LocatorLike[];
  fieldNameHint?: string;
}

interface GuidedScrapeSelectionModel {
  selections: GuidedScrapeTarget[];
  rowLocators: LocatorLike[];
  rowCount: number;
}

interface RecorderStateInfo {
  isRecording: boolean;
  actionCount: number;
  manualLoopCaptureActive: boolean;
  locatorCaptureArmed: boolean;
  locatorCaptureReady: boolean;
}

export interface RecorderStatePush {
  isRecording: boolean;
  actionCount: number;
  manualLoopCaptureActive: boolean;
  locatorCaptureArmed: boolean;
  locatorCaptureReady: boolean;
  guidedActive: boolean;
  guidedPhase: string;
  guidedCapturePending: boolean;
  guidedCaptureKind: string;
  guidedCaptureSequence: bigint|number;
  guidedCaptureResultJson: string;
  guidedScrapeTargetCount: number;
}

function locatorRank(type: string): number {
  // Prefer "rich" locators (a11y/text) over raw selectors so it's obvious when
  // we are harvesting enhanced locator bundles.
  switch (String(type || '').toUpperCase()) {
    case 'ACCESSIBILITY_NAME':
      return 0;
    case 'ACCESSIBLE_NAME':
      return 0;
    case 'ARIA_LABEL':
      return 1;
    case 'ATTR_DATA_TESTID':
      return 2;
    case 'LINK_TEXT':
      return 3;
    case 'PARTIAL_LINK_TEXT':
      return 4;
    case 'ATTR_NAME':
      return 5;
    case 'TEXT_CONTENT':
      return 6;
    case 'ID':
      return 7;
    case 'NAME':
      return 8;
    case 'CSS_SELECTOR':
    case 'CSS':
      return 9;
    case 'XPATH':
      return 10;
    case 'SHADOW_DOM':
      return 11;
    case 'CLASS_NAME':
      return 12;
    case 'TAG_NAME':
    case 'TAG':
      return 13;
    case 'JS_EVAL':
      return 14;
    case 'VISUAL_COORDS':
      return 15;
    case 'VISUAL_AI':
      return 16;
    default:
      return 99;
  }
}

function bestLocatorSummary(locs: LocatorLike[]|unknown): string {
  if (!Array.isArray(locs) || !locs.length) {
    return '';
  }

  let best: LocatorLike|undefined;
  let bestRank = 1e9;
  for (const l of locs) {
    if (!l || typeof l !== 'object') {
      continue;
    }
    const loc = l as LocatorLike;
    const rank = locatorRank(String(loc.type || ''));
    if (rank < bestRank) {
      bestRank = rank;
      best = loc;
    }
  }
  if (!best) {
    return '';
  }

  const t = String(best.type || '').toUpperCase();
  const v = String(best.value || '').trim();
  const shown = v.length > 80 ? v.slice(0, 80) + '…' : v;
  const extra = locs.length > 1 ? ` +${locs.length - 1}` : '';
  return t ? ` (${t}${shown ? `=${shown}` : ''}${extra})` : '';
}

function safeJsonSummary(json: string): string {
  try {
    const obj = JSON.parse(json);
    const kind = obj?.action ?? obj?.type ?? 'action';
    if (kind === 'key_press') {
      const key = obj?.params?.key;
      if (typeof key === 'string' && key.length) {
        return `key_press: ${key}`;
      }
      return 'key_press';
    }
    if (kind === 'type') {
      const text = obj?.params?.text;
      if (typeof text === 'string') {
        const shown = text.length > 80 ? text.slice(0, 80) + '…' : text;
        return `type: ${shown}`;
      }
      return 'type';
    }
    const locators = obj?.locators ?? obj?.params?.locators;
    return `${String(kind)}${bestLocatorSummary(locators)}`;
  } catch {
    return json.slice(0, 120);
  }
}

function tryParseActionJson(json: string): any|null {
  try {
    const parsed = JSON.parse(json);
    return parsed && typeof parsed === 'object' ? parsed : null;
  } catch {
    return null;
  }
}

function getActionKind(action: any): string {
  return String(action?.action || action?.type || '').toLowerCase();
}

function getActionParams(action: any): any {
  if (action && typeof action === 'object' &&
      action.params && typeof action.params === 'object') {
    return action.params;
  }
  return {};
}

function getActionLocators(action: any): LocatorLike[] {
  const direct = Array.isArray(action?.locators) ? action.locators : null;
  if (direct) {
    return direct as LocatorLike[];
  }
  const params = getActionParams(action);
  return Array.isArray(params?.locators) ? params.locators as LocatorLike[] : [];
}

function getLoopSteps(action: any): any[] {
  if (Array.isArray(action?.steps)) {
    return action.steps;
  }
  const params = getActionParams(action);
  if (Array.isArray(params?.steps)) {
    return params.steps;
  }
  return [];
}

function guidedShortcutDirectionFromAction(action: any): 'next'|'previous'|null {
  const kind = getActionKind(action);
  const params = getActionParams(action);
  const key = String(params?.key || '').toLowerCase();
  if (kind === 'key_press' || kind === 'key_down' || kind === 'key') {
    if (key === 'arrowright' || key === 'right') {
      return 'next';
    }
    if (key === 'f7') {
      return 'previous';
    }
  }
  return null;
}

export class AutomationToolsController {
  private root: HTMLElement = getRequiredElement('automationTools');
  private popover!: HTMLElement;
  private popoverBody!: HTMLElement;
  private scriptSelect!: HTMLSelectElement;
  private statusEl!: HTMLElement;
  private loopCapturePillEl!: HTMLElement;
  private recorderButton!: HTMLButtonElement;
  private refreshButton!: HTMLButtonElement;
  private guestModeButton?: HTMLButtonElement;
  private stockGeminiGuest = stockGeminiGuestSelected();
  private runFocusedButton!: HTMLButtonElement;
  private runNewTabButton!: HTMLButtonElement;
  private startStopRecorderButton!: HTMLButtonElement;
  private captureLocatorsButton!: HTMLButtonElement;
  private guidedLoopButton!: HTMLButtonElement;
  private recordingToolsGroup!: HTMLDivElement;
  private guidedShortcutHint!: HTMLElement;
  private stopRunButton!: HTMLButtonElement;
  private executionId: string = '';
  private lastIsRecording: boolean = false;
  private lastLocatorCaptureArmed: boolean = false;
  private lastLocatorCaptureReady: boolean = false;
  private lastActionCount: number = 0;
  private lastActionsJson: string[] = [];
  private recorderHintOverride: string = '';
  private scriptNamesLower: Set<string> = new Set();
  private saveAsDraft: string = '';
  private guidedLoopActive: boolean = false;
  private guidedLoopPhase:
      'idle'|'await_loop_target'|'select_scrape_targets'|'record_return' =
          'idle';
  private guidedCapturePending: boolean = false;
  private guidedCaptureKind: 'none'|'loop_target'|'scrape_target' = 'none';
  private guidedCaptureSequence: number = 0;
  private guidedScrapeTargets: GuidedScrapeTarget[] = [];
  private guidedPredictedRowLocators: LocatorLike[] = [];
  private guidedPredictedRowCount: number = 0;
  private loopCaptureActive: boolean = false;
  private readonly kMaxGuidedScrapeTargets = 20;
  private lastAckedGuidedStateSignature: string = '';
  private pendingGuidedStateSignature: string = '';
  private guidedCaptureConsumeInFlight: boolean = false;
  private lastConsumedGuidedCaptureSequence: number = 0;
  private guidedShortcutArtifactInFlight: boolean = false;
  private oneShotLocatorCapturePending: boolean = false;

  // Mojo responses are named tuples; keep this tolerant in case codegen
  // renames the success field in future schema edits.
  private isOkResult(res: any): boolean {
    if (!res || typeof res !== 'object') {
      return false;
    }
    if (typeof res.ok === 'boolean') {
      return res.ok;
    }
    if (typeof res.success === 'boolean') {
      return res.success;
    }
    if (typeof res.result === 'boolean') {
      return res.result;
    }
    return false;
  }

  private async getRecorderState(): Promise<RecorderStateInfo> {
    const getState = (this.browserProxy.handler as any).getRecorderState;
    if (typeof getState === 'function') {
      const res = await getState.call(this.browserProxy.handler);
      return {
        isRecording: Boolean(res?.isRecording),
        actionCount: Number(res?.actionCount || 0),
        manualLoopCaptureActive: Boolean(res?.manualLoopCaptureActive),
        locatorCaptureArmed: Boolean(res?.locatorCaptureArmed),
        locatorCaptureReady: Boolean(res?.locatorCaptureReady),
      };
    }
    const snapshot = await this.browserProxy.handler.getRecorderSnapshot();
    return {
      isRecording: Boolean(snapshot?.isRecording),
      actionCount: Array.isArray(snapshot?.actionsJson) ?
          snapshot.actionsJson.length :
          0,
      manualLoopCaptureActive: false,
      locatorCaptureArmed: false,
      locatorCaptureReady: false,
    };
  }

  private async fetchRecorderActionsJson(): Promise<string[]> {
    const snapshot = await this.browserProxy.handler.getRecorderSnapshot();
    return Array.isArray(snapshot.actionsJson) ? snapshot.actionsJson : [];
  }

  constructor(private browserProxy: BrowserProxyImpl) {}

  private getGuidedScrapeSelectionSubphase(): 'seed'|'predict' {
    return this.guidedLoopPhase === 'select_scrape_targets' &&
            this.guidedScrapeTargets.length > 0 ?
        'predict' :
        'seed';
  }

  onRecorderStateChanged(push: RecorderStatePush): void {
    this.applyRecorderState(push, /*forceRender=*/false);
  }

  isGuidedLoopActive(): boolean {
    return this.guidedLoopActive;
  }

  shouldCaptureEscape(): boolean {
    return this.guidedLoopActive;
  }

  handleEscapeShortcut(): boolean {
    if (!this.guidedLoopActive) {
      return false;
    }
    void this.toggleGuidedLoopFlow();
    return true;
  }

  handleGuidedKeyboardShortcut(event: KeyboardEvent): boolean {
    if (!this.guidedLoopActive || event.altKey || event.metaKey) {
      return false;
    }

    const target = event.target as HTMLElement | null;
    if (target &&
        (target.isContentEditable || target.tagName === 'INPUT' ||
         target.tagName === 'TEXTAREA' || target.tagName === 'SELECT')) {
      return false;
    }

    const key = String(event.key || '');
    const code = String(event.code || '');
    const isNextShortcut = key === 'ArrowRight' || code === 'ArrowRight';
    const isUndoShortcut = key === 'ArrowLeft' || code === 'ArrowLeft';

    if (isUndoShortcut) {
      void this.undoGuidedStep();
      return true;
    }

    if (isNextShortcut &&
        (this.guidedLoopPhase === 'await_loop_target' ||
         this.guidedLoopPhase === 'select_scrape_targets')) {
      void this.handleGuidedNextStep();
      return true;
    }

    return false;
  }

  private async undoGuidedStep(): Promise<void> {
    if (!this.guidedLoopActive) {
      return;
    }
    if (this.guidedLoopPhase === 'record_return') {
      await this.undoGuidedReturnStep();
      return;
    }
    if (this.guidedLoopPhase === 'select_scrape_targets') {
      if (!this.guidedScrapeTargets.length) {
        const selectedTargets = await this.fetchGuidedScrapeSelectionsFromHost();
        if (selectedTargets !== null) {
          this.applyGuidedSelectionModel(selectedTargets);
        }
      }
      if (!this.guidedScrapeTargets.length) {
        await this.handleGuidedPreviousStep();
        return;
      }
      await this.popLastGuidedScrapeTarget();
      return;
    }
    if (this.guidedLoopPhase === 'await_loop_target' && this.loopCaptureActive) {
      await this.resetGuidedLoopTarget();
    }
  }

  init(): void {
    if (!loadTimeData.getBoolean('enableAutomationTools')) {
      return;
    }

    this.root.hidden = false;
    this.root.textContent = '';

    // Slim left tool rail (56px).
    const rail = document.createElement('nav');
    rail.className = 'tool-rail';
    rail.setAttribute('aria-label', 'Automation tools');

    const topGroup = document.createElement('div');
    topGroup.className = 'tool-rail-group tool-rail-top';

    if (loadTimeData.valueExists('simulachromeAgentGuest') &&
        loadTimeData.getBoolean('simulachromeAgentGuest')) {
      this.guestModeButton = document.createElement('button');
      this.guestModeButton.type = 'button';
      this.guestModeButton.id = 'automation-guest-mode';
      this.guestModeButton.className = 'guest-mode-toggle';
      this.guestModeButton.addEventListener('click', (event) => {
        event.preventDefault();
        event.stopPropagation();
        this.toggleGuestSurface();
      });
      topGroup.appendChild(this.guestModeButton);
      this.applyGuestModeButton();
    }

    this.refreshButton =
        this.createRailButton('refresh', 'Refresh', () => void this.refreshScripts());
    topGroup.appendChild(this.refreshButton);
    this.runFocusedButton = this.createRailButton(
        'play', 'Run Focused',
        () => void this.startSelected(AutomationRunTarget.kFocusedTab));
    topGroup.appendChild(this.runFocusedButton);
    this.runNewTabButton = this.createRailButton(
        'nwtab', 'Run New Tab',
        () => void this.startSelected(AutomationRunTarget.kNewForegroundTab));
    topGroup.appendChild(this.runNewTabButton);

    this.captureLocatorsButton = this.createRailButton(
        'crosshair', 'Capture Locators (next click only)',
        () => void this.armLocatorCapture());
    this.captureLocatorsButton.id = 'automation-capture-locators';
    // Record toggle between groups.
    this.startStopRecorderButton = this.createRailButton(
        'record', 'Start Recording', () => void this.toggleRecorder());
    this.startStopRecorderButton.id = 'automation-record-toggle';
    this.guidedLoopButton = this.createRailButton(
        'loop',
        'Guided Scrape Table',
        () => void this.handleGuidedPrimaryButton());
    this.guidedLoopButton.id = 'automation-loop-guided';
    this.guidedLoopButton.classList.add('guided-step-button');

    const spacer = document.createElement('div');
    spacer.className = 'tool-rail-spacer';
    this.recordingToolsGroup = document.createElement('div');
    this.recordingToolsGroup.className = 'tool-rail-group recording-tools';
    this.recordingToolsGroup.hidden = true;
    this.guidedShortcutHint = document.createElement('div');
    this.guidedShortcutHint.className = 'guided-shortcuts';
    this.guidedShortcutHint.hidden = true;
    this.guidedShortcutHint.textContent =
        'Next: Right Arrow';
    this.recordingToolsGroup.appendChild(this.guidedLoopButton);
    this.recordingToolsGroup.appendChild(this.guidedShortcutHint);

    const bottomGroup = document.createElement('div');
    bottomGroup.className = 'tool-rail-group tool-rail-bottom';

    this.recorderButton =
        this.createRailButton('list', 'Rec Log', () => this.togglePopover());
    bottomGroup.appendChild(this.recorderButton);

    rail.appendChild(topGroup);
    rail.appendChild(this.captureLocatorsButton);
    rail.appendChild(this.startStopRecorderButton);
    rail.appendChild(this.recordingToolsGroup);
    rail.appendChild(spacer);
    rail.appendChild(bottomGroup);
    this.root.appendChild(rail);

    // Popover floats to the right of the rail.
    this.popover = document.createElement('div');
    this.popover.id = 'automationToolsPopover';
    this.popover.hidden = true;
    this.root.appendChild(this.popover);

    const popHeader = document.createElement('div');
    popHeader.className = 'popover-header';

    this.scriptSelect = document.createElement('select');
    this.scriptSelect.className = 'script-select';
    this.scriptSelect.title = 'Dashboard scripts';
    popHeader.appendChild(this.scriptSelect);

    const stopButton = document.createElement('button');
    stopButton.className = 'btn-secondary';
    stopButton.textContent = 'Stop';
    stopButton.title = 'Stop active run';
    stopButton.addEventListener('click', () => void this.stopRun());
    this.stopRunButton = stopButton;
    popHeader.appendChild(stopButton);

    this.statusEl = document.createElement('div');
    this.statusEl.className = 'status';
    this.statusEl.textContent = 'Idle';
    popHeader.appendChild(this.statusEl);

    this.loopCapturePillEl = document.createElement('div');
    this.loopCapturePillEl.className = 'loop-capture-pill';
    this.loopCapturePillEl.hidden = true;
    this.loopCapturePillEl.textContent = 'LOOP CAPTURE';
    this.loopCapturePillEl.title =
        'Loop capture is active: record nested steps, then continue with Guided Scrape.';
    popHeader.appendChild(this.loopCapturePillEl);
    this.updateRunControls();

    this.popover.appendChild(popHeader);

    this.popoverBody = document.createElement('div');
    this.popoverBody.className = 'popover-body';
    this.popover.appendChild(this.popoverBody);

    this.refreshScripts();
    this.updateGuidedControlsState();
    void this.syncRecorderStateOnce(/*forceRender=*/true);
  }

  private normalizeGuidedPhase(value: string): (
      'idle'|'await_loop_target'|'select_scrape_targets'|'record_return') {
    return (value === 'await_loop_target' || value === 'select_scrape_targets' ||
            value === 'record_return') ?
        value :
        'idle';
  }

  private normalizeGuidedCaptureKind(value: string): (
      'none'|'loop_target'|'scrape_target') {
    return (value === 'loop_target' || value === 'scrape_target') ? value :
                                                                    'none';
  }

  private isShortcutDirectionAllowed(direction: 'next'|'previous'): boolean {
    if (!this.guidedLoopActive) {
      return false;
    }
    if (direction === 'next') {
      return this.guidedLoopPhase === 'await_loop_target' ||
          this.guidedLoopPhase === 'select_scrape_targets';
    }
    return this.guidedLoopPhase === 'select_scrape_targets';
  }

  private async consumeGuidedShortcutArtifacts(
      actionsJson: string[]): Promise<string[]> {
    if (!this.guidedLoopActive || this.guidedLoopPhase === 'record_return' ||
        !actionsJson.length || this.guidedShortcutArtifactInFlight) {
      return actionsJson;
    }

    const artifacts: Array<{index: number, direction: 'next'|'previous'}> = [];
    actionsJson.forEach((json, index) => {
      const parsed = tryParseActionJson(json);
      if (!parsed) {
        return;
      }
      const direction = guidedShortcutDirectionFromAction(parsed);
      if (!direction || !this.isShortcutDirectionAllowed(direction)) {
        return;
      }
      artifacts.push({index, direction});
    });
    if (!artifacts.length) {
      return actionsJson;
    }

    this.guidedShortcutArtifactInFlight = true;
    let newestDirection: 'next'|'previous' = artifacts[artifacts.length - 1]!.direction;
    try {
      for (let i = artifacts.length - 1; i >= 0; --i) {
        await this.browserProxy.handler.removeRecordedAction(artifacts[i]!.index);
      }

      let cleaned = actionsJson;
      const snapshot = await this.browserProxy.handler.getRecorderSnapshot();
      if (Array.isArray(snapshot.actionsJson)) {
        cleaned = snapshot.actionsJson;
      }
      this.lastActionCount = cleaned.length;
      this.lastActionsJson = cleaned;

      if (newestDirection === 'previous') {
        await this.handleGuidedPreviousStep();
      } else {
        await this.handleGuidedNextStep();
      }
      return cleaned;
    } catch {
      return actionsJson;
    } finally {
      this.guidedShortcutArtifactInFlight = false;
    }
  }

  private applyRecorderState(push: RecorderStatePush, forceRender: boolean): void {
    const wasRecording = this.lastIsRecording;
    const wasGuidedActive = this.guidedLoopActive;
    const previousActionCount = this.lastActionCount;
    const previousGuidedScrapeTargetCount = this.guidedScrapeTargets.length;
    const previousGuidedCapturePending = this.guidedCapturePending;
    const previousGuidedCaptureKind = this.guidedCaptureKind;
    const previousGuidedCaptureSequence = this.guidedCaptureSequence;

    this.lastIsRecording = Boolean(push.isRecording);
    this.lastLocatorCaptureArmed = Boolean(push.locatorCaptureArmed);
    this.lastLocatorCaptureReady = Boolean(push.locatorCaptureReady);
    this.lastActionCount = Math.max(0, Number(push.actionCount || 0));
    this.loopCaptureActive =
        this.lastIsRecording && Boolean(push.manualLoopCaptureActive);

    this.guidedLoopActive = Boolean(push.guidedActive);
    this.guidedLoopPhase =
        this.normalizeGuidedPhase(String(push.guidedPhase || 'idle'));
    this.guidedCapturePending = Boolean(push.guidedCapturePending);
    this.guidedCaptureKind =
        this.normalizeGuidedCaptureKind(String(push.guidedCaptureKind || 'none'));
    this.guidedCaptureSequence =
        this.sequenceToNumber(push.guidedCaptureSequence as any);
    if (this.oneShotLocatorCapturePending && !this.lastLocatorCaptureArmed) {
      this.oneShotLocatorCapturePending = false;
    }

    const scrapeTargetCount = Math.max(
        0, Math.min(this.kMaxGuidedScrapeTargets,
                    Number(push.guidedScrapeTargetCount || 0)));
    if (scrapeTargetCount === 0) {
      this.guidedScrapeTargets = [];
      this.guidedPredictedRowLocators = [];
      this.guidedPredictedRowCount = 0;
    } else if (this.guidedScrapeTargets.length > scrapeTargetCount) {
      this.guidedScrapeTargets = this.guidedScrapeTargets.slice(0, scrapeTargetCount);
    } else {
      while (this.guidedScrapeTargets.length < scrapeTargetCount) {
        this.guidedScrapeTargets.push({locators: []});
      }
    }

    if (this.guidedLoopActive && this.guidedLoopPhase === 'select_scrape_targets' &&
        !this.guidedCapturePending &&
        previousGuidedScrapeTargetCount !== scrapeTargetCount) {
      this.statusEl.textContent = scrapeTargetCount > 0 ?
          'Step 2b/3: seeded predictions are active. Orange outlines peer fields and blue outlines matching column cells. Keep selecting fields or press Right Arrow.' :
          'Step 2a/3: click the first field to seed predictions for matching row fields and column cells.';
    }

    const hostGuidedSignature = JSON.stringify({
      active: this.guidedLoopActive,
      phase: this.guidedLoopPhase,
      capturePending: this.guidedCapturePending,
      captureKind: this.guidedCaptureKind,
      captureSequence: this.guidedCaptureSequence,
      scrapeTargetCount: this.guidedScrapeTargets.length,
    });
    this.lastAckedGuidedStateSignature = hostGuidedSignature;
    if (this.pendingGuidedStateSignature === hostGuidedSignature) {
      this.pendingGuidedStateSignature = '';
    }

    this.updateUIState(
        this.lastIsRecording,
        this.lastLocatorCaptureArmed || this.lastLocatorCaptureReady);
    this.updateLoopCapturePill(this.loopCaptureActive);
    this.updateGuidedControlsState();

    if (wasRecording && !this.lastIsRecording && wasGuidedActive) {
      this.statusEl.textContent = 'Recorder stopped. Guided loop canceled.';
    }
    if (!wasRecording && this.lastIsRecording) {
      this.recorderHintOverride = '';
    }

    const captureJustCompleted =
        previousGuidedCapturePending && !this.guidedCapturePending;
    const captureResultJson = String(push.guidedCaptureResultJson || '');
    if (captureJustCompleted && previousGuidedCaptureKind !== 'none' &&
        this.guidedCaptureSequence > previousGuidedCaptureSequence &&
        this.guidedCaptureSequence > this.lastConsumedGuidedCaptureSequence) {
      void this.consumeGuidedCaptureResult(
          previousGuidedCaptureKind, this.guidedCaptureSequence,
          captureResultJson);
    } else if (captureJustCompleted && previousGuidedCaptureKind !== 'none' &&
               this.guidedLoopPhase === 'select_scrape_targets') {
      this.guidedCaptureKind = 'none';
      this.updateGuidedControlsState();
    }

    if (wasRecording && !this.lastIsRecording && this.lastActionCount > 0) {
      this.popover.hidden = false;
      this.recorderButton.classList.add('active');
      this.statusEl.textContent =
          'Recording stopped — choose Save as / Append / Discard';
    }

    const shouldRenderRecorder = !this.popover.hidden;
    const actionCountChanged = previousActionCount !== this.lastActionCount;
    const shouldScanGuidedShortcutArtifacts =
        actionCountChanged && this.guidedLoopActive &&
        (this.guidedLoopPhase === 'await_loop_target' ||
         this.guidedLoopPhase === 'select_scrape_targets');
    const loopStepsLikelyChanged =
        shouldRenderRecorder && this.loopCaptureActive && this.lastIsRecording;
    const shouldFetchActions =
        (shouldRenderRecorder &&
         (forceRender || actionCountChanged ||
          (wasRecording !== this.lastIsRecording) || loopStepsLikelyChanged)) ||
        shouldScanGuidedShortcutArtifacts;
    if (shouldFetchActions) {
      void this.fetchRecorderActionsJson().then(async (actionsJson) => {
        const cleanedActions =
            await this.consumeGuidedShortcutArtifacts(actionsJson);
        this.lastActionsJson = cleanedActions;
        this.lastActionCount = cleanedActions.length;
        if (shouldRenderRecorder) {
          this.renderRecorderActions(cleanedActions, this.lastIsRecording);
        }
      });
    } else if (this.popover.hidden && !this.guidedLoopActive &&
               !this.loopCaptureActive) {
      this.lastActionsJson = [];
    }
  }

  private async syncRecorderStateOnce(forceRender: boolean = false):
      Promise<void> {
    try {
      const state = await this.getRecorderState();
      let guided = {
        active: this.guidedLoopActive,
        phase: this.guidedLoopPhase,
        capturePending: this.guidedCapturePending,
        captureKind: this.guidedCaptureKind,
        captureSequence: this.guidedCaptureSequence,
        captureResultJson: '',
        scrapeTargetCount: this.guidedScrapeTargets.length,
      };
      if (state.isRecording) {
        try {
          const resp =
              await this.browserProxy.handler.getRecorderGuidedLoopUiState();
          guided = {
            active: Boolean((resp as any).active),
            phase: this.normalizeGuidedPhase(
                String((resp as any).phase || 'idle')),
            capturePending: Boolean((resp as any).capturePending),
            captureKind: this.normalizeGuidedCaptureKind(
                String((resp as any).captureKind || 'none')),
            captureSequence: this.sequenceToNumber(
                (resp as any).captureSequence ?? 0),
            captureResultJson: String((resp as any).captureResultJson || ''),
            scrapeTargetCount: Math.max(
                0,
                Math.min(this.kMaxGuidedScrapeTargets,
                         Number((resp as any).scrapeTargetCount || 0))),
          };
        } catch {
        }
      }
      this.applyRecorderState(
          {
            isRecording: state.isRecording,
            actionCount: state.actionCount,
            manualLoopCaptureActive: state.manualLoopCaptureActive,
            locatorCaptureArmed: state.locatorCaptureArmed,
            locatorCaptureReady: state.locatorCaptureReady,
            guidedActive: guided.active,
            guidedPhase: guided.phase,
            guidedCapturePending: guided.capturePending,
            guidedCaptureKind: guided.captureKind,
            guidedCaptureSequence: guided.captureSequence,
            guidedCaptureResultJson: guided.captureResultJson,
            guidedScrapeTargetCount: guided.scrapeTargetCount,
          },
          forceRender);
    } catch {
      // Ignore transient host reconnection failures.
    }
  }

  private buildGuidedStatePayload() {
    return {
      active: this.guidedLoopActive,
      phase: this.guidedLoopPhase,
      capturePending: this.guidedCapturePending,
      captureKind: this.guidedCaptureKind,
      captureSequence: this.guidedCaptureSequence,
      scrapeTargetCount: this.guidedScrapeTargets.length,
    };
  }

  private async pushGuidedStateToHostNow(force: boolean = false):
      Promise<boolean> {
    let signature = '';
    try {
      const payload = this.buildGuidedStatePayload();
      signature = JSON.stringify(payload);
      if (!force &&
          (signature === this.lastAckedGuidedStateSignature ||
           signature === this.pendingGuidedStateSignature)) {
        return true;
      }
      this.pendingGuidedStateSignature = signature;
      const resp = await this.browserProxy.handler.setRecorderGuidedLoopUiState(
          payload.active, payload.phase, payload.capturePending,
          payload.captureKind, BigInt(payload.captureSequence),
          payload.scrapeTargetCount);
      if (resp?.ok) {
        this.lastAckedGuidedStateSignature = signature;
      }
      if (this.pendingGuidedStateSignature === signature) {
        this.pendingGuidedStateSignature = '';
      }
      return Boolean(resp?.ok);
    } catch {
      if (signature && this.pendingGuidedStateSignature === signature) {
        this.pendingGuidedStateSignature = '';
      }
      return false;
    }
  }

  private pushGuidedStateToHost(): void {
    try {
      const payload = this.buildGuidedStatePayload();
      const signature = JSON.stringify(payload);
      if (signature === this.lastAckedGuidedStateSignature ||
          signature === this.pendingGuidedStateSignature) {
        return;
      }
      void this.pushGuidedStateToHostNow();
    } catch {
      // Ignore transient host errors.
    }
  }

  private applyGuestModeButton(): void {
    const button = this.guestModeButton;
    if (!button) {
      return;
    }
    const mode = this.stockGeminiGuest ? 'gemini' : 'agent';
    const label = this.stockGeminiGuest ? 'GEMINI' : 'AGENT';
    const tooltip = this.stockGeminiGuest ?
        'Gemini API. Click to switch back to the local agent.' :
        'Local agent. Click to switch to the Gemini API.';
    button.dataset['mode'] = mode;
    button.dataset['tooltip'] = tooltip;
    button.setAttribute('aria-pressed', this.stockGeminiGuest ? 'true' : 'false');
    button.setAttribute('aria-label', tooltip);
    button.replaceChildren();
    const kicker = document.createElement('span');
    kicker.className = 'guest-mode-kicker';
    kicker.textContent = 'NOW';
    const value = document.createElement('span');
    value.className = 'guest-mode-value';
    value.textContent = label;
    button.appendChild(kicker);
    button.appendChild(value);
  }

  private toggleGuestSurface(): void {
    this.stockGeminiGuest = !this.stockGeminiGuest;
    this.applyGuestModeButton();
    const app = (window as unknown as {
      switchGlicGuestSurface?: (stock: boolean) => void,
    }).switchGlicGuestSurface;
    if (app) {
      app(this.stockGeminiGuest);
      return;
    }
    setForceStockGeminiGuest(this.stockGeminiGuest);
    window.location.reload();
  }

  private createRailButton(icon: string, tooltip: string, onClick: () => void):
      HTMLButtonElement {
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'tool-btn';
    btn.dataset['tooltip'] = tooltip;
    btn.dataset['icon'] = icon;
    btn.setAttribute('aria-label', tooltip);
    btn.addEventListener('click', (e) => {
      e.preventDefault();
      onClick();
    });
    this.setButtonIcon(btn, icon);
    return btn;
  }

  private setButtonIcon(btn: HTMLButtonElement, icon: string): void {
    btn.dataset['icon'] = icon;
    btn.textContent = '';

    if (icon.startsWith('guided-step-')) {
      const step = icon.slice('guided-step-'.length);
      const badge = document.createElement('span');
      badge.className = 'guided-step-icon';
      badge.textContent = `${step}/3`;
      btn.appendChild(badge);
      return;
    }

    if (icon === 'list') {
      const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
      svg.setAttribute('viewBox', '0 0 24 24');
      svg.setAttribute('fill', 'none');
      svg.setAttribute('stroke', 'currentColor');
      svg.setAttribute('stroke-width', '1.5');
      svg.setAttribute('stroke-linecap', 'round');
      svg.setAttribute('stroke-linejoin', 'round');
      const mkLine = (y: number) => {
        const l = document.createElementNS('http://www.w3.org/2000/svg', 'line');
        l.setAttribute('x1', '6');
        l.setAttribute('x2', '18');
        l.setAttribute('y1', String(y));
        l.setAttribute('y2', String(y));
        return l;
      };
      svg.appendChild(mkLine(7));
      svg.appendChild(mkLine(12));
      svg.appendChild(mkLine(17));
      btn.appendChild(svg);
      return;
    }

    if (icon === 'crosshair') {
      const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
      svg.setAttribute('viewBox', '0 0 24 24');
      svg.setAttribute('fill', 'none');
      svg.setAttribute('stroke', 'currentColor');
      svg.setAttribute('stroke-width', '1.75');
      svg.setAttribute('stroke-linecap', 'round');
      svg.setAttribute('stroke-linejoin', 'round');
      const outer =
          document.createElementNS('http://www.w3.org/2000/svg', 'circle');
      outer.setAttribute('cx', '12');
      outer.setAttribute('cy', '12');
      outer.setAttribute('r', '7');
      svg.appendChild(outer);
      const center =
          document.createElementNS('http://www.w3.org/2000/svg', 'circle');
      center.setAttribute('cx', '12');
      center.setAttribute('cy', '12');
      center.setAttribute('r', '1.25');
      svg.appendChild(center);
      const line = (x1: number, y1: number, x2: number, y2: number) => {
        const l = document.createElementNS('http://www.w3.org/2000/svg', 'line');
        l.setAttribute('x1', String(x1));
        l.setAttribute('y1', String(y1));
        l.setAttribute('x2', String(x2));
        l.setAttribute('y2', String(y2));
        return l;
      };
      svg.appendChild(line(12, 2.5, 12, 5));
      svg.appendChild(line(12, 19, 12, 21.5));
      svg.appendChild(line(2.5, 12, 5, 12));
      svg.appendChild(line(19, 12, 21.5, 12));
      btn.appendChild(svg);
      return;
    }

    if (icon === 'loop') {
      const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
      svg.setAttribute('viewBox', '0 0 24 24');
      svg.setAttribute('fill', 'none');
      svg.setAttribute('stroke', 'currentColor');
      svg.setAttribute('stroke-width', '1.8');
      svg.setAttribute('stroke-linecap', 'round');
      svg.setAttribute('stroke-linejoin', 'round');
      const p1 = document.createElementNS('http://www.w3.org/2000/svg', 'path');
      p1.setAttribute('d', 'M7 7h7a4 4 0 0 1 4 4v1');
      const p2 = document.createElementNS('http://www.w3.org/2000/svg', 'path');
      p2.setAttribute('d', 'M17 9l2 3-3 2');
      const p3 = document.createElementNS('http://www.w3.org/2000/svg', 'path');
      p3.setAttribute('d', 'M17 17H10a4 4 0 0 1-4-4v-1');
      const p4 = document.createElementNS('http://www.w3.org/2000/svg', 'path');
      p4.setAttribute('d', 'M7 15l-2-3 3-2');
      svg.appendChild(p1);
      svg.appendChild(p2);
      svg.appendChild(p3);
      svg.appendChild(p4);
      btn.appendChild(svg);
      return;
    }

    if (icon === 'gemini' || icon === 'agent') {
      const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
      svg.setAttribute('viewBox', '0 0 24 24');
      svg.setAttribute('fill', 'none');
      svg.setAttribute('stroke', 'currentColor');
      svg.setAttribute('stroke-width', '1.7');
      svg.setAttribute('stroke-linecap', 'round');
      svg.setAttribute('stroke-linejoin', 'round');
      const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
      path.setAttribute(
          'd',
          icon === 'gemini' ?
              'M12 3l1.6 5.2L19 10l-5.4 1.8L12 17l-1.6-5.2L5 10l5.4-1.8L12 3z' :
              'M5 6h14v12H5zM8 10h8M8 14h5');
      svg.appendChild(path);
      btn.appendChild(svg);
      return;
    }

    if (icon === 'next') {
      const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
      svg.setAttribute('viewBox', '0 0 24 24');
      svg.setAttribute('fill', 'none');
      svg.setAttribute('stroke', 'currentColor');
      svg.setAttribute('stroke-width', '1.8');
      svg.setAttribute('stroke-linecap', 'round');
      svg.setAttribute('stroke-linejoin', 'round');
      const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
      line.setAttribute('x1', '6');
      line.setAttribute('y1', '12');
      line.setAttribute('x2', '18');
      line.setAttribute('y2', '12');
      const head = document.createElementNS('http://www.w3.org/2000/svg', 'path');
      head.setAttribute('d', 'M13 7l5 5-5 5');
      svg.appendChild(line);
      svg.appendChild(head);
      btn.appendChild(svg);
      return;
    }

    const img = document.createElement('img');
    img.src = `icons/${icon}.png`;
    img.alt = '';
    img.draggable = false;
    btn.appendChild(img);
    return;
  }

  private sequenceToNumber(seq: number|bigint|undefined|null): number {
    if (typeof seq === 'bigint') {
      return Number(seq);
    }
    if (typeof seq === 'number' && Number.isFinite(seq)) {
      return Math.max(0, Math.floor(seq));
    }
    return 0;
  }

  private extractCaptureLocators(resultJson: string): LocatorLike[] {
    if (!resultJson) {
      return [];
    }
    try {
      const result = JSON.parse(resultJson);
      const locators = Array.isArray(result?.locators) ? result.locators : [];
      return locators.filter((loc: unknown) => !!loc && typeof loc === 'object')
          .map((loc: any) => ({type: String(loc.type || ''), value: String(loc.value || '')}));
    } catch {
      return [];
    }
  }

  private normalizeScrapeFieldLocators(locs: LocatorLike[]): LocatorLike[] {
    const priority = (type: string): number => {
      switch (String(type || '').toUpperCase()) {
        case 'ACCESSIBILITY_NAME':
          return 0;
        case 'LINK_TEXT':
          return 1;
        case 'PARTIAL_LINK_TEXT':
          return 2;
        case 'TEXT_CONTENT':
          return 3;
        case 'ID':
          return 4;
        case 'NAME':
          return 5;
        case 'CSS_SELECTOR':
        case 'CSS':
          return 6;
        case 'XPATH':
          return 7;
        case 'CLASS_NAME':
          return 8;
        case 'TAG_NAME':
          return 9;
        default:
          return 99;
      }
    };

    const seen = new Set<string>();
    return locs.filter((loc) => {
      return !!loc && !!String(loc.type || '').trim() &&
          !!String(loc.value || '').trim();
    }).map((loc) => {
      return {
        type: String(loc.type || '').trim(),
        value: String(loc.value || '').trim(),
      };
    }).filter((loc) => {
      const upperType = String(loc.type || '').toUpperCase();
      const lowerValue = String(loc.value || '').toLowerCase();
      if (upperType === 'VISUAL_COORDS' || upperType === 'VISUAL_AI' ||
          upperType === 'JS_EVAL') {
        return false;
      }
      if (lowerValue.includes('__dash_')) {
        return false;
      }
      if (upperType === 'TAG_NAME' &&
          ['a', 'body', 'div', 'html', 'span'].includes(lowerValue)) {
        return false;
      }
      if (upperType === 'CSS_SELECTOR' &&
          ['a', 'body', 'div', 'html', 'span', 'tr', 'td', 'th'].includes(
              lowerValue)) {
        return false;
      }
      if (upperType === 'XPATH' &&
          ['//a', '//body', '//div', '//html', '//span'].includes(lowerValue)) {
        return false;
      }
      return true;
    }).sort((a, b) => priority(a.type) - priority(b.type))
        .filter((loc) => {
          const key = `${String(loc.type).toUpperCase()}:${loc.value}`;
          if (seen.has(key)) {
            return false;
          }
          seen.add(key);
          return true;
        })
        .slice(0, 6);
  }

  private toSafeFieldName(rawValue: string, fallbackIndex: number): string {
    const normalized = String(rawValue || '').trim().toLowerCase()
                           .replace(/['"]/g, '')
                           .replace(/[^a-z0-9]+/g, '_')
                           .replace(/^_+|_+$/g, '')
                           .slice(0, 36);
    if (!normalized) {
      return `field_${fallbackIndex + 1}`;
    }
    if (/^[0-9]/.test(normalized)) {
      return `field_${normalized}`;
    }
    return normalized;
  }

  private deriveGuidedFieldName(
      locs: LocatorLike[], index: number, usedNames: Set<string>): string {
    const candidateTypes = [
      'ACCESSIBILITY_NAME',
      'LINK_TEXT',
      'TEXT_CONTENT',
      'NAME',
      'ID',
      'CSS_SELECTOR',
    ];
    let baseName = '';
    for (const type of candidateTypes) {
      const match = locs.find((loc) => {
        return String(loc.type || '').toUpperCase() === type &&
            typeof loc.value === 'string' && loc.value.trim();
      });
      if (!match) {
        continue;
      }
      const value = String(match.value || '').trim();
      if (type === 'CSS_SELECTOR') {
        const parts = value.split(/[ >+~:#.\[]/).filter(Boolean);
        const selectorLeaf = parts.length > 0 ? parts[parts.length - 1]! : value;
        baseName = this.toSafeFieldName(
            selectorLeaf, index);
      } else {
        baseName = this.toSafeFieldName(value, index);
      }
      if (baseName && baseName !== 'body' && baseName !== 'html') {
        break;
      }
      baseName = '';
    }
    if (!baseName) {
      baseName = `field_${index + 1}`;
    }
    let uniqueName = baseName;
    let suffix = 2;
    while (usedNames.has(uniqueName)) {
      uniqueName = `${baseName}_${suffix}`;
      suffix += 1;
    }
    usedNames.add(uniqueName);
    return uniqueName;
  }

  private inferGuidedRowLocatorsFromTargets(targets: GuidedScrapeTarget[]):
      LocatorLike[] {
    const out: LocatorLike[] = [];
    const seen = new Set<string>();
    const pushCss = (value: string) => {
      const normalized = String(value || '').trim()
                             .replace(/\s+/g, ' ')
                             .replace(/\s*>\s*/g, ' > ')
                             .trim();
      if (!normalized) {
        return;
      }
      const key = `CSS_SELECTOR:${normalized}`;
      if (seen.has(key)) {
        return;
      }
      seen.add(key);
      out.push({type: 'CSS_SELECTOR', value: normalized});
    };
    const generalizeRowCss = (value: string): string => {
      let css = String(value || '').trim();
      if (!css) {
        return '';
      }
      css = css.replace(
          /\s*>\s*(td|th)(:nth-(?:of-type|child)\(\d+\))?\s*$/i, '');
      css = css.replace(
          /\s*>\s*\[role="(?:cell|gridcell|rowheader|columnheader)"\]\s*$/i,
          '');
      css = css.replace(
          /\b(tr|li)(:nth-(?:of-type|child)\(\d+\))/gi, '$1');
      css = css.replace(/\s+/g, ' ').trim();
      return css;
    };

    for (const target of targets) {
      const cssLocator = (target.locators || []).find((loc) => {
        return String(loc.type || '').toUpperCase() === 'CSS_SELECTOR' &&
            typeof loc.value === 'string' && loc.value.includes('>');
      });
      if (!cssLocator) {
        continue;
      }
      const rowCss = generalizeRowCss(String(cssLocator.value || ''));
      if (!rowCss || rowCss === 'body' || rowCss === 'html') {
        continue;
      }
      pushCss(rowCss);
      const tbodyRowCss = rowCss.replace(
          /table([^>]*) > tr$/i, 'table$1 tbody > tr');
      if (tbodyRowCss !== rowCss) {
        pushCss(tbodyRowCss);
      }
    }
    return this.normalizeScrapeFieldLocators(out);
  }

  private findCssLocator(locs: LocatorLike[]): string {
    for (const loc of locs) {
      const type = String(loc.type || '').toUpperCase();
      if ((type === 'CSS_SELECTOR' || type === 'CSS') &&
          typeof loc.value === 'string' && loc.value.trim()) {
        return loc.value.trim();
      }
    }
    return '';
  }

  private updateGuidedControlsState(): void {
    const guided = this.guidedLoopActive;
    const awaitingCapture = guided && this.guidedCapturePending;
    const loopTargetSet =
        guided && this.guidedLoopPhase === 'await_loop_target' &&
        this.loopCaptureActive;
    const showGuidedTools = this.lastIsRecording;
    if (this.recordingToolsGroup) {
      this.recordingToolsGroup.hidden = !showGuidedTools;
      this.recordingToolsGroup.classList.toggle(
          'recording-tools-visible', showGuidedTools);
    }
    const nextStepBlockedByCapture =
        awaitingCapture && this.guidedLoopPhase !== 'select_scrape_targets';
    const canAdvanceStep =
        !nextStepBlockedByCapture &&
        (this.guidedLoopPhase !== 'await_loop_target' || loopTargetSet);

    if (this.guidedLoopButton) {
      this.guidedLoopButton.classList.toggle('active', guided);
      this.guidedLoopButton.classList.toggle('guided-step-active', guided);
      this.guidedLoopButton.disabled =
          !showGuidedTools ||
          (guided &&
           (!canAdvanceStep || this.guidedLoopPhase === 'record_return'));
      let tooltip = 'Guided Scrape Table (start while recording)';
      let guidedIcon = 'loop';
      if (!guided) {
        this.guidedLoopButton.removeAttribute('data-phase');
      } else {
        guidedIcon = 'guided-step-1';
        if (this.guidedLoopPhase === 'select_scrape_targets') {
          guidedIcon = 'guided-step-2';
          tooltip = this.getGuidedScrapeSelectionSubphase() === 'predict' ?
              'Step 2b/3: seeded predictions are active. Orange outlines peer fields, blue outlines matching column cells. Right Arrow confirms, Left Arrow undoes.' :
              'Step 2a/3: click the first field to seed predictions. After that, matching row fields and column cells will be outlined.';
        } else if (this.guidedLoopPhase === 'record_return') {
          guidedIcon = 'guided-step-3';
          tooltip = 'Step 3/3: waiting for origin URL match. Left Arrow undoes the inserted scrape_table step.';
        } else if (loopTargetSet) {
          tooltip =
              'Step 1/3 complete. Click to enter Step 2. Left Arrow resets the loop target.';
        } else {
          tooltip =
              'Step 1/3: click one repeated item to set the loop target. Later clicks record normal loop steps.';
        }
        this.guidedLoopButton.removeAttribute('data-phase');
      }
      this.guidedLoopButton.dataset['tooltip'] = tooltip;
      this.guidedLoopButton.setAttribute('aria-label', tooltip);
      this.setButtonIcon(this.guidedLoopButton, guidedIcon);
    }

    if (this.captureLocatorsButton) {
      if (guided && this.guidedLoopPhase === 'select_scrape_targets') {
        this.captureLocatorsButton.dataset['tooltip'] =
            'Selection mode is active. Click page elements to toggle fields.';
      } else if (!guided &&
                 (this.lastLocatorCaptureArmed ||
                  this.lastLocatorCaptureReady ||
                  this.oneShotLocatorCapturePending)) {
        this.captureLocatorsButton.dataset['tooltip'] =
            'Cancel active locator capture';
      } else if (guided && this.guidedLoopPhase === 'await_loop_target' &&
                 !loopTargetSet) {
        this.captureLocatorsButton.dataset['tooltip'] =
            'Optional: capture loop target from next click only';
      } else if (guided && this.guidedLoopPhase === 'await_loop_target' &&
                 loopTargetSet) {
        this.captureLocatorsButton.dataset['tooltip'] =
            'Loop target set. Click Guided Scrape when ready to pick scrape fields.';
      } else {
        this.captureLocatorsButton.dataset['tooltip'] =
            'Capture Locators (next click only)';
      }
    }

    if (this.refreshButton) this.refreshButton.disabled = guided;
    if (this.runFocusedButton) this.runFocusedButton.disabled = guided;
    if (this.runNewTabButton) this.runNewTabButton.disabled = guided;
    if (this.startStopRecorderButton) {
      this.startStopRecorderButton.disabled = guided;
    }
    if (this.recorderButton) this.recorderButton.disabled = false;
    if (this.scriptSelect) this.scriptSelect.disabled = guided;
    if (this.stopRunButton) this.stopRunButton.disabled = guided || !this.executionId;
    if (this.captureLocatorsButton) {
      this.captureLocatorsButton.disabled =
          guided ? (this.guidedLoopPhase === 'select_scrape_targets' ||
                    awaitingCapture || this.guidedLoopPhase === 'record_return' ||
                    (this.guidedLoopPhase === 'await_loop_target' &&
                     loopTargetSet))
                 : false;
    }
    if (this.guidedShortcutHint) {
      if (!guided) {
        this.guidedShortcutHint.hidden = true;
      } else if (this.guidedLoopPhase === 'record_return') {
        this.guidedShortcutHint.hidden = false;
        this.guidedShortcutHint.textContent =
            'Step 3/3 shortcuts: Left Arrow undo scrape step, return to origin URL to finish.';
      } else if (this.guidedLoopPhase === 'select_scrape_targets') {
        this.guidedShortcutHint.hidden = false;
        this.guidedShortcutHint.textContent =
            this.getGuidedScrapeSelectionSubphase() === 'predict' ?
            'Step 2b/3 shortcuts: Right Arrow confirm scrape fields, Left Arrow undo latest field or return to seed mode.' :
            'Step 2a/3 shortcuts: click the first field to seed predictions, then Right Arrow continues once fields are selected.';
      } else {
        this.guidedShortcutHint.hidden = false;
        this.guidedShortcutHint.textContent =
            loopTargetSet ?
            'Step 1/3 shortcuts: Right Arrow next, Left Arrow reset loop target.' :
            'Step 1/3 shortcut: Right Arrow after the loop target is set.';
      }
    }
    this.pushGuidedStateToHost();
  }

  private async ensureRecorderRunning(): Promise<boolean> {
    const state = await this.getRecorderState();
    if (state.isRecording) {
      return true;
    }
    const start = await this.browserProxy.handler.startRecorder();
    return this.isOkResult(start) || Boolean((start as any)?.started);
  }

  private async armGuidedLocatorCapture(
      kind: 'loop_target'|'scrape_target'): Promise<boolean> {
    this.guidedCapturePending = true;
    this.guidedCaptureKind = kind;
    this.updateGuidedControlsState();
    const pushed = await this.pushGuidedStateToHostNow(/*force=*/true);
    if (!pushed) {
      this.guidedCapturePending = false;
      this.guidedCaptureKind = 'none';
      this.statusEl.textContent = 'Unable to arm guided capture state.';
      this.updateGuidedControlsState();
      return false;
    }

    const resp = await this.browserProxy.handler.armLocatorCaptureForDashboard();
    if (!this.isOkResult(resp)) {
      const err = (resp && typeof (resp as any).error === 'string') ?
          (resp as any).error :
          'capture_failed';
      this.statusEl.textContent = `Capture failed: ${err}`;
      this.guidedCapturePending = false;
      this.guidedCaptureKind = 'none';
      this.updateGuidedControlsState();
      await this.pushGuidedStateToHostNow(/*force=*/true);
      return false;
    }
    this.captureLocatorsButton.classList.add('active');
    window.setTimeout(() => this.captureLocatorsButton.classList.remove('active'), 1200);
    if (kind === 'loop_target') {
      this.statusEl.textContent =
          'Guided scrape table: click a repeated item to define loop target.';
    } else {
      this.statusEl.textContent =
          `Guided loop: select fields (${this.guidedScrapeTargets.length}/${this.kMaxGuidedScrapeTargets}).`;
    }
    this.updateGuidedControlsState();
    return true;
  }

  private async appendGuidedScrapeActions(): Promise<boolean> {
    if (!this.guidedScrapeTargets.length) {
      return false;
    }

    const usedFieldNames = new Set<string>();
    const fields = this.guidedScrapeTargets.map((target, index) => {
      const normalizedAbsoluteLocators =
          this.normalizeScrapeFieldLocators(target.locators);
      const normalizedRelativeLocators = this.normalizeScrapeFieldLocators(
          Array.isArray(target.relativeLocators) ? target.relativeLocators : []);
      const fieldLocators = normalizedRelativeLocators.length ?
          normalizedRelativeLocators :
          normalizedAbsoluteLocators;
      return {
        name: target.fieldNameHint ?
            this.deriveGuidedFieldName(
                [{type: 'TEXT_CONTENT', value: target.fieldNameHint},
                 ...normalizedAbsoluteLocators],
                index, usedFieldNames) :
            this.deriveGuidedFieldName(
                normalizedAbsoluteLocators, index, usedFieldNames),
        locators: fieldLocators,
      };
    });
    const rowLocators = this.normalizeScrapeFieldLocators(
        this.guidedPredictedRowLocators);
    const inferredRowLocators = rowLocators.length ? [] :
        this.inferGuidedRowLocatorsFromTargets(this.guidedScrapeTargets);
    const effectiveRowLocators =
        rowLocators.length ? rowLocators : inferredRowLocators;
    if (!effectiveRowLocators.length) {
      this.statusEl.textContent =
          'Guided scrape-table needs a predicted row scope. Re-click the first field to reseed row detection before continuing.';
      return false;
    }

    const action = {
      action: 'scrape_table',
      params: {
        variableName: 'guided_records',
        rowLocators: effectiveRowLocators,
        fields,
        rowSelectionMode: 'all',
        saveCsv: true,
        appendCsv: true,
        csvFileName: 'guided_records.csv',
      },
      comment:
          rowLocators.length ?
          'Guided scrape-table insertion with predicted row scope and row-relative fields.' :
          'Guided scrape-table insertion with inferred row scope and row-relative fields.',
    };

    const resp = await this.browserProxy.handler.appendRecorderActionJson(
        JSON.stringify(action));
    if (!this.isOkResult(resp)) {
      const error = (resp && typeof (resp as any).error === 'string') ?
          (resp as any).error :
          'append_failed';
      this.statusEl.textContent =
          `Failed to append scrape_table action: ${error}`;
      return false;
    }
    await this.syncRecorderStateOnce(true);
    return true;
  }

  private parseGuidedSelectionsJson(rawJson: string): GuidedScrapeSelectionModel {
    const emptyModel: GuidedScrapeSelectionModel = {
      selections: [],
      rowLocators: [],
      rowCount: 0,
    };
    if (!rawJson) {
      return emptyModel;
    }
    try {
      const parsed = JSON.parse(rawJson);
      const rawSelections = Array.isArray(parsed) ? parsed :
          (Array.isArray(parsed?.selections) ? parsed.selections : []);
      const rowLocators = this.normalizeScrapeFieldLocators(
          Array.isArray(parsed?.rowLocators) ? parsed.rowLocators : []);
      const rowCount = Math.max(0, Number(parsed?.rowCount || 0));
      if (!Array.isArray(rawSelections)) {
        return emptyModel;
      }
      const out: GuidedScrapeTarget[] = [];
      const seen = new Set<string>();
      for (const entry of rawSelections) {
        const entryObject =
            entry && typeof entry === 'object' && !Array.isArray(entry) ? entry :
                                                                        null;
        const rawLocators = Array.isArray(entry) ? entry :
            (Array.isArray(entryObject?.locators) ? entryObject.locators : []);
        if (!Array.isArray(rawLocators)) {
          continue;
        }
        const locators = rawLocators
                             .filter((loc: unknown) => !!loc &&
                                 typeof loc === 'object')
                             .map((loc: any) => {
                               return {
                                 type: String(loc.type || '').trim(),
                                 value: String(loc.value || '').trim(),
                               };
                             })
                             .filter((loc: LocatorLike) => !!loc.type && !!loc.value);
        if (!locators.length) {
          continue;
        }
        const relativeLocators = this.normalizeScrapeFieldLocators(
            Array.isArray(entryObject?.relativeLocators) ?
                entryObject.relativeLocators :
                []);
        const fieldNameHint =
            typeof entryObject?.fieldNameHint === 'string' ?
            String(entryObject.fieldNameHint).trim() :
            '';
        const key = JSON.stringify(locators);
        if (seen.has(key)) {
          continue;
        }
        seen.add(key);
        out.push({
          locators,
          relativeLocators,
          fieldNameHint,
        });
        if (out.length >= this.kMaxGuidedScrapeTargets) {
          break;
        }
      }
      return {
        selections: out,
        rowLocators,
        rowCount: rowLocators.length ? Math.max(rowCount, 1) : rowCount,
      };
    } catch {
      return emptyModel;
    }
  }

  private async fetchGuidedScrapeSelectionsFromHost():
      Promise<GuidedScrapeSelectionModel|null> {
    try {
      const resp =
          await (this.browserProxy.handler as any).getGuidedScrapeSelectionLocators();
      if (!resp || !resp.ok) {
        const err = resp && typeof resp.error === 'string' && resp.error ?
            resp.error :
            'selection_fetch_failed';
        this.statusEl.textContent =
            `Unable to read selected fields: ${err}`;
        return null;
      }
      return this.parseGuidedSelectionsJson(String(resp.selectionsJson || '[]'));
    } catch {
      this.statusEl.textContent = 'Unable to read selected fields.';
      return null;
    }
  }

  private applyGuidedSelectionModel(model: GuidedScrapeSelectionModel): void {
    this.guidedScrapeTargets = Array.isArray(model.selections) ?
        model.selections.slice(0, this.kMaxGuidedScrapeTargets) :
        [];
    this.guidedPredictedRowLocators = Array.isArray(model.rowLocators) ?
        model.rowLocators :
        [];
    this.guidedPredictedRowCount = Math.max(0, Number(model.rowCount || 0));
  }

  private clearGuidedSelectionModel(): void {
    this.guidedScrapeTargets = [];
    this.guidedPredictedRowLocators = [];
    this.guidedPredictedRowCount = 0;
  }

  private async consumeGuidedCaptureResult(
      kind: 'loop_target'|'scrape_target', sequence: number,
      resultJson: string): Promise<void> {
    if (this.guidedCaptureConsumeInFlight) {
      return;
    }
    if (!this.guidedLoopActive) {
      return;
    }
    try {
      this.guidedCaptureConsumeInFlight = true;
      this.lastConsumedGuidedCaptureSequence =
          Math.max(this.lastConsumedGuidedCaptureSequence, sequence);
      const locators = this.extractCaptureLocators(resultJson);
      if (!locators.length) {
        this.statusEl.textContent = 'Capture returned no locators. Try again.';
        this.guidedCaptureKind = 'none';
        this.updateGuidedControlsState();
        await this.pushGuidedStateToHostNow(/*force=*/true);
        return;
      }
      await this.processGuidedCaptureLocators(kind, locators);
    } catch {
    } finally {
      this.guidedCaptureConsumeInFlight = false;
    }
  }

  private async processGuidedCaptureLocators(
      kind: 'loop_target'|'scrape_target', locators: LocatorLike[]):
      Promise<void> {
    if (!this.guidedLoopActive) {
      return;
    }
    if (kind === 'loop_target') {
      const css = this.findCssLocator(locators);
      if (!css) {
        this.statusEl.textContent =
            'Loop target needs a CSS locator; click a different repeated item.';
        await this.armGuidedLocatorCapture('loop_target');
        return;
      }
      const begin = await this.browserProxy.handler.beginRecorderManualLoopCapture(
          css, 20);
      if (!this.isOkResult(begin)) {
        const err = (begin && typeof (begin as any).error === 'string') ?
            (begin as any).error :
            'loop_start_failed';
        this.statusEl.textContent = `Loop start failed: ${err}`;
        this.guidedCaptureKind = 'none';
        this.updateGuidedControlsState();
        await this.pushGuidedStateToHostNow(/*force=*/true);
        return;
      }
      this.guidedLoopPhase = 'await_loop_target';
      this.guidedCapturePending = false;
      this.guidedCaptureKind = 'none';
      this.statusEl.textContent =
          'Loop target captured. Further clicks now record normal loop steps. Press Left Arrow to retarget, or click Guided Scrape when ready to select fields.';
      this.updateGuidedControlsState();
      await this.pushGuidedStateToHostNow(/*force=*/true);
      return;
    }

    if (kind === 'scrape_target') {
      const serialized = JSON.stringify(locators);
      const duplicateIndex = this.guidedScrapeTargets.findIndex(
          existing => JSON.stringify(existing.locators) === serialized);
      if (duplicateIndex >= 0) {
        this.guidedScrapeTargets.splice(duplicateIndex, 1);
        this.guidedPredictedRowLocators = [];
        this.guidedPredictedRowCount = 0;
      } else if (this.guidedScrapeTargets.length < this.kMaxGuidedScrapeTargets) {
        this.guidedScrapeTargets.push({locators});
        this.guidedPredictedRowLocators = [];
        this.guidedPredictedRowCount = 0;
      } else {
        this.statusEl.textContent =
            `Maximum guided fields reached (${this.kMaxGuidedScrapeTargets}).`;
      }
      const refreshedSelections = await this.fetchGuidedScrapeSelectionsFromHost();
      if (refreshedSelections !== null) {
        this.applyGuidedSelectionModel(refreshedSelections);
      }
      if (duplicateIndex >= 0) {
        this.statusEl.textContent =
            this.guidedScrapeTargets.length > 0 ?
            `Deselected field. ${this.guidedScrapeTargets.length}/${this.kMaxGuidedScrapeTargets} field(s) remain selected.` :
            'Deselected field. No fields selected.';
      } else if (this.guidedScrapeTargets.length > 0) {
        this.statusEl.textContent =
            this.guidedPredictedRowLocators.length > 0 ?
            `Selected ${this.guidedScrapeTargets.length}/${this.kMaxGuidedScrapeTargets} field(s). Row predictions found for ${this.guidedPredictedRowCount} repeated row(s). Right Arrow continues.` :
            `Selected ${this.guidedScrapeTargets.length}/${this.kMaxGuidedScrapeTargets} field(s). Row predictions are still warming up; click another field in the same repeated row if needed.`;
      }
      this.guidedCapturePending = false;
      this.guidedCaptureKind = 'none';
      this.updateGuidedControlsState();
      await this.pushGuidedStateToHostNow(/*force=*/true);
    }
  }

  private async resetGuidedLoopTarget(): Promise<void> {
    if (!this.guidedLoopActive || this.guidedLoopPhase !== 'await_loop_target' ||
        !this.loopCaptureActive) {
      return;
    }

    try {
      const resp = await (this.browserProxy.handler as any)
          .resetRecorderGuidedLoopStepOne();
      if (!resp?.ok) {
        const error = String(resp?.error || 'reset_failed');
        this.statusEl.textContent =
            error === 'navigate_to_loop_origin_first' ?
            'Return to the loop origin page before resetting the loop target.' :
            `Unable to reset loop target: ${error}`;
        return;
      }
      this.guidedLoopPhase = 'await_loop_target';
      this.guidedCapturePending = false;
      this.guidedCaptureKind = 'none';
      this.clearGuidedSelectionModel();
      this.loopCaptureActive = false;
      this.statusEl.textContent =
          'Step 1/3 reset. Click a repeated item to choose a new loop target.';
      this.updateGuidedControlsState();
      await this.pushGuidedStateToHostNow(/*force=*/true);
      await this.syncRecorderStateOnce(true);
    } catch {
      this.statusEl.textContent = 'Unable to reset loop target.';
    }
  }

  private async popLastGuidedScrapeTarget(): Promise<void> {
    if (!this.guidedLoopActive || this.guidedLoopPhase !== 'select_scrape_targets') {
      return;
    }

    try {
      const resp = await (this.browserProxy.handler as any)
          .popGuidedScrapeSelection();
      if (!resp?.ok) {
        const error = String(resp?.error || 'selection_pop_failed');
        this.statusEl.textContent =
            `Unable to remove selected field: ${error}`;
        return;
      }
      const selectedTargets = await this.fetchGuidedScrapeSelectionsFromHost();
      if (selectedTargets !== null) {
        this.applyGuidedSelectionModel(selectedTargets);
      }
      const remaining = Math.max(
          0,
          Number(resp?.remainingCount ?? this.guidedScrapeTargets.length));
      this.statusEl.textContent = remaining > 0 ?
          `Removed latest field. ${remaining}/${this.kMaxGuidedScrapeTargets} selected.` :
          'Removed latest field. No fields selected.';
      this.updateGuidedControlsState();
      await this.pushGuidedStateToHostNow(/*force=*/true);
    } catch {
      this.statusEl.textContent = 'Unable to remove selected field.';
    }
  }

  private async undoGuidedReturnStep(): Promise<void> {
    if (!this.guidedLoopActive || this.guidedLoopPhase !== 'record_return') {
      return;
    }

    try {
      const resp = await (this.browserProxy.handler as any)
          .popRecorderManualLoopStep();
      if (!resp?.ok) {
        const error = String(resp?.error || 'loop_step_pop_failed');
        this.statusEl.textContent =
            `Unable to undo scrape step: ${error}`;
        return;
      }
      if (String(resp?.removedAction || '').toLowerCase() !== 'scrape_table') {
        this.statusEl.textContent =
            'Removed the latest loop step. Guided field selection was not restored because the last step was not scrape_table.';
        await this.syncRecorderStateOnce(true);
        return;
      }
      this.guidedLoopPhase = 'select_scrape_targets';
      this.guidedCaptureKind = 'none';
      this.guidedCapturePending = false;
      this.statusEl.textContent =
          'Undid scrape_table insertion. Step 2/3 is active again.';
      this.updateGuidedControlsState();
      await this.pushGuidedStateToHostNow(/*force=*/true);
      await this.syncRecorderStateOnce(true);
    } catch {
      this.statusEl.textContent = 'Unable to undo scrape step.';
    }
  }

  private async handleGuidedPrimaryButton(): Promise<void> {
    if (!this.guidedLoopActive) {
      await this.toggleGuidedLoopFlow();
      return;
    }
    await this.handleGuidedNextStep();
  }

  private async toggleGuidedLoopFlow(): Promise<void> {
    if (this.guidedLoopActive) {
      await this.browserProxy.handler.endRecorderManualLoopCapture();
      this.guidedLoopActive = false;
      this.guidedLoopPhase = 'idle';
      this.guidedCapturePending = false;
      this.guidedCaptureKind = 'none';
      this.clearGuidedSelectionModel();
      this.statusEl.textContent = 'Guided loop canceled.';
      this.updateGuidedControlsState();
      await this.pushGuidedStateToHostNow(/*force=*/true);
      await this.syncRecorderStateOnce(true);
      return;
    }

    this.statusEl.textContent = 'Starting guided scrape table…';
    try {
      const ready = await this.ensureRecorderRunning();
      if (!ready) {
        this.statusEl.textContent =
            'Unable to start recorder for guided scrape table.';
        return;
      }
      this.guidedLoopActive = true;
      this.guidedLoopPhase = 'await_loop_target';
      this.guidedCapturePending = false;
      this.guidedCaptureKind = 'none';
      this.clearGuidedSelectionModel();
      this.updateGuidedControlsState();
      const pushed = await this.pushGuidedStateToHostNow(/*force=*/true);
      if (!pushed) {
        this.guidedLoopActive = false;
        this.guidedLoopPhase = 'idle';
        this.guidedCapturePending = false;
        this.guidedCaptureKind = 'none';
        this.clearGuidedSelectionModel();
        this.statusEl.textContent = 'Failed to arm guided state on host.';
        this.updateGuidedControlsState();
        return;
      }
      this.statusEl.textContent =
          'Step 1/3: click a repeated item to set loop target. After that, clicks record normal loop steps until you enter field selection.';
    } catch {
      this.guidedLoopActive = false;
      this.guidedLoopPhase = 'idle';
      this.statusEl.textContent = 'Failed to start guided scrape table.';
      this.updateGuidedControlsState();
    }
  }

  private async handleGuidedNextStep(): Promise<void> {
    if (!this.guidedLoopActive) {
      return;
    }
    if (this.guidedCapturePending &&
        this.guidedLoopPhase !== 'select_scrape_targets') {
      return;
    }

    if (this.guidedLoopPhase === 'await_loop_target') {
      if (!this.loopCaptureActive) {
        await this.syncRecorderStateOnce(/*forceRender=*/false);
      }
      if (!this.loopCaptureActive) {
        this.statusEl.textContent =
            'Step 1/3: click a repeated item to set loop target first.';
        return;
      }
      this.guidedLoopPhase = 'select_scrape_targets';
      this.guidedCaptureKind = 'none';
      this.guidedCapturePending = false;
      this.clearGuidedSelectionModel();
      this.statusEl.textContent =
          'Step 2a/3: click the first field to seed predictions. After that, matching row fields will be outlined in orange and matching column cells in blue. Left Arrow goes back.';
      this.updateGuidedControlsState();
      await this.pushGuidedStateToHostNow(/*force=*/true);
      return;
    }

    if (this.guidedLoopPhase === 'select_scrape_targets') {
      const selectedTargets = await this.fetchGuidedScrapeSelectionsFromHost();
      if (selectedTargets === null) {
        return;
      }
      this.applyGuidedSelectionModel(selectedTargets);
      if (!this.guidedScrapeTargets.length) {
        this.statusEl.textContent =
            this.guidedPredictedRowCount > 0 ?
            `Step 2a/3: select the first field to seed predictions. The page model already sees ${this.guidedPredictedRowCount} repeated row(s).` :
            'Step 2a/3: select the first field to seed predictions for matching row fields and column cells.';
        return;
      }
      this.statusEl.textContent = 'Adding scrape_table action into loop…';
      const appended = await this.appendGuidedScrapeActions();
      if (!appended) {
        this.updateGuidedControlsState();
        return;
      }
      this.guidedLoopPhase = 'record_return';
      this.guidedCaptureKind = 'none';
      this.guidedCapturePending = false;
      this.statusEl.textContent =
          'scrape_table added. Navigate back to the loop origin URL to complete the guided loop automatically.';
      this.updateGuidedControlsState();
      await this.pushGuidedStateToHostNow(/*force=*/true);
      return;
    }

    if (this.guidedLoopPhase === 'record_return') {
      await this.browserProxy.handler.endRecorderManualLoopCapture();
      this.guidedLoopActive = false;
      this.guidedLoopPhase = 'idle';
      this.guidedCaptureKind = 'none';
      this.guidedCapturePending = false;
      this.clearGuidedSelectionModel();
      this.statusEl.textContent =
          'Guided loop complete. Recording remains on for any follow-up steps.';
      this.updateGuidedControlsState();
      await this.pushGuidedStateToHostNow(/*force=*/true);
      await this.syncRecorderStateOnce(true);
    }
  }

  private async handleGuidedPreviousStep(): Promise<void> {
    if (!this.guidedLoopActive || this.guidedLoopPhase !== 'select_scrape_targets') {
      return;
    }

    const prevState = {
      phase: this.guidedLoopPhase,
      capturePending: this.guidedCapturePending,
      captureKind: this.guidedCaptureKind,
    };

    this.guidedLoopPhase = 'await_loop_target';
    this.guidedCaptureKind = 'none';
    this.guidedCapturePending = false;
    this.updateGuidedControlsState();
    const pushed = await this.pushGuidedStateToHostNow(/*force=*/true);
    if (!pushed) {
      this.guidedLoopPhase = prevState.phase;
      this.guidedCapturePending = prevState.capturePending;
      this.guidedCaptureKind = prevState.captureKind;
      this.statusEl.textContent =
          'To return to Step 1/3, first navigate back to the loop origin page.';
      this.updateGuidedControlsState();
      return;
    }
    this.statusEl.textContent =
        'Returned to Step 1/3. Loop target remains set; click Guided Scrape to resume field selection.';
    this.updateGuidedControlsState();
  }

  // Update the tool rail state for recording.
  private updateUIState(isRecording: boolean, locatorCaptureArmed: boolean): void {
    const btn = this.startStopRecorderButton;
    if (!btn) return;

    const showFullRecording =
        isRecording && !locatorCaptureArmed && !this.oneShotLocatorCapturePending;
    if (showFullRecording) {
      this.setButtonIcon(btn, 'stop');
      btn.classList.add('recording-pulse');
      btn.style.color = '#ff4444';
      btn.dataset['tooltip'] = 'Stop Recording';
      btn.setAttribute('aria-label', 'Stop Recording');
    } else {
      this.setButtonIcon(btn, 'record');
      btn.classList.remove('recording-pulse');
      btn.style.color = 'inherit';
      btn.dataset['tooltip'] = 'Start Recording';
      btn.setAttribute('aria-label', 'Start Recording');
    }
    this.updateGuidedControlsState();
  }

  private suggestNewScriptName(): string {
    // Use a predictable default and avoid duplicate suggestions.
    // We only promise uniqueness of the suggestion, not of a typed value.
    let n = 1;
    while (this.scriptNamesLower.has(`newscript${n}`)) {
      n++;
      if (n > 9999) {
        break;
      }
    }
    return `NewScript${n}`;
  }

  private updateLoopCapturePill(active: boolean): void {
    if (!this.loopCapturePillEl) {
      return;
    }
    this.loopCapturePillEl.hidden = !active;
  }

  private buildGuidedWorkflowCard(parsed: any): HTMLElement|null {
    const kind = getActionKind(parsed);
    const params = getActionParams(parsed);
    const rawMeta = (parsed?.meta && typeof parsed.meta === 'object') ? parsed.meta :
        {};
    const guidedMeta =
        (rawMeta.guided && typeof rawMeta.guided === 'object') ?
        rawMeta.guided :
        ((params.guidedMeta && typeof params.guidedMeta === 'object') ?
             params.guidedMeta :
             {});

    let loopLocators: LocatorLike[] = [];
    let scrapeAction: any|null = null;
    if (kind === 'scrape_table') {
      scrapeAction = parsed;
      loopLocators = Array.isArray(params.rowLocators) ? params.rowLocators :
                                                         getActionLocators(parsed);
    } else if (kind === 'loop') {
      loopLocators = getActionLocators(parsed);
      const steps = getLoopSteps(parsed);
      scrapeAction =
          steps.find((step: any) => getActionKind(step) === 'scrape_table') || null;
      if (!scrapeAction) {
        return null;
      }
    } else {
      return null;
    }

    const scrapeParams = getActionParams(scrapeAction);
    const fields = Array.isArray(scrapeParams.fields) ? scrapeParams.fields : [];
    const originUrl = String(guidedMeta.originUrl || guidedMeta.parentUrl || '').trim();
    const loopCount = Number(guidedMeta.loopTargetCount || 0);
    const details = document.createElement('details');
    details.className = 'guided-workflow-card';

    const summary = document.createElement('summary');
    summary.textContent = 'Guided Scrape Workflow';
    details.appendChild(summary);

    const body = document.createElement('div');
    body.className = 'guided-workflow-body';

    const step1 = document.createElement('section');
    step1.className = 'guided-workflow-step';
    const step1Title = document.createElement('div');
    step1Title.className = 'guided-workflow-step-title';
    step1Title.textContent = 'Step 1/3';
    const step1Body = document.createElement('div');
    step1Body.className = 'guided-workflow-step-body';
    const targetSummary = bestLocatorSummary(loopLocators) || 'No loop locator captured yet';
    step1Body.textContent =
        `Loop target${loopCount > 0 ? ` (${loopCount} candidates)` : ''}${targetSummary}`;
    step1.appendChild(step1Title);
    step1.appendChild(step1Body);

    const step2 = document.createElement('section');
    step2.className = 'guided-workflow-step';
    const step2Title = document.createElement('div');
    step2Title.className = 'guided-workflow-step-title';
    step2Title.textContent = 'Step 2/3';
    const step2Body = document.createElement('div');
    step2Body.className = 'guided-workflow-step-body';
    if (!fields.length) {
      step2Body.textContent = 'No scrape fields selected.';
    } else {
      const intro = document.createElement('div');
      intro.textContent =
          'Selected fields on the detail page. Each field becomes an output column.';
      step2Body.appendChild(intro);
      const table = document.createElement('table');
      table.className = 'guided-workflow-table';
      const thead = document.createElement('thead');
      const headerRow = document.createElement('tr');
      const headerName = document.createElement('th');
      headerName.textContent = 'Field';
      const headerLoc = document.createElement('th');
      headerLoc.textContent = 'Locator';
      headerRow.appendChild(headerName);
      headerRow.appendChild(headerLoc);
      thead.appendChild(headerRow);
      table.appendChild(thead);
      const tbody = document.createElement('tbody');
      fields.slice(0, 20).forEach((field: any, idx: number) => {
        const tr = document.createElement('tr');
        const name = String(field?.name || `field_${idx + 1}`).trim() ||
            `field_${idx + 1}`;
        const locs = Array.isArray(field?.locators) ? field.locators : [];
        const nameCell = document.createElement('td');
        nameCell.textContent = name;
        const locatorCell = document.createElement('td');
        locatorCell.textContent = bestLocatorSummary(locs) || 'locator pending';
        tr.appendChild(nameCell);
        tr.appendChild(locatorCell);
        tbody.appendChild(tr);
      });
      table.appendChild(tbody);
      step2Body.appendChild(table);
    }
    step2.appendChild(step2Title);
    step2.appendChild(step2Body);

    const step3 = document.createElement('section');
    step3.className = 'guided-workflow-step';
    const step3Title = document.createElement('div');
    step3Title.className = 'guided-workflow-step-title';
    step3Title.textContent = 'Step 3/3';
    const step3Body = document.createElement('div');
    step3Body.className = 'guided-workflow-step-body';
    step3Body.textContent = originUrl ?
        `Return to origin URL: ${originUrl}` :
        'Return to the page where Step 1 started to close loop recording.';
    step3.appendChild(step3Title);
    step3.appendChild(step3Body);

    const advanced = document.createElement('details');
    advanced.className = 'guided-workflow-advanced';
    const advancedSummary = document.createElement('summary');
    advancedSummary.textContent = 'Raw / Advanced';
    advanced.appendChild(advancedSummary);
    const pre = document.createElement('pre');
    pre.textContent = JSON.stringify(scrapeAction, null, 2);
    advanced.appendChild(pre);

    body.appendChild(step1);
    body.appendChild(step2);
    body.appendChild(step3);
    body.appendChild(advanced);
    details.appendChild(body);
    return details;
  }

  private renderRecorderActions(actionsJson: string[],
                                isRecording: boolean): void {
    this.popoverBody.textContent = '';

    const saveBar = document.createElement('div');
    saveBar.className = 'savebar';

    const hint = document.createElement('div');
    hint.className = 'hint';
    hint.textContent = this.recorderHintOverride ||
        (actionsJson.length ?
             (isRecording ? 'Recording… (stop to save)' : 'Save recording:') :
             '(no recorded actions yet)');

    const saveAsBtn = document.createElement('button');
    saveAsBtn.textContent = 'Save as:';
    saveAsBtn.disabled = isRecording || !actionsJson.length;

    const saveAsInput = document.createElement('input');
    saveAsInput.className = 'saveas';
    saveAsInput.type = 'text';
    saveAsInput.placeholder = this.suggestNewScriptName();
    saveAsInput.value = this.saveAsDraft;
    saveAsInput.disabled = isRecording || !actionsJson.length;
    saveAsInput.title = 'New script name (Enter to save)';
    saveAsInput.addEventListener('input', () => {
      this.saveAsDraft = saveAsInput.value;
    });

    const doSaveAs = async () => {
      const name = (saveAsInput.value || '').trim() ||
          (saveAsInput.placeholder || '').trim();
      if (!name) return;
      const prevActions = actionsJson.slice();
      this.recorderHintOverride = `Saving new script: "${name}"…`;
      this.statusEl.textContent = 'Saving new script…';
      this.saveAsDraft = '';
      this.lastActionsJson = [];
      if (!this.popover.hidden) {
        this.renderRecorderActions([], /*isRecording=*/false);
      }
      try {
        const res =
            await this.browserProxy.handler.saveRecorderToNewDashboardScript(
                name);
        if (this.isOkResult(res)) {
          this.statusEl.textContent =
              `Saved new script (${res.scriptId.slice(0, 8)})`;
          this.recorderHintOverride = `Saved as "${name}" (${res.scriptId.slice(0, 8)})`;
          await this.refreshScripts();
          await this.syncRecorderStateOnce(true);
        } else {
          this.statusEl.textContent = 'Save failed';
          this.recorderHintOverride = '';
          this.lastActionsJson = prevActions;
          if (!this.popover.hidden) {
            this.renderRecorderActions(prevActions, /*isRecording=*/false);
          }
        }
      } catch (e) {
        this.statusEl.textContent = 'Save failed';
        this.recorderHintOverride = '';
        this.lastActionsJson = prevActions;
        if (!this.popover.hidden) {
          this.renderRecorderActions(prevActions, /*isRecording=*/false);
        }
      }
    };
    saveAsBtn.addEventListener('click', () => void doSaveAs());
    saveAsInput.addEventListener('keydown', (e: KeyboardEvent) => {
      if (e.key === 'Enter') {
        e.preventDefault();
        void doSaveAs();
      }
    });

    const appendBtn = document.createElement('button');
    appendBtn.textContent = 'Append';
    appendBtn.disabled =
        isRecording || !actionsJson.length || !this.scriptSelect.value;
    const selectedName =
        this.scriptSelect.selectedOptions?.[0]?.textContent?.trim() || '';
    const selectedId = this.scriptSelect.value;
    const selectedLabel = selectedName || selectedId;
    appendBtn.title = selectedId ?
        `Append to: ${selectedLabel} (${selectedId})` :
        'Select a script first';
    appendBtn.addEventListener('click', async () => {
      const id = this.scriptSelect.value;
      if (!id) {
        return;
      }
      const name =
          this.scriptSelect.selectedOptions?.[0]?.textContent?.trim() || '';
      const label = name || id;
      const count = actionsJson.length;
      const prevActions = actionsJson.slice();
      this.recorderHintOverride =
          `Appending ${count} to "${label}" (${id.slice(0, 8)})…`;
      this.statusEl.textContent = `Appending ${count}… (${id.slice(0, 8)})`;
      this.lastActionsJson = [];
      if (!this.popover.hidden) {
        this.renderRecorderActions([], /*isRecording=*/false);
      }
      try {
        const res =
            await this.browserProxy.handler.appendRecorderToDashboardScript(id);
        if (this.isOkResult(res)) {
          this.statusEl.textContent =
              `Appended ${count} to "${label}" (${id.slice(0, 8)}), cleared log`;
          this.recorderHintOverride =
              `Appended to "${label}" (${id.slice(0, 8)}), cleared log`;
          await this.refreshScripts();
          await this.syncRecorderStateOnce(true);
        } else {
          this.statusEl.textContent = 'Append failed';
          this.recorderHintOverride = '';
          this.lastActionsJson = prevActions;
          if (!this.popover.hidden) {
            this.renderRecorderActions(prevActions, /*isRecording=*/false);
          }
        }
      } catch (e) {
        this.statusEl.textContent = 'Append failed';
        this.recorderHintOverride = '';
        this.lastActionsJson = prevActions;
        if (!this.popover.hidden) {
          this.renderRecorderActions(prevActions, /*isRecording=*/false);
        }
      }
    });

    const discardBtn = document.createElement('button');
    discardBtn.textContent = 'Discard';
    discardBtn.disabled = isRecording || !actionsJson.length;
    discardBtn.addEventListener('click', async () => {
      const count = actionsJson.length;
      const prevActions = actionsJson.slice();
      this.recorderHintOverride = `Discarding ${count}…`;
      this.statusEl.textContent = 'Discarding…';
      this.lastActionsJson = [];
      if (!this.popover.hidden) {
        this.renderRecorderActions([], /*isRecording=*/false);
      }
      try {
        const res = await this.browserProxy.handler.discardRecorderActions();
        if (this.isOkResult(res)) {
          this.statusEl.textContent = `Discarded ${count}, cleared log`;
          this.recorderHintOverride = `Discarded ${count}, cleared log`;
          await this.syncRecorderStateOnce(true);
        } else {
          this.statusEl.textContent = 'Discard failed';
          this.recorderHintOverride = '';
          this.lastActionsJson = prevActions;
          if (!this.popover.hidden) {
            this.renderRecorderActions(prevActions, /*isRecording=*/false);
          }
        }
      } catch (e) {
        this.statusEl.textContent = 'Discard failed';
        this.recorderHintOverride = '';
        this.lastActionsJson = prevActions;
        if (!this.popover.hidden) {
          this.renderRecorderActions(prevActions, /*isRecording=*/false);
        }
      }
    });

    saveBar.appendChild(hint);
    saveBar.appendChild(saveAsBtn);
    saveBar.appendChild(saveAsInput);
    saveBar.appendChild(appendBtn);
    saveBar.appendChild(discardBtn);
    this.popoverBody.appendChild(saveBar);

    if (!actionsJson.length) {
      return;
    }

    const visibleEntries = actionsJson.map((json, index) => {
      const parsed = tryParseActionJson(json);
      return {json, index, parsed};
    }).filter((entry) => {
      const direction = guidedShortcutDirectionFromAction(entry.parsed);
      return !(direction && this.isShortcutDirectionAllowed(direction));
    });

    visibleEntries.forEach(({json, index, parsed}) => {
      const row = document.createElement('div');
      row.className = 'row action-row';

      const main = document.createElement('div');
      main.className = 'action-row-main';
      const label = document.createElement('div');
      label.className = 'label';
      label.textContent = `${index}: ${safeJsonSummary(json)}`;
      main.appendChild(label);
      const guidedCard = this.buildGuidedWorkflowCard(parsed);
      if (guidedCard) {
        main.appendChild(guidedCard);
      }
      row.appendChild(main);

      const del = document.createElement('button');
      del.textContent = 'Del';
      del.addEventListener('click', async () => {
        await this.browserProxy.handler.removeRecordedAction(index);
        const resp = await this.browserProxy.handler.getRecorderSnapshot();
        this.lastIsRecording = resp.isRecording;
        this.lastLocatorCaptureArmed = false;
        this.lastLocatorCaptureReady = false;
        this.lastActionsJson = resp.actionsJson || [];
        this.renderRecorderActions(this.lastActionsJson, this.lastIsRecording);
      });
      row.appendChild(del);

      this.popoverBody.appendChild(row);
    });
  }

  private async refreshScripts(): Promise<void> {
    this.statusEl.textContent = 'Loading scripts…';
    this.scriptSelect.textContent = '';
    this.scriptNamesLower.clear();
    try {
      const resp = await this.browserProxy.handler.getDashboardScripts();
      const scripts = (resp.scripts as ScriptInfo[]) || [];
      scripts.forEach(s => {
        const opt = document.createElement('option');
        opt.value = s.id;
        const name = (s.name || s.id || '').trim();
        opt.textContent = name || s.id;
        this.scriptSelect.appendChild(opt);
        if (name) {
          this.scriptNamesLower.add(name.toLowerCase());
        }
      });
      this.statusEl.textContent =
          scripts.length ? `Scripts: ${scripts.length}` : 'No scripts found';
    } catch (e) {
      this.statusEl.textContent = 'Failed to load scripts';
    }
  }

  private async startSelected(target: AutomationRunTarget): Promise<void> {
    if (this.guidedLoopActive) {
      this.statusEl.textContent = 'Finish guided loop flow before starting a run.';
      return;
    }
    const id = this.scriptSelect.value;
    if (!id) {
      this.statusEl.textContent = 'Pick a script';
      return;
    }
    this.statusEl.textContent = 'Starting…';
    try {
      const resp =
          await this.browserProxy.handler.startDashboardScript(id, target);
      this.executionId = resp.executionId || '';
      this.updateRunControls();
      this.statusEl.textContent = this.executionId ?
          `Running ${id} (${this.executionId.slice(0, 8)})` :
          'Start failed';
    } catch (e) {
      this.executionId = '';
      this.updateRunControls();
      this.statusEl.textContent = 'Start failed';
    }
  }

  private async stopRun(): Promise<void> {
    if (!this.executionId) {
      this.statusEl.textContent = 'No active run';
      return;
    }
    try {
      const resp = await this.browserProxy.handler.stopDashboardRun(
          this.executionId, 'stopped from Glic');
      this.statusEl.textContent = resp.stopped ? 'Stopped' : 'No active run';
      if (resp.stopped) {
        this.executionId = '';
      } else {
        // Stale execution id in the UI; clear so repeated Stop clicks are no-op.
        this.executionId = '';
      }
      this.updateRunControls();
    } catch (e) {
      this.statusEl.textContent = 'Stop failed';
    }
  }

  private updateRunControls(): void {
    if (!this.stopRunButton) {
      return;
    }
    this.stopRunButton.disabled = !this.executionId;
    this.updateGuidedControlsState();
  }

  private async toggleRecorder(): Promise<void> {
    if (this.guidedLoopActive) {
      this.statusEl.textContent =
          'Guided loop flow is active. Use Guided Scrape to advance or Esc to cancel.';
      return;
    }
    try {
      const state = await this.getRecorderState();
      if (state.isRecording) {
        this.oneShotLocatorCapturePending = false;
        this.lastLocatorCaptureArmed = false;
        this.lastLocatorCaptureReady = false;
        await this.browserProxy.handler.stopRecorder();
      } else {
        this.oneShotLocatorCapturePending = false;
        await this.browserProxy.handler.startRecorder();
      }
      await this.syncRecorderStateOnce(true);
    } catch {
    }
  }

  private async armLocatorCapture(): Promise<void> {
    if (this.guidedLoopActive) {
      if (this.guidedCapturePending) {
        return;
      }
      if (this.guidedLoopPhase === 'await_loop_target') {
        await this.armGuidedLocatorCapture('loop_target');
        return;
      }
      if (this.guidedLoopPhase === 'select_scrape_targets') {
        this.statusEl.textContent =
            'Step 2 is direct selection mode. Click page elements to toggle fields.';
        return;
      }
      return;
    }

    if (this.lastLocatorCaptureArmed || this.lastLocatorCaptureReady ||
        this.oneShotLocatorCapturePending) {
      this.statusEl.textContent = 'Canceling locator capture…';
      try {
        const resp = await this.browserProxy.handler
                           .cancelLocatorCaptureForDashboard();
        if (this.isOkResult(resp)) {
          this.oneShotLocatorCapturePending = false;
          this.lastLocatorCaptureArmed = false;
          this.lastLocatorCaptureReady = false;
          this.updateUIState(this.lastIsRecording, false);
          this.statusEl.textContent = 'Locator capture canceled.';
        } else {
          const error = (resp && typeof resp.error === 'string' && resp.error) ?
              resp.error :
              'cancel_failed';
          this.statusEl.textContent = `Cancel failed: ${error}`;
        }
      } catch {
        this.statusEl.textContent = 'Cancel failed';
      }
      return;
    }

    if (this.captureLocatorsButton) {
      this.captureLocatorsButton.disabled = true;
    }
    this.statusEl.textContent = 'Arming locator capture…';
    try {
      const resp = await this.browserProxy.handler.armLocatorCaptureForDashboard();
      if (this.isOkResult(resp)) {
        this.oneShotLocatorCapturePending = true;
        this.lastLocatorCaptureArmed = true;
        this.lastLocatorCaptureReady = true;
        this.updateUIState(
            this.lastIsRecording,
            this.lastLocatorCaptureArmed || this.lastLocatorCaptureReady);
        this.statusEl.textContent =
            'Capture ready: next click captures locators only (no action).';
        this.captureLocatorsButton.classList.add('active');
        window.setTimeout(() => {
          this.captureLocatorsButton.classList.remove('active');
        }, 1200);
      } else {
        this.oneShotLocatorCapturePending = false;
        this.lastLocatorCaptureArmed = false;
        this.lastLocatorCaptureReady = false;
        this.updateUIState(
            this.lastIsRecording,
            this.lastLocatorCaptureArmed || this.lastLocatorCaptureReady);
        const error = (resp && typeof resp.error === 'string' && resp.error) ?
            resp.error :
            'capture_failed';
        this.statusEl.textContent = `Capture failed: ${error}`;
      }
    } catch {
      this.oneShotLocatorCapturePending = false;
      this.lastLocatorCaptureArmed = false;
      this.lastLocatorCaptureReady = false;
      this.updateUIState(
          this.lastIsRecording,
          this.lastLocatorCaptureArmed || this.lastLocatorCaptureReady);
      this.statusEl.textContent = 'Capture failed';
    } finally {
      if (this.captureLocatorsButton) {
        this.captureLocatorsButton.disabled = false;
      }
    }
  }

  private togglePopover(): void {
    this.popover.hidden = !this.popover.hidden;
    this.recorderButton.classList.toggle('active', !this.popover.hidden);
    if (!this.popover.hidden) {
      this.renderRecorderActions(this.lastActionsJson, this.lastIsRecording);
      void this.syncRecorderStateOnce(true);
    }
  }
}
