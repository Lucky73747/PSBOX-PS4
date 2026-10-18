#!/usr/bin/env python3
"""CLI entry point for local scripts using the PSBOX PS4 Manager."""

from __future__ import annotations

import argparse
import ftplib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

from core import (KLOG_CENTER, close_ftp, connect_ftp, console_operation_lock,
                  install_pkg_experimental, list_pkg_files, load_saved_settings)


def settings(args) -> tuple[str, int, str, str, str]:
    saved = load_saved_settings()
    host = args.host or os.environ.get("PS4_HOST") or saved.get("host", "")
    if not host:
        raise ValueError("Informe --host, defina PS4_HOST ou configure o endereço no PS4 Manager.")
    port = args.port or int(os.environ.get("PS4_FTP_PORT", saved.get("port", 2121)))
    user = os.environ.get("PS4_FTP_USER", saved.get("user", "anonymous"))
    password = os.environ.get("PS4_FTP_PASSWORD", "")
    remote = args.remote_dir or saved.get("remote_dir", "/data/pkg")
    return host, int(port), user, password, remote


def pkg_list(args) -> int:
    config = settings(args)
    with console_operation_lock():
        ftp = connect_ftp(config)
        try:
            files = list_pkg_files(ftp)
        finally:
            close_ftp(ftp)
    records = [{"name": name, "bytes": size} for name, size in sorted(files.items())]
    if args.json:
        print(json.dumps(records, ensure_ascii=False))
    else:
        for item in records:
            print(f"{item['name']}\t{item['bytes']}")
        print(f"{len(records)} PKG(s) em {config[4]}")
    return 0


def pkg_upload(args) -> int:
    local = Path(args.pkg).expanduser().resolve(strict=True)
    if not local.is_file() or local.suffix.lower() != ".pkg":
        raise ValueError("pkg precisa apontar para um arquivo .pkg")
    config = settings(args)
    remote = local.name
    temp = f".{remote}.{uuid.uuid4().hex}.uploading"
    backup = f".{remote}.{uuid.uuid4().hex}.replaced"
    with console_operation_lock():
        ftp = connect_ftp(config)
        old_moved = False
        try:
            try:
                existing = ftp.size(remote)
            except ftplib.error_perm as exc:
                if not str(exc).startswith("550"):
                    raise
                existing = None
            if existing is not None and not args.replace:
                raise FileExistsError(f"{remote} existe. Passe --replace explicitamente.")
            total = local.stat().st_size
            sent = 0

            def report(block: bytes) -> None:
                nonlocal sent
                sent += len(block)
                print(f"progress={sent}/{total}", flush=True)

            with local.open("rb") as stream:
                ftp.storbinary(f"STOR {temp}", stream, blocksize=1024 * 1024, callback=report)
            if ftp.size(temp) != total:
                raise IOError("tamanho do arquivo remoto não confere")
            if existing is not None:
                ftp.rename(remote, backup)
                old_moved = True
            try:
                ftp.rename(temp, remote)
            except Exception:
                if old_moved:
                    ftp.rename(backup, remote)
                    old_moved = False
                raise
            if old_moved:
                old_moved = False
                try:
                    ftp.delete(backup)
                except ftplib.all_errors:
                    print(f"warning=old_copy_preserved name={backup}", file=sys.stderr)
            print(json.dumps({"status": "uploaded", "name": remote, "bytes": total,
                              "remote": f"{config[4]}/{remote}"}))
            return 0
        except Exception:
            try:
                ftp.delete(temp)
            except ftplib.all_errors:
                pass
            if old_moved:
                try:
                    ftp.rename(backup, remote)
                except ftplib.all_errors:
                    pass
            raise
        finally:
            close_ftp(ftp)


