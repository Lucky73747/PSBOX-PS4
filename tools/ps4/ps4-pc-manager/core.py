"""Shared PSBOX PS4 Manager operations used by the GUI and local agent CLI."""

from __future__ import annotations

from contextlib import contextmanager
from datetime import datetime
from email.utils import formatdate
import errno
import fcntl
import ftplib
from concurrent.futures import ThreadPoolExecutor, as_completed
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import ipaddress
import json
import os
from pathlib import Path, PurePosixPath
import re
import signal
import socket
import subprocess
import sys
import threading
import time
from typing import Callable
from urllib.error import HTTPError, URLError
from urllib.parse import quote, unquote, urlparse
from urllib.request import Request, urlopen
import uuid

ROOT = Path(__file__).resolve().parent
DATA_ROOT = Path.home() / ".local" / "share" / "psbox-pc-manager"
CONFIG_PATH = Path.home() / ".config" / "psbox-pc-manager" / "config.json"
KLOG_ROOT = DATA_ROOT / "klogs"
KLOG_CENTER = ROOT / "klog-center.py"
PSBOX_CAPTURE = ROOT / "psbox-run-capture.py"
PSBOX_CAPTURE_LOCK = KLOG_ROOT / ".psbox-run-capture.lock"
LOG_DIR = DATA_ROOT / "logs"
PSBOX_CAPTURE_LOG = LOG_DIR / "psbox-autocapture.log"
OP_LOCK = Path.home() / ".cache" / "ps4-pc-manager" / "console-operation.lock"
INSTALL_LOCK = Path.home() / ".cache" / "ps4-pc-manager" / "install.lock"


@contextmanager
def console_operation_lock():
    """Serialize package mutations across the GUI and concurrent local agents."""
    OP_LOCK.parent.mkdir(parents=True, exist_ok=True)
    with OP_LOCK.open("a+") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock.fileno(), fcntl.LOCK_UN)


@contextmanager
def install_operation_lock():
    """Allow only one remote install while leaving read-only FTP access available."""
    INSTALL_LOCK.parent.mkdir(parents=True, exist_ok=True)
    with INSTALL_LOCK.open("a+") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock.fileno(), fcntl.LOCK_UN)


def connect_ftp(settings: tuple[str, int, str, str, str]) -> ftplib.FTP:
    host, port, user, password, remote_dir = settings
    ftp = ftplib.FTP()
    try:
        ftp.connect(host, port, timeout=20)
        ftp.login(user, password)
        ftp.cwd(remote_dir)
        ftp.voidcmd("TYPE I")
        return ftp
    except Exception:
        ftp.close()
        raise


def close_ftp(ftp: ftplib.FTP) -> None:
    try:
        ftp.quit()
    except ftplib.all_errors:
        ftp.close()


def list_pkg_files(ftp: ftplib.FTP) -> dict[str, int]:
    files: dict[str, int] = {}
    for entry in ftp.nlst():
        name = PurePosixPath(entry.rstrip("/")).name
        if name in (".", "..") or not name.lower().endswith(".pkg"):
            continue
        try:
            size = ftp.size(name)
        except ftplib.all_errors:
            continue
        if size is not None:
            files[name] = size
    return files


def _direct_child_name(entry: str, remote_dir: str) -> str:
    path = PurePosixPath(entry.rstrip("/"))
    if path.is_absolute():
        if path.parent != PurePosixPath(remote_dir):
            raise RuntimeError(f"FTP listou um caminho fora de {remote_dir}: {entry}")
    elif path.parent != PurePosixPath("."):
        raise RuntimeError(f"FTP listou um item que não é filho direto de {remote_dir}: {entry}")
    name = path.name
    if name in ("", ".", "..") or any(char in name for char in ("\r", "\n", "/", "\\")):
        raise RuntimeError(f"Nome FTP inseguro; limpeza cancelada: {entry!r}")
    return name


