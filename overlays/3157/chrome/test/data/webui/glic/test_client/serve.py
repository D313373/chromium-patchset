#!/usr/bin/env python3
# Copyright 2025 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""
Builds and serves the glic test client.
"""

import argparse
import base64
import http.server
import os
import shutil
import socketserver
import time
import subprocess
import sys
import json
import urllib.request
import urllib.error
import re
import threading
from html.parser import HTMLParser

_HERE_PATH = os.path.dirname(__file__)
_SRC_PATH = os.path.normpath(
    os.path.join(_HERE_PATH, '..', '..', '..', '..', '..', '..'))
_THIRD_PARTY_PROTOBUF = os.path.join(_SRC_PATH, 'third_party', 'protobuf',
                                     'python')
if os.environ.get('USE_THIRD_PARTY_PROTOBUF') == '1':
    sys.path.insert(0, _THIRD_PARTY_PROTOBUF)

from google.protobuf.message import DecodeError
from google.protobuf import json_format
from google.protobuf import text_format


class HTMLStripper(HTMLParser):
    """Simple HTML tag stripper to convert HTML to plain text."""
    def __init__(self):
        super().__init__()
        self.reset()
        self.strict = False
        self.convert_charrefs = True
        self.text = []

    def handle_data(self, data):
        self.text.append(data)

    def get_text(self):
        return ' '.join(self.text)


def strip_html_tags(html_content):
    """Strip HTML tags and return plain text."""
    if not html_content or not isinstance(html_content, str):
        return html_content

    stripper = HTMLStripper()
    try:
        stripper.feed(html_content)
        text = stripper.get_text()
        # Clean up whitespace
        text = re.sub(r'\s+', ' ', text).strip()
        return text
    except Exception:
        # Fallback to regex if HTML parser fails
        text = re.sub(r'<[^>]+>', '', html_content)
        text = re.sub(r'\s+', ' ', text).strip()
        return text


def sanitize_payload_recursive(obj, depth=0, max_depth=20, preserved_keys=None,
                               preserve_images=False):
    """
    Recursively sanitize payload to remove binary data while preserving node IDs.

    Args:
        obj: The object to sanitize (dict, list, or primitive)
        depth: Current recursion depth
        max_depth: Maximum recursion depth to prevent infinite loops
        preserved_keys: Set of keys that must be preserved (node IDs, etc.)
        preserve_images: When True (Anthropic vision), keep multimodal image
            parts / data URLs instead of stripping them.

    Returns:
        Sanitized object
    """
    if preserved_keys is None:
        preserved_keys = {
            'backend_node_id', 'backendNodeId', 'backend_id',
            'ax_node_id', 'axNodeId', 'node_id', 'nodeId',
            'element_id', 'elementId', 'target_id', 'targetId',
            'id', 'role', 'name', 'value', 'text', 'label',
            'action', 'type', 'tag', 'tagName'
        }

    # Screenshot and binary data keys to remove
    binary_keys = {
        'screenshot', 'imageData', 'image_data', 'binaryPayload',
        'binary_payload', 'thumbnail', 'snapshot', 'binary_content',
        'binaryContent', 'screenshotData', 'screenshot_data',
        'viewportScreenshot', 'viewport_screenshot', 'image', 'img',
        'base64', 'dataUrl', 'data_url', 'blob'
    }

    # Performance/debug keys to remove
    debug_keys = {
        'trace', 'performance', 'metrics', 'timing', 'profiling',
        'debug', 'debugInfo', 'debug_info', 'stackTrace', 'stack_trace'
    }

    if depth > max_depth:
        return None

    if isinstance(obj, dict):
        sanitized = {}
        for key, value in obj.items():
            # Always preserve node ID fields
            if key in preserved_keys:
                sanitized[key] = value
                continue

            # Remove binary/screenshot data (unless preserving vision payloads)
            if key in binary_keys:
                if preserve_images and key in (
                        'image', 'image_url', 'imageUrl', 'source', 'data',
                        'base64', 'dataUrl', 'data_url', 'media_type',
                        'mediaType'):
                    sanitized[key] = sanitize_payload_recursive(
                        value, depth + 1, max_depth, preserved_keys,
                        preserve_images)
                    continue
                print(f"  [SANITIZE] Removed binary field: {key} (size: {len(str(value))} chars)")
                continue

            # Remove debug/trace data
            if key in debug_keys:
                print(f"  [SANITIZE] Removed debug field: {key}")
                continue

            # Recursively sanitize nested structures
            sanitized_value = sanitize_payload_recursive(value, depth + 1, max_depth, preserved_keys, preserve_images)

            # Keep the key even if the value becomes empty (preserves structure)
            sanitized[key] = sanitized_value

        return sanitized

    elif isinstance(obj, list):
        return [sanitize_payload_recursive(item, depth + 1, max_depth, preserved_keys, preserve_images) for item in obj]

    elif isinstance(obj, str):
        # Keep intentional vision data URLs / raw image base64 for Anthropic.
        if preserve_images and (
                obj.startswith('data:image') or
                (len(obj) > 10000 and
                 re.match(r'^[A-Za-z0-9+/=]+$', obj[:100]))):
            return obj
        # If string is very long and looks like base64, truncate it
        if len(obj) > 10000 and re.match(r'^[A-Za-z0-9+/=]+$', obj[:100]):
            print(f"  [SANITIZE] Truncated suspected base64 string (original: {len(obj)} chars)")
            return "[BASE64_DATA_REMOVED]"

        # If string contains HTML tags and is very long, strip tags
        if len(obj) > 5000 and '<' in obj and '>' in obj:
            stripped = strip_html_tags(obj)
            # Truncate to ~6k tokens (24k chars)
            if len(stripped) > 24000:
                stripped = stripped[:24000] + "... [TRUNCATED]"
                print(f"  [SANITIZE] Stripped HTML and truncated (original: {len(obj)} -> {len(stripped)} chars)")
            return stripped

        return obj

    else:
        return obj


def sanitize_openai_payload(payload_str, preserve_images=False):
    """
    Sanitize OpenAI chat completion payload for local LLM.
    Removes screenshots, simplifies DOM data, preserves node IDs.

    Args:
        payload_str: JSON string of the request payload
        preserve_images: Keep multimodal image parts (Anthropic vision).

    Returns:
        Sanitized JSON string
    """
    try:
        payload = json.loads(payload_str)
        original_size = len(payload_str)

        print(f"\n[PAYLOAD SANITIZATION]")
        print(f"  Original size: {original_size:,} bytes ({original_size/1024:.1f} KB)")
        if preserve_images:
            print("  preserve_images=True (Anthropic vision pass-through)")

        # Sanitize the entire payload recursively
        sanitized = sanitize_payload_recursive(
            payload, preserve_images=preserve_images)

        # Add system prompt for Actor functionality if not present
        if 'messages' in sanitized and isinstance(sanitized['messages'], list):
            has_system_prompt = any(
                msg.get('role') == 'system'
                for msg in sanitized['messages']
            )

            if not has_system_prompt:
                actor_system_prompt = {
                    "role": "system",
                    "content": (
                        "You are a web browser agent assistant. Interactive elements in the page "
                        "have content_node_id numbers (commonAncestorDomNodeId) that identify them. "
                        "When you want to interact with an element, output JSON like: "
                        '{"action": "click", "target_id": <content_node_id>} or '
                        '{"action": "type", "target_id": <content_node_id>, "text": "..."}. '
                        "Always preserve and reference these IDs when discussing page elements. "
                        "When a task is complete, output JSON like: "
                        '{"action": "complete", "success": true}.'
                    )
                }
                sanitized['messages'].insert(0, actor_system_prompt)
                print(f"  [SANITIZE] Added Actor system prompt")

        sanitized_str = json.dumps(sanitized)
        sanitized_size = len(sanitized_str)

        print(f"  Sanitized size: {sanitized_size:,} bytes ({sanitized_size/1024:.1f} KB)")
        print(f"  Reduction: {original_size - sanitized_size:,} bytes ({(1 - sanitized_size/original_size)*100:.1f}%)")

        return sanitized_str.encode('utf-8')

    except json.JSONDecodeError as e:
        print(f"  [ERROR] Failed to parse JSON: {e}")
        return payload_str.encode('utf-8') if isinstance(payload_str, str) else payload_str
    except Exception as e:
        print(f"  [ERROR] Sanitization failed: {e}")
        return payload_str.encode('utf-8') if isinstance(payload_str, str) else payload_str


def build(outdir: str):
    subprocess.run([
        shutil.which('autoninja'), '-C', outdir,
        'chrome/test/data/webui/glic:generate_test_files'
    ],
                   stdout=sys.stdout,
                   stderr=sys.stderr,
                   check=True)


def _read_json_bytes(data: bytes):
    if not data:
        return None
    try:
        return json.loads(data.decode('utf-8'))
    except Exception:
        return None



def _normalize_provider(raw):
    provider = str(raw or '').strip().lower()
    if provider in ('ollama', 'cerebras', 'gemini', 'openai', 'anthropic'):
        return provider
    return 'ollama'


def _default_endpoint_for_provider(provider):
    return {
        'ollama': 'http://127.0.0.1:11434/v1/chat/completions',
        'cerebras': 'https://api.cerebras.ai/v1/chat/completions',
        'gemini': 'https://generativelanguage.googleapis.com/v1beta',
        'openai': 'https://api.openai.com/v1/chat/completions',
        'anthropic': 'https://api.anthropic.com/v1/messages',
    }.get(provider, 'http://127.0.0.1:11434/v1/chat/completions')


def _api_key_env_for_provider(provider):
    return {
        'anthropic': 'ANTHROPIC_API_KEY',
        'openai': 'OPENAI_API_KEY',
        'cerebras': 'CEREBRAS_API_KEY',
        'gemini': 'GEMINI_API_KEY',
    }.get(provider, '')

def _devtools_port():
    raw = os.environ.get('DASHBOARD_DEVTOOLS_PORT') or os.environ.get(
        'GLIC_DEVTOOLS_PORT') or '9223'
    try:
        return int(raw)
    except Exception:
        return 9223


def _provider_response_to_openai(provider, resp_json, model=''):
    if not isinstance(resp_json, dict):
        text = str(resp_json or '')
        return {
            'object': 'chat.completion',
            'model': model,
            'choices': [{
                'index': 0,
                'message': {'role': 'assistant', 'content': text},
                'finish_reason': 'stop',
            }],
        }
    if isinstance(resp_json.get('choices'), list):
        return resp_json
    if provider == 'anthropic':
        return _anthropic_response_to_openai(resp_json)
    if provider == 'gemini':
        return _gemini_response_to_openai(resp_json)
    return {
        'object': 'chat.completion',
        'model': model or str(resp_json.get('model') or ''),
        'choices': [{
            'index': 0,
            'message': {
                'role': 'assistant',
                'content': json.dumps(resp_json),
            },
            'finish_reason': 'stop',
        }],
    }


def _find_glic_host_target(rds, port):
    """Find the already-open chrome://glic host. Never creates tabs."""
    targets = rds._http_json(port, '/json/list')
    for target in targets or []:
        url = str(target.get('url') or '')
        if (url.startswith('chrome://glic') and
                target.get('webSocketDebuggerUrl')):
            return target
    return None


