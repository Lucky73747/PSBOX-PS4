#!/usr/bin/env python3
"""Capture a sanitized Klog and PSBOX app-log delta for every PSBOX launch."""
from __future__ import annotations

import argparse
import datetime as dt
import fcntl
import ftplib
import json
import os
from contextlib import nullcontext
from pathlib import Path, PurePosixPath
import re
import signal
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parent
DATA_ROOT = Path.home() / ".local" / "share" / "psbox-pc-manager"
KLOG_ROOT = DATA_ROOT / "klogs"
ARCHIVE = KLOG_ROOT / "archive"
RUNS = KLOG_ROOT / "psbox"
LOGS = DATA_ROOT / "logs"
WATCH_LOCK = KLOG_ROOT / ".psbox-run-capture.lock"
MANAGER = Path(__file__).resolve().parent
KLOG_CENTER = ROOT / "klog-center.py"
DEFAULT_HOST = ""
DEFAULT_KLOG_PORT = 3232
DEFAULT_FTP_PORT = 2121
DEFAULT_TITLE_ID = "XCHM00001"
FTP_LOG_DIRS = (
    "/data/PSBOX",
)
ALLOWED_LOG_SUFFIXES = {".log"}
BUILD_RE = re.compile(rb"XCLOUD-PS4-HOMEBREW-([^\]\s]+)")
APPVER_RE = re.compile(rb"appVer=\{([^}]+)\}")
FTP_LOCK_FACTORY = None
PRIVATE_RE = re.compile(
    rb"(?i)[\"']?(?:access[_ -]?token|refresh[_ -]?token|token|password|secret|authorization|cookie|sdp|ice[_ -]?candidate|xuid|gamertag|email|username|auth[_ -]?code|device[_ -]?code|user[_ -]?code)[\"']?\s*[=:]\s*[\"']?(?:bearer\s+)?[^\s,;\"']+"
)
URL_RE = re.compile(rb"(?i)https?://[^\s\"']+")
IP_RE = re.compile(rb"\b(?:\d{1,3}\.){3}\d{1,3}\b")
IPV6_RE = re.compile(rb"(?<![\w:])(?:[0-9a-f]{1,4}:){2,7}[0-9a-f]{0,4}(?![\w:])", re.I)
EMAIL_RE = re.compile(rb"[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}", re.I)


def redact_line(line: bytes) -> bytes:
    line = URL_RE.sub(b"[URL]", line)
    line = IP_RE.sub(b"[IP]", line)
    line = IPV6_RE.sub(b"[IP]", line)
    line = EMAIL_RE.sub(b"[EMAIL]", line)
    line = PRIVATE_RE.sub(b"[REDACTED]", line)
    stripped = line.lstrip().lower()
    if b"candidate:" in line.lower() or stripped.startswith((b"v=0", b"o=", b"s=", b"t=", b"m=", b"a=")):
        return b"[REDACTED SDP/ICE line]"
    return line


def now() -> dt.datetime:
    return dt.datetime.now().astimezone()


def stamp(value: dt.datetime | None = None) -> str:
    return (value or now()).strftime("%Y%m%d-%H%M%S%z")


def safe_component(value: str) -> str:
    result = "".join(c if c.isalnum() or c in "._-" else "_" for c in value)
    return result.strip("._") or "run"


def is_launch(line: bytes, title_id: str) -> bool:
    return f"launchApp({title_id})".encode() in line


def is_process_term(line: bytes, title_id: str) -> bool:
    text = line.decode("utf-8", errors="replace")
    return "[sceProcessStarter] ProcessTerm()" in text and title_id in text


def is_crash(line: bytes, title_id: str) -> bool:
    text = line.decode("utf-8", errors="replace")
    return "notifyAppCrash()" in text and title_id in text


def is_coredump_complete(line: bytes) -> bool:
    text = line.decode("utf-8", errors="replace").casefold()
    return "coredump complete" in text or "[coredump] end" in text


def matching_log(name: str) -> bool:
    return Path(name).suffix.casefold() in ALLOWED_LOG_SUFFIXES


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    tmp.replace(path)


