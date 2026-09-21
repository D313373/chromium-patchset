#!/bin/bash
# Quick-start script for Glic development with local LLM
# This script starts the proxy server and launches Chrome with all necessary flags

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Glic Local LLM Development Environment${NC}"
echo -e "${GREEN}========================================${NC}"

# Determine script directory
# Script is at: chrome/test/data/webui/glic/test_client/start_glic_dev.sh
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# Go up to chrome/ directory (5 levels up from test_client)
CHROME_DIR="$(cd "$SCRIPT_DIR/../../../../.." && pwd)"
# Go up to src/ directory (1 level up from chrome)
SRC_DIR="$(cd "$CHROME_DIR/.." && pwd)"
# Build directory is at src/out/Default
OUT_DIR="${OUT_DIR:-$SRC_DIR/out/Default}"
PORT="${PORT:-8000}"
SIMULACHROME_HOME="${SIMULACHROME_HOME:-$HOME/Library/Application Support/Simulachrome}"
LOG_DIR="${LOG_DIR:-$HOME/notrainnogain}"
USER_DATA_DIR="${USER_DATA_DIR:-$SIMULACHROME_HOME/DevProfile}"
CHROME_LOG_FILE="${CHROME_LOG_FILE:-/tmp/glic_chrome_$(date +%s).log}"
DASHBOARD_SCRIPTS_DIR="${DASHBOARD_SCRIPTS_DIR:-$SIMULACHROME_HOME/Scripts}"
GLOBAL_SETTINGS_JSON="${GLOBAL_SETTINGS_JSON:-$DASHBOARD_SCRIPTS_DIR/global_settings.json}"
HEADLESS_LOG_DIR="${HEADLESS_LOG_DIR:-$SIMULACHROME_HOME/Headless}"
HEADLESS_SHARED_PROFILE="${HEADLESS_SHARED_PROFILE:-$HEADLESS_LOG_DIR/headless_shared}"
HEADLESS_LAUNCH_LOG="${HEADLESS_LAUNCH_LOG:-$HEADLESS_LOG_DIR/headless_listener_launch.log}"
HEADLESS_EVENTS_LOG="${HEADLESS_EVENTS_LOG:-$HEADLESS_LOG_DIR/headless_listener_events.jsonl}"
STREAM_HEADLESS_LOGS="${STREAM_HEADLESS_LOGS:-0}"
STREAM_CHROME_LOGS="${STREAM_CHROME_LOGS:-0}"
SIMULACHROME_PERF_MODE="${SIMULACHROME_PERF_MODE:-1}"
SIMULACHROME_PERF_WATCH="${SIMULACHROME_PERF_WATCH:-0}"
SIMULACHROME_PERF_WATCH_INTERVAL_SEC="${SIMULACHROME_PERF_WATCH_INTERVAL_SEC:-2}"
SIMULACHROME_PERF_WATCH_LOG="${SIMULACHROME_PERF_WATCH_LOG:-/tmp/glic_perf_watch_$(date +%s).log}"
SIMULACHROME_TRACE_STARTUP="${SIMULACHROME_TRACE_STARTUP:-0}"
SIMULACHROME_TRACE_STARTUP_DURATION_SEC="${SIMULACHROME_TRACE_STARTUP_DURATION_SEC:-25}"
SIMULACHROME_TRACE_STARTUP_FILE="${SIMULACHROME_TRACE_STARTUP_FILE:-/tmp/glic_startup_trace_$(date +%s).json}"
SIMULACHROME_TRACE_STARTUP_CATEGORIES="${SIMULACHROME_TRACE_STARTUP_CATEGORIES:-toplevel,benchmark,blink,cc,renderer.scheduler,sequence_manager,ipc,input,ui,mojom,v8,loading}"
SIMULACHROME_ENABLE_ZERO_STATE_PREFETCH="${SIMULACHROME_ENABLE_ZERO_STATE_PREFETCH:-0}"
SIMULACHROME_FORCE_KILL_ORPHAN_HEADLESS_ON_EXIT="${SIMULACHROME_FORCE_KILL_ORPHAN_HEADLESS_ON_EXIT:-1}"
# Headless listener defaults (override by exporting before running this script).
SIMULACHROME_HEADLESS_HOST_SCRIPT="${SIMULACHROME_HEADLESS_HOST_SCRIPT:-$CHROME_DIR/browser/resources/dashboard/headless_listener_host.py}"
SIMULACHROME_HEADLESS_STARTUP_TIMEOUT_SEC="${SIMULACHROME_HEADLESS_STARTUP_TIMEOUT_SEC:-180}"
SIMULACHROME_HEADLESS_STARTUP_RETRIES="${SIMULACHROME_HEADLESS_STARTUP_RETRIES:-3}"
SIMULACHROME_HEADLESS_TARGET_CREATE_TIMEOUT_SEC="${SIMULACHROME_HEADLESS_TARGET_CREATE_TIMEOUT_SEC:-90}"
SIMULACHROME_HEADLESS_USE_NATIVE_EXECUTOR="${SIMULACHROME_HEADLESS_USE_NATIVE_EXECUTOR:-1}"
ENABLE_GLIC_DEBUG_WEBVIEW="${ENABLE_GLIC_DEBUG_WEBVIEW:-0}"
SIMULACHROME_DISABLE_OCCLUDED_WINDOW_BACKGROUNDED="${SIMULACHROME_DISABLE_OCCLUDED_WINDOW_BACKGROUNDED:-1}"
SIMULACHROME_DISABLE_BACKGROUND_TIMER_THROTTLING="${SIMULACHROME_DISABLE_BACKGROUND_TIMER_THROTTLING:-1}"
SIMULACHROME_FORCE_VISIBLE_WIDGETS="${SIMULACHROME_FORCE_VISIBLE_WIDGETS:-1}"
DASHBOARD_DEVTOOLS_PORT="${DASHBOARD_DEVTOOLS_PORT:-9223}"
OLLAMA_HOST="${OLLAMA_HOST:-127.0.0.1}"
OLLAMA_PORT="${OLLAMA_PORT:-11434}"
OLLAMA_API_URL="${OLLAMA_API_URL:-http://${OLLAMA_HOST}:${OLLAMA_PORT}}"
OLLAMA_AUTO_START="${OLLAMA_AUTO_START:-0}"
OLLAMA_START_TIMEOUT_SECS="${OLLAMA_START_TIMEOUT_SECS:-60}"
OLLAMA_POLL_INTERVAL_SECS="${OLLAMA_POLL_INTERVAL_SECS:-1}"
OLLAMA_LOG="${OLLAMA_LOG:-/tmp/glic_ollama_$(date +%s).log}"
OLLAMA_STARTED_BY_SCRIPT=0
OLLAMA_PID=""
RUN_DASHBOARD_SCRIPT=""
RUN_DASHBOARD_SCRIPT_ITERATIONS=1
RUN_DASHBOARD_SCRIPT_TIMEOUT_SEC="${RUN_DASHBOARD_SCRIPT_TIMEOUT_SEC:-600}"
RUN_DASHBOARD_SCRIPT_SCRAPE_DIR=""
EXIT_AFTER_DASHBOARD_SCRIPT=0
NOBUILD=0
FRESH_PROFILE=0
EXTRA_CHROME_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --nobuild|-n)
            NOBUILD=1
            shift
            ;;
        --fresh-profile)
            FRESH_PROFILE=1
            shift
            ;;
        --port|-p)
            PORT="$2"
            shift 2
            ;;
        --outdir|-C)
            OUT_DIR="$2"
            shift 2
            ;;
        --run-dashboard-script)
            RUN_DASHBOARD_SCRIPT="$2"
            shift 2
            ;;
        --dashboard-script-iterations)
            RUN_DASHBOARD_SCRIPT_ITERATIONS="$2"
            shift 2
            ;;
        --dashboard-script-timeout-sec)
            RUN_DASHBOARD_SCRIPT_TIMEOUT_SEC="$2"
            shift 2
            ;;
        --dashboard-devtools-port)
            DASHBOARD_DEVTOOLS_PORT="$2"
            shift 2
            ;;
        --dashboard-script-scrape-dir)
            RUN_DASHBOARD_SCRIPT_SCRAPE_DIR="$2"
            shift 2
            ;;
        --exit-after-dashboard-script)
            EXIT_AFTER_DASHBOARD_SCRIPT=1
            shift
            ;;
        --no-ollama-autostart)
            OLLAMA_AUTO_START=0
            shift
            ;;
        --)
            shift
            EXTRA_CHROME_ARGS=("$@")
            break
            ;;
        *)
            EXTRA_CHROME_ARGS+=("$1")
            shift
            ;;
    esac