def _chat_via_glic_host(provider, payload, endpoint=''):
    """Run hosted-provider chat through the existing chrome://glic WebUI.

    Uses Data > Secrets via dashboardAgentOllamaChat on the glic host itself.
    Does not open chrome://dashboard or any other tab.
    """
    import run_dashboard_script as rds

    port = _devtools_port()
    rds._wait_for_devtools(port, timeout_sec=8.0)
    host = _find_glic_host_target(rds, port)
    if not host:
        raise RuntimeError(
            'glic_host_unavailable: chrome://glic is not open; '
            'hosted providers need the Glic side panel/host (no dashboard tab).')

    ws_url = str(host.get('webSocketDebuggerUrl') or '')
    ws = rds.WebSocketCDP(ws_url, timeout=30.0)
    try:
        messages = payload.get('messages') if isinstance(payload, dict) else []
        model = str((payload or {}).get('model') or '')
        # chrome.send is available on the glic host after GlicUI registers the
        # dashboard agent chat handler. No dashboardHandler JS object required.
        expr = (
            '(async () => {\n'
            '  const payload = {\n'
            f'    provider: {json.dumps(provider)},\n'
            f'    model: {json.dumps(model)},\n'
            f'    endpoint: {json.dumps(str(endpoint or ""))},\n'
            f'    messages: {json.dumps(messages)},\n'
            '    max_tokens: 4096,\n'
            '  };\n'
            '  return await new Promise((resolve) => {\n'
            '    const cb = "__glicAgentChatCb_" + Date.now() + "_" +\n'
            '      Math.random().toString(36).slice(2);\n'
            '    let settled = false;\n'
            '    const finish = (res) => {\n'
            '      if (settled) return;\n'
            '      settled = true;\n'
            '      try { delete window[cb]; } catch (_) {}\n'
            '      resolve(res && typeof res === "object" ? res :\n'
            '        { ok: false, error: "invalid_chat_response" });\n'
            '    };\n'
            '    window[cb] = finish;\n'
            '    try {\n'
            '      if (!window.chrome || typeof window.chrome.send !== "function") {\n'
            '        finish({ ok: false, error: "chrome.send_unavailable_on_glic_host" });\n'
            '        return;\n'
            '      }\n'
            '      window.chrome.send("dashboardAgentOllamaChat", [cb, payload]);\n'
            '    } catch (e) {\n'
            '      finish({ ok: false, error: String(e && e.message ? e.message : e) });\n'
            '      return;\n'
            '    }\n'
            '    setTimeout(() => finish({ ok: false, error: "glic_host_chat_timeout" }),\n'
            '               120000);\n'
            '  });\n'
            '})()'
        )
        result = rds._runtime_eval(ws, expr, timeout_sec=130.0)
        if not isinstance(result, dict):
            raise RuntimeError('glic_host_chat_invalid_result')
        if not result.get('ok'):
            err = str(result.get('error') or 'glic_host_chat_failed')
            raise RuntimeError(err)
        body_text = str(result.get('body') or '')
        try:
            body_json = json.loads(body_text) if body_text else {}
        except Exception:
            body_json = {'raw': body_text}
        return _provider_response_to_openai(provider, body_json, model=model)
    finally:
        try:
            ws.close()
        except Exception:
            pass