def load_manager_settings(args: argparse.Namespace) -> tuple[str, int, str, str]:
    sys.path.insert(0, str(MANAGER))
    from core import console_operation_lock, load_saved_settings

    global FTP_LOCK_FACTORY
    FTP_LOCK_FACTORY = console_operation_lock

    saved = load_saved_settings()
    host = args.ftp_host or os.environ.get("PS4_HOST") or saved.get("host", "")
    port = args.ftp_port or int(os.environ.get("PS4_FTP_PORT", saved.get("port", DEFAULT_FTP_PORT)))
    user = os.environ.get("PS4_FTP_USER", saved.get("user", "anonymous"))
    password = os.environ.get("PS4_FTP_PASSWORD", "")
    return host, int(port), user, password


class FtpLogCollector:
    """Read only changed text/log files from the app's FTP log folders."""

    def __init__(self, host: str, port: int, user: str, password: str, timeout: int = 12):
        self.host, self.port, self.user, self.password = host, port, user, password
        self.timeout = timeout
        self.baseline: dict[str, dict] | None = None
        self.last_seen: dict[str, tuple[int | None, str | None]] = {}
        self.errors: list[str] = []
        self.log_offset: int | None = None
        self.log_partial = bytearray()

    def _connect(self, remote_dir: str) -> ftplib.FTP:
        ftp = ftplib.FTP()
        ftp.connect(self.host, self.port, timeout=self.timeout)
        ftp.login(self.user, self.password)
        ftp.cwd(remote_dir)
        ftp.voidcmd("TYPE I")
        return ftp

    @staticmethod
    def _mtime(ftp: ftplib.FTP, name: str) -> str | None:
        try:
            response = ftp.sendcmd(f"MDTM {name}")
            return response[4:].strip() if response.startswith("213 ") else None
        except ftplib.all_errors:
            return None

    def inventory(self) -> dict[str, dict]:
        result: dict[str, dict] = {}
        with FTP_LOCK_FACTORY() if FTP_LOCK_FACTORY else nullcontext():
            for remote_dir in FTP_LOG_DIRS:
                ftp = None
                try:
                    ftp = self._connect(remote_dir)
                    for raw_name in ftp.nlst():
                        name = PurePosixPath(raw_name.rstrip("/")).name
                        if name in ("", ".", "..") or not matching_log(name):
                            continue
                        if any(c in name for c in ("/", "\\", "\r", "\n")):
                            continue
                        try:
                            size = ftp.size(name)
                        except ftplib.all_errors:
                            continue
                        key = f"{remote_dir}/{name}"
                        result[key] = {
                            "remote": key,
                            "size": size,
                            "mtime_utc": self._mtime(ftp, name),
                        }
                        if key == "/data/PSBOX/xcloud.log" and self.log_offset is None:
                            # Establish a launch-time baseline; do not copy older sessions.
                            self.log_offset = size
                except (OSError, *ftplib.all_errors) as exc:
                    self.errors.append(f"inventory {remote_dir}: {type(exc).__name__}: {exc}")
                finally:
                    if ftp is not None:
                        try:
                            ftp.quit()
                        except ftplib.all_errors:
                            ftp.close()
        return result

    @staticmethod
    def _fingerprint(record: dict) -> tuple[int | None, str | None]:
        return record.get("size"), record.get("mtime_utc")

    def snapshot(self, run_dir: Path, final: bool = False,
                 since_epoch: int | None = None) -> list[dict]:
        current = self.inventory()
        if self.baseline is None:
            self.baseline = current.copy()
            if current:
                self.errors.append("FTP baseline was unavailable at app launch; current log files may include older records")

        downloaded: list[dict] = []
        for remote, record in current.items():
            if remote == "/data/PSBOX/xcloud.log":
                item = self._snapshot_xcloud_log(remote, record, run_dir, final)
                if item:
                    downloaded.append(item)
                continue
            fingerprint = self._fingerprint(record)
            base = self._fingerprint(self.baseline.get(remote, {})) if self.baseline is not None else None
            changed_since_start = remote not in (self.baseline or {}) or fingerprint != base
            if not changed_since_start:
                continue
            if self.last_seen.get(remote) == fingerprint and not final:
                continue

            remote_dir, name = remote.rsplit("/", 1)
            relative = Path("ftp") / remote_dir.lstrip("/") / name
            destination = run_dir / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            temp_path = destination.with_name(destination.name + ".part")
            ftp = None
            with FTP_LOCK_FACTORY() if FTP_LOCK_FACTORY else nullcontext():
                try:
                    ftp = self._connect(remote_dir)
                    with temp_path.open("wb") as out:
                        ftp.retrbinary(f"RETR {name}", out.write, blocksize=256 * 1024)
                    actual_size = temp_path.stat().st_size
                    if record.get("size") is not None and actual_size != record["size"]:
                        raise IOError(f"download size mismatch: expected {record['size']}, got {actual_size}")
                    temp_path.replace(destination)
                    self.last_seen[remote] = fingerprint
                    downloaded.append({
                        **record,
                        "local": str(destination.relative_to(run_dir)),
                        "downloaded_at": now().isoformat(),
                        "snapshot": "final" if final else "live",
                    })
                except (OSError, *ftplib.all_errors) as exc:
                    temp_path.unlink(missing_ok=True)
                    self.errors.append(f"download {remote}: {type(exc).__name__}: {exc}")
                finally:
                    if ftp is not None:
                        try:
                            ftp.quit()
                        except ftplib.all_errors:
                            ftp.close()
        if final:
            downloaded.extend(self.snapshot_coredump_reports(run_dir, since_epoch))
        return downloaded

    def _snapshot_xcloud_log(self, remote: str, record: dict, run_dir: Path,
                             final: bool) -> dict | None:
        size = record.get("size")
        if size is None:
            return None
        if self.log_offset is None:
            self.log_offset = size
            self.errors.append("xcloud.log baseline unavailable; started capture at current file end")
            return None
        if size < self.log_offset:
            self.errors.append("xcloud.log was truncated during capture; new content starts at current file end")
            self.log_offset = size
            self.log_partial.clear()
            return None
        if size == self.log_offset:
            return None

        remote_dir, name = remote.rsplit("/", 1)
        destination = run_dir / "ftp" / "data" / "PSBOX" / "xcloud.log"
        destination.parent.mkdir(parents=True, exist_ok=True)
        received = 0
        ftp = None
        with FTP_LOCK_FACTORY() if FTP_LOCK_FACTORY else nullcontext():
            try:
                ftp = self._connect(remote_dir)
                with destination.open("ab", buffering=0) as out:
                    def consume(block: bytes) -> None:
                        nonlocal received
                        received += len(block)
                        self.log_partial.extend(block)
                        while b"\n" in self.log_partial:
                            line, _, rest = self.log_partial.partition(b"\n")
                            self.log_partial[:] = rest
                            out.write(redact_line(bytes(line)).rstrip(b"\r") + b"\n")

                    ftp.retrbinary(f"RETR {name}", consume,
                                   blocksize=256 * 1024, rest=self.log_offset)
                    if final and self.log_partial:
                        out.write(redact_line(bytes(self.log_partial)) + b"\n")
                        self.log_partial.clear()
                self.log_offset += received
                self.last_seen[remote] = self._fingerprint(record)
                if received == 0:
                    return None
                return {**record, "local": str(destination.relative_to(run_dir)),
                        "downloaded_bytes": received, "downloaded_at": now().isoformat(),
                        "snapshot": "final" if final else "live", "redacted": True}
            except (OSError, *ftplib.all_errors) as exc:
                self.errors.append(f"download delta {remote}: {type(exc).__name__}: {exc}")
                return None
            finally:
                if ftp is not None:
                    try:
                        ftp.quit()
                    except ftplib.all_errors:
                        ftp.close()

    def snapshot_coredump_reports(self, run_dir: Path,
                                  since_epoch: int | None) -> list[dict]:
        """Save small text reports only; never copy binary dumps or crash videos."""
        ftp = None
        records: list[dict] = []
        with FTP_LOCK_FACTORY() if FTP_LOCK_FACTORY else nullcontext():
            try:
                ftp = self._connect("/data/sce_coredumps")
                candidates = [PurePosixPath(x.rstrip("/")).name for x in ftp.nlst()]
                for dirname in candidates[-20:]:
                    if not dirname.startswith("XCHM00001_"):
                        continue
                    try:
                        dump_epoch = int(dirname.rsplit("_", 1)[-1])
                    except ValueError:
                        continue
                    if since_epoch is not None and dump_epoch < since_epoch:
                        continue
                    try:
                        ftp.cwd(dirname)
                    except ftplib.all_errors:
                        continue
                    try:
                        names = [PurePosixPath(x.rstrip("/")).name for x in ftp.nlst()]
                        for name in names:
                            if name not in {"report.log", "playgo-status.xml"} and not name.endswith(".orbisdmpmanifest"):
                                continue
                            try:
                                size = ftp.size(name)
                            except ftplib.all_errors:
                                continue
                            if size is None or size > 1024 * 1024:
                                continue
                            relative = Path("ftp") / "data" / "sce_coredumps" / dirname / name
                            target = run_dir / relative
                            target.parent.mkdir(parents=True, exist_ok=True)
                            chunks: list[bytes] = []
                            ftp.retrbinary(f"RETR {name}", chunks.append, blocksize=32768)
                            target.write_bytes(redact_line(b"".join(chunks)))
                            records.append({"remote": f"/data/sce_coredumps/{dirname}/{name}",
                                            "size": size, "local": str(relative),
                                            "downloaded_at": now().isoformat(), "redacted": True})
                    finally:
                        ftp.cwd("/data/sce_coredumps")
            except (OSError, *ftplib.all_errors) as exc:
                self.errors.append(f"coredump reports: {type(exc).__name__}: {exc}")
            finally:
                if ftp is not None:
                    try:
                        ftp.quit()
                    except ftplib.all_errors:
                        ftp.close()
        return records


