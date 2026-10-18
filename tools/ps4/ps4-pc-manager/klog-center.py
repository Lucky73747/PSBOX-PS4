#!/usr/bin/env python3
"""One GoldHEN klog connection, shared by concurrent local capture clients."""
from __future__ import annotations

import argparse
import datetime as dt
import fcntl
import os
from pathlib import Path
import re
import signal
import socket
import sys
import time
import uuid

ROOT = Path.home() / ".local" / "share" / "psbox-pc-manager" / "klogs"
ARCHIVE = ROOT / "archive"
CAPTURES = ROOT / "captures"
DEFAULT_HOST = ""
DEFAULT_PORT = 3232
ROTATE_BYTES = 64 * 1024 * 1024
RETENTION_DAYS = 30
LOCK_PATH = ROOT / ".klog-center.lock"
_URL_RE = re.compile(rb"(?i)https?://[^\s\"']+")
_IP_RE = re.compile(rb"\b(?:\d{1,3}\.){3}\d{1,3}\b")
_SECRET_RE = re.compile(rb"(?i)[\"']?(?:access[_ -]?token|refresh[_ -]?token|token|password|secret|authorization|cookie|xuid|gamertag|email|username|auth[_ -]?code|device[_ -]?code|user[_ -]?code)[\"']?\s*[=:]\s*[\"']?(?:bearer\s+)?[^\s,;\"']+")
_EMAIL_RE = re.compile(rb"[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}", re.I)
_IPV6_RE = re.compile(rb"(?<![\w:])(?:[0-9a-f]{1,4}:){2,7}[0-9a-f]{0,4}(?![\w:])", re.I)


def redact(data: bytes) -> bytes:
    lines = []
    for line in data.splitlines():
        line = _URL_RE.sub(b"[URL]", line)
        line = _IP_RE.sub(b"[IP]", line)
        line = _IPV6_RE.sub(b"[IP]", line)
        line = _EMAIL_RE.sub(b"[EMAIL]", line)
        line = _SECRET_RE.sub(b"[REDACTED]", line)
        if line.lstrip().lower().startswith((b"v=0", b"o=", b"s=", b"t=", b"m=", b"a=")) or b"candidate:" in line.lower():
            line = b"[REDACTED SDP/ICE line]"
        lines.append(line)
    return b"\n".join(lines) + (b"\n" if data.endswith(b"\n") else b"")


def stamp() -> str:
    return dt.datetime.now().astimezone().strftime("%Y%m%d-%H%M%S%z")


def ensure_dirs() -> None:
    ARCHIVE.mkdir(parents=True, exist_ok=True)
    CAPTURES.mkdir(parents=True, exist_ok=True)


def prune_archive() -> None:
    cutoff = time.time() - RETENTION_DAYS * 86400
    for p in ARCHIVE.glob("ps4-klog-raw-*.log"):
        try:
            if p.stat().st_mtime < cutoff:
                p.unlink()
        except FileNotFoundError:
            pass


def daemon(args: argparse.Namespace) -> int:
    if not args.host:
        raise ValueError("Informe o IP do PS4 com --host ou defina PS4_HOST.")
    ensure_dirs()
    lock = open(LOCK_PATH, "a+")
    try:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print("klog-center: daemon já está ativo; use 'status'.", file=sys.stderr)
        return 2
    lock.seek(0)
    lock.truncate()
    lock.write(f"pid={os.getpid()} host=[configured] port={args.port}\n")
    lock.flush()

    stopping = False

    def stop(_sig, _frame):
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    print(f"klog-center: conectando uma única sessão ao host configurado na porta {args.port}", flush=True)
    part = 0
    while not stopping:
        sock = None
        try:
            sock = socket.create_connection((args.host, args.port), timeout=8)
            sock.settimeout(1.0)
            part += 1
            path = ARCHIVE / f"ps4-klog-raw-{stamp()}-p{part:03d}.log"
            print(f"klog-center: conectado; arquivo {path}", flush=True)
            with path.open("ab", buffering=0) as out:
                out.write((f"\n[KLOG-CENTER connected={dt.datetime.now().astimezone().isoformat()} "
                           f"host=[configured]]\n").encode())
                size = out.tell()
                pending = bytearray()
                last_prune = time.monotonic()
                while not stopping:
                    try:
                        data = sock.recv(65536)
                    except socket.timeout:
                        data = None
                    if data == b"":
                        raise ConnectionError("servidor fechou a conexão")
                    if data:
                        pending.extend(data)
                        complete = bytearray()
                        while b"\n" in pending:
                            line, _, rest = pending.partition(b"\n")
                            complete.extend(redact(line + b"\n"))
                            pending[:] = rest
                        if size and size + len(complete) > ROTATE_BYTES:
                            out.write(b"\n[KLOG-CENTER rotate]\n")
                            break
                        if complete:
                            out.write(complete)
                            size += len(complete)
                    if time.monotonic() - last_prune > 3600:
                        prune_archive()
                        last_prune = time.monotonic()
                if pending:
                    out.write(redact(bytes(pending) + b"\n"))
                out.write((f"\n[KLOG-CENTER disconnected={dt.datetime.now().astimezone().isoformat()}]\n").encode())
        except (OSError, ConnectionError) as exc:
            if not stopping:
                print(f"klog-center: conexão interrompida ({exc}); nova tentativa em {args.retry}s", flush=True)
                time.sleep(args.retry)
        finally:
            if sock is not None:
                sock.close()
    print("klog-center: encerrado", flush=True)
    return 0