def list_remote_files_preserving_directories(
        ftp: ftplib.FTP, remote_dir: str) -> tuple[list[tuple[str, int]], list[str]]:
    """List direct files and directories, failing closed if an item is ambiguous."""
    try:
        entries = list(ftp.mlsd())
    except ftplib.error_perm:
        entries = None

    if entries is not None:
        files: list[tuple[str, int]] = []
        directories: list[str] = []
        for raw_name, facts in entries:
            kind = facts.get("type", "").lower()
            if kind in ("cdir", "pdir"):
                continue
            name = _direct_child_name(raw_name, remote_dir)
            if kind == "dir":
                directories.append(name)
            elif kind == "file":
                size_fact = facts.get("size")
                if size_fact is None:
                    size = ftp.size(name)
                    if size is None:
                        raise RuntimeError(f"Não consegui confirmar o tamanho do arquivo {name}; limpeza cancelada.")
                else:
                    try:
                        size = int(size_fact)
                    except (TypeError, ValueError) as exc:
                        raise RuntimeError(f"Tamanho inválido para {name}; limpeza cancelada.") from exc
                files.append((name, size))
            else:
                raise RuntimeError(f"Tipo FTP desconhecido para {name}; limpeza cancelada para preservar pastas.")
        return files, directories

    # GoldHEN builds that do not implement MLSD: identify directories by trying
    # CWD, then require SIZE to positively identify every remaining item as a file.
    original_dir = ftp.pwd()
    files = []
    directories = []
    for entry in ftp.nlst():
        if PurePosixPath(entry.rstrip("/")).name in (".", ".."):
            continue
        name = _direct_child_name(entry, remote_dir)
        if name in directories or any(existing == name for existing, _ in files):
            continue
        try:
            ftp.cwd(name)
        except ftplib.error_perm:
            ftp.cwd(original_dir)
            try:
                size = ftp.size(name)
            except ftplib.error_perm as exc:
                raise RuntimeError(
                    f"Não consegui distinguir arquivo de pasta para {name}; nenhuma limpeza automática será feita."
                ) from exc
            if size is None:
                raise RuntimeError(f"Tamanho desconhecido para {name}; limpeza cancelada para preservar pastas.")
            files.append((name, size))
        else:
            directories.append(name)
            ftp.cwd(original_dir)
    return files, directories


def load_saved_settings() -> dict:
    try:
        return json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def klog_status() -> tuple[bool, str]:
    proc = subprocess.run([sys.executable, str(KLOG_CENTER), "status"],
                          text=True, capture_output=True, timeout=5)
    return proc.returncode == 0, (proc.stdout + proc.stderr).strip()


def psbox_autocapture_status() -> tuple[bool, str]:
    proc = subprocess.run([sys.executable, str(PSBOX_CAPTURE), "status"],
                          text=True, capture_output=True, timeout=5)
    return proc.returncode == 0, (proc.stdout + proc.stderr).strip()


def ensure_psbox_autocapture(klog_host: str, klog_port: int,
                             ftp_host: str, ftp_port: int) -> str:
    if not klog_host or not ftp_host:
        raise ValueError("Informe o endereço do PS4 antes de iniciar a captura automática.")
    running, report = psbox_autocapture_status()
    if running:
        return report or "Captura automática do PSBOX ativa"

    LOG_DIR.mkdir(parents=True, exist_ok=True)
    output = PSBOX_CAPTURE_LOG.open("ab", buffering=0)
    command = [sys.executable, str(PSBOX_CAPTURE), "watch", "--ensure-daemon",
               "--klog-host", klog_host, "--klog-port", str(klog_port),
               "--ftp-host", ftp_host, "--ftp-port", str(ftp_port)]
    proc = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=output,
                            stderr=subprocess.STDOUT, start_new_session=True,
                            close_fds=True)
    output.close()

    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        time.sleep(0.25)
        running, report = psbox_autocapture_status()
        if running:
            return report or f"Captura automática iniciada (PID {proc.pid})"
        if proc.poll() is not None:
            break
    tail = ""
    try:
        tail = PSBOX_CAPTURE_LOG.read_text(encoding="utf-8", errors="replace")[-2000:]
    except OSError:
        pass
    raise RuntimeError(
        f"Não consegui iniciar a captura automática. Veja {PSBOX_CAPTURE_LOG}.\n{tail.strip()}"
    )