def _parse_data_image_url(url):
    raw = str(url or '')
    if not raw.startswith('data:') or ',' not in raw:
        return None, None
    meta, data = raw.split(',', 1)
    meta = meta[len('data:'):]
    media_type = meta.split(';', 1)[0].strip()
    if not media_type or not data:
        return None, None
    return media_type, data


def _openai_content_to_anthropic_parts(content):
    parts = []
    if isinstance(content, list):
        for part in content:
            if not isinstance(part, dict):
                if part:
                    parts.append({'type': 'text', 'text': str(part)})
                continue
            ptype = str(part.get('type') or '').strip().lower()
            if ptype in ('', 'text'):
                text = str(part.get('text') or '')
                if text:
                    parts.append({'type': 'text', 'text': text})
                continue
            if ptype == 'image_url':
                image_url = part.get('image_url') if isinstance(part.get('image_url'), dict) else {}
                url = image_url.get('url') or part.get('url')
                media_type, data = _parse_data_image_url(url)
                if media_type and data:
                    parts.append({
                        'type': 'image',
                        'source': {
                            'type': 'base64',
                            'media_type': media_type,
                            'data': data,
                        },
                    })
                continue
            if ptype == 'image':
                source = part.get('source') if isinstance(part.get('source'), dict) else {}
                media_type = str(source.get('media_type') or '').strip()
                data = str(source.get('data') or '')
                if media_type and data:
                    parts.append({
                        'type': 'image',
                        'source': {
                            'type': 'base64',
                            'media_type': media_type,
                            'data': data,
                        },
                    })
        return parts
    text = str(content)
    if text:
        parts.append({'type': 'text', 'text': text})
    return parts


