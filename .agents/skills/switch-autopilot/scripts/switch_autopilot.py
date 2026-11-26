#!/usr/bin/env python3
"""Nintendo Switch sys-autopilot-plus HTTP REST client.

Zero third-party dependencies (Python 3 standard library only).
Default target: http://192.168.1.143:4150 (override via SWITCH_HOST,
SWITCH_PORT, SWITCH_TOKEN, or --host / --port / --token).
"""
import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

DEFAULT_HOST = os.environ.get('SWITCH_HOST', '192.168.1.143')
DEFAULT_PORT = int(os.environ.get('SWITCH_PORT', '4150'))
DEFAULT_TOKEN = os.environ.get('SWITCH_TOKEN', '')
DEFAULT_NRO_LOCAL = 'build-switch/pipensx.nro'
DEFAULT_NRO_REMOTE = '/switch/pipensx/pipensx.nro'
PIPENSX_TITLE_ID = '03DB1F658AB5E000'

BUTTONS = (
    'A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT '
    'DUP DDOWN DLEFT DRIGHT START SELECT LSTICK RSTICK HOME CAPTURE'
).split()


def normalize_sd_path(path: str) -> str:
    if path.startswith('sdmc:'):
        path = path[len('sdmc:'):]
    if not path.startswith('/'):
        path = '/' + path
    return path


class AutopilotClient:
    def __init__(self, host: str, port: int, token: str = '', timeout: float = 30.0):
        self.host = host
        self.port = port
        self.token = token
        self.timeout = timeout
        self.base_url = f'http://{host}:{port}'

    def _headers(self, extra=None):
        headers = {}
        if self.token:
            headers['Authorization'] = f'Bearer {self.token}'
        if extra:
            headers.update(extra)
        return headers

    def request_raw(self, method: str, path: str, body: bytes = None,
                    content_type: str = None, timeout: float = None):
        headers = self._headers()
        if content_type:
            headers['Content-Type'] = content_type
        req = urllib.request.Request(
            self.base_url + path,
            data=body,
            headers=headers,
            method=method,
        )
        try:
            with urllib.request.urlopen(req, timeout=timeout or self.timeout) as resp:
                return resp.status, resp.read(), resp.headers
        except urllib.error.HTTPError as exc:
            err_body = exc.read().decode('utf-8', errors='replace')
            raise RuntimeError(f'HTTP {exc.code} {method} {path}: {err_body}') from None
        except urllib.error.URLError as exc:
            raise RuntimeError(f'Connection failed ({self.base_url}{path}): {exc.reason}') from None

    def request_json(self, method: str, path: str, payload=None, timeout: float = None):
        body = None
        ctype = None
        if payload is not None:
            body = json.dumps(payload).encode('utf-8')
            ctype = 'application/json'
        _, raw, _ = self.request_raw(method, path, body=body, content_type=ctype, timeout=timeout)
        if not raw:
            return {}
        try:
            return json.loads(raw.decode('utf-8'))
        except json.JSONDecodeError:
            return {'raw': raw.decode('utf-8', errors='replace')}

    def stream_upload(self, method: str, path: str, local_path: str, timeout: float = 600.0):
        file_path = Path(local_path)
        if not file_path.is_file():
            raise FileNotFoundError(f'Local file not found: {local_path}')
        size = file_path.stat().st_size
        sha256 = hashlib.sha256()
        conn = http.client.HTTPConnection(self.host, self.port, timeout=timeout)
        try:
            conn.putrequest(method, path)
            conn.putheader('Content-Length', str(size))
            conn.putheader('Content-Type', 'application/octet-stream')
            if self.token:
                conn.putheader('Authorization', f'Bearer {self.token}')
            conn.endheaders()

            sent = 0
            start = time.monotonic()
            last_report = start
            with file_path.open('rb') as f:
                while True:
                    chunk = f.read(64 * 1024)
                    if not chunk:
                        break
                    sha256.update(chunk)
                    conn.send(chunk)
                    sent += len(chunk)
                    now = time.monotonic()
                    if now - last_report >= 2.0 or sent == size:
                        elapsed = max(now - start, 0.001)
                        pct = (sent / size * 100.0) if size else 100.0
                        speed = (sent / elapsed) / (1024 * 1024)
                        print(f'  {sent}/{size} bytes ({pct:.1f}%) - {speed:.2f} MB/s', flush=True)
                        last_report = now

            resp = conn.getresponse()
            raw = resp.read()
            if resp.status >= 400:
                raise RuntimeError(
                    f'HTTP {resp.status} {method} {path}: {raw.decode("utf-8", errors="replace")}'
                )
            elapsed = max(time.monotonic() - start, 0.001)
            data = json.loads(raw.decode('utf-8')) if raw else {}
            return data, sha256.hexdigest(), size, elapsed
        finally:
            conn.close()