def stop_psbox_autocapture() -> str:
    running, report = psbox_autocapture_status()
    if not running:
        return report or "Captura automática já estava parada"

    try:
        lock_text = PSBOX_CAPTURE_LOCK.read_text(encoding="utf-8")
        match = re.search(r"\bpid=(\d+)\b", lock_text)
        pid = int(match.group(1)) if match else 0
    except (OSError, ValueError):
        pid = 0
    if pid <= 1:
        raise RuntimeError("Watcher ativo, mas não consegui ler o PID com segurança.")

    try:
        command_line = Path(f"/proc/{pid}/cmdline").read_bytes().replace(b"\0", b" ")
    except OSError as exc:
        raise RuntimeError(f"Não consegui validar o processo do watcher PID {pid}: {exc}") from exc
    if str(PSBOX_CAPTURE).encode() not in command_line or b"watch" not in command_line:
        raise RuntimeError(f"PID {pid} não corresponde ao watcher PSBOX; não encerrei nenhum processo.")

    os.kill(pid, signal.SIGTERM)
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        time.sleep(0.2)
        running, report = psbox_autocapture_status()
        if not running:
            return f"Captura automática encerrada (PID {pid})"
    raise RuntimeError(f"Watcher PID {pid} não encerrou; veja {PSBOX_CAPTURE_LOG}.")


def ensure_klog_daemon(host: str, port: int = 3232) -> str:
    if not host:
        raise ValueError("Informe o endereço do PS4 para iniciar o klog-center.")
    running, report = klog_status()
    if running:
        return report or "klog daemon ativo"
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    daemon_log = LOG_DIR / "klog-daemon.log"
    output = daemon_log.open("ab", buffering=0)
    subprocess.Popen([sys.executable, str(KLOG_CENTER), "daemon", "--host", host,
                      "--port", str(port)], stdin=subprocess.DEVNULL,
                     stdout=output, stderr=subprocess.STDOUT, start_new_session=True,
                     close_fds=True)
    output.close()
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        time.sleep(0.25)
        running, report = klog_status()
        if running:
            return report or "klog daemon iniciado"
    raise RuntimeError(f"Não consegui iniciar klog-center. Veja {daemon_log}")


def start_klog_capture(name: str, build: str = "PS4-Link", kind: str = "app",
                       filters: str = "install,bgft,package",
                       host: str = "", port: int = 3232) -> tuple[subprocess.Popen, Path]:
    if not host:
        raise ValueError("Informe o endereço do PS4 para iniciar a captura klog.")
    ensure_klog_daemon(host, port)
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    capture_log = (LOG_DIR / f"klog-capture-{int(time.time())}-{uuid.uuid4().hex[:6]}.out").open("ab", buffering=0)
    command = [sys.executable, str(KLOG_CENTER), "capture", "--kind", kind,
               "--name", name, "--build", build, "--filter", filters]
    proc = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=capture_log, text=True, start_new_session=True,
                            bufsize=1, close_fds=True)
    assert proc.stdout is not None
    line = proc.stdout.readline()
    match = re.search(r"Capturando em:\s*(.+?)\s*$", line)
    if not match:
        proc.terminate()
        proc.wait(timeout=3)
        capture_log.close()
        raise RuntimeError(f"klog-center não iniciou captura: {line.strip()}")
    capture_path = Path(match.group(1))

    def drain() -> None:
        try:
            assert proc.stdout is not None
            for line in proc.stdout:
                capture_log.write(line.encode("utf-8", errors="replace"))
        finally:
            capture_log.close()

    threading.Thread(target=drain, daemon=True).start()
    return proc, capture_path


def stop_klog_capture(proc: subprocess.Popen | None, timeout: float = 6) -> None:
    if proc is None or proc.poll() is not None:
        return
    try:
        os.killpg(proc.pid, signal.SIGINT)
        proc.wait(timeout=timeout)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass


def can_connect(host: str, port: int, timeout: float = 3) -> bool:
    with socket.create_connection((host, port), timeout=timeout):
        return True