def _openai_messages_to_anthropic(payload):
    messages = payload.get('messages') or []
    system_parts = []
    anthropic_messages = []
    for item in messages:
        if not isinstance(item, dict):
            continue
        role = str(item.get('role') or '').strip().lower()
        content = item.get('content')
        if content is None:
            continue
        if role == 'system':
            if isinstance(content, list):
                text = ' '.join(
                    str(part.get('text') or '')
                    for part in content
                    if isinstance(part, dict))
            else:
                text = str(content)
            if text.strip():
                system_parts.append(text)
            continue
        if role not in ('user', 'assistant'):
            continue
        parts = _openai_content_to_anthropic_parts(content)
        if not parts:
            continue
        anthropic_messages.append({
            'role': role,
            'content': parts,
        })
    body = {
        'model': payload.get('model') or 'claude-haiku-4-5',
        'max_tokens': int(payload.get('max_tokens') or 4096),
        'messages': anthropic_messages,
    }
    # Newer Claude models reject `temperature` (deprecated). Only forward it
    # when the client explicitly sets a non-default value AND the model is not
    # known to reject it.
    model_name = str(body['model'] or '').lower()
    temp_deprecated = any(
        token in model_name for token in (
            'claude-sonnet-4-5',
            'claude-opus-4',
            'claude-haiku-4-5',
            'sonnet-4-5',
            'opus-4-5',
            'haiku-4-5',
        ))
    if 'temperature' in payload and payload.get('temperature') is not None and not temp_deprecated:
        try:
            body['temperature'] = float(payload.get('temperature'))
        except (TypeError, ValueError):
            pass
    if system_parts:
        body['system'] = '\n\n'.join(system_parts)
    return body


def _anthropic_response_to_openai(resp_json):
    text_parts = []
    content = resp_json.get('content') if isinstance(resp_json, dict) else None
    if isinstance(content, list):
        for part in content:
            if isinstance(part, dict) and part.get('type') == 'text':
                text_parts.append(str(part.get('text') or ''))
    text = ''.join(text_parts)
    return {
        'id': (resp_json or {}).get('id') if isinstance(resp_json, dict) else '',
        'object': 'chat.completion',
        'model': (resp_json or {}).get('model') if isinstance(resp_json, dict) else '',
        'choices': [{
            'index': 0,
            'message': {'role': 'assistant', 'content': text},
            'finish_reason': 'stop',
        }],
    }


def _openai_messages_to_gemini(payload):
    messages = payload.get('messages') or []
    system_parts = []
    contents = []
    for item in messages:
        if not isinstance(item, dict):
            continue
        role = str(item.get('role') or '').strip().lower()
        content = str(item.get('content') or '')
        if role == 'system':
            if content.strip():
                system_parts.append(content)
            continue
        if role not in ('user', 'assistant'):
            continue
        contents.append({
            'role': 'model' if role == 'assistant' else 'user',
            'parts': [{'text': content}],
        })
    body = {
        'contents': contents,
        'generationConfig': {
            'temperature': float(payload.get('temperature') or 0),
            'maxOutputTokens': int(payload.get('max_tokens') or 4096),
        },
    }
    if system_parts:
        body['systemInstruction'] = {
            'parts': [{'text': '\n\n'.join(system_parts)}]
        }
    return body


def _gemini_response_to_openai(resp_json):
    text = ''
    try:
        candidates = resp_json.get('candidates') or []
        parts = (((candidates[0] or {}).get('content') or {}).get('parts') or [])
        text = ''.join(str(part.get('text') or '') for part in parts if isinstance(part, dict))
    except Exception:
        text = ''
    return {
        'object': 'chat.completion',
        'choices': [{
            'index': 0,
            'message': {'role': 'assistant', 'content': text},
            'finish_reason': 'stop',
        }],
    }



def _prefs_path():
    """Stable prefs file outside the Chromium profile (survives browser reset)."""
    if RequestHandler.log_dir:
        return os.path.join(RequestHandler.log_dir, 'glic_client_prefs.json')
    return os.path.join(_HERE_PATH, 'glic_client_prefs.json')


def _read_client_prefs():
    path = _prefs_path()
    try:
        with open(path, 'r', encoding='utf-8') as handle:
            data = json.load(handle)
        return data if isinstance(data, dict) else {}
    except Exception:
        return {}


def _write_client_prefs(payload):
    if not isinstance(payload, dict):
        return False, 'prefs payload must be an object'
    path = _prefs_path()
    current = _read_client_prefs()
    # Shallow merge so partial POSTs do not wipe other keys.
    for key in ('provider', 'model', 'ollama_endpoint', 'prompt_history'):
        if key in payload:
            current[key] = payload[key]
    current['updated_at'] = time.time()
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as handle:
        json.dump(current, handle, ensure_ascii=True, indent=2)
        handle.write('\n')
    os.replace(tmp, path)
    return True, current

