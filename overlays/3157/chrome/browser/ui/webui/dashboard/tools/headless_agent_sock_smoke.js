#!/usr/bin/env node
/**
 * Smoke: framed JSON over headless control.sock.
 * Usage: node tools/headless_agent_sock_smoke.js [socket_path]
 * Requires a running --headless-listener Chromium with control.sock.
 */
const fs = require('fs');
const net = require('net');
const path = require('path');
const os = require('os');

function defaultSock() {
  const home = os.homedir();
  const candidates = [
    path.join(home, 'Library', 'Application Support', 'Chromium',
              'HeadlessListener', 'control.sock'),
    path.join(home, '.config', 'chromium', 'HeadlessListener', 'control.sock'),
  ];
  for (const p of candidates) {
    if (fs.existsSync(p)) return p;
  }
  return candidates[0];
}

const sockPath = process.argv[2] || defaultSock();

function writeFrame(socket, obj) {
  const body = Buffer.from(JSON.stringify(obj), 'utf8');
  const hdr = Buffer.alloc(4);
  hdr.writeUInt32LE(body.length, 0);
  socket.write(Buffer.concat([hdr, body]));
}

function readFrames(buf) {
  const msgs = [];
  let offset = 0;
  while (buf.length - offset >= 4) {
    const len = buf.readUInt32LE(offset);
    if (buf.length - offset - 4 < len) break;
    offset += 4;
    const slice = buf.subarray(offset, offset + len);
    offset += len;
    msgs.push(JSON.parse(slice.toString('utf8')));
  }
  return {msgs, rest: buf.subarray(offset)};
}

function request(socket, type, extra = {}) {
  return new Promise((resolve, reject) => {
    const request_id = `smoke_${Date.now()}_${Math.random().toString(16).slice(2)}`;
    let buf = Buffer.alloc(0);
    const onData = (chunk) => {
      buf = Buffer.concat([buf, chunk]);
      const {msgs, rest} = readFrames(buf);
      buf = rest;
      for (const msg of msgs) {
        if (msg.request_id === request_id ||
            (msg.payload && msg.request_id === request_id)) {
          socket.off('data', onData);
          resolve(msg);
          return;
        }
        if (msg.type === 'response' && msg.request_id === request_id) {
          socket.off('data', onData);
          resolve(msg);
          return;
        }
        // Some replies are {ok, request_id, ...}
        if (msg.request_id === request_id) {
          socket.off('data', onData);
          resolve(msg);
          return;
        }
      }
    };
    socket.on('data', onData);
    writeFrame(socket, {type, request_id, ...extra});
    setTimeout(() => {
      socket.off('data', onData);
      reject(new Error(`timeout waiting for ${type}`));
    }, 45000);
  });
}

async function main() {
  if (!fs.existsSync(sockPath)) {
    console.error('SKIP: control.sock not found at', sockPath);
    process.exit(0);
  }
  const socket = net.createConnection(sockPath);
  await new Promise((resolve, reject) => {
    socket.once('connect', resolve);
    socket.once('error', reject);
  });
  console.log('connected', sockPath);

  const warm = await request(socket, 'warm');
  console.log('warm', JSON.stringify(warm).slice(0, 200));

  const exec = await request(socket, 'execute_actions', {
    actions: [{action: 'wait', params: {waitType: 'time', time: 50}}],
    return_context: true,
  });
  console.log('execute_actions ok=', exec.ok, 'target=',
              (exec.payload && exec.payload.target_id) || exec.target_id);

  const tid = (exec.payload && exec.payload.target_id) || exec.target_id;
  const look = await request(socket, 'investigate_page', {
    target_id: tid,
    close_target: false,
  });
  console.log('investigate reuse ok=', look.ok,
              'url=', (look.payload && look.payload.url) || look.url);

  socket.end();
  console.log('SMOKE_OK');
}

main().catch((e) => {
  console.error('SMOKE_FAIL', e);
  process.exit(1);
});