def discover_ps4_hosts(preferred_host: str = "",
                       ftp_port: int = 2121, timeout: float = 0.35) -> list[dict]:
    """Find local hosts exposing a GoldHEN-style FTP greeting on the expected port."""
    networks: list[ipaddress.IPv4Network] = []
    local_ips: set[ipaddress.IPv4Address] = set()
    try:
        result = subprocess.run(["ip", "-j", "-4", "addr", "show", "up"],
                                text=True, capture_output=True, timeout=3, check=True)
        for interface in json.loads(result.stdout):
            if "LOOPBACK" in interface.get("flags", []):
                continue
            for address in interface.get("addr_info", []):
                if address.get("family") != "inet":
                    continue
                ip = ipaddress.ip_address(address["local"])
                if not ip.is_private or ip.is_loopback or ip.is_link_local:
                    continue
                local_ips.add(ip)
                network = ipaddress.ip_network(f"{ip}/{address['prefixlen']}", strict=False)
                # Keep discovery bounded on broad LANs; scan the local /24 containing
                # this interface address instead of thousands of unrelated hosts.
                if network.num_addresses > 256:
                    network = ipaddress.ip_network(f"{ip}/24", strict=False)
                if network.num_addresses > 2 and network not in networks:
                    networks.append(network)
    except (OSError, subprocess.SubprocessError, ValueError, KeyError, TypeError,
            json.JSONDecodeError):
        pass

    # If `ip` is unavailable, infer the active interface from the route to the
    # saved/default PS4 address and scan only that local /24.
    if not networks:
        try:
            source_ip = ipaddress.ip_address(_local_ip_for(preferred_host, ftp_port))
            if source_ip.is_private and not source_ip.is_loopback and not source_ip.is_link_local:
                local_ips.add(source_ip)
                networks.append(ipaddress.ip_network(f"{source_ip}/24", strict=False))
        except OSError:
            pass
    if not networks:
        raise RuntimeError("Não consegui identificar uma rede IPv4 privada ativa para procurar o PS4.")

    preferred_source = None
    try:
        preferred_source = ipaddress.ip_address(_local_ip_for(preferred_host, ftp_port))
    except OSError:
        pass
    networks.sort(key=lambda net: 0 if preferred_source is not None and preferred_source in net else 1)
    candidates: set[str] = set()
    for network in networks[:4]:
        candidates.update(str(address) for address in network.hosts() if address not in local_ips)
    if not candidates:
        raise RuntimeError("Não encontrei endereços locais para verificar.")

    def probe_ftp(host: str) -> tuple[str, bool]:
        try:
            with socket.create_connection((host, ftp_port), timeout=timeout) as conn:
                conn.settimeout(timeout)
                return host, conn.recv(256).startswith(b"220")
        except OSError:
            return host, False

    found: list[str] = []
    with ThreadPoolExecutor(max_workers=64) as executor:
        futures = [executor.submit(probe_ftp, host) for host in candidates]
        for future in as_completed(futures):
            host, has_ftp_greeting = future.result()
            if has_ftp_greeting:
                found.append(host)

    def port_open(host: str, port: int) -> bool:
        try:
            with socket.create_connection((host, port), timeout=timeout):
                return True
        except OSError:
            return False

    records = []
    for host in sorted(found, key=lambda value: tuple(int(part) for part in value.split("."))):
        with ThreadPoolExecutor(max_workers=2) as executor:
            klog_future = executor.submit(port_open, host, 3232)
            rpi_future = executor.submit(port_open, host, 12800)
            records.append({"host": host, "ftp_port": ftp_port,
                            "klog": klog_future.result(), "rpi": rpi_future.result()})
    return records


def _local_ip_for(host: str, port: int) -> str:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect((host, port))
        return sock.getsockname()[0]
    finally:
        sock.close()


