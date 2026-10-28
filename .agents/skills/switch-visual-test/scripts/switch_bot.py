#!/usr/bin/env python3
"""Nintendo Switch sys-botbase automation client with file transfer and UI navigation.

No third-party dependencies required.
Supports:
  - probe (firmware/module version, title ID)
  - screenshot (JPEG screen capture via pixelPeek)
  - click / touch / detach (virtual controller automation)
  - upload / download / file-list / file-size / file-delete (SD card file transfer)
  - kill-game (terminate active title)
  - launch-pipensx (launch pipensx from HOME screen or verify running state)
"""
import argparse
import os
from pathlib import Path
import socket
import sys
import time

DEFAULT_HOST = os.environ.get('SWITCH_HOST', '192.168.1.143')
DEFAULT_PORT = int(os.environ.get('SWITCH_PORT', 6000))
DEFAULT_NRO_LOCAL = 'build-switch/pipensx.nro'
DEFAULT_NRO_REMOTE = 'sdmc:/switch/pipensx/pipensx.nro'
PIPENSX_TITLE_ID = '03DB1F658AB5E000'
CHUNK_SIZE = 64 * 1024  # 64 KB matches sys-botbase internal buffer

BUTTONS = 'A B X Y L R ZL ZR PLUS MINUS LSTICK RSTICK DLEFT DUP DRIGHT DDOWN HOME CAPTURE'.split()


def line(stream, limit=2_100_000):
    data = stream.readline(limit + 1)
    if not data.endswith(b'\n') or len(data) > limit:
        raise RuntimeError('Incomplete or oversized reply')
    return data.strip()


def send_input_command(sock, stream, cmd):
    sock.sendall(cmd.encode('ascii') + b'\r\n')
    # Input commands have no reply. A query on the same connection
    # acts as a completion barrier, so closing cannot discard queued input.
    sock.sendall(b'getVersion\r\n')
    res = line(stream).decode('ascii')
    time.sleep(0.3)
    return res


def action_probe(sock, stream):
    for command in ('getVersion', 'getTitleID'):
        sock.sendall(command.encode('ascii') + b'\r\n')
        print(command + ': ' + line(stream).decode('ascii'))


def action_screenshot(sock, stream, output_path):
    sock.sendall(b'pixelPeek\r\n')
    raw = line(stream)
    if not raw:
        raise RuntimeError('Empty capture: check Atmosphere debug capture support')
    data = bytes.fromhex(raw.decode('ascii'))
    if not data.startswith(b'\xff\xd8') or not data.endswith(b'\xff\xd9'):
        raise RuntimeError('Capture is not a complete JPEG')
    Path(output_path).write_bytes(data)
    print(f'{output_path}: {len(data)} bytes')


def action_click(sock, stream, button):
    send_input_command(sock, stream, f'click {button}')
    print(f'click {button}: ok')


def action_touch(sock, stream, x, y):
    send_input_command(sock, stream, f'touch {x} {y}')
    print(f'touch {x} {y}: ok')


def action_detach(sock, stream):
    send_input_command(sock, stream, 'detachController')
    print('detachController: ok')


def action_kill_game(sock, stream):
    sock.sendall(b'killGame\r\n')
    resp = line(stream).decode('ascii')
    print(f'killGame: {resp}')


def action_file_size(sock, stream, remote_path):
    sock.sendall(f'fileSize {remote_path}\r\n'.encode('ascii'))
    resp = line(stream).decode('ascii')
    print(f'fileSize {remote_path}: {resp}')
    return resp


def action_file_delete(sock, stream, remote_path):
    sock.sendall(f'fileDelete {remote_path}\r\n'.encode('ascii'))
    resp = line(stream).decode('ascii')
    print(f'fileDelete {remote_path}: {resp}')
    return resp


def action_file_list(sock, stream, remote_path):
    sock.sendall(f'fileList {remote_path}\r\n'.encode('ascii'))
    # fileList responds with directory entries then an empty line or EOF
    entries = []
    while True:
        try:
            raw = stream.readline(4096)
            if not raw or raw == b'\n' or raw == b'\r\n':
                break
            entry = raw.decode('ascii', errors='replace').strip()
            if entry:
                entries.append(entry)
        except (socket.timeout, OSError):
            break
    print(f'Directory listing of {remote_path}:')
    for e in entries:
        print('  ' + e)