class RunSession:
    def __init__(self, title_id: str, archive: Path, launch_line: bytes, collector: FtpLogCollector | None):
        self.title_id = title_id
        self.started = now()
        self.archive = archive
        self.run_token = uuid.uuid4().hex[:6]
        self.run_id = f"{stamp(self.started)}-{safe_component(title_id)}-{self.run_token}"
        self.directory = RUNS / self.run_id
        self.directory.mkdir(parents=True, exist_ok=False)
        self.klog_path = self.directory / "klog.log"
        self.klog = self.klog_path.open("ab", buffering=0)
        self.collector = collector
        self.metadata = {
            "run_id": self.run_id,
            "title_id": title_id,
            "started_at": self.started.isoformat(),
            "ended_at": None,
            "duration_seconds": None,
            "build_label": None,
            "app_version": None,
            "process_pid": None,
            "crashed": False,
            "end_reason": None,
            "trigger": redact_line(launch_line).decode("utf-8", errors="replace").rstrip(),
            "klog_archive_source": str(archive),
            "klog_capture": str(self.klog_path),
            "ftp_host": "[configured]" if collector else None,
            "ftp_logs": [],
            "ftp_errors": [],
        }
        self.write(launch_line)
        self.update_from_line(launch_line)
        write_json(self.directory / "metadata.json", self.metadata)
        self.write_summary("capture active")
        print(f"[PSBOX_CAPTURE] started id={self.run_id} dir={self.directory}", flush=True)

    def write(self, line: bytes) -> None:
        line = redact_line(line)
        self.klog.write(line)
        if not line.endswith(b"\n"):
            self.klog.write(b"\n")

    def update_from_line(self, line: bytes) -> None:
        build = BUILD_RE.search(line)
        if build:
            label = build.group(1).decode("ascii", errors="replace")
            self.metadata["build_label"] = label
            self._rename_for_build(label)
        appver = APPVER_RE.search(line)
        if appver:
            self.metadata["app_version"] = appver.group(1).decode("ascii", errors="replace")
        pid = re.search(rb"\bnew pid=0x([0-9a-fA-F]+)", line)
        if pid:
            self.metadata["process_pid"] = int(pid.group(1), 16)
        if self.metadata["build_label"] or self.metadata["app_version"] or self.metadata["process_pid"]:
            write_json(self.directory / "metadata.json", self.metadata)

    def _rename_for_build(self, label: str) -> None:
        name = f"{stamp(self.started)}-{safe_component(label)}-{self.run_token}"
        destination = RUNS / name
        if destination == self.directory:
            return
        try:
            self.directory.rename(destination)
        except OSError as exc:
            self.metadata["directory_rename_error"] = f"{type(exc).__name__}: {exc}"
            return
        self.directory = destination
        self.run_id = name
        self.metadata["run_id"] = name
        write_json(self.directory / "metadata.json", self.metadata)
        self.write_summary("capture active")

    def append(self, line: bytes) -> None:
        self.write(line)
        self.update_from_line(line)
        if is_crash(line, self.title_id):
            self.metadata["crashed"] = True

    def ftp_snapshot(self, final: bool = False) -> None:
        if self.collector is None:
            return
        records = self.collector.snapshot(self.directory, final=final,
                                          since_epoch=int(self.started.timestamp()))
        if records:
            self.metadata["ftp_logs"].extend(records)
            self._learn_build_label(records)
        self.metadata["ftp_errors"] = [redact_line(x.encode()).decode("utf-8", errors="replace")
                                        for x in self.collector.errors]
        write_json(self.directory / "metadata.json", self.metadata)
        self.write_summary("capture active" if not final else "capture complete")

    def _learn_build_label(self, records: list[dict]) -> None:
        for item in records:
            local = self.directory / item["local"]
            try:
                data = local.read_bytes()[:512 * 1024]
            except OSError:
                continue
            match = BUILD_RE.search(data)
            if match:
                label = match.group(1).decode("ascii", errors="replace")
                self.metadata["build_label"] = label
                self._rename_for_build(label)
                write_json(self.directory / "metadata.json", self.metadata)
                self.write_summary("capture active")
                return

    def write_summary(self, status: str) -> None:
        label = self.metadata.get("build_label") or "não detectado"
        lines = [
            f"# Captura PSBOX — {self.run_id}", "",
            f"- Estado: {status}",
            f"- Title ID: `{self.title_id}`",
            f"- Build: `{label}`",
            f"- Versão do pacote: `{self.metadata.get('app_version') or 'não detectada'}`",
            f"- Início: {self.metadata['started_at']}",
            f"- Fim: {self.metadata.get('ended_at') or 'em execução'}",
            f"- Crash report detectado no klog: `{self.metadata['crashed']}`",
            f"- Klog da sessão: `klog.log`",
            "- Logs FTP alterados nesta execução: `metadata.json` e a subpasta `ftp/`",
            f"- Arquivo bruto de origem: `{self.archive}`", "",
            "A captura guarda todo o intervalo do klog entre o lançamento e o término do processo.",
            "Logs de aplicação são baixados apenas a partir do início desta execução e passam por redação de dados sensíveis.",
            "Do coredump, só relatórios pequenos de texto são copiados; dumps binários e vídeos de crash ficam no console.",
        ]
        (self.directory / "SUMMARY.md").write_text("\n".join(lines) + "\n", encoding="utf-8")

    def finish(self, reason: str) -> None:
        self.metadata["ended_at"] = now().isoformat()
        self.metadata["duration_seconds"] = round(
            (now() - self.started).total_seconds(), 3)
        self.metadata["end_reason"] = reason
        self.ftp_snapshot(final=True)
        self.klog.close()
        write_json(self.directory / "metadata.json", self.metadata)
        self.write_summary("capture complete")
        print(f"[PSBOX_CAPTURE] finished id={self.run_id} reason={reason} dir={self.directory}", flush=True)