class RequestHandler(http.server.SimpleHTTPRequestHandler):
    directory = None
    flaky = False
    log_dir = None
    allow_read_root = None
    log_lock = threading.Lock()
    session_id = ''
    event_counter = 0

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=RequestHandler.directory, **kwargs)

    @classmethod
    def _next_event_id(cls):
        cls.event_counter += 1
        return f"{cls.session_id}-{cls.event_counter:06d}"

    @classmethod
    def _write_event(cls, event):
        if not cls.log_dir:
            return
        os.makedirs(cls.log_dir, exist_ok=True)
        log_path = os.path.join(cls.log_dir, 'events.jsonl')
        with cls.log_lock:
            with open(log_path, 'a', encoding='utf-8') as handle:
                handle.write(json.dumps(event, ensure_ascii=True) + '\n')

    def _read_json_body(self):
        content_length = int(self.headers.get('Content-Length', '0'))
        body = self.rfile.read(content_length) if content_length else b''
        data = _read_json_bytes(body)
        return body, data

    def _send_json(self, status, payload):
        self.send_response(status)
        self.send_header('Content-type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps(payload).encode('utf-8'))

    def _get_task_info(self):
        task_header = self.headers.get('X-Glic-Task')
        if not task_header:
            return None
        try:
            decoded = base64.b64decode(task_header)
            return json.loads(decoded.decode('utf-8'))
        except Exception:
            return None

    def do_GET(self):
        if self.path.split('?', 1)[0] == '/glic-prefs':
            self._send_json(200, _read_client_prefs())
            return
        if self.flaky:
            if time.localtime().tm_min % 2 != 0:
                self.send_response(404)
                self.send_header('Content-type', 'text/html')
                self.end_headers()
                return
        super().do_GET()

    def _read_apc_from_request(self):
        """Reads and deserializes AnnotatedPageContent from the request."""
        content_length = int(self.headers['Content-Length'])
        serialized_apc = self.rfile.read(content_length)
        from components.optimization_guide.proto.features import common_quality_data_pb2
        apc = common_quality_data_pb2.AnnotatedPageContent()
        apc.ParseFromString(serialized_apc)
        return apc

    def _parse_apc(self):
        """Deserializes AnnotatedPageContent from the request payload and
           converts it to JSON (which is sent as a response)."""
        try:
            apc = self._read_apc_from_request()
            result = json_format.MessageToJson(apc)
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.end_headers()
            self.wfile.write(result.encode())
        except DecodeError:
            self.send_error(400, 'proto could not be parsed')
        except Exception as e:
            self.send_error(500, f'proto parse failed: {e}')

    def _parse_apc_text(self):
        """Deserializes AnnotatedPageContent from the request payload and
           converts it to TEXTPROTO (which is sent as a response)."""
        try:
            apc = self._read_apc_from_request()
            result = text_format.MessageToString(apc)
            self.send_response(200)
            self.send_header('Content-type', 'text/plain')
            self.end_headers()
            self.wfile.write(result.encode())
        except DecodeError:
            self.send_error(400, 'proto could not be parsed')
        except Exception as e:
            self.send_error(500, f'proto parse failed: {e}')

    def _task_start(self):
        _, payload = self._read_json_body()
        if payload is None:
            self.send_error(400, 'invalid JSON payload')
            return
        task_id = payload.get('task_id') or payload.get('id') or self._next_event_id()
        event = {
            'event': 'task_start',
            'timestamp': time.time(),
            'task_id': task_id,
            'task': payload,
        }
        self._write_event(event)
        self._send_json(200, {'task_id': task_id})

    def _task_end(self):
        _, payload = self._read_json_body()
        if payload is None:
            self.send_error(400, 'invalid JSON payload')
            return
        task_id = payload.get('task_id') or payload.get('id')
        event = {
            'event': 'task_end',
            'timestamp': time.time(),
            'task_id': task_id,
            'result': payload,
        }
        self._write_event(event)
        self._send_json(200, {'ok': True})

    def _task_build_action(self):
        _, payload = self._read_json_body()
        if payload is None:
            self.send_error(400, 'invalid JSON payload')
            return
        action = payload.get('action', {})
        document_id = str(payload.get('document_id', '')).strip()
        action_type = str(action.get('action', '')).strip().lower()
        if not action_type:
            self.send_error(400, 'action missing')
            return
        try:
            task_id = int(payload.get('task_id', 0) or 0)
        except (TypeError, ValueError):
            task_id = 0
        try:
            tab_id = int(payload.get('tab_id', 0) or 0)
        except (TypeError, ValueError):
            tab_id = 0
        try:
            target_id = int(action.get('target_id', 0) or 0)
        except (TypeError, ValueError):
            target_id = 0
        coordinate = action.get('coordinate') if isinstance(action, dict) else None
        if not isinstance(coordinate, dict):
            coordinate = {}
        try:
            coord_x = coordinate.get('x', action.get('x'))
        except Exception:
            coord_x = None
        try:
            coord_y = coordinate.get('y', action.get('y'))
        except Exception:
            coord_y = None
        try:
            coord_x = float(coord_x) if coord_x is not None else None
            coord_y = float(coord_y) if coord_y is not None else None
        except (TypeError, ValueError):
            coord_x = None
            coord_y = None

        if not hasattr(RequestHandler, 'actions_pb2'):
            self.send_error(500, 'actions proto unavailable')
            return
        if action_type not in ('click', 'hover', 'type', 'scroll', 'scroll_to',
                               'navigate'):
            self.send_error(400, f'unsupported action: {action_type}')
            return
        if action_type != 'navigate' and not document_id:
            self.send_error(400, 'document_id missing')
            return

        actions_pb2 = RequestHandler.actions_pb2
        actions = actions_pb2.Actions()
        if task_id:
            actions.task_id = task_id
        actions.skip_async_observation_collection = True

        action_msg = actions.actions.add()
        def apply_target(target):
            if coord_x is not None and coord_y is not None:
                target.coordinate.x = int(coord_x)
                target.coordinate.y = int(coord_y)
                if hasattr(RequestHandler, 'common_quality_data_pb2'):
                    target.coordinate.pixel_type = (
                        RequestHandler.common_quality_data_pb2
                        .Coordinate.PIXEL_TYPE_DIPS)
            else:
                target.content_node_id = target_id
            target.document_identifier.serialized_token = document_id

        if action_type == 'click':
            click = action_msg.click
            apply_target(click.target)
            click.click_type = actions_pb2.ClickAction.LEFT
            click.click_count = actions_pb2.ClickAction.SINGLE
            click.tab_id = tab_id
        elif action_type == 'hover':
            move_mouse = action_msg.move_mouse
            apply_target(move_mouse.target)
            move_mouse.tab_id = tab_id
        elif action_type == 'type':
            type_action = action_msg.type
            apply_target(type_action.target)
            type_action.text = str(action.get('text', ''))
            type_action.follow_by_enter = bool(action.get('follow_by_enter', False))
            type_action.mode = actions_pb2.TypeAction.DELETE_EXISTING
            type_action.tab_id = tab_id
        elif action_type == 'scroll':
            scroll = action_msg.scroll
            apply_target(scroll.target)
            scroll.direction = actions_pb2.ScrollAction.DOWN
            scroll.distance = float(action.get('distance', 600))
            scroll.tab_id = tab_id
        elif action_type == 'scroll_to':
            scroll_to = action_msg.scroll_to
            scroll_to.target.content_node_id = target_id
            scroll_to.target.document_identifier.serialized_token = document_id
            scroll_to.tab_id = tab_id
        elif action_type == 'navigate':
            navigate = action_msg.navigate
            navigate_url = str(action.get('url', '')).strip()
            if not navigate_url:
                self.send_error(400, 'navigate url missing')
                return
            navigate.url = navigate_url
            navigate.tab_id = tab_id

        serialized = actions.SerializeToString()
        encoded = base64.b64encode(serialized).decode('ascii')
        self._send_json(200, {'base64': encoded})

    def _task_action(self):
        _, payload = self._read_json_body()
        if payload is None:
            self.send_error(400, 'invalid JSON payload')
            return
        event = {
            'event': 'action_' + str(payload.get('phase', 'unknown')),
            'timestamp': time.time(),
            'task_id': payload.get('task_id'),
            'tab_id': payload.get('tab_id'),
            'action': payload.get('action'),
        }
        passthrough_keys = (
            'document_id',
            'timeline_action',
            'fallback_engine',
            'fallback_response',
            'actor_result_code',
            'index_of_failed_action',
            'source',
            'metadata',
            'url',
            'document_url',
            'request_id',
        )
        for key in passthrough_keys:
            if key in payload:
                event[key] = payload.get(key)
        action_payload = payload.get('action')
        if isinstance(action_payload, dict):
            locators = action_payload.get('locators')
            if isinstance(locators, list):
                event['action_locator_count'] = len(locators)
        timeline_action = payload.get('timeline_action')
        if isinstance(timeline_action, dict):
            params = timeline_action.get('params')
            if isinstance(params, dict):
                timeline_locators = params.get('locators')
                if isinstance(timeline_locators, list):
                    event['timeline_locator_count'] = len(timeline_locators)
        action_result_code = None
        index_of_failed_action = None
        if payload.get('base64'):
            try:
                result_bytes = base64.b64decode(payload['base64'])
                if hasattr(RequestHandler, 'actions_pb2'):
                    result = RequestHandler.actions_pb2.ActionsResult()
                    result.ParseFromString(result_bytes)
                    action_result_code = result.action_result
                    index_of_failed_action = result.index_of_failed_action
                    event['action_result_code'] = action_result_code
                    event['index_of_failed_action'] = index_of_failed_action
                    event['actions_result'] = json.loads(
                        json_format.MessageToJson(result))
            except Exception as e:
                event['result_error'] = str(e)
        if payload.get('error'):
            event['error'] = payload.get('error')
        self._write_event(event)
        response = {'ok': True}
        if action_result_code is not None:
            response['action_result_code'] = action_result_code
        if index_of_failed_action is not None:
            response['index_of_failed_action'] = index_of_failed_action
        self._send_json(200, response)

    def do_POST(self):
        if self.path == '/parse-apc':
            self._parse_apc()
        elif self.path == '/parse-apc-text':
            self._parse_apc_text()
        elif self.path == '/task/start':
            self._task_start()
        elif self.path == '/task/end':
            self._task_end()
        elif self.path == '/task/build_action':
            self._task_build_action()
        elif self.path == '/task/action':
            self._task_action()
        elif self.path == '/task/save_script':
            self._task_save_script()
        elif self.path == '/openai-chat':
            self._proxy_openai_chat()
        elif self.path == '/glic-prefs':
            self._glic_prefs_post()
        else:
            self.send_error(404, f'invalid path: ${self.path}')


    def _task_save_script(self):
        """Persist agent task actions as a Simulachrome script JSON on disk."""
        _, payload = self._read_json_body()
        if payload is None:
            self._send_json(400, {'error': 'invalid JSON payload'})
            return
        actions = payload.get('actions')
        if not isinstance(actions, list) or not actions:
            self._send_json(400, {'error': 'actions list required'})
            return
        task_id = str(payload.get('task_id') or payload.get('id') or 'manual')
        goal = str(payload.get('goal') or payload.get('name') or 'Agent task').strip()
        name = str(payload.get('name') or '').strip() or (goal[:80] or 'Agent task')
        status = str(payload.get('status') or 'success')
        root = RequestHandler.log_dir or _HERE_PATH
        scripts_dir = os.path.join(root, 'scripts')
        os.makedirs(scripts_dir, exist_ok=True)
        safe_id = ''.join(c if c.isalnum() or c in '-_' else '_' for c in task_id)[:80]
        stamp = time.strftime('%Y%m%d_%H%M%S', time.localtime())
        path = os.path.join(scripts_dir, f'{stamp}_{safe_id}.json')
        script = {
            'name': name,
            'goal': goal,
            'task_id': task_id,
            'status': status,
            'created_at': time.time(),
            'actions': actions,
        }
        with open(path, 'w', encoding='utf-8') as handle:
            json.dump(script, handle, ensure_ascii=True, indent=2)
            handle.write('\n')
        event = {
            'event': 'script_saved',
            'timestamp': time.time(),
            'task_id': task_id,
            'path': path,
            'action_count': len(actions),
            'name': name,
            'status': status,
        }
        self._write_event(event)
        self._send_json(200, {'ok': True, 'path': path, 'action_count': len(actions)})

    def _glic_prefs_post(self):
        _, payload = self._read_json_body()
        if payload is None:
            self._send_json(400, {'error': 'invalid JSON payload'})
            return
        ok, result = _write_client_prefs(payload)
        if not ok:
            self._send_json(400, {'error': result})
            return
        self._send_json(200, {'ok': True, 'prefs': result})

    def _proxy_openai_chat(self):
        provider = _normalize_provider(self.headers.get('X-Glic-Provider'))
        default_endpoint = _default_endpoint_for_provider(provider)
        if provider == 'ollama':
            upstream_url = (
                self.headers.get('X-Glic-Ollama-Endpoint') or '').strip()
            if upstream_url and not upstream_url.endswith('/v1/chat/completions'):
                upstream_url = upstream_url.rstrip('/') + '/v1/chat/completions'
            if not upstream_url:
                upstream_url = default_endpoint
        else:
            upstream_url = (
                self.headers.get('X-Glic-Provider-Endpoint') or '').strip() or (
                    default_endpoint)

        event_id = self._next_event_id()
        task_info = self._get_task_info()
        task_id = self.headers.get('X-Glic-Task-Id')
        if not task_id and isinstance(task_info, dict):
            task_id = task_info.get('id') or task_info.get('task_id')
        try:
            content_length = int(self.headers.get('Content-Length', '0'))
            body = self.rfile.read(content_length) if content_length else b''
            body_text = body.decode('utf-8', errors='replace')

            # Sanitize payload to remove screenshots and massive DOM data
            # This is CRITICAL for local LLMs that can't handle base64 images.
            # Anthropic may attach a last-resort viewport screenshot.
            sanitized_body = sanitize_openai_payload(
                body_text, preserve_images=(provider == 'anthropic'))

            sanitized_json = None
            try:
                sanitized_json = json.loads(sanitized_body.decode('utf-8'))
            except Exception:
                sanitized_json = None

            # Hosted providers: use Chromium dashboard handler so Data > Secrets
            # API keys are applied automatically (same path as Settings/heal).
            if provider != 'ollama':
                if not isinstance(sanitized_json, dict):
                    raise RuntimeError('invalid_chat_payload')
                response_json = _chat_via_glic_host(
                    provider,
                    sanitized_json,
                    endpoint=upstream_url if upstream_url != default_endpoint else '')
                resp_body = json.dumps(response_json).encode('utf-8')
                event = {
                    'event': 'model_call',
                    'event_id': event_id,
                    'timestamp': time.time(),
                    'task_id': task_id,
                    'task': task_info,
                    'provider': provider,
                    'upstream_url': 'glicHost.dashboardAgentOllamaChat',
                    'request_size': len(body),
                    'sanitized_request_size': len(sanitized_body),
                    'request': sanitized_json,
                    'response_status': 200,
                    'response': response_json,
                }
                self._write_event(event)
                self.send_response(200)
                self.send_header('Content-type', 'application/json')
                self.end_headers()
                self.wfile.write(resp_body)
                return

            request_headers = {
                'Content-Type':
                    self.headers.get('Content-Type', 'application/json'),
                'Accept': 'application/json',
                'User-Agent': 'SimulachromeGlicProxy/1.0',
            }
            req = urllib.request.Request(
                upstream_url,
                data=sanitized_body,
                method='POST',
                headers=request_headers,
            )
            # Increased timeout to 30 minutes for slow local LLM responses
            with urllib.request.urlopen(req, timeout=1800) as resp:
                resp_body = resp.read()
                response_text = resp_body.decode('utf-8', errors='replace')
                response_json = None
                try:
                    response_json = json.loads(response_text)
                except Exception:
                    response_json = response_text

                event = {
                    'event': 'model_call',
                    'event_id': event_id,
                    'timestamp': time.time(),
                    'task_id': task_id,
                    'task': task_info,
                    'provider': provider,
                    'upstream_url': upstream_url,
                    'request_size': len(body),
                    'sanitized_request_size': len(sanitized_body),
                    'request': sanitized_json,
                    'response_status': resp.status,
                    'response': response_json,
                }
                self._write_event(event)

                self.send_response(resp.status)
                self.send_header('Content-type', 'application/json')
                self.end_headers()
                self.wfile.write(resp_body)
        except urllib.error.HTTPError as e:
            err_body = e.read() if e.fp else b''
            event = {
                'event': 'model_error',
                'event_id': event_id,
                'timestamp': time.time(),
                'task_id': task_id,
                'task': task_info,
                'provider': provider,
                'error': str(e),
                'status': e.code,
                'body': err_body.decode('utf-8', errors='replace'),
            }
            self._write_event(event)
            self.send_response(e.code)
            self.send_header('Content-type', 'text/plain')
            self.end_headers()
            self.wfile.write(err_body or str(e).encode())
        except Exception as e:
            err_text = str(e)
            key_name = _api_key_env_for_provider(provider)
            if key_name and 'missing_api_key' in err_text:
                err_text = (
                    f'missing_api_key:{key_name} '
                    f'(set it in Dashboard Data > Secrets, then retry)')
            event = {
                'event': 'model_error',
                'event_id': event_id,
                'timestamp': time.time(),
                'task_id': task_id,
                'task': task_info,
                'provider': provider,
                'error': err_text,
            }
            self._write_event(event)
            self.send_response(502)
            self.send_header('Content-type', 'text/plain')
            self.end_headers()
            self.wfile.write(err_text.encode())

    def end_headers(self):
        self.send_header("Cache-Control", "no-cache, no-store")
        self.send_header("Expires", "0")
        http.server.SimpleHTTPRequestHandler.end_headers(self)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-C',
                        '--outdir',
                        help='Build output directory',
                        required=True)
    parser.add_argument('-p',
                        '--port',
                        help='HTTP Port',
                        type=int,
                        default=8000)
    parser.add_argument('-n',
                        '--nobuild',
                        help='Skips the build step',
                        action='store_true')
    parser.add_argument('--log-dir',
                        help='Directory to write dataset logs (JSONL)',
                        default=os.environ.get('GLIC_LOG_DIR', ''))
    parser.add_argument('--flaky',
                        help="Alternates between 200 and" +
                        " 404 responses, every minute",
                        action="store_true")
    parser.add_argument('--bind-all-interfaces',
                        help='Serves on all interfaces' +
                        ' (by default serves only localhost)',
                        action='store_true')
    args = parser.parse_args()

    RequestHandler.directory = f'{args.outdir}/gen/chrome/test/data/webui/glic'
    RequestHandler.extensions_map['.js'] = 'text/javascript'
    RequestHandler.flaky = args.flaky
    RequestHandler.log_dir = args.log_dir
    RequestHandler.allow_read_root = args.log_dir
    RequestHandler.session_id = time.strftime('%Y%m%d_%H%M%S', time.localtime())

    if args.log_dir:
        os.makedirs(args.log_dir, exist_ok=True)
        print(f"Logging dataset events to: {args.log_dir}")

    if not args.nobuild:
        try:
            build(args.outdir)
            print("Test client build done.")
        except subprocess.CalledProcessError as e:
            print("Test client build error; check build output above.")
            sys.exit(1)

    if not os.path.isdir(RequestHandler.directory):
        print(f'Directory does not exist: {RequestHandler.directory}',
              file=sys.stderr)
        sys.exit(1)

    # Allows us to import generated proto bindings.
    sys.path.insert(0, os.path.join(args.outdir, 'pyproto'))
    try:
        from components.optimization_guide.proto.features import actions_data_pb2
        from components.optimization_guide.proto.features import common_quality_data_pb2
        RequestHandler.actions_pb2 = actions_data_pb2
        RequestHandler.common_quality_data_pb2 = common_quality_data_pb2
    except Exception as e:
        print(f'Actions proto unavailable: {e}', file=sys.stderr)

    server_addr = '' if args.bind_all_interfaces else '127.0.0.1'

    with socketserver.ThreadingTCPServer((server_addr, args.port),
                                         RequestHandler) as httpd:
        url_prefix = f"http://localhost:{args.port}/glic/test_client"
        print(f"Server started on port {args.port}.",
              "Use the following command line arguments to connect to it:")
        print(f'--glic-fre-url="{url_prefix}/fre.html"',
              f'--glic-guest-url="{url_prefix}/index.html"')
        print(f'--glic-guest-url="{url_prefix}/ollama.html"')
        httpd.serve_forever()


if __name__ == '__main__':
    main()