def status(_args: argparse.Namespace) -> int:
    ensure_dirs()
    lock = open(LOCK_PATH, "a+")
    try:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        lock.seek(0)
        print("daemon=running " + lock.read().strip())
        return 0
    else:
        print("daemon=stopped")
        return 1


def choose_metadata(args: argparse.Namespace) -> tuple[str, str, str]:
    kind = args.kind
    name = args.name
    build = args.build or "unspecified"
    if not kind:
        print("Qual log deseja capturar?\n  1) jogo/dependência\n  2) app\n  3) personalizado")
        choice = input("Escolha [1-3]: ").strip()
        kind = {"1": "game-dependency", "2": "app", "3": "custom"}.get(choice)
        if not kind:
            raise ValueError("opção inválida")
    if not name:
        prompts = {
            "game-dependency": "Jogo/dependência: ",
            "app": "Nome ou Title ID do app: ",
            "custom": "Nome desta captura: ",
        }
        name = input(prompts[kind]).strip()
    if not name:
        raise ValueError("o nome não pode ficar vazio")
    if args.build is None:
        build = input("Build/versão (Enter = unspecified): ").strip() or "unspecified"
    return kind, name, build


def capture(args: argparse.Namespace) -> int:
    ensure_dirs()
    try:
        kind, name, build = choose_metadata(args)
    except (ValueError, KeyError) as exc:
        print(f"klog-center: {exc}", file=sys.stderr)
        return 2
    safe = "".join(c if c.isalnum() or c in "._-" else "_" for c in name).strip("._") or "capture"
    path = CAPTURES / f"{stamp()}-{safe}-{uuid.uuid4().hex[:6]}.log"
    filters = [part.strip().casefold() for part in (args.filter or "").split(",") if part.strip()]
    filtered_path = path.with_name(path.stem + ".filtered.log") if filters else None
    started = dt.datetime.now().astimezone()
    with path.open("xb", buffering=0) as out:
        out.write((f"[CAPTURE start={started.isoformat()} kind={kind} name={name!r} "
                   f"build={build!r} source=shared-klog-archive preroll_bytes={args.preroll_bytes} "
                   f"filter={filters!r}]\n").encode())
        filtered = filtered_path.open("xb", buffering=0) if filtered_path else None
        if filtered:
            filtered.write((f"[FILTER source={path.name} contains_any={filters!r}]\n").encode())
        print(f"Capturando em: {path}\nCtrl+C encerra apenas esta captura.", flush=True)
        if filtered_path:
            print(f"Linhas correspondentes ao filtro: {filtered_path}", flush=True)
        files_seen: set[Path] = set()
        current: Path | None = None
        offset = 0
        pending = bytearray()

        def emit(data: bytes) -> None:
            out.write(data)
            if filtered is None:
                return
            pending.extend(data)
            while b"\n" in pending:
                line, _, rest = pending.partition(b"\n")
                pending[:] = rest
                lowered = line.decode("utf-8", errors="replace").casefold()
                if any(term in lowered for term in filters):
                    filtered.write(line + b"\n")

        try:
            while True:
                files = sorted(ARCHIVE.glob("ps4-klog-raw-*.log"))
                if current is None and files:
                    current = files[-1]
                    size = current.stat().st_size
                    offset = max(0, size - args.preroll_bytes)
                    if offset:
                        with current.open("rb") as src:
                            src.seek(offset)
                            # Align to the next complete record where possible.
                            src.readline()
                            offset = src.tell()
                            preroll = src.read()
                        if preroll:
                            emit(preroll)
                            offset += len(preroll)
                    files_seen.add(current)
                elif current is not None and not current.exists():
                    current = None
                    offset = 0
                if current is not None and current.exists():
                    with current.open("rb") as src:
                        src.seek(offset)
                        data = src.read()
                    if data:
                        emit(data)
                        offset += len(data)
                    else:
                        later = [p for p in files if p not in files_seen and p > current]
                        if later:
                            current = later[0]
                            offset = 0
                            files_seen.add(current)
                elif files:
                    current = files[-1]
                    offset = current.stat().st_size
                    files_seen.add(current)
                time.sleep(0.15)
        except KeyboardInterrupt:
            ended = dt.datetime.now().astimezone()
            out.write((f"\n[CAPTURE end={ended.isoformat()} duration_s={(ended-started).total_seconds():.3f}]\n").encode())
            if filtered and pending:
                lowered = pending.decode("utf-8", errors="replace").casefold()
                if any(term in lowered for term in filters):
                    filtered.write(bytes(pending) + b"\n")
            if filtered:
                filtered.close()
            print(f"Captura encerrada: {path}", flush=True)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    d = sub.add_parser("daemon", help="mantém a única conexão klog com o PS4")
    d.add_argument("--host", default=DEFAULT_HOST,
                   help="endereço do PS4 (ou defina PS4_HOST)")
    d.add_argument("--port", type=int, default=DEFAULT_PORT)
    d.add_argument("--retry", type=float, default=2.0)
    d.set_defaults(func=daemon)
    c = sub.add_parser("capture", help="cria um arquivo de captura local independente")
    c.add_argument("--kind", choices=("game-dependency", "app", "custom"))
    c.add_argument("--name")
    c.add_argument("--build")
    c.add_argument("--preroll-bytes", type=int, default=0,
                   help="inclui bytes recentes do arquivo bruto atual antes do início")
    c.add_argument("--filter", help="termos literais separados por vírgula; gera também .filtered.log")
    c.set_defaults(func=capture)
    s = sub.add_parser("status", help="mostra se o daemon está ativo")
    s.set_defaults(func=status)
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