def pkg_delete(args) -> int:
    if not args.confirm:
        raise ValueError("exclusão requer --confirm")
    name = args.name
    if "/" in name or "\\" in name or not name.lower().endswith(".pkg"):
        raise ValueError("aceita somente o nome simples de um arquivo .pkg")
    config = settings(args)
    with console_operation_lock():
        ftp = connect_ftp(config)
        try:
            files = list_pkg_files(ftp)
            if name not in files:
                raise FileNotFoundError(f"PKG não listado como arquivo em {config[4]}: {name}")
            ftp.delete(name)
        finally:
            close_ftp(ftp)
    print(json.dumps({"status": "deleted", "name": name}))
    return 0


def klog(args) -> int:
    cmd = [sys.executable, str(KLOG_CENTER), args.action]
    if args.action == "daemon":
        cmd += ["--host", args.host, "--port", str(args.port)]
    elif args.action == "capture":
        if not args.name:
            raise ValueError("klog capture exige --name")
        cmd += ["--kind", args.kind, "--name", args.name, "--build", args.build,
                "--filter", args.filter, "--preroll-bytes", str(args.preroll_bytes)]
    return subprocess.call(cmd)


def install(args) -> int:
    if not args.experimental or not args.confirm:
        raise ValueError("instalação exige --experimental --confirm")
    pkg = Path(args.pkg).expanduser()

    def progress(done: int, total: int) -> None:
        print(json.dumps({"event": "progress", "done": done, "total": total}), flush=True)

    result = install_pkg_experimental(pkg, args.host, args.rpi_port,
                                      progress=progress, log=lambda line: print(line, flush=True),
                                      timeout_seconds=args.timeout,
                                      request_timeout_seconds=args.request_timeout,
                                      http_port=args.http_port)
    print(json.dumps(result, ensure_ascii=False))
    return 0 if result["status"] == "complete" else 3


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subs = parser.add_subparsers(dest="command", required=True)
    pkg = subs.add_parser("pkg", help="list, upload, or delete PKGs through FTP")
    pkg_sub = pkg.add_subparsers(dest="pkg_action", required=True)
    for action in ("list", "upload", "delete"):
        command = pkg_sub.add_parser(action)
        command.add_argument("--host")
        command.add_argument("--port", type=int)
        command.add_argument("--remote-dir")
        if action == "upload":
            command.add_argument("pkg")
            command.add_argument("--replace", action="store_true")
            command.set_defaults(func=pkg_upload)
        elif action == "delete":
            command.add_argument("name")
            command.add_argument("--confirm", action="store_true")
            command.set_defaults(func=pkg_delete)
        else:
            command.add_argument("--json", action="store_true")
            command.set_defaults(func=pkg_list)

    logs = subs.add_parser("klog", help="use the shared klog-center daemon/captures")
    logs.add_argument("action", choices=("status", "daemon", "capture"))
    logs.add_argument("--host", default="")
    logs.add_argument("--port", type=int, default=3232)
    logs.add_argument("--kind", choices=("game-dependency", "app", "custom"), default="app")
    logs.add_argument("--name")
    logs.add_argument("--build", default="PSBOX-agent")
    logs.add_argument("--filter", default="")
    logs.add_argument("--preroll-bytes", type=int, default=0)
    logs.set_defaults(func=klog)

    auto = subs.add_parser("install", help="experimental RPI install from a local PKG")
    auto.add_argument("pkg")
    auto.add_argument("--host", default=load_saved_settings().get("host", ""))
    auto.add_argument("--rpi-port", type=int, default=12800)
    auto.add_argument("--timeout", type=int, default=4 * 60 * 60)
    auto.add_argument("--request-timeout", type=int, default=120,
                      help="timeout in seconds for RPI API requests (default: 120)")
    auto.add_argument("--http-port", type=int,
                      default=load_saved_settings().get("http_port", 9898),
                      help="local HTTP port for serving PKG to the PS4 (default: 9898)")
    auto.add_argument("--experimental", action="store_true")
    auto.add_argument("--confirm", action="store_true")
    auto.set_defaults(func=install)

    args = parser.parse_args()
    try:
        return args.func(args)
    except Exception as exc:
        print(f"error={type(exc).__name__}: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