def print_json(data):
    print(json.dumps(data, indent=2, ensure_ascii=False))


def cmd_probe(client: AutopilotClient, _args):
    status = client.request_json('GET', '/status')
    try:
        proc = client.request_json('GET', '/process')
    except RuntimeError:
        proc = None
    out = {'status': status}
    if proc is not None:
        out['process'] = proc
    print_json(out)


def cmd_screenshot(client: AutopilotClient, args):
    query = f'?stack={urllib.parse.quote(args.stack)}' if args.stack else ''
    _, data, _ = client.request_raw('GET', f'/screenshot{query}')
    if not data.startswith(b'\xff\xd8'):
        raise RuntimeError('Response is not a JPEG image')
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    print(f'{out}: {len(data)} bytes')


def cmd_tap(client: AutopilotClient, args):
    payload = {'buttons': [b.upper() for b in args.buttons]}
    if args.duration_ms is not None:
        payload['durationMs'] = args.duration_ms
    print_json(client.request_json('POST', '/input/tap', payload))


def cmd_touch(client: AutopilotClient, args):
    payload = {'x': args.x, 'y': args.y}
    if args.x2 is not None and args.y2 is not None:
        payload['x2'] = args.x2
        payload['y2'] = args.y2
    if args.duration_ms is not None:
        payload['durationMs'] = args.duration_ms
    print_json(client.request_json('POST', '/input/touch', payload))


def cmd_stick(client: AutopilotClient, args):
    payload = {'side': args.side, 'x': args.x, 'y': args.y}
    if args.duration_ms is not None:
        payload['durationMs'] = args.duration_ms
    print_json(client.request_json('POST', '/input/stick', payload))


def cmd_upload(client: AutopilotClient, args):
    remote = normalize_sd_path(args.remote)
    print(f'Uploading {args.local} -> sdmc:{remote}...')
    query = urllib.parse.urlencode({'path': remote})
    data, local_sha, size, elapsed = client.stream_upload('PUT', f'/files?{query}', args.local)
    speed = (size / elapsed) / (1024 * 1024)
    print(f'Uploaded {size} bytes in {elapsed:.2f}s ({speed:.2f} MB/s)')
    if not args.no_verify:
        hash_resp = client.request_json('GET', f'/files/hash?{query}')
        remote_sha = (hash_resp.get('sha256') or '').lower()
        if remote_sha and remote_sha != local_sha.lower():
            raise RuntimeError(f'SHA-256 mismatch: local={local_sha}, remote={remote_sha}')
        print(f'SHA-256 verified: {local_sha}')
    if data:
        print_json(data)


def cmd_download(client: AutopilotClient, args):
    remote = normalize_sd_path(args.remote)
    query = urllib.parse.urlencode({'path': remote})
    _, data, _ = client.request_raw('GET', f'/files?{query}', timeout=120.0)
    out = Path(args.local)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    print(f'{out}: {len(data)} bytes')


def cmd_tail(client: AutopilotClient, args):
    remote = normalize_sd_path(args.remote)
    query = urllib.parse.urlencode({'path': remote, 'offset': -abs(args.bytes)})
    _, data, _ = client.request_raw('GET', f'/files?{query}')
    sys.stdout.write(data.decode('utf-8', errors='replace'))


def cmd_ls(client: AutopilotClient, args):
    remote = normalize_sd_path(args.remote)
    if not remote.endswith('/'):
        remote += '/'
    query = urllib.parse.urlencode({'path': remote})
    print_json(client.request_json('GET', f'/files?{query}'))