def ensure_daemon(host: str, port: int) -> subprocess.Popen | None:
    status = subprocess.run([sys.executable, str(KLOG_CENTER), "status"], capture_output=True, text=True)
    if status.returncode == 0:
        print("[PSBOX_CAPTURE] klog-center already running", flush=True)
        return None
    LOGS.mkdir(parents=True, exist_ok=True)
    daemon_log = (LOGS / "klog-center-daemon.log").open("ab", buffering=0)
    proc = subprocess.Popen(
        [sys.executable, str(KLOG_CENTER), "daemon", "--host", host, "--port", str(port)],
        stdin=subprocess.DEVNULL, stdout=daemon_log, stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        time.sleep(0.25)
        status = subprocess.run([sys.executable, str(KLOG_CENTER), "status"], capture_output=True, text=True)
        if status.returncode == 0:
            print(f"[PSBOX_CAPTURE] started klog-center pid={proc.pid} ({status.stdout.strip()})", flush=True)
            return proc
        if proc.poll() is not None:
            break
    raise RuntimeError(f"klog-center did not start; see {LOGS / 'klog-center-daemon.log'}")


def follow_archive(start_at_end: bool = True):
    offsets: dict[Path, int] = {}
    pending: dict[Path, bytearray] = {}
    if start_at_end:
        for path in sorted(ARCHIVE.glob("ps4-klog-raw-*.log")):
            try:
                offsets[path] = path.stat().st_size
            except FileNotFoundError:
                pass
    while True:
        paths = sorted(ARCHIVE.glob("ps4-klog-raw-*.log"))
        emitted = False
        for path in paths:
            if path not in offsets:
                offsets[path] = 0
            try:
                with path.open("rb") as source:
                    source.seek(offsets[path])
                    data = source.read()
                    offsets[path] = source.tell()
            except FileNotFoundError:
                continue
            if not data:
                continue
            buffer = pending.setdefault(path, bytearray())
            buffer.extend(data)
            while b"\n" in buffer:
                line, _, rest = buffer.partition(b"\n")
                buffer[:] = rest
                emitted = True
                yield path, bytes(line) + b"\n"
        if not emitted:
            yield None, b""
        time.sleep(0.12)


def _watch_locked(args: argparse.Namespace) -> int:
    if args.ensure_daemon:
        ensure_daemon(args.klog_host, args.klog_port)
    elif subprocess.run([sys.executable, str(KLOG_CENTER), "status"], capture_output=True).returncode != 0:
        raise RuntimeError("klog-center está parado; rode klog-center.py daemon ou use --ensure-daemon")

    RUNS.mkdir(parents=True, exist_ok=True)
    collector = None
    if not args.no_ftp:
        host, port, user, password = load_manager_settings(args)
        collector = FtpLogCollector(host, port, user, password)

    current: RunSession | None = None
    stopping = False
    crash_started_at: float | None = None
    crash_finish_at: float | None = None

    def stop(_sig, _frame):
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    print(f"[PSBOX_CAPTURE] watching title_id={args.title_id}; output={RUNS}", flush=True)
    next_snapshot = time.monotonic() + args.ftp_snapshot_seconds

    try:
        for archive, line in follow_archive(start_at_end=True):
            if stopping:
                break
            if current is not None and crash_started_at is not None:
                now_mono = time.monotonic()
                if crash_finish_at is not None and now_mono >= crash_finish_at:
                    current.finish("crash_coredump_complete")
                    current = None
                    crash_started_at = None
                    crash_finish_at = None
                elif now_mono - crash_started_at >= args.crash_timeout_seconds:
                    current.finish("crash_coredump_timeout")
                    current = None
                    crash_started_at = None
                    crash_finish_at = None
            if archive is None or not line:
                continue
            if current is None:
                if is_launch(line, args.title_id):
                    current = RunSession(args.title_id, archive, line, collector)
                    crash_started_at = None
                    crash_finish_at = None
                    if collector:
                        current.metadata["ftp_baseline_started_at"] = now().isoformat()
                        current.collector.log_offset = None
                        current.collector.log_partial.clear()
                        current.collector.baseline = current.collector.inventory()
                        current.metadata["ftp_baseline_files"] = len(current.collector.baseline)
                        current.metadata["ftp_errors"] = [redact_line(x.encode()).decode("utf-8", errors="replace")
                                                           for x in current.collector.errors]
                        write_json(current.directory / "metadata.json", current.metadata)
                        current.ftp_snapshot(final=False)
                    next_snapshot = time.monotonic() + args.ftp_snapshot_seconds
                continue

            if is_launch(line, args.title_id):
                current.finish("next_launch_seen_before_process_term")
                current = RunSession(args.title_id, archive, line, collector)
                crash_started_at = None
                crash_finish_at = None
                if collector:
                    current.collector = collector
                    current.collector.log_offset = None
                    current.collector.log_partial.clear()
                    current.collector.baseline = current.collector.inventory()
                    current.metadata["ftp_baseline_files"] = len(current.collector.baseline)
                    write_json(current.directory / "metadata.json", current.metadata)
                next_snapshot = time.monotonic() + args.ftp_snapshot_seconds
                continue

            current.append(line)
            if is_crash(line, args.title_id) and crash_started_at is None:
                crash_started_at = time.monotonic()
            if current.metadata["crashed"] and is_coredump_complete(line):
                crash_finish_at = time.monotonic() + args.crash_tail_seconds
            if is_process_term(line, args.title_id) and crash_started_at is None:
                current.finish("process_term")
                current = None
                crash_started_at = None
                crash_finish_at = None
                next_snapshot = time.monotonic() + args.ftp_snapshot_seconds
                continue

            if collector and time.monotonic() >= next_snapshot:
                current.ftp_snapshot(final=False)
                next_snapshot = time.monotonic() + args.ftp_snapshot_seconds
    finally:
        if current is not None:
            current.finish("watcher_stopped_while_app_active")
    return 0


def watch(args: argparse.Namespace) -> int:
    WATCH_LOCK.parent.mkdir(parents=True, exist_ok=True)
    lock_file = WATCH_LOCK.open("a+")
    try:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        lock_file.seek(0)
        print(f"psbox-run-capture: watcher already running ({lock_file.read().strip()})", file=sys.stderr)
        lock_file.close()
        return 2
    lock_file.seek(0)
    lock_file.truncate()
    lock_file.write(f"pid={os.getpid()} title_id={args.title_id} started={now().isoformat()}\n")
    lock_file.flush()
    try:
        return _watch_locked(args)
    finally:
        lock_file.close()


def status(_args: argparse.Namespace) -> int:
    WATCH_LOCK.parent.mkdir(parents=True, exist_ok=True)
    lock_file = WATCH_LOCK.open("a+")
    try:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        lock_file.seek(0)
        print("watcher=running " + lock_file.read().strip())
        return 0
    else:
        print("watcher=stopped")
        return 1
    finally:
        lock_file.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subs = parser.add_subparsers(dest="command", required=True)
    s = subs.add_parser("status", help="show whether the automatic capture watcher is running")
    s.set_defaults(func=status)
    w = subs.add_parser("watch", help="watch shared klog and bundle each PSBOX run")
    w.add_argument("--title-id", default=DEFAULT_TITLE_ID)
    w.add_argument("--klog-host", default=DEFAULT_HOST)
    w.add_argument("--klog-port", type=int, default=DEFAULT_KLOG_PORT)
    w.add_argument("--ftp-host")
    w.add_argument("--ftp-port", type=int)
    w.add_argument("--ftp-snapshot-seconds", type=float, default=20.0)
    w.add_argument("--crash-timeout-seconds", type=float, default=180.0,
                   help="close a crashed session after this many seconds if no coredump completion marker arrives")
    w.add_argument("--crash-tail-seconds", type=float, default=2.0,
                   help="capture this much klog after coredump completion")
    w.add_argument("--no-ftp", action="store_true", help="capture klog only")
    w.add_argument("--ensure-daemon", action="store_true", help="start klog-center if it is stopped")
    w.set_defaults(func=watch)
    args = parser.parse_args()
    try:
        return args.func(args)
    except KeyboardInterrupt:
        return 130
    except Exception as exc:
        print(f"psbox-run-capture: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