done

echo -e "\n${YELLOW}Configuration:${NC}"
echo "  Chrome dir: $CHROME_DIR"
echo "  Build dir:  $OUT_DIR"
echo "  Proxy dir:  $SCRIPT_DIR"
echo "  Port:       $PORT"
echo "  Log dir:    $LOG_DIR"
echo "  Profile:    $USER_DATA_DIR"
echo "  Chrome log: $CHROME_LOG_FILE"
echo "  Scripts:    $DASHBOARD_SCRIPTS_DIR"
echo "  Settings:   $GLOBAL_SETTINGS_JSON"
echo "  Headless log dir: $HEADLESS_LOG_DIR"
echo "  Headless launch log: $HEADLESS_LAUNCH_LOG"
echo "  Headless events log: $HEADLESS_EVENTS_LOG"
echo "  Stream headless logs: $STREAM_HEADLESS_LOGS"
echo "  Headless host script: $SIMULACHROME_HEADLESS_HOST_SCRIPT"
echo "  Force-kill orphan headless on exit: $SIMULACHROME_FORCE_KILL_ORPHAN_HEADLESS_ON_EXIT"
echo "  Headless startup timeout: ${SIMULACHROME_HEADLESS_STARTUP_TIMEOUT_SEC}s"
echo "  Headless startup retries: $SIMULACHROME_HEADLESS_STARTUP_RETRIES"
echo "  Headless target-create timeout: ${SIMULACHROME_HEADLESS_TARGET_CREATE_TIMEOUT_SEC}s"
echo "  Perf mode:  $SIMULACHROME_PERF_MODE"
echo "  Perf watch: $SIMULACHROME_PERF_WATCH"
echo "  Perf watch log: $SIMULACHROME_PERF_WATCH_LOG"
echo "  Trace startup: $SIMULACHROME_TRACE_STARTUP"
echo "  Trace startup file: $SIMULACHROME_TRACE_STARTUP_FILE"
echo "  Zero-state prefetch: $SIMULACHROME_ENABLE_ZERO_STATE_PREFETCH"
echo "  Glic debug webview: $ENABLE_GLIC_DEBUG_WEBVIEW"
echo "  Disable occluded-window backgrounding: $SIMULACHROME_DISABLE_OCCLUDED_WINDOW_BACKGROUNDED"
echo "  Disable background timer throttling: $SIMULACHROME_DISABLE_BACKGROUND_TIMER_THROTTLING"
echo "  Force visible widgets: $SIMULACHROME_FORCE_VISIBLE_WIDGETS"
echo "  Dashboard DevTools port: $DASHBOARD_DEVTOOLS_PORT"
echo "  Ollama API: $OLLAMA_API_URL"
echo "  Ollama auto-start: $OLLAMA_AUTO_START"
if [[ -n "$RUN_DASHBOARD_SCRIPT" ]]; then
    echo "  Auto-run Dashboard script: $RUN_DASHBOARD_SCRIPT"
    echo "  Dashboard script iterations: $RUN_DASHBOARD_SCRIPT_ITERATIONS"
    echo "  Dashboard script timeout: ${RUN_DASHBOARD_SCRIPT_TIMEOUT_SEC}s"
    echo "  Exit after Dashboard script: $EXIT_AFTER_DASHBOARD_SCRIPT"
    if [[ -n "$RUN_DASHBOARD_SCRIPT_SCRAPE_DIR" ]]; then
        echo "  Dashboard scrape dir override: $RUN_DASHBOARD_SCRIPT_SCRAPE_DIR"
    fi
fi

echo -e "\n${YELLOW}Dashboard execution settings:${NC}"
python3 - "$GLOBAL_SETTINGS_JSON" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
settings = {}
if path.is_file():
    try:
        loaded = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(loaded, dict):
            settings = loaded
    except Exception as exc:
        print(f"  WARNING: failed to parse {path}: {exc}")
else:
    print(f"  Settings file not found: {path}")

execution = settings.get("execution") if isinstance(settings.get("execution"), dict) else {}
input_behavior = settings.get("inputBehavior") if isinstance(settings.get("inputBehavior"), dict) else {}
listener_mode = settings.get("listenerMode") if isinstance(settings.get("listenerMode"), dict) else {}