def cmd_rm(client: AutopilotClient, args):
    remote = normalize_sd_path(args.remote)
    query = urllib.parse.urlencode({'path': remote})
    print_json(client.request_json('DELETE', f'/files?{query}'))


def cmd_hash(client: AutopilotClient, args):
    remote = normalize_sd_path(args.remote)
    query = urllib.parse.urlencode({'path': remote})
    print_json(client.request_json('GET', f'/files/hash?{query}'))


def cmd_launch_title(client: AutopilotClient, args):
    print_json(client.request_json('POST', '/process/launch_title', {'titleId': args.title_id}))


def cmd_launch_nro(client: AutopilotClient, args):
    remote = normalize_sd_path(args.path)
    print_json(client.request_json('POST', '/process/launch_nro', {'path': remote}))


def cmd_launch_pipensx(client: AutopilotClient, _args):
    proc = client.request_json('GET', '/process')
    running_tid = (proc.get('titleId') or '').upper()
    if proc.get('running') and running_tid == PIPENSX_TITLE_ID:
        print(f'pipensx is already running (TitleID: {running_tid})')
        return
    if proc.get('running') and running_tid:
        print(f'Terminating active title {running_tid}...')
        client.request_json('POST', '/process/terminate', {})
        time.sleep(1.5)
    print(f'Launching pipensx ({PIPENSX_TITLE_ID})...')
    resp = client.request_json('POST', '/process/launch_title', {'titleId': PIPENSX_TITLE_ID})
    print_json(resp)