class PackageHttpServer:
    def __init__(self, pkg: Path, bind_ip: str, port: int = 9898,
                 request_callback: Callable[[dict], None] | None = None):
        self.pkg = pkg.resolve(strict=True)
        self.pkg_size = self.pkg.stat().st_size
        # Use a plain package path in the URL. RPI/BGFT consumes this URL again
        # after its metadata probe; some versions drop the query string on that
        # second request. Keeping the route self-contained avoids a stale 404.
        self.route = f"/{quote(self.pkg.name, safe='')}"
        self._request_callback = request_callback
        self._requests: list[dict] = []
        self._request_lock = threading.Lock()
        self._body_bytes_written = 0
        owner = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, fmt, *args):
                return

            def do_HEAD(self):
                self._serve(head_only=True)

            def do_GET(self):
                self._serve(head_only=False)

            def _serve(self, head_only: bool):
                started = time.monotonic()
                range_header = self.headers.get("Range")
                status = 500
                response_length = 0
                body_bytes_written = 0
                error = None
                parsed = urlparse(self.path)
                try:
                    # Only serve this operation's one PKG. Accept a query only
                    # for compatibility; selection is determined by the exact
                    # basename in the path, so a lost query cannot break BGFT.
                    requested_name = unquote(parsed.path.lstrip("/"))
                    if requested_name != owner.pkg.name:
                        status = 404
                        self.send_error(status)
                        return

                    start, end = 0, owner.pkg_size - 1
                    if range_header:
                        match = re.fullmatch(r"bytes=(\d*)-(\d*)", range_header.strip())
                        if not match:
                            status = 416
                            self.send_response(status)
                            self.send_header("Content-Range", f"bytes */{owner.pkg_size}")
                            self.send_header("Content-Length", "0")
                            self.end_headers()
                            return
                        left, right = match.groups()
                        if left:
                            start = int(left)
                            end = int(right) if right else end
                        elif right:
                            suffix = int(right)
                            start = max(0, owner.pkg_size - suffix)
                        if start >= owner.pkg_size or end < start:
                            status = 416
                            self.send_response(status)
                            self.send_header("Content-Range", f"bytes */{owner.pkg_size}")
                            self.send_header("Content-Length", "0")
                            self.end_headers()
                            return
                        end = min(end, owner.pkg_size - 1)

                    response_length = end - start + 1
                    status = 206 if range_header else 200
                    self.send_response(status)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Length", str(response_length))
                    self.send_header("Accept-Ranges", "bytes")
                    self.send_header("Connection", "Keep-Alive")
                    self.send_header("Content-Disposition", f'attachment; filename="{owner.pkg.name}"')
                    self.send_header("Last-Modified", formatdate(owner.pkg.stat().st_mtime, usegmt=True))
                    if range_header:
                        self.send_header("Content-Range", f"bytes {start}-{end}/{owner.pkg_size}")
                    self.end_headers()
                    if head_only:
                        return

                    with owner.pkg.open("rb") as source:
                        source.seek(start)
                        remaining = response_length
                        while remaining:
                            block = source.read(min(1024 * 1024, remaining))
                            if not block:
                                break
                            written = self.wfile.write(block)
                            if written != len(block):
                                raise OSError(f"short HTTP write: {written}/{len(block)}")
                            position = response_length - remaining
                            owner.record_range(start + position, start + position + written - 1)
                            body_bytes_written += written
                            remaining -= written
                except (BrokenPipeError, ConnectionResetError, OSError) as exc:
                    error = f"{type(exc).__name__}: {exc}"
                finally:
                    owner.record_request({
                        "method": "HEAD" if head_only else "GET",
                        "client_ip": self.client_address[0],
                        "path": parsed.path,
                        "range": range_header,
                        "status": status,
                        "response_length": response_length,
                        "body_bytes_written": body_bytes_written,
                        "elapsed_ms": round((time.monotonic() - started) * 1000, 3),
                        "error": error,
                    })

        self._intervals: list[tuple[int, int]] = []
        self._interval_lock = threading.Lock()
        try:
            self.server = ThreadingHTTPServer((bind_ip, port), Handler)
        except OSError as exc:
            if exc.errno == errno.EADDRINUSE:
                raise RuntimeError(f"A porta HTTP {port} já está em uso; escolha outra porta para o servidor do PKG.") from exc
            raise
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_address[1]

    def record_range(self, start: int, end: int) -> None:
        with self._interval_lock:
            intervals = sorted(self._intervals + [(start, end)])
            merged: list[tuple[int, int]] = []
            for left, right in intervals:
                if merged and left <= merged[-1][1] + 1:
                    merged[-1] = (merged[-1][0], max(merged[-1][1], right))
                else:
                    merged.append((left, right))
            self._intervals = merged

    def record_request(self, record: dict) -> None:
        with self._request_lock:
            self._requests.append(record)
            self._body_bytes_written += int(record["body_bytes_written"])
        if self._request_callback:
            try:
                self._request_callback(record)
            except Exception:
                pass

    @property
    def request_summary(self) -> dict:
        with self._request_lock:
            return {
                "request_count": len(self._requests),
                "last_request": self._requests[-1] if self._requests else None,
                "body_bytes_written_total": self._body_bytes_written,
            }

    @property
    def bytes_served_unique(self) -> int:
        with self._interval_lock:
            return sum(right - left + 1 for left, right in self._intervals)

    def close(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


def _rpi_post(host: str, port: int, endpoint: str, data: dict, timeout: float = 5) -> dict:
    body = json.dumps(data).encode("utf-8")
    request = Request(f"http://{host}:{port}{endpoint}", data=body,
                      headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urlopen(request, timeout=timeout) as response:
            raw = response.read().decode("utf-8", errors="replace")
    except HTTPError as exc:
        raw = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"RPI HTTP {exc.code}: {raw}") from exc
    except URLError as exc:
        if isinstance(exc.reason, (TimeoutError, socket.timeout)):
            raise TimeoutError(f"RPI {endpoint} timed out after {timeout:g}s") from exc
        raise RuntimeError(f"RPI {endpoint} request failed: {exc.reason}") from exc
    except (TimeoutError, socket.timeout) as exc:
        raise TimeoutError(f"RPI {endpoint} timed out after {timeout:g}s") from exc
    except OSError as exc:
        raise RuntimeError(f"RPI {endpoint} request failed: {exc}") from exc
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        raise RuntimeError(f"RPI returned non-JSON: {raw[:500]}") from None


def install_pkg_experimental(pkg_path: Path, ps4_host: str, rpi_port: int = 12800,
                             progress: Callable[[int, int], None] | None = None,
                             log: Callable[[str], None] | None = None,
                             timeout_seconds: int = 4 * 60 * 60,
                             klog_host: str | None = None,
                             klog_port: int = 3232,
                             request_timeout_seconds: int = 120,
                             http_port: int = 9898) -> dict:
    """Submit a local PKG to Remote Package Installer and preserve a detailed log."""
    pkg = pkg_path.expanduser().resolve(strict=True)
    if not pkg.is_file() or pkg.suffix.lower() != ".pkg":
        raise ValueError("O instalador exige um arquivo .pkg local.")
    if request_timeout_seconds < 1:
        raise ValueError("O timeout da API RPI deve ser pelo menos 1 segundo.")
    if not 1 <= http_port <= 65535:
        raise ValueError("A porta HTTP local precisa estar entre 1 e 65535.")
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().astimezone().strftime("%Y%m%d-%H%M%S%z")
    log_path = LOG_DIR / f"install-{stamp}-{pkg.stem[:48]}.jsonl"

    def emit(event: str, **fields) -> None:
        record = {"time": datetime.now().astimezone().isoformat(timespec="milliseconds"),
                  "event": event, **fields}
        with log_path.open("a", encoding="utf-8") as out:
            out.write(json.dumps(record, ensure_ascii=False) + "\n")
        if log:
            log(json.dumps(record, ensure_ascii=False))

    capture_proc = None
    capture_path = None
    server = None
    phase = "starting"
    emit("begin", pkg=str(pkg), bytes=pkg.stat().st_size,
         ps4_host=ps4_host, rpi_port=rpi_port,
         request_timeout_seconds=request_timeout_seconds, http_port=http_port)
    try:
        phase = "starting_klog_capture"
        capture_proc, capture_path = start_klog_capture(
            f"PKG-{pkg.stem[:40]}", build="PS4-Link-auto-install",
            kind="app", filters="bgft,install,package,appinst",
            host=klog_host or ps4_host, port=klog_port)
        emit("klog_capture_started", path=str(capture_path))
        phase = "starting_http_server"
        local_ip = _local_ip_for(ps4_host, rpi_port)
        server = PackageHttpServer(
            pkg, "0.0.0.0", http_port,
            request_callback=lambda request: emit("pkg_http_request", **request))
        package_url = f"http://{local_ip}:{server.port}{server.route}"
        emit("http_server_ready", local_ip=local_ip, port=server.port, url=package_url,
             bind="0.0.0.0", only_pkg_exposed=pkg.name,
             route=f"/{quote(pkg.name, safe='')}")
        if progress:
            progress(0, server.pkg_size)

        with install_operation_lock():
            phase = "rpi_install_api"
            emit("install_api_request_started", endpoint="/api/install",
                 timeout_seconds=request_timeout_seconds)
            response = _rpi_post(ps4_host, rpi_port, "/api/install",
                                {"type": "direct", "packages": [quote(package_url, safe="")]},
                                timeout=request_timeout_seconds)
            emit("install_api_response", response=response)
            if response.get("status") != "success":
                raise RuntimeError(f"RPI rejeitou a instalação: {response}")
            task_id = response.get("task_id")
            if not isinstance(task_id, int):
                raise RuntimeError(f"RPI não retornou task_id: {response}")
            emit("task_registered", task_id=task_id, title=response.get("title"))

            deadline = time.monotonic() + timeout_seconds
            phase = "serving_pkg"
            last_progress = -1
            transfer_done = False
            while time.monotonic() < deadline:
                served = server.bytes_served_unique
                if progress:
                    progress(served, server.pkg_size)
                if served >= server.pkg_size:
                    transfer_done = True
                    emit("pkg_http_transfer_complete", bytes=served)
                    break
                if served != last_progress:
                    emit("pkg_http_transfer_progress", bytes=served, total=server.pkg_size)
                    last_progress = served
                time.sleep(0.25)
            if not transfer_done:
                raise TimeoutError("O PS4 não terminou de buscar o PKG no tempo limite.")

            # The package has been delivered to BGFT. RPI's progress endpoint can
            # temporarily block while it serves the package; query only afterward.
            final_progress = None
            poll_deadline = time.monotonic() + min(timeout_seconds, 30 * 60)
            phase = "polling_bgft"
            while time.monotonic() < poll_deadline:
                try:
                    state = _rpi_post(ps4_host, rpi_port, "/api/get_task_progress",
                                      {"task_id": task_id}, timeout=request_timeout_seconds)
                    emit("bgft_progress", task_id=task_id, response=state)
                    final_progress = state
                    if state.get("status") != "success":
                        raise RuntimeError(f"RPI falhou ao consultar o progresso: {state}")
                    if int(state.get("error", 0)) != 0:
                        raise RuntimeError(f"BGFT reportou erro: {state}")
                    total = int(state.get("length_total", 0) or 0)
                    transferred = int(state.get("transferred_total", 0) or 0)
                    prep = int(state.get("preparing_percent", 0) or 0)
                    copy = int(state.get("local_copy_percent", 0) or 0)
                    percent = max(int(transferred * 100 / total) if total else 0, prep, copy)
                    if progress:
                        progress(min(100, max(0, percent)), 100)
                    if total > 0 and transferred >= total and prep >= 100 and copy >= 100:
                        emit("bgft_complete", task_id=task_id, response=state)
                        return {"task_id": task_id, "log": str(log_path),
                                "klog": str(capture_path), "state": state,
                                "status": "complete"}
                except RuntimeError:
                    raise
                except Exception as exc:
                    emit("bgft_progress_unavailable", task_id=task_id, error=str(exc))
                time.sleep(4)
            emit("bgft_final_state_unconfirmed", task_id=task_id, last_response=final_progress)
            return {"task_id": task_id, "log": str(log_path), "klog": str(capture_path),
                    "state": final_progress, "status": "submitted_unconfirmed"}
    except Exception as exc:
        emit("failure", phase=phase, type=type(exc).__name__, error=str(exc),
             pkg_bytes_served=server.bytes_served_unique if server is not None else 0,
             pkg_bytes_total=server.pkg_size if server is not None else 0,
             pkg_http=server.request_summary if server is not None else None)
        raise RuntimeError(f"{exc}\nLog: {log_path}") from exc
    finally:
        if server is not None:
            server.close()
        stop_klog_capture(capture_proc)
        if capture_path:
            emit("klog_capture_stopped", path=str(capture_path))