primary = execution.get("primaryPath") or execution.get("primary") or "cdp"
fallback = execution.get("fallbackPath") or execution.get("fallback") or ""
overlay = input_behavior.get("showPlaybackOverlay", True)
headless_listeners = listener_mode.get("runListenersInHeadlessMode", True)

print(f"  execution.primaryPath: {primary}")
print(f"  execution.fallbackPath: {fallback or '(none)'}")
print(f"  inputBehavior.showPlaybackOverlay: {overlay}")
print(f"  listenerMode.runListenersInHeadlessMode: {headless_listeners}")

if str(primary).lower() == "native":
    print("  WARNING: primaryPath is 'native'; background-tab click/type/drag often fail in this mode.")
if not overlay:
    print("  WARNING: playback overlay is disabled in global settings.")
if not fallback:
    print("  NOTE: no fallback execution path is configured.")
PY

for arg in "${EXTRA_CHROME_ARGS[@]}"; do
    if [[ "$arg" == "--simulachrome-force-window-activation" ]]; then
        echo -e "${YELLOW}WARNING:${NC} extra Chrome args include --simulachrome-force-window-activation"
        echo "         Native click/type/key actions may bring the tab/window to the front."
    fi
done

# Check if build exists
if [ ! -f "$OUT_DIR/Chromium.app/Contents/MacOS/Chromium" ]; then
    echo -e "\n${RED}ERROR: Chromium build not found at $OUT_DIR${NC}"
    echo "Please build Chrome first:"
    echo "  cd $SRC_DIR"
    echo "  autoninja -C out/Default chrome"
    exit 1
fi

# Check if Ollama is running; start it in the background when needed.
ollama_api_ready() {
    curl -s "${OLLAMA_API_URL}/api/tags" > /dev/null 2>&1
}

wait_for_ollama_api() {
    local timeout_secs="$1"
    local start_time
    start_time=$(date +%s)
    while true; do
        if ollama_api_ready; then
            return 0
        fi
        local now elapsed
        now=$(date +%s)
        elapsed=$((now - start_time))
        if [[ $elapsed -ge $timeout_secs ]]; then
            return 1
        fi
        sleep "$OLLAMA_POLL_INTERVAL_SECS"
    done
}

ensure_ollama_server() {
    if ollama_api_ready; then
        echo -e "${GREEN}✅ Ollama API is reachable at ${OLLAMA_API_URL}${NC}"
        return 0
    fi

    if [[ "$OLLAMA_AUTO_START" != "1" ]]; then
        echo -e "\n${YELLOW}WARNING: Ollama doesn't appear to be running at ${OLLAMA_API_URL}${NC}"
        echo "Start it manually, for example:"
        echo "  ollama serve"
        echo "  # or on macOS: open -a Ollama"
        echo ""
        read -p "Continue anyway? (y/N) " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            exit 1
        fi
        return 0
    fi

    echo -e "\n${YELLOW}Ollama is not reachable; attempting to start it...${NC}"

    if [[ "$(uname -s)" == "Darwin" ]] && [[ -d "/Applications/Ollama.app" ]]; then
        echo -e "${YELLOW}   Launching Ollama.app...${NC}"
        open -a Ollama >/dev/null 2>&1 || true
        if wait_for_ollama_api "$OLLAMA_START_TIMEOUT_SECS"; then
            echo -e "${GREEN}✅ Ollama API is ready (via Ollama.app)${NC}"
            return 0
        fi
        echo -e "${YELLOW}   Ollama.app did not become ready in ${OLLAMA_START_TIMEOUT_SECS}s; trying ollama serve...${NC}"
    fi

    if ! command -v ollama >/dev/null 2>&1; then
        echo -e "${RED}ERROR: ollama CLI not found in PATH and API is unreachable.${NC}"
        echo "Install Ollama from https://ollama.com or add it to PATH."
        exit 1
    fi

    echo -e "${YELLOW}   Starting: ollama serve${NC}"
    echo -e "${YELLOW}   Ollama logs: $OLLAMA_LOG${NC}"
    # Background the server in this shell (no new terminal window needed).
    OLLAMA_HOST="$OLLAMA_HOST" OLLAMA_PORT="$OLLAMA_PORT" \
        ollama serve > "$OLLAMA_LOG" 2>&1 &
    OLLAMA_PID=$!
    OLLAMA_STARTED_BY_SCRIPT=1

    if ! wait_for_ollama_api "$OLLAMA_START_TIMEOUT_SECS"; then
        echo -e "${RED}ERROR: Ollama API is still unreachable after ${OLLAMA_START_TIMEOUT_SECS}s${NC}"
        if [[ -f "$OLLAMA_LOG" ]]; then
            echo -e "${RED}Last 40 lines of Ollama log:${NC}"
            tail -40 "$OLLAMA_LOG"
        fi
        if [[ -n "$OLLAMA_PID" ]] && kill -0 "$OLLAMA_PID" 2>/dev/null; then
            kill "$OLLAMA_PID" 2>/dev/null || true
        fi
        exit 1
    fi

    echo -e "${GREEN}✅ Ollama server started (PID: $OLLAMA_PID)${NC}"
    echo -e "${YELLOW}   API: ${OLLAMA_API_URL}${NC}"
    echo -e "${YELLOW}   Monitor logs with: tail -f $OLLAMA_LOG${NC}"
}

ensure_ollama_server

# Run unit tests first
echo -e "\n${YELLOW}Running sanitization tests...${NC}"
if python3 "$SCRIPT_DIR/test_sanitization.py"; then
    echo -e "${GREEN}✅ All tests passed${NC}"
else
    echo -e "${RED}❌ Tests failed${NC}"
    read -p "Continue anyway? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# Persist scripts across runs by default:
# - Stable profile dir unless --fresh-profile is provided
# - Stable scripts dir via DASHBOARD_SCRIPTS_DIR (independent of profile)
mkdir -p "$LOG_DIR" "$DASHBOARD_SCRIPTS_DIR" "$HEADLESS_LOG_DIR"
# Chromium creates $USER_DATA_DIR (DevProfile) on first launch via --user-data-dir.
touch "$HEADLESS_LAUNCH_LOG" "$HEADLESS_EVENTS_LOG"
HEADLESS_STATE_PATH="${HEADLESS_STATE_PATH:-$HEADLESS_LOG_DIR/headless_listener_state.json}"
HEADLESS_SCREENSHOTS_DIR="${HEADLESS_SCREENSHOTS_DIR:-$HEADLESS_LOG_DIR/headless_listener_screenshots}"
mkdir -p "$HEADLESS_SHARED_PROFILE" "$HEADLESS_SCREENSHOTS_DIR"
export SIMULACHROME_HOME
export HEADLESS_LOG_DIR
export HEADLESS_SHARED_PROFILE
export HEADLESS_LAUNCH_LOG
export HEADLESS_EVENTS_LOG
export HEADLESS_STATE_PATH
export HEADLESS_SCREENSHOTS_DIR
export DASHBOARD_SCRIPTS_DIR
export SIMULACHROME_HEADLESS_HOST_SCRIPT
export SIMULACHROME_HEADLESS_STARTUP_TIMEOUT_SEC
export SIMULACHROME_HEADLESS_STARTUP_RETRIES
export SIMULACHROME_HEADLESS_TARGET_CREATE_TIMEOUT_SEC
export SIMULACHROME_HEADLESS_USE_NATIVE_EXECUTOR
if [[ ! -f "$SIMULACHROME_HEADLESS_HOST_SCRIPT" ]]; then
    echo -e "${YELLOW}WARNING: Headless host script not found at:${NC} $SIMULACHROME_HEADLESS_HOST_SCRIPT"
    echo -e "${YELLOW}         Headless listeners may fail and fall back to background tabs.${NC}"
fi
if [[ $FRESH_PROFILE -eq 1 ]]; then
    USER_DATA_DIR="/tmp/glic_dev_$(date +%s)"
    echo -e "\n${YELLOW}Using fresh user data dir: $USER_DATA_DIR${NC}"
else
    echo -e "\n${YELLOW}Using persistent user data dir: $USER_DATA_DIR${NC}"
fi

if [[ -z "$RUN_DASHBOARD_SCRIPT_SCRAPE_DIR" ]]; then
    RUN_DASHBOARD_SCRIPT_SCRAPE_DIR="$USER_DATA_DIR/Default/SimulachromeData/scrapes"
fi

find_profile_chromium_pids() {
    local profile_dir="$1"
    ps -axo pid=,command= | awk -v needle="--user-data-dir=${profile_dir}" '
        index($0, "/Chromium.app/Contents/MacOS/Chromium") && index($0, needle) {
            print $1;
        }'
}