def cmd_install(client: AutopilotClient, args):
    query = urllib.parse.urlencode({'storage': args.storage})
    print(f'Streaming install {args.file} -> {args.storage}...')
    data, _, size, elapsed = client.stream_upload('PUT', f'/install?{query}', args.file, timeout=1800.0)
    speed = (size / elapsed) / (1024 * 1024)
    print(f'Install finished in {elapsed:.2f}s ({speed:.2f} MB/s)')
    print_json(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default=DEFAULT_HOST, help=f'Switch host/IP (default: {DEFAULT_HOST})')
    parser.add_argument('--port', type=int, default=DEFAULT_PORT, help=f'sys-autopilot-plus port (default: {DEFAULT_PORT})')
    parser.add_argument('--token', default=DEFAULT_TOKEN, help='Optional Bearer token (or SWITCH_TOKEN env)')
    sub = parser.add_subparsers(dest='action', required=True)

    sub.add_parser('probe', aliases=['status'], help='Query server status and foreground process')

    sc = sub.add_parser('screenshot', help='Save JPEG screenshot to file')
    sc.add_argument('output', help='Output JPEG path')
    sc.add_argument('--stack', choices=('screenshot', 'default', 'lcd', 'recording', 'lastframe'))

    tp = sub.add_parser('tap', aliases=['click'], help='Tap one or more buttons')
    tp.add_argument('buttons', nargs='+', help=f'Button names: {", ".join(BUTTONS)}')
    tp.add_argument('--duration-ms', type=int)

    tc = sub.add_parser('touch', help='Tap or swipe touchscreen (1280x720)')
    tc.add_argument('x', type=int)
    tc.add_argument('y', type=int)
    tc.add_argument('--x2', type=int)
    tc.add_argument('--y2', type=int)
    tc.add_argument('--duration-ms', type=int)

    st = sub.add_parser('stick', help='Move analog stick (-1.0..1.0)')
    st.add_argument('side', choices=('left', 'right'))
    st.add_argument('x', type=float)
    st.add_argument('y', type=float)
    st.add_argument('--duration-ms', type=int)

    sub.add_parser('handshake', help='Pulse L+R to pair virtual Pro Controller')
    sub.add_parser('detach', help='Detach virtual controller')
    sub.add_parser('clear-input', help='Release all buttons and recenter sticks')

    up = sub.add_parser('upload', help='Upload local file to SD card with SHA-256 verification')
    up.add_argument('local', nargs='?', default=DEFAULT_NRO_LOCAL)
    up.add_argument('remote', nargs='?', default=DEFAULT_NRO_REMOTE)
    up.add_argument('--no-verify', action='store_true', help='Skip post-upload SHA-256 check')

    dl = sub.add_parser('download', help='Download file from SD card')
    dl.add_argument('remote')
    dl.add_argument('local')

    tl = sub.add_parser('tail', help='Print last N bytes of a remote text file')
    tl.add_argument('remote')
    tl.add_argument('bytes', nargs='?', type=int, default=4096)

    ls = sub.add_parser('ls', aliases=['file-list'], help='List SD card directory')
    ls.add_argument('remote', nargs='?', default='/switch/pipensx')

    rm = sub.add_parser('rm', aliases=['file-delete'], help='Delete SD card file or empty dir')
    rm.add_argument('remote')

    hs = sub.add_parser('hash', help='SHA-256 digest of a file on SD card')
    hs.add_argument('remote')

    lt = sub.add_parser('launch-title', help='Launch installed title by Title ID')
    lt.add_argument('title_id')

    ln = sub.add_parser('launch-nro', help='Launch homebrew NRO by SD path')
    ln.add_argument('path')

    sub.add_parser('launch-pipensx', help='Launch pipensx forwarder (03DB1F658AB5E000)')
    sub.add_parser('terminate', aliases=['kill-game'], help='Terminate active foreground title')

    inst = sub.add_parser('install', help='Stream-install an NSP or XCI')
    inst.add_argument('file')
    inst.add_argument('--storage', choices=('sd', 'nand'), default='sd')

    sub.add_parser('titles', help='List installed titles')
    sub.add_parser('telemetry', help='Read clocks, temps, battery, and process telemetry')
    sub.add_parser('crash', help='Read latest Atmosphere crash report')
    sub.add_parser('restart', help='Reboot console into CFW')
    sub.add_parser('sleep', help='Put console to sleep')

    args = parser.parse_args()
    client = AutopilotClient(args.host, args.port, args.token)

    try:
        if args.action in ('probe', 'status'):
            cmd_probe(client, args)
        elif args.action == 'screenshot':
            cmd_screenshot(client, args)
        elif args.action in ('tap', 'click'):
            cmd_tap(client, args)
        elif args.action == 'touch':
            cmd_touch(client, args)
        elif args.action == 'stick':
            cmd_stick(client, args)
        elif args.action == 'handshake':
            print_json(client.request_json('POST', '/controller/handshake', {}))
        elif args.action == 'detach':
            print_json(client.request_json('POST', '/controller/detach', {}))
        elif args.action == 'clear-input':
            print_json(client.request_json('POST', '/input/clear', {}))
        elif args.action == 'upload':
            cmd_upload(client, args)
        elif args.action == 'download':
            cmd_download(client, args)
        elif args.action == 'tail':
            cmd_tail(client, args)
        elif args.action in ('ls', 'file-list'):
            cmd_ls(client, args)
        elif args.action in ('rm', 'file-delete'):
            cmd_rm(client, args)
        elif args.action == 'hash':
            cmd_hash(client, args)
        elif args.action == 'launch-title':
            cmd_launch_title(client, args)
        elif args.action == 'launch-nro':
            cmd_launch_nro(client, args)
        elif args.action == 'launch-pipensx':
            cmd_launch_pipensx(client, args)
        elif args.action in ('terminate', 'kill-game'):
            print_json(client.request_json('POST', '/process/terminate', {}))
        elif args.action == 'install':
            cmd_install(client, args)
        elif args.action == 'titles':
            print_json(client.request_json('GET', '/titles'))
        elif args.action == 'telemetry':
            print_json(client.request_json('GET', '/telemetry'))
        elif args.action == 'crash':
            print_json(client.request_json('GET', '/diagnostics/crash'))
        elif args.action == 'restart':
            print_json(client.request_json('POST', '/power/restart', {}))
        elif args.action == 'sleep':
            print_json(client.request_json('POST', '/power/sleep', {}))
    except (OSError, ValueError, RuntimeError) as exc:
        parser.exit(1, f'{exc}\n')


if __name__ == '__main__':
    main()