def action_upload(host, port, local_path, remote_path):
    if not os.path.isfile(local_path):
        raise FileNotFoundError(f'Local file not found: {local_path}')
    size = os.path.getsize(local_path)
    upload_temp = remote_path + '.upload'
    print(f'Uploading {local_path} ({size} bytes) -> {remote_path} via {upload_temp}...')
    start_time = time.time()

    # Step 1: Upload to temp file
    with socket.create_connection((host, port), timeout=15) as sock:
        sock.settimeout(60)
        sock.sendall(f'fileWrite {upload_temp} {size}\r\n'.encode('ascii'))
        sent = 0
        last_report = start_time
        with open(local_path, 'rb') as f:
            while sent < size:
                chunk = f.read(CHUNK_SIZE)
                if not chunk:
                    break
                sock.sendall(chunk)
                sent += len(chunk)
                now = time.time()
                if now - last_report >= 2.0 or sent == size:
                    pct = (sent / size) * 100
                    speed = (sent / (now - start_time)) / (1024 * 1024)
                    print(f'  {sent}/{size} bytes ({pct:.1f}%) - {speed:.2f} MB/s', flush=True)
                    last_report = now

        with sock.makefile('rb') as stream:
            resp = line(stream).decode('ascii')
            if not resp.startswith('ok'):
                raise RuntimeError(f'fileWrite failed: {resp}')

    # Step 2: Verify size, delete target, rename temp
    with socket.create_connection((host, port), timeout=15) as sock:
        sock.settimeout(10)
        with sock.makefile('rb') as stream:
            sock.sendall(f'fileSize {upload_temp}\r\n'.encode('ascii'))
            resp = line(stream).decode('ascii')
            if resp != str(size):
                raise RuntimeError(f'Uploaded size mismatch: expected {size}, got {resp}')

            # Remove old target
            sock.sendall(f'fileDelete {remote_path}\r\n'.encode('ascii'))
            line(stream)

            # Rename temp to target
            sock.sendall(f'fileRename {upload_temp} {remote_path}\r\n'.encode('ascii'))
            ren_resp = line(stream).decode('ascii')
            if not ren_resp.startswith('ok'):
                raise RuntimeError(f'fileRename failed: {ren_resp}')

            # Verify final target size
            sock.sendall(f'fileSize {remote_path}\r\n'.encode('ascii'))
            final_size = line(stream).decode('ascii')
            if final_size != str(size):
                raise RuntimeError(f'Final size mismatch: expected {size}, got {final_size}')

    total_time = time.time() - start_time
    avg_speed = (size / total_time) / (1024 * 1024)
    print(f'Upload complete in {total_time:.2f}s ({avg_speed:.2f} MB/s)!')


def action_download(host, port, remote_path, local_path):
    print(f'Downloading {remote_path} -> {local_path}...')
    with socket.create_connection((host, port), timeout=15) as sock:
        sock.settimeout(60)
        with sock.makefile('rb') as stream:
            sock.sendall(f'fileRead {remote_path}\r\n'.encode('ascii'))
            hdr = line(stream).decode('ascii')
            if not hdr.isdigit():
                raise RuntimeError(f'fileRead error: {hdr}')
            size = int(hdr)
            received = 0
            with open(local_path, 'wb') as f:
                while received < size:
                    to_read = min(CHUNK_SIZE, size - received)
                    buf = stream.read(to_read)
                    if not buf:
                        raise RuntimeError(f'Unexpected EOF after {received}/{size} bytes')
                    f.write(buf)
                    received += len(buf)
    print(f'Download complete: {local_path} ({received} bytes)')


def action_launch_pipensx(host, port):
    print('Checking Switch state...')
    with socket.create_connection((host, port), timeout=10) as sock:
        sock.settimeout(15)
        with sock.makefile('rb') as stream:
            sock.sendall(b'getTitleID\r\n')
            title_id = line(stream).decode('ascii').upper()
            if title_id == PIPENSX_TITLE_ID:
                print(f'pipensx is already running (TitleID: {title_id})')
                return

            if title_id:
                print(f'Another title is running ({title_id}), sending killGame...')
                sock.sendall(b'killGame\r\n')
                line(stream)
                time.sleep(2.0)

            # Bring to HOME screen
            send_input_command(sock, stream, 'click HOME')
            time.sleep(1.0)

            # Move focus to leftmost tile (Super Mario Maker 2)
            for _ in range(5):
                send_input_command(sock, stream, 'click DLEFT')
                time.sleep(0.1)

            # pipensx forwarder is tile #3 (3 clicks DRIGHT from tile 0)
            for _ in range(3):
                send_input_command(sock, stream, 'click DRIGHT')
                time.sleep(0.2)

            # Launch
            print('Sending launch click A...')
            send_input_command(sock, stream, 'click A')
            send_input_command(sock, stream, 'detachController')

    # Wait for app boot
    print('Waiting for pipensx to initialize...')
    for _ in range(10):
        time.sleep(1.0)
        with socket.create_connection((host, port), timeout=5) as sock:
            with sock.makefile('rb') as stream:
                sock.sendall(b'getTitleID\r\n')
                tid = line(stream).decode('ascii').upper()
                if tid:
                    print(f'pipensx launched successfully! TitleID: {tid}')
                    return

    print('Warning: Title ID not detected after 10s. Inspect screenshot.')