terminate_profile_chromium_session() {
    local profile_dir="$1"
    local timeout_sec="${2:-20}"
    local pids=()
    local pid=""

    while IFS= read -r pid; do
        [[ -n "$pid" ]] && pids+=("$pid")
    done < <(find_profile_chromium_pids "$profile_dir")

    if [[ ${#pids[@]} -eq 0 ]]; then
        return
    fi

    echo -e "${YELLOW}Closing existing Chromium session for profile:${NC} $profile_dir"
    printf '  %s\n' "${pids[@]}"

    kill "${pids[@]}" 2>/dev/null || true

    local start_ts now remaining_pids=()
    start_ts=$(date +%s)
    while true; do
        remaining_pids=()
        while IFS= read -r pid; do
            [[ -n "$pid" ]] && remaining_pids+=("$pid")
        done < <(find_profile_chromium_pids "$profile_dir")

        if [[ ${#remaining_pids[@]} -eq 0 ]]; then
            echo -e "${GREEN}✅ Existing session closed${NC}"
            return
        fi

        now=$(date +%s)
        if (( now - start_ts >= timeout_sec )); then
            echo -e "${YELLOW}Existing session did not exit cleanly; force-killing profile browser process(es).${NC}"
            kill -KILL "${remaining_pids[@]}" 2>/dev/null || true
            sleep 1
            return
        fi

        sleep 1
    done
}

if [[ -n "$RUN_DASHBOARD_SCRIPT" && $FRESH_PROFILE -eq 0 ]]; then
    terminate_profile_chromium_session "$USER_DATA_DIR"
fi

# Prefer packaged chrome://simulachrome-agent guest (no localhost/serve.py).
# Set USE_PACKAGED_GUEST=0 (or USE_LOCALHOST_GUEST=1) to force classic --glic-dev + serve.py.
USE_PACKAGED_GUEST="${USE_PACKAGED_GUEST:-1}"
if [[ "${USE_LOCALHOST_GUEST:-0}" == "1" ]]; then
  USE_PACKAGED_GUEST=0
fi

PACKAGE_AGENT_UI="${PACKAGE_AGENT_UI:-1}"
AGENT_SRC="$OUT_DIR/gen/chrome/test/data/webui/glic/glic/test_client"
AGENT_DST="$OUT_DIR/Chromium.app/Contents/Resources/simulachrome_agent"
if [[ "$PACKAGE_AGENT_UI" == "1" ]]; then
  if [[ -d "$AGENT_SRC" ]]; then
    mkdir -p "$AGENT_DST"
    rsync -a --delete "$AGENT_SRC/" "$AGENT_DST/" 2>/dev/null || cp -R "$AGENT_SRC/." "$AGENT_DST/"
    export SIMULACHROME_AGENT_DIR="$AGENT_DST"
    echo -e "${GREEN}Packaged agent UI -> $AGENT_DST${NC}"
  else
    echo -e "${YELLOW}Agent gen dir missing; will use serve.py (--glic-dev) if needed.${NC}"
    if [[ "$USE_PACKAGED_GUEST" == "1" ]]; then
      USE_PACKAGED_GUEST=0
    fi
  fi
fi

NEED_SERVE_PY=1
if [[ "$USE_PACKAGED_GUEST" == "1" ]] && [[ -d "${AGENT_DST:-}" || -n "${SIMULACHROME_AGENT_DIR:-}" ]]; then
  NEED_SERVE_PY=0
fi

# Start proxy server only when localhost guest is required.
PROXY_PID=""
PROXY_LOG=""
if [[ "$NEED_SERVE_PY" == "1" ]]; then
PROXY_LOG="/tmp/glic_proxy_$(date +%s).log"
echo -e "\n${YELLOW}Starting proxy server...${NC}"
echo -e "${YELLOW}   Proxy logs: $PROXY_LOG${NC}"
cd "$CHROME_DIR"
SERVER_ARGS=("$SCRIPT_DIR/serve.py" -C "$OUT_DIR" -p "$PORT" --log-dir "$LOG_DIR")
if [[ $NOBUILD -eq 1 ]]; then
    SERVER_ARGS+=(--nobuild)
fi
python3 "${SERVER_ARGS[@]}" > "$PROXY_LOG" 2>&1 &
PROXY_PID=$!

# Wait for proxy to start and check if it's actually listening
echo -e "${YELLOW}   Waiting for proxy to start...${NC}"

# serve.py may spend time building generated test files before binding the port.
# Poll for readiness instead of failing quickly.
PROXY_START_TIMEOUT_SECS="${PROXY_START_TIMEOUT_SECS:-120}"
PROXY_POLL_INTERVAL_SECS="${PROXY_POLL_INTERVAL_SECS:-1}"

START_TIME=$(date +%s)
while true; do
    # If proxy process died, bail out early.
    if ! kill -0 $PROXY_PID 2>/dev/null; then
        echo -e "${RED}ERROR: Proxy server failed to start${NC}"
        echo -e "${RED}Last 40 lines of proxy log:${NC}"
        tail -40 "$PROXY_LOG"
        exit 1
    fi

    if curl -s "http://localhost:${PORT}/" > /dev/null 2>&1; then
        break
    fi

    NOW=$(date +%s)
    ELAPSED=$((NOW - START_TIME))
    if [[ $ELAPSED -ge $PROXY_START_TIMEOUT_SECS ]]; then
        echo -e "${RED}ERROR: Proxy is not responding on port ${PORT} after ${PROXY_START_TIMEOUT_SECS}s${NC}"
        echo -e "${RED}Last 60 lines of proxy log:${NC}"
        tail -60 "$PROXY_LOG"
        kill $PROXY_PID 2>/dev/null
        exit 1
    fi

    sleep "$PROXY_POLL_INTERVAL_SECS"
done

echo -e "${GREEN}✅ Proxy server started (PID: $PROXY_PID)${NC}"
echo -e "${YELLOW}   Server is listening on http://localhost:${PORT}${NC}"
echo -e "${YELLOW}   Logs will show payload sanitization details${NC}"
echo -e "${YELLOW}   Monitor logs with: tail -f $PROXY_LOG${NC}"
else
    echo -e "${GREEN}Skipping serve.py (packaged chrome://simulachrome-agent guest).${NC}"
    echo -e "${YELLOW}Use USE_LOCALHOST_GUEST=1 to force the classic localhost proxy.${NC}"
fi

kill_stream_group() {
    local leader_pid="$1"
    if [[ -z "$leader_pid" ]]; then
        return
    fi
    if kill -0 "$leader_pid" 2>/dev/null; then
        # Prefer killing the process group (when setsid is available), but
        # fall back to the leader PID on platforms without setsid.
        kill -TERM -- "-$leader_pid" 2>/dev/null || kill "$leader_pid" 2>/dev/null || true
    fi
}

kill_orphan_headless_tailers() {
    # Clean up stale streamers from prior runs that exited without trap cleanup.
    # These orphaned tailers cause duplicated log output.
    pkill -f "tail -n 0 -F $HEADLESS_LAUNCH_LOG" 2>/dev/null || true
    pkill -f "tail -n 0 -F $HEADLESS_EVENTS_LOG" 2>/dev/null || true
}

# Always kill stale headless Chromium (detached via start_new_session).
# Prior logic skipped kill when listeners were registered, which left an old
# binary on :9222 across rebuilds / start_glic_dev.sh restarts.
kill_stale_headless() {
    local reason="${1:-cleanup}"
    echo -e "${YELLOW}Killing stale headless Chromium (${reason})...${NC}"

    # Match the detached headless process regardless of listener state.
    pkill -f "Chromium.*--headless-listener" 2>/dev/null || true
    pkill -f "Chromium.*--user-data-dir=${HEADLESS_SHARED_PROFILE}" 2>/dev/null || true

    # Belt-and-suspenders: anything still holding the CDP port.
    if command -v lsof >/dev/null 2>&1; then
        local pids
        pids="$(lsof -nP -iTCP:9222 -sTCP:LISTEN -t 2>/dev/null || true)"
        if [[ -n "$pids" ]]; then
            # shellcheck disable=SC2086
            kill $pids 2>/dev/null || true
        fi
    fi

    sleep 0.4
    pkill -KILL -f "Chromium.*--headless-listener" 2>/dev/null || true
    pkill -KILL -f "Chromium.*--user-data-dir=${HEADLESS_SHARED_PROFILE}" 2>/dev/null || true
    if command -v lsof >/dev/null 2>&1; then
        local pids
        pids="$(lsof -nP -iTCP:9222 -sTCP:LISTEN -t 2>/dev/null || true)"
        if [[ -n "$pids" ]]; then
            # shellcheck disable=SC2086
            kill -KILL $pids 2>/dev/null || true
        fi
    fi

    # Drop listener bookkeeping so the next host launch does not reuse ghosts.
    if [[ -n "${HEADLESS_STATE_PATH:-}" ]]; then
        python3 -c '
import json, os
from pathlib import Path
path = Path(os.environ.get("HEADLESS_STATE_PATH") or "")
if path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"browser_pid": 0, "listeners": {}}, indent=2) + "\n", encoding="utf-8")
' 2>/dev/null || true
    fi

    if command -v lsof >/dev/null 2>&1 && lsof -nP -iTCP:9222 -sTCP:LISTEN >/dev/null 2>&1; then
        echo -e "${RED}WARNING: port 9222 still in use after headless kill${NC}"
    else
        echo -e "${GREEN}✅ Stale headless cleared (port 9222 free)${NC}"
    fi
}

reap_orphan_headless() {
    if [[ "$SIMULACHROME_FORCE_KILL_ORPHAN_HEADLESS_ON_EXIT" != "1" ]]; then
        return
    fi
    kill_stale_headless "exit"
}

# Cleanup function
cleanup() {
    echo -e "\n${YELLOW}Shutting down...${NC}"
    if [[ -n "${PERF_WATCH_PID:-}" ]]; then
        kill "$PERF_WATCH_PID" 2>/dev/null || true
    fi
    kill_stream_group "${HEADLESS_LAUNCH_TAIL_PID:-}"
    kill_stream_group "${HEADLESS_EVENTS_TAIL_PID:-}"
    if [[ "$OLLAMA_STARTED_BY_SCRIPT" == "1" ]] && [[ -n "${OLLAMA_PID:-}" ]] && kill -0 "$OLLAMA_PID" 2>/dev/null; then
        kill "$OLLAMA_PID" 2>/dev/null || true
        echo -e "${GREEN}✅ Ollama server stopped${NC}"
    fi
    if kill -0 $PROXY_PID 2>/dev/null; then
        kill $PROXY_PID
        echo -e "${GREEN}✅ Proxy server stopped${NC}"
    fi
    reap_orphan_headless
}

trap cleanup EXIT

# Fresh headless every run so rebuilds are not shadowed by a stale :9222 process.
kill_stale_headless "startup"

# Launch Chrome
echo -e "\n${YELLOW}Launching Chrome with Glic...${NC}"
echo -e "${YELLOW}Press Ctrl+C to stop everything${NC}"
echo -e "${YELLOW}Chrome will open in a new window${NC}\n"
echo -e "${YELLOW}Chrome logs: $CHROME_LOG_FILE${NC}"

if [[ "$STREAM_HEADLESS_LOGS" == "1" ]]; then
    kill_orphan_headless_tailers
    echo -e "${YELLOW}Streaming headless logs in this terminal:${NC}"
    echo -e "${YELLOW}  [headless-launch] $HEADLESS_LAUNCH_LOG${NC}"
    echo -e "${YELLOW}  [headless-event]  $HEADLESS_EVENTS_LOG${NC}"
    if command -v setsid >/dev/null 2>&1; then
        setsid bash -c "tail -n 0 -F \"$HEADLESS_LAUNCH_LOG\" | sed -u 's/^/[headless-launch] /'" &
    else
        bash -c "tail -n 0 -F \"$HEADLESS_LAUNCH_LOG\" | sed -u 's/^/[headless-launch] /'" &
    fi
    HEADLESS_LAUNCH_TAIL_PID=$!
    if command -v setsid >/dev/null 2>&1; then
        setsid bash -c "tail -n 0 -F \"$HEADLESS_EVENTS_LOG\" | sed -u 's/^/[headless-event]  /'" &
    else
        bash -c "tail -n 0 -F \"$HEADLESS_EVENTS_LOG\" | sed -u 's/^/[headless-event]  /'" &
    fi
    HEADLESS_EVENTS_TAIL_PID=$!
else
    echo -e "${YELLOW}Headless log streaming disabled (STREAM_HEADLESS_LOGS=$STREAM_HEADLESS_LOGS).${NC}"
fi

sleep 1

# Conservative perf profile for Simulachrome dev sessions.
# Can be disabled with: SIMULACHROME_PERF_MODE=0 ./start_glic_dev.sh
PERF_CHROME_ARGS=()
PERF_DISABLE_FEATURES=()
if [[ "$SIMULACHROME_PERF_MODE" == "1" ]]; then
    PERF_CHROME_ARGS+=(
        --log-level=0
        --disable-background-networking
        --disable-component-update
        --disable-domain-reliability
        --disable-client-side-phishing-detection
        --metrics-recording-only
        --disable-default-apps
    )
    PERF_DISABLE_FEATURES+=(
        MediaRouter
        OptimizationHints
        OptimizationGuideModelDownloading
        AutofillServerCommunication
        ContextualCueing
        GlicZeroStateSuggestions
        ZeroStateSuggestionsV2
        ContextualTasks
        ContextualTasksContext
        ContextualTasksSuggestionsEnabled
    )
fi

SETTINGS_CHROME_ARGS=()
SETTINGS_DISABLE_FEATURES=()

append_setting_arg_if_missing() {
    local candidate="$1"
    for existing in "${SETTINGS_CHROME_ARGS[@]}"; do
        if [[ "$existing" == "$candidate" ]]; then
            return
        fi
    done
    SETTINGS_CHROME_ARGS+=("$candidate")
}

if [[ -f "$GLOBAL_SETTINGS_JSON" ]]; then
    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        if [[ "$line" == arg::* ]]; then
            SETTINGS_CHROME_ARGS+=("${line#arg::}")
        elif [[ "$line" == disable_feature::* ]]; then
            SETTINGS_DISABLE_FEATURES+=("${line#disable_feature::}")
        fi
    done < <(python3 - "$GLOBAL_SETTINGS_JSON" <<'PY'
import json
import sys
from pathlib import Path

def read_bool(obj, key, default):
    if isinstance(obj, dict) and isinstance(obj.get(key), bool):
        return obj.get(key)
    return default

path = Path(sys.argv[1])
try:
    settings = json.loads(path.read_text(encoding="utf-8"))
except Exception:
    settings = {}
if not isinstance(settings, dict):
    settings = {}

performance = settings.get("performance") if isinstance(settings.get("performance"), dict) else {}
subsystem = settings.get("subsystem") if isinstance(settings.get("subsystem"), dict) else {}

if read_bool(performance, "aggressivePerformance", False):
    print("arg::--disable-site-isolation-trials")
    print("arg::--process-per-site")
    print("arg::--renderer-process-limit=4")

if read_bool(performance, "deterministicMode", False):
    print("disable_feature::BackForwardCache")
    print("disable_feature::PrerenderFallbackToPreconnect")
    print("disable_feature::InstantUsesSpareRenderer")

if not read_bool(subsystem, "extensionsEnabled", True):
    print("arg::--disable-extensions")

if not read_bool(subsystem, "printPreviewEnabled", True):
    print("arg::--disable-print-preview")

if read_bool(subsystem, "printStubEnabled", False):
    print("arg::--simulachrome-print-stub")

if not read_bool(subsystem, "safeBrowsingEnabled", True):
    print("arg::--safebrowsing-disable-auto-update")
    print("arg::--disable-client-side-phishing-detection")

if not read_bool(subsystem, "serviceDiscoveryEnabled", True):
    print("disable_feature::MediaRouter")
PY
)
fi

COMBINED_DISABLE_FEATURES=("${PERF_DISABLE_FEATURES[@]}" "${SETTINGS_DISABLE_FEATURES[@]}")
if [[ ${#COMBINED_DISABLE_FEATURES[@]} -gt 0 ]]; then
    UNIQUE_DISABLE_FEATURES=()
    while IFS= read -r feature; do
        [[ -z "$feature" ]] && continue
        UNIQUE_DISABLE_FEATURES+=("$feature")
    done < <(printf '%s\n' "${COMBINED_DISABLE_FEATURES[@]}" | awk 'NF && !seen[$0]++')
    if [[ ${#UNIQUE_DISABLE_FEATURES[@]} -gt 0 ]]; then
        SETTINGS_CHROME_ARGS+=("--disable-features=$(IFS=, ; echo "${UNIQUE_DISABLE_FEATURES[*]}")")
    fi
fi

if [[ "$SIMULACHROME_DISABLE_OCCLUDED_WINDOW_BACKGROUNDED" == "1" ]]; then
    append_setting_arg_if_missing "--disable-backgrounding-occluded-windows"
fi

if [[ "$SIMULACHROME_DISABLE_BACKGROUND_TIMER_THROTTLING" == "1" ]]; then
    append_setting_arg_if_missing "--disable-background-timer-throttling"
fi

if [[ "$SIMULACHROME_FORCE_VISIBLE_WIDGETS" == "1" ]]; then
    append_setting_arg_if_missing "--simulachrome-force-visible-widgets"
fi

REMOTE_DEBUGGING_PORT_OVERRIDE=""
for arg in "${SETTINGS_CHROME_ARGS[@]}" "${EXTRA_CHROME_ARGS[@]}"; do
    if [[ "$arg" == --remote-debugging-port=* ]]; then
        REMOTE_DEBUGGING_PORT_OVERRIDE="${arg#*=}"
        break
    fi
done
REMOTE_DEBUGGING_CHROME_ARGS=()
SIMULACHROME_REMOTE_DEBUGGING="${SIMULACHROME_REMOTE_DEBUGGING:-0}"
# Mirror Settings > subsystem.remoteDebuggingEnabled when present.
if [[ -f "$GLOBAL_SETTINGS_JSON" ]]; then
  if python3 -c 'import json,sys; from pathlib import Path; s=json.loads(Path(sys.argv[1]).read_text()); sub=s.get("subsystem") if isinstance(s.get("subsystem"), dict) else {}; raise SystemExit(0 if sub.get("remoteDebuggingEnabled") is True else 1)' "$GLOBAL_SETTINGS_JSON"; then
    SIMULACHROME_REMOTE_DEBUGGING=1
  fi
fi
if [[ -n "$REMOTE_DEBUGGING_PORT_OVERRIDE" ]]; then
    DASHBOARD_DEVTOOLS_PORT="$REMOTE_DEBUGGING_PORT_OVERRIDE"
    SIMULACHROME_REMOTE_DEBUGGING=1
fi
if [[ "$SIMULACHROME_REMOTE_DEBUGGING" == "1" ]]; then
    REMOTE_DEBUGGING_CHROME_ARGS+=("--remote-debugging-port=$DASHBOARD_DEVTOOLS_PORT")
    REMOTE_DEBUGGING_CHROME_ARGS+=("--remote-allow-origins=*")
else
    echo -e "${YELLOW}Remote debugging disabled (enable in Settings or SIMULACHROME_REMOTE_DEBUGGING=1).${NC}"
fi

if [[ ${#SETTINGS_CHROME_ARGS[@]} -gt 0 ]]; then
    echo -e "${YELLOW}Applying restart-required settings flags:${NC}"
    for arg in "${SETTINGS_CHROME_ARGS[@]}"; do
        echo "  $arg"
    done
fi

EXTENSIONS_DISABLED=0
for arg in "${SETTINGS_CHROME_ARGS[@]}"; do
    if [[ "$arg" == "--disable-extensions" ]]; then
        EXTENSIONS_DISABLED=1
        break
    fi
done
if [[ "$EXTENSIONS_DISABLED" == "1" ]]; then
    echo -e "${YELLOW}Extensions runtime disabled: muting extension directory warnings in terminal output.${NC}"
fi

Glic_FEATURES="Glic,GlicDefaultTabContextSetting"
if [[ "$ENABLE_GLIC_DEBUG_WEBVIEW" == "1" ]]; then
    Glic_FEATURES="${Glic_FEATURES},GlicDebugWebview"
fi

TRACE_CHROME_ARGS=()
if [[ "$SIMULACHROME_TRACE_STARTUP" == "1" ]]; then
    TRACE_CHROME_ARGS+=(
        --trace-startup
        --trace-startup-duration="$SIMULACHROME_TRACE_STARTUP_DURATION_SEC"
        --trace-startup-file="$SIMULACHROME_TRACE_STARTUP_FILE"
        --trace-startup-categories="$SIMULACHROME_TRACE_STARTUP_CATEGORIES"
    )
fi

ZERO_STATE_ARGS=()
if [[ "$SIMULACHROME_ENABLE_ZERO_STATE_PREFETCH" == "1" ]]; then
    ZERO_STATE_ARGS+=(--enable-glic-zero-state-prefetch)
fi


# Guest URL: packaged WebUI by default; localhost only when explicitly requested.
GLIC_GUEST_ARGS=()
if [[ "$USE_PACKAGED_GUEST" == "1" ]] && [[ -d "${AGENT_DST:-}" || -n "${SIMULACHROME_AGENT_DIR:-}" ]]; then
  GLIC_GUEST_ARGS+=(
    --simulachrome-agent
    --glic-guest-url="chrome://simulachrome-agent/ollama.html"
    --glic-fre-url="chrome://simulachrome-agent/fre.html"
  )
  echo -e "${GREEN}Guest UI: chrome://simulachrome-agent (packaged; no --glic-dev)${NC}"
else
  GLIC_GUEST_ARGS+=(
    --glic-dev
    --glic-guest-url="http://localhost:${PORT}/glic/test_client/ollama.html"
    --glic-fre-url="http://localhost:${PORT}/glic/test_client/fre.html"
  )
  echo -e "${YELLOW}Guest UI: localhost:${PORT} via serve.py (--glic-dev)${NC}"
fi

# Launch Chrome. By default, keep Chromium stderr/stdout in the log file instead
# of flooding the terminal with unrelated subsystem warnings.
if [[ "$EXTENSIONS_DISABLED" == "1" ]]; then
  if [[ "$STREAM_CHROME_LOGS" == "1" ]]; then
    "$OUT_DIR/Chromium.app/Contents/MacOS/Chromium" \
      --user-data-dir="$USER_DATA_DIR" \
      --no-first-run \
      --enable-logging=stderr \
      --enable-features="$Glic_FEATURES" \
      "${GLIC_GUEST_ARGS[@]}" \
      --glic-open-on-startup=attached \
      "${REMOTE_DEBUGGING_CHROME_ARGS[@]}" \
      "${ZERO_STATE_ARGS[@]}" \
      "${TRACE_CHROME_ARGS[@]}" \
      "${PERF_CHROME_ARGS[@]}" \
      "${SETTINGS_CHROME_ARGS[@]}" \
      "${EXTRA_CHROME_ARGS[@]}" 2>&1 \
      | tee "$CHROME_LOG_FILE" \
      | sed -u \
        -e '/extensions\/browser\/extension_protocols\.cc:293/d' \
        -e '/Failed to get directory for extension/d' &
  else
    "$OUT_DIR/Chromium.app/Contents/MacOS/Chromium" \
      --user-data-dir="$USER_DATA_DIR" \
      --no-first-run \
      --enable-logging=stderr \
      --enable-features="$Glic_FEATURES" \
      "${GLIC_GUEST_ARGS[@]}" \
      --glic-open-on-startup=attached \
      "${REMOTE_DEBUGGING_CHROME_ARGS[@]}" \
      "${ZERO_STATE_ARGS[@]}" \
      "${TRACE_CHROME_ARGS[@]}" \
      "${PERF_CHROME_ARGS[@]}" \
      "${SETTINGS_CHROME_ARGS[@]}" \
      "${EXTRA_CHROME_ARGS[@]}" >> "$CHROME_LOG_FILE" 2>&1 &
  fi
  CHROME_PID=$!
else
  if [[ "$STREAM_CHROME_LOGS" == "1" ]]; then
    "$OUT_DIR/Chromium.app/Contents/MacOS/Chromium" \
      --user-data-dir="$USER_DATA_DIR" \
      --no-first-run \
      --enable-logging=stderr \
      --enable-features="$Glic_FEATURES" \
      "${GLIC_GUEST_ARGS[@]}" \
      --glic-open-on-startup=attached \
      "${REMOTE_DEBUGGING_CHROME_ARGS[@]}" \
      "${ZERO_STATE_ARGS[@]}" \
      "${TRACE_CHROME_ARGS[@]}" \
      "${PERF_CHROME_ARGS[@]}" \
      "${SETTINGS_CHROME_ARGS[@]}" \
      "${EXTRA_CHROME_ARGS[@]}" 2>&1 | tee "$CHROME_LOG_FILE" &
  else
    "$OUT_DIR/Chromium.app/Contents/MacOS/Chromium" \
      --user-data-dir="$USER_DATA_DIR" \
      --no-first-run \
      --enable-logging=stderr \
      --enable-features="$Glic_FEATURES" \
      "${GLIC_GUEST_ARGS[@]}" \
      --glic-open-on-startup=attached \
      "${REMOTE_DEBUGGING_CHROME_ARGS[@]}" \
      "${ZERO_STATE_ARGS[@]}" \
      "${TRACE_CHROME_ARGS[@]}" \
      "${PERF_CHROME_ARGS[@]}" \
      "${SETTINGS_CHROME_ARGS[@]}" \
      "${EXTRA_CHROME_ARGS[@]}" >> "$CHROME_LOG_FILE" 2>&1 &
  fi
  CHROME_PID=$!
fi

AUTO_DASHBOARD_SCRIPT_EXIT=0
if [[ -n "$RUN_DASHBOARD_SCRIPT" ]]; then
    echo -e "${YELLOW}Waiting for DevTools and auto-running Dashboard script...${NC}"
    python3 "$SCRIPT_DIR/run_dashboard_script.py" \
        --devtools-port "$DASHBOARD_DEVTOOLS_PORT" \
        --script "$RUN_DASHBOARD_SCRIPT" \
        --iterations "$RUN_DASHBOARD_SCRIPT_ITERATIONS" \
        --run-timeout-sec "$RUN_DASHBOARD_SCRIPT_TIMEOUT_SEC" \
        --scrape-dir "$RUN_DASHBOARD_SCRIPT_SCRAPE_DIR"
    AUTO_DASHBOARD_SCRIPT_EXIT=$?
    if [[ "$AUTO_DASHBOARD_SCRIPT_EXIT" != "0" ]]; then
        echo -e "${RED}Dashboard script auto-run failed${NC}"
    else
        echo -e "${GREEN}Dashboard script auto-run completed${NC}"
    fi
    if [[ "$EXIT_AFTER_DASHBOARD_SCRIPT" == "1" ]]; then
        echo -e "${YELLOW}Exit-after-run enabled; closing Chrome...${NC}"
        kill "$CHROME_PID" 2>/dev/null || true
    fi
fi

if [[ "$SIMULACHROME_PERF_WATCH" == "1" ]]; then
    echo -e "${YELLOW}Starting perf watch (${SIMULACHROME_PERF_WATCH_INTERVAL_SEC}s interval)...${NC}"
    echo -e "${YELLOW}  Perf watch log: $SIMULACHROME_PERF_WATCH_LOG${NC}"
    {
      while kill -0 "$CHROME_PID" 2>/dev/null; do
        TS="$(date '+%Y-%m-%d %H:%M:%S')"
        echo "=== $TS ==="
        ps -axo pid,ppid,%cpu,%mem,rss,command \
          | awk '/Chromium.app\\/Contents\\/MacOS\\/Chromium|Chromium Helper/ {print}' \
          | sort -k3 -nr \
          | head -n 40
        ps -axo pid,ppid,%cpu,%mem,rss,command \
          | awk '
              /Chromium.app\/Contents\/MacOS\/Chromium|Chromium Helper/ {
                total_cpu += $3;
                total_mem += $4;
                total_rss += $5;
                count += 1;
              }
              END {
                printf("summary count=%d total_cpu=%.1f total_mem=%.1f total_rss_mb=%.1f\n",
                       count, total_cpu, total_mem, total_rss / 1024.0);
              }'
        echo
        sleep "$SIMULACHROME_PERF_WATCH_INTERVAL_SEC"
      done
    } >> "$SIMULACHROME_PERF_WATCH_LOG" 2>&1 &
    PERF_WATCH_PID=$!
fi

CHROME_WAIT_EXIT=0
if ! wait "$CHROME_PID"; then
    CHROME_WAIT_EXIT=$?
fi

if [[ "$AUTO_DASHBOARD_SCRIPT_EXIT" != "0" ]]; then
    exit "$AUTO_DASHBOARD_SCRIPT_EXIT"
fi

if [[ "$EXIT_AFTER_DASHBOARD_SCRIPT" == "1" && "$CHROME_WAIT_EXIT" == "143" ]]; then
    CHROME_WAIT_EXIT=0
fi

if [[ "$CHROME_WAIT_EXIT" != "0" ]]; then
    exit "$CHROME_WAIT_EXIT"
fi

echo -e "\n${GREEN}Chrome exited${NC}"