def coordinate(value):
    number = int(value)
    if not 0 <= number < 1280:
        raise argparse.ArgumentTypeError('Coordinate out of range')
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default=DEFAULT_HOST, help=f'Switch IP address (default: {DEFAULT_HOST})')
    parser.add_argument('--port', type=int, default=DEFAULT_PORT, help=f'sys-botbase port (default: {DEFAULT_PORT})')
    sub = parser.add_subparsers(dest='action', required=True)

    # Core bot commands
    sub.add_parser('probe', help='Query sys-botbase version and active title ID')
    sc = sub.add_parser('screenshot', help='Capture JPEG screenshot via pixelPeek')
    sc.add_argument('output', help='Output JPEG file path')
    cl = sub.add_parser('click', help='Click controller button')
    cl.add_argument('button', choices=BUTTONS)
    tc = sub.add_parser('touch', help='Touch screen coordinates (1280x720)')
    tc.add_argument('x', type=coordinate)
    tc.add_argument('y', type=coordinate)
    sub.add_parser('detach', help='Detach virtual controller')

    # App lifecycle
    sub.add_parser('kill-game', help='Terminate running game/title')
    sub.add_parser('launch-pipensx', help='Navigate HOME menu, launch pipensx forwarder and detach')

    # SD Card File commands
    up = sub.add_parser('upload', help='Upload local file to SD card via sys-botbase')
    up.add_argument('local', nargs='?', default=DEFAULT_NRO_LOCAL, help=f'Local file path (default: {DEFAULT_NRO_LOCAL})')
    up.add_argument('remote', nargs='?', default=DEFAULT_NRO_REMOTE, help=f'Remote SD path (default: {DEFAULT_NRO_REMOTE})')

    dl = sub.add_parser('download', help='Download file from SD card via sys-botbase')
    dl.add_argument('remote', help='Remote SD path')
    dl.add_argument('local', help='Local file path')

    fl = sub.add_parser('file-list', help='List directory contents on SD card')
    fl.add_argument('remote', nargs='?', default='sdmc:/switch/pipensx', help='Remote SD directory (default: sdmc:/switch/pipensx)')

    fs = sub.add_parser('file-size', help='Query file size on SD card')
    fs.add_argument('remote', help='Remote SD file path')

    fd = sub.add_parser('file-delete', help='Delete file on SD card')
    fd.add_argument('remote', help='Remote SD file path')

    args = parser.parse_args()

    if args.action == 'touch' and args.y >= 720:
        parser.error('Y must be below 720')

    try:
        # Multi-connection or custom streaming actions
        if args.action == 'upload':
            action_upload(args.host, args.port, args.local, args.remote)
        elif args.action == 'download':
            action_download(args.host, args.port, args.remote, args.local)
        elif args.action == 'launch-pipensx':
            action_launch_pipensx(args.host, args.port)
        else:
            # Single-connection commands
            with socket.create_connection((args.host, args.port), timeout=10) as sock:
                sock.settimeout(20)
                with sock.makefile('rb') as stream:
                    if args.action == 'probe':
                        action_probe(sock, stream)
                    elif args.action == 'screenshot':
                        action_screenshot(sock, stream, args.output)
                    elif args.action == 'click':
                        action_click(sock, stream, args.button)
                    elif args.action == 'touch':
                        action_touch(sock, stream, args.x, args.y)
                    elif args.action == 'detach':
                        action_detach(sock, stream)
                    elif args.action == 'kill-game':
                        action_kill_game(sock, stream)
                    elif args.action == 'file-list':
                        action_file_list(sock, stream, args.remote)
                    elif args.action == 'file-size':
                        action_file_size(sock, stream, args.remote)
                    elif args.action == 'file-delete':
                        action_file_delete(sock, stream, args.remote)
    except (OSError, ValueError, RuntimeError) as exc:
        parser.exit(1, f'{exc}\n')


if __name__ == '__main__':
    main()
