#!/usr/bin/env python3
"""Linux desktop UI for PSBOX logs and PS4 package management over GoldHEN FTP."""

from __future__ import annotations

import json
import ftplib
import sys
from pathlib import Path, PurePosixPath
import uuid

from PyQt6.QtCore import QObject, QRunnable, QThreadPool, Qt, pyqtSignal, pyqtSlot
from PyQt6.QtWidgets import (
    QApplication, QComboBox, QFileDialog, QFrame, QGridLayout, QHeaderView,
    QLabel, QInputDialog, QLineEdit, QListWidget, QListWidgetItem, QMainWindow, QMessageBox,
    QCheckBox, QPlainTextEdit, QProgressBar, QPushButton, QTableWidget, QTableWidgetItem,
    QTabWidget, QVBoxLayout, QWidget,
)
from PyQt6.QtGui import QDesktopServices, QGuiApplication
from PyQt6.QtCore import QUrl

from core import (KLOG_ROOT, close_ftp, connect_ftp,
                  console_operation_lock, ensure_klog_daemon, install_pkg_experimental,
                  discover_ps4_hosts, list_pkg_files, list_remote_files_preserving_directories,
                  start_klog_capture,
                  stop_klog_capture, klog_status, ensure_psbox_autocapture,
                  stop_psbox_autocapture)

APP_NAME = "PSBOX PS4 Manager"
APP_VERSION = "0.1"
DEFAULT_HOST = ""
DEFAULT_PORT = "2121"
DEFAULT_REMOTE_DIR = "/data/pkg"
CONFIG_PATH = Path.home() / ".config" / "psbox-pc-manager" / "config.json"


class Signals(QObject):
    progress = pyqtSignal(int, int)
    result = pyqtSignal(object)
    error = pyqtSignal(str)


class Task(QRunnable):
    def __init__(self, action):
        super().__init__()
        self.action = action
        self.signals = Signals()

    def run(self) -> None:
        try:
            self.signals.result.emit(self.action(self.signals.progress.emit))
        except Exception as exc:
            self.signals.error.emit(f"{type(exc).__name__}: {exc}")


class Ps4Link(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle(f"{APP_NAME} {APP_VERSION}")
        self.resize(920, 610)
        self.setMinimumSize(760, 480)
        self.pool = QThreadPool.globalInstance()
        self.files: dict[str, int] = {}
        self.busy = False
        self.settings: tuple[str, int, str, str, str] | None = None
        self.active_task: Task | None = None
        self.active_result_handler = None
        self.capture_processes = {}
        self._build_ui()
        self._load_config()
        self._apply_theme(self.dark_mode_checkbox.isChecked())
        self.psbox_autocapture_checkbox.toggled.connect(self._psbox_autocapture_toggled)
        self._configure_psbox_autocapture(self.psbox_autocapture_checkbox.isChecked())

    def _build_ui(self) -> None:
        root = QWidget()
        layout = QVBoxLayout(root)
        layout.setContentsMargins(20, 18, 20, 18)
        layout.setSpacing(12)

        title = QLabel("PSBOX PS4 Manager")
        title.setObjectName("title")
        subtitle = QLabel("Gerencie PKGs e capture automaticamente logs das sessões PSBOX.  •  versão 0.1")
        subtitle.setObjectName("muted")
        layout.addWidget(title)
        layout.addWidget(subtitle)

        connection = QFrame()
        connection.setObjectName("card")
        grid = QGridLayout(connection)
        grid.setContentsMargins(13, 11, 13, 12)
        grid.setHorizontalSpacing(10)
        self.host = QLineEdit()
        self.port = QLineEdit()
        self.user = QLineEdit()
        self.password = QLineEdit()
        self.password.setEchoMode(QLineEdit.EchoMode.Password)
        self.remote_dir = QLineEdit()
        self.http_port = QLineEdit("9898")
        fields = [("Endereço do PS4", self.host), ("Porta FTP", self.port),
                  ("Usuário", self.user), ("Senha", self.password),
                  ("Pasta remota", self.remote_dir)]
        for col, (label, edit) in enumerate(fields):
            grid.addWidget(QLabel(label), 0, col)
            grid.addWidget(edit, 1, col)
            grid.setColumnStretch(col, (3, 1, 2, 2, 3)[col])
        self.connect_button = QPushButton("Conectar e atualizar")
        self.connect_button.clicked.connect(self.refresh)
        self.detect_button = QPushButton("Detectar PS4 na rede")
        self.detect_button.setToolTip("Procura banners FTP na rede local e verifica klog/RPI nos aparelhos encontrados.")
        self.detect_button.clicked.connect(self.detect_ps4)
        self.status = QLabel("Desconectado")
        self.status.setObjectName("muted")
        grid.addWidget(self.connect_button, 2, 0, 1, 2)
        grid.addWidget(self.detect_button, 2, 2)
        grid.addWidget(self.status, 2, 3, 1, 2)
        grid.addWidget(QLabel("Porta HTTP do PC para o RPI"), 3, 0)
        grid.addWidget(self.http_port, 3, 1)
        grid.addWidget(QLabel("A porta precisa aceitar conexões do PS4."), 3, 2, 1, 3)
        card = QFrame()
        card.setObjectName("card")
        card_layout = QVBoxLayout(card)
        card_layout.setContentsMargins(12, 10, 12, 12)
        heading = QWidget()
        heading_layout = QGridLayout(heading)
        heading_layout.setContentsMargins(0, 0, 0, 5)
        heading_label = QLabel("PKGs no console")
        heading_label.setStyleSheet("font-size: 12pt; font-weight: 700;")
        self.count_label = QLabel("0 arquivos")
        self.count_label.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        heading_layout.addWidget(heading_label, 0, 0)
        heading_layout.addWidget(self.count_label, 0, 1)
        card_layout.addWidget(heading)

        self.table = QTableWidget(0, 2)
        self.table.setHorizontalHeaderLabels(["Arquivo", "Tamanho"])
        self.table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self.table.setSelectionMode(QTableWidget.SelectionMode.ExtendedSelection)
        self.table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.table.verticalHeader().setVisible(False)
        self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        self.table.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeMode.ResizeToContents)
        card_layout.addWidget(self.table, 1)
        layout.addWidget(card, 1)

        actions = QWidget()
        actions_layout = QGridLayout(actions)
        actions_layout.setContentsMargins(0, 0, 0, 0)
        self.upload_button = QPushButton("Enviar arquivo…")
        self.upload_button.setObjectName("primary")
        self.install_button = QPushButton("Instalar PKG (experimental)…")
        self.download_button = QPushButton("Baixar selecionado")
        self.delete_button = QPushButton("Excluir selecionado")
        self.refresh_button = QPushButton("Atualizar lista")
        self.clean_button = QPushButton("Limpar builds antigas")
        self.progress = QProgressBar()
        self.progress.setRange(0, 100)
        self.progress.setValue(0)
        self.upload_button.clicked.connect(self.upload)
        self.install_button.clicked.connect(self.install)
        self.download_button.clicked.connect(self.download)
        self.delete_button.clicked.connect(self.delete)
        self.refresh_button.clicked.connect(self.refresh)
        self.clean_button.clicked.connect(self.clean_builds)
        for col, button in enumerate((self.upload_button, self.install_button, self.download_button,
                                      self.delete_button, self.refresh_button)):
            actions_layout.addWidget(button, 0, col)
        actions_layout.addWidget(self.progress, 0, 5)
        actions_layout.setColumnStretch(5, 1)
        actions_layout.addWidget(self.clean_button, 1, 0)
        clean_hint = QLabel("Apaga arquivos da pasta remota configurada; preserva todas as pastas.")
        clean_hint.setObjectName("muted")
        actions_layout.addWidget(clean_hint, 1, 1, 1, 5)
        layout.addWidget(actions)

        tabs = QTabWidget()
        tabs.addTab(root, "PKGs")
        tabs.addTab(self._build_klog_tab(), "Klog")
        tabs.addTab(self._build_agent_tab(), "Agentes")
        tabs.addTab(self._build_settings_tab(connection), "Configurações")
        self.setCentralWidget(tabs)

    def _build_settings_tab(self, connection: QFrame) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(18, 16, 18, 16)
        layout.setSpacing(12)
        title = QLabel("Configurações")
        title.setStyleSheet("font-size: 15pt; font-weight: 700;")
        layout.addWidget(title)
        layout.addWidget(connection)

        appearance = QFrame()
        appearance.setObjectName("card")
        appearance_layout = QVBoxLayout(appearance)
        appearance_layout.setContentsMargins(13, 11, 13, 12)
        appearance_layout.setSpacing(6)
        appearance_title = QLabel("Aparência")
        appearance_title.setStyleSheet("font-weight: 700;")
        self.dark_mode_checkbox = QCheckBox("Usar tema escuro")
        self.dark_mode_checkbox.setChecked(True)
        self.dark_mode_checkbox.toggled.connect(self._on_theme_changed)
        appearance_layout.addWidget(appearance_title)
        appearance_layout.addWidget(self.dark_mode_checkbox)
        layout.addWidget(appearance)

        save_button = QPushButton("Salvar configurações")
        save_button.setObjectName("primary")
        save_button.clicked.connect(self._save_settings_clicked)
        layout.addWidget(save_button, 0, Qt.AlignmentFlag.AlignLeft)

        about = QLabel(f"PSBOX PS4 Manager {APP_VERSION}\nPreferências salvas em {CONFIG_PATH}")
        about.setObjectName("muted")
        layout.addWidget(about)
        layout.addStretch(1)
        return page

    def _apply_theme(self, dark: bool) -> None:
        if dark:
            style = """
                QMainWindow, QWidget { background: #11151c; color: #e6eaf0; font: 10pt 'Sans'; }
                QTabWidget::pane { border: 1px solid #2b3442; border-radius: 7px; top: -1px; }
                QTabBar::tab { background: #1a202a; color: #aeb8c7; padding: 9px 16px; margin-right: 3px; border-top-left-radius: 6px; border-top-right-radius: 6px; }
                QTabBar::tab:selected { background: #273244; color: #ffffff; }
                QFrame#card { background: #191f29; border: 1px solid #2b3442; border-radius: 9px; }
                QLabel#title { font-size: 20pt; font-weight: 700; color: #f0f4fa; }
                QLabel#muted { color: #9aa7b8; }
                QLineEdit, QComboBox, QPlainTextEdit, QListWidget { background: #10151d; color: #e6eaf0; border: 1px solid #354153; border-radius: 5px; padding: 7px; selection-background-color: #315d9b; }
                QComboBox QAbstractItemView { background: #191f29; color: #e6eaf0; selection-background-color: #315d9b; }
                QPushButton { background: #283241; color: #e6eaf0; border: 1px solid #394658; border-radius: 5px; padding: 9px 13px; font-weight: 600; }
                QPushButton:hover { background: #35445a; }
                QPushButton:disabled { color: #798597; background: #202733; }
                QPushButton#primary { color: white; background: #3169c6; border-color: #3169c6; }
                QPushButton#primary:hover { background: #3d79db; }
                QTableWidget { background: #151b24; alternate-background-color: #1a212c; color: #e6eaf0; border: 0; gridline-color: #283241; selection-background-color: #315d9b; selection-color: white; }
                QHeaderView::section { background: #202936; color: #dce3ed; border: 0; border-bottom: 1px solid #354153; padding: 8px; font-weight: 700; }
                QProgressBar { border: 1px solid #354153; border-radius: 5px; background: #10151d; color: #e6eaf0; text-align: center; min-height: 16px; }
                QProgressBar::chunk { background: #3976d2; border-radius: 4px; }
                QCheckBox { spacing: 8px; }
                QToolTip { background: #202936; color: #f0f4fa; border: 1px solid #46556a; }
            """
        else:
            style = """
                QMainWindow, QWidget { background: #f2f4f7; color: #172033; font: 10pt 'Sans'; }
                QTabWidget::pane { border: 1px solid #dfe4ec; border-radius: 7px; top: -1px; }
                QTabBar::tab { background: #e7ebf1; color: #475467; padding: 9px 16px; margin-right: 3px; border-top-left-radius: 6px; border-top-right-radius: 6px; }
                QTabBar::tab:selected { background: white; color: #172033; }
                QFrame#card { background: white; border: 1px solid #e1e6ee; border-radius: 9px; }
                QLabel#title { font-size: 20pt; font-weight: 700; color: #172b4d; }
                QLabel#muted { color: #667085; }
                QLineEdit, QComboBox, QPlainTextEdit, QListWidget { background: white; color: #172033; border: 1px solid #cbd3df; border-radius: 5px; padding: 7px; }
                QPushButton { background: #e9edf3; color: #172033; border: 0; border-radius: 5px; padding: 9px 13px; font-weight: 600; }
                QPushButton:hover { background: #dce4ef; }
                QPushButton:disabled { color: #8a93a2; background: #e9edf3; }
                QPushButton#primary { color: white; background: #2457c5; }
                QPushButton#primary:hover { background: #1947a8; }
                QTableWidget { background: white; alternate-background-color: #f7f8fa; color: #172033; border: 0; gridline-color: #edf0f4; selection-background-color: #dce8ff; selection-color: #172033; }
                QHeaderView::section { background: #f7f8fa; color: #172033; border: 0; border-bottom: 1px solid #dfe4ec; padding: 8px; font-weight: 700; }
                QProgressBar { border: 1px solid #d5dbe5; border-radius: 5px; background: white; color: #172033; text-align: center; min-height: 16px; }
                QProgressBar::chunk { background: #2e65d1; border-radius: 4px; }
            """
        app = QApplication.instance()
        if app:
            app.setStyleSheet(style)

    def _on_theme_changed(self, dark: bool) -> None:
        self._apply_theme(dark)
        self._save_config()

    def _save_settings_clicked(self) -> None:
        try:
            self.settings = self._settings_from_ui()
            http_port = int(self.http_port.text().strip())
            if not 1 <= http_port <= 65535:
                raise ValueError("A porta HTTP precisa estar entre 1 e 65535.")
            self._save_config()
        except ValueError as exc:
            QMessageBox.warning(self, "Configurações", str(exc))
            return
        QMessageBox.information(self, "Configurações", "Preferências salvas.")

    def _build_klog_tab(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(18, 16, 18, 16)
        layout.setSpacing(10)
        title = QLabel("Captura compartilhada do klog")
        title.setStyleSheet("font-size: 15pt; font-weight: 700;")
        layout.addWidget(title)
        layout.addWidget(QLabel("Uma conexão GoldHEN alimenta o arquivo bruto; cada captura é um recorte independente."))

        daemon_row = QGridLayout()
        self.klog_host = QLineEdit(DEFAULT_HOST)
        self.klog_port = QLineEdit("3232")
        self.klog_status_label = QLabel("Estado desconhecido")
        self.klog_check_button = QPushButton("Verificar daemon")
        self.klog_start_button = QPushButton("Iniciar daemon compartilhado")
        self.klog_check_button.clicked.connect(self.check_klog)
        self.klog_start_button.clicked.connect(self.start_klog_daemon)
        daemon_row.addWidget(QLabel("PS4"), 0, 0)
        daemon_row.addWidget(self.klog_host, 0, 1)
        daemon_row.addWidget(QLabel("Porta klog"), 0, 2)
        daemon_row.addWidget(self.klog_port, 0, 3)
        daemon_row.addWidget(self.klog_check_button, 0, 4)
        daemon_row.addWidget(self.klog_start_button, 0, 5)
        daemon_row.addWidget(self.klog_status_label, 1, 0, 1, 6)
        daemon_row.setColumnStretch(1, 2)
        layout.addLayout(daemon_row)

        auto_frame = QFrame()
        auto_frame.setObjectName("card")
        auto_layout = QVBoxLayout(auto_frame)
        auto_layout.setContentsMargins(12, 9, 12, 10)
        self.psbox_autocapture_checkbox = QCheckBox("Capturar automaticamente cada execução do PSBOX")
        self.psbox_autocapture_checkbox.setChecked(True)
        self.psbox_autocapture_status = QLabel("As sessões serão salvas na pasta local de dados do PSBOX PC Manager")
        self.psbox_autocapture_status.setObjectName("muted")
        open_runs_button = QPushButton("Abrir pasta das sessões")
        open_runs_button.clicked.connect(self._open_psbox_runs)
        auto_layout.addWidget(self.psbox_autocapture_checkbox)
        auto_layout.addWidget(self.psbox_autocapture_status)
        auto_layout.addWidget(open_runs_button, 0, Qt.AlignmentFlag.AlignLeft)
        layout.addWidget(auto_frame)

        capture_frame = QFrame()
        capture_frame.setObjectName("card")
        form = QGridLayout(capture_frame)
        form.setContentsMargins(12, 10, 12, 10)
        self.capture_kind = QComboBox()
        self.capture_kind.addItem("App", "app")
        self.capture_kind.addItem("Jogo/dependência", "game-dependency")
        self.capture_kind.addItem("Personalizado", "custom")
        self.capture_name = QLineEdit("PS4-Link")
        self.capture_build = QLineEdit("PS4-Link")
        self.capture_filter = QLineEdit("install,bgft,package")
        self.capture_filter.setPlaceholderText("Filtro opcional; termos separados por vírgula")
        self.capture_start_button = QPushButton("Iniciar captura")
        self.capture_start_button.clicked.connect(self.begin_capture)
        form.addWidget(QLabel("Categoria"), 0, 0)
        form.addWidget(self.capture_kind, 0, 1)
        form.addWidget(QLabel("Nome"), 0, 2)
        form.addWidget(self.capture_name, 0, 3)
        form.addWidget(QLabel("Build"), 1, 0)
        form.addWidget(self.capture_build, 1, 1)
        form.addWidget(QLabel("Filtro"), 1, 2)
        form.addWidget(self.capture_filter, 1, 3)
        form.addWidget(self.capture_start_button, 0, 4, 2, 1)
        form.setColumnStretch(3, 2)
        layout.addWidget(capture_frame)

        layout.addWidget(QLabel("Capturas recentes — sessões independentes de agentes também aparecem aqui:"))
        self.capture_list = QListWidget()
        layout.addWidget(self.capture_list, 1)
        buttons = QGridLayout()
        self.capture_refresh_button = QPushButton("Atualizar capturas")
        self.capture_open_button = QPushButton("Abrir selecionada")
        self.capture_stop_button = QPushButton("Parar minha captura selecionada")
        self.capture_refresh_button.clicked.connect(self.refresh_captures)
        self.capture_open_button.clicked.connect(self.open_capture)
        self.capture_stop_button.clicked.connect(self.stop_capture)
        buttons.addWidget(self.capture_refresh_button, 0, 0)
        buttons.addWidget(self.capture_open_button, 0, 1)
        buttons.addWidget(self.capture_stop_button, 0, 2)
        layout.addLayout(buttons)
        self.refresh_captures()
        return page

    def _build_agent_tab(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(18, 16, 18, 16)
        layout.setSpacing(10)
        title = QLabel("Interface local para agentes e scripts")
        title.setStyleSheet("font-size: 15pt; font-weight: 700;")
        layout.addWidget(title)
        layout.addWidget(QLabel("Use a CLI abaixo. Ela compartilha as preferências do app e serializa operações de PKG entre processos."))
        examples = QPlainTextEdit()
        examples.setReadOnly(True)
        examples.setPlainText(
            "# Listar PKGs\n"
            "python3 tools/ps4/ps4-pc-manager/agent_cli.py pkg list --json\n\n"
            "# Enviar PKG (não substitui sem confirmação explícita)\n"
            "python3 tools/ps4/ps4-pc-manager/agent_cli.py pkg upload ./build.pkg\n\n"
            "# Substituir PKG existente\n"
            "python3 tools/ps4/ps4-pc-manager/agent_cli.py pkg upload ./build.pkg --replace\n\n"
            "# Excluir apenas um PKG validado\n"
            "python3 tools/ps4/ps4-pc-manager/agent_cli.py pkg delete build.pkg --confirm\n\n"
            "# Captura klog independente (pode haver várias em paralelo)\n"
            "python3 tools/ps4/ps4-pc-manager/agent_cli.py klog capture --name PSBOX --build teste-01 --filter XCLOUD,crash\n\n"
            "# Instalação remota experimental: exige endpoint RPI ativo e confirmação\n"
            "python3 tools/ps4/ps4-pc-manager/agent_cli.py install ./build.pkg --experimental --confirm"
        )
        layout.addWidget(examples, 1)
        copy_button = QPushButton("Copiar exemplos")
        copy_button.clicked.connect(lambda: QGuiApplication.clipboard().setText(examples.toPlainText()))
        layout.addWidget(copy_button)
        layout.addWidget(QLabel("A CLI não precisa da janela aberta. Todos usam o mesmo lock local para serializar operações de PKG."))
        return page

    def _load_config(self) -> None:
        try:
            data = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            data = {}
        self.host.setText(data.get("host", DEFAULT_HOST))
        self.port.setText(str(data.get("port", DEFAULT_PORT)))
        self.user.setText(data.get("user", "anonymous"))
        self.remote_dir.setText(data.get("remote_dir", DEFAULT_REMOTE_DIR))
        self.http_port.setText(str(data.get("http_port", 9898)))
        self.dark_mode_checkbox.setChecked(bool(data.get("dark_mode", True)))
        self.psbox_autocapture_checkbox.setChecked(bool(data.get("psbox_autocapture", True)))
        self.password.setText("")

    def _settings_from_ui(self) -> tuple[str, int, str, str, str]:
        host = self.host.text().strip()
        if not host:
            raise ValueError("Informe o endereço IP ou nome do PS4.")
        try:
            port = int(self.port.text().strip())
            if not 1 <= port <= 65535:
                raise ValueError
        except ValueError:
            raise ValueError("A porta precisa estar entre 1 e 65535.") from None
        remote_dir = self.remote_dir.text().strip() or DEFAULT_REMOTE_DIR
        if not remote_dir.startswith("/"):
            raise ValueError("A pasta remota precisa começar com /.")
        return host, port, self.user.text().strip() or "anonymous", self.password.text(), remote_dir

    def _save_config(self) -> None:
        host = self.host.text().strip() 
        user = self.user.text().strip() or "anonymous"
        remote_dir = self.remote_dir.text().strip() or DEFAULT_REMOTE_DIR
        try:
            port = int(self.port.text().strip())
            if not 1 <= port <= 65535:
                port = int(DEFAULT_PORT)
        except ValueError:
            port = int(DEFAULT_PORT)
        try:
            http_port = int(self.http_port.text().strip())
            if not 1 <= http_port <= 65535:
                http_port = 9898
        except ValueError:
            http_port = 9898
        CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
        CONFIG_PATH.write_text(json.dumps({"host": host, "port": port, "user": user,
                                           "remote_dir": remote_dir,
                                           "http_port": http_port,
                                           "dark_mode": self.dark_mode_checkbox.isChecked(),
                                           "psbox_autocapture": self.psbox_autocapture_checkbox.isChecked()},
                                          indent=2), encoding="utf-8")

    def _psbox_autocapture_toggled(self, enabled: bool) -> None:
        self._save_config()
        self._configure_psbox_autocapture(enabled)

    def _open_psbox_runs(self) -> None:
        path = KLOG_ROOT / "psbox"
        path.mkdir(parents=True, exist_ok=True)
        QDesktopServices.openUrl(QUrl.fromLocalFile(str(path)))

    def _configure_psbox_autocapture(self, enabled: bool) -> None:
        if enabled:
            klog_host = self.klog_host.text().strip() 
            ftp_host = self.host.text().strip() 
            if not klog_host or not ftp_host:
                self.psbox_autocapture_status.setText("Informe o endereço do PS4 para iniciar a captura automática.")
                return
            try:
                klog_port = int(self.klog_port.text().strip())
                ftp_port = int(self.port.text().strip() or DEFAULT_PORT)
                if not (1 <= klog_port <= 65535 and 1 <= ftp_port <= 65535):
                    raise ValueError
            except ValueError:
                self.psbox_autocapture_status.setText("Host ou porta inválida; captura automática não iniciada.")
                return

            def start(_progress):
                return ensure_psbox_autocapture(klog_host, klog_port, ftp_host, ftp_port)

            def started(message):
                self.psbox_autocapture_status.setText(
                    f"Ativa — {message}. Sessões em {KLOG_ROOT / 'psbox'}"
                )
                self.klog_status_label.setText("Captura automática do PSBOX ativa")
                self.refresh_captures()

            self._run_task("Ativando captura automática do PSBOX…", start, started,
                           requires_connection=False)
        else:
            def stop(_progress):
                return stop_psbox_autocapture()

            def stopped(message):
                self.psbox_autocapture_status.setText(
                    f"Desativada — {message}. Arquivos existentes em {KLOG_ROOT / 'psbox'}"
                )
                self.refresh_captures()

            self._run_task("Encerrando captura automática do PSBOX…", stop, stopped,
                           requires_connection=False)

    def _run_task(self, label: str, action, result_handler=None, *, requires_connection: bool = True) -> None:
        if self.busy:
            return
        if requires_connection:
            try:
                self.settings = self._settings_from_ui()
                self._save_config()
            except (OSError, ValueError) as exc:
                QMessageBox.critical(self, "Conexão", str(exc))
                return
        self.busy = True
        self.status.setText(label)
        self.progress.setValue(0)
        self._set_busy(True)
        task = Task(action)
        self.active_task = task
        self.active_result_handler = result_handler
        task.signals.progress.connect(self._show_progress)
        task.signals.error.connect(self._task_error)
        task.signals.result.connect(self._task_done)
        self.pool.start(task)

    def _set_busy(self, busy: bool) -> None:
        self.busy = busy
        for button in (self.connect_button, self.detect_button, self.upload_button, self.download_button,
                       self.install_button, self.delete_button, self.refresh_button, self.clean_button):
            button.setEnabled(not busy)
        self.psbox_autocapture_checkbox.setEnabled(not busy)

    def detect_ps4(self) -> None:
        try:
            ftp_port = int(self.port.text().strip())
            if not 1 <= ftp_port <= 65535:
                raise ValueError
        except ValueError:
            QMessageBox.warning(self, "Descoberta", "Informe uma porta FTP válida antes de procurar.")
            return

        preferred_host = self.host.text().strip() 

        def action(_progress):
            return discover_ps4_hosts(preferred_host, ftp_port)

        def found(records):
            if not records:
                self.status.setText("Nenhum serviço FTP compatível encontrado")
                QMessageBox.information(
                    self, "Busca concluída",
                    f"Nenhum servidor FTP respondeu na porta {ftp_port} nas redes IPv4 privadas locais. "
                    "Confira se o FTP do GoldHEN está ativo e se o PC está na mesma rede do PS4.",
                )
                return
            labels = []
            for record in records:
                services = [f"FTP {record['ftp_port']}"]
                if record["klog"]:
                    services.append("klog")
                if record["rpi"]:
                    services.append("RPI")
                labels.append(f"{record['host']} — {', '.join(services)}")
            selected = labels[0]
            if len(labels) > 1:
                selected, accepted = QInputDialog.getItem(
                    self, "Escolher PS4", "Aparelhos encontrados:", labels, 0, False)
                if not accepted:
                    return
            index = labels.index(selected)
            host = records[index]["host"]
            self.host.setText(host)
            self.klog_host.setText(host)
            self._save_config()
            services = labels[index].split(" — ", 1)[1]
            self.status.setText(f"Encontrado: {host} ({services})")

        self._run_task("Procurando PS4 na rede local…", action, found,
                       requires_connection=False)

    @pyqtSlot(int, int)
    def _show_progress(self, sent: int, total: int) -> None:
        percent = int(sent * 100 / total) if total else 0
        self.progress.setValue(max(0, min(100, percent)))
        if total == 100:
            self.status.setText(f"Instalação BGFT: {max(0, min(100, sent))}%")
        else:
            self.status.setText(f"Transferindo {self._size(sent)} / {self._size(total)}")

    @pyqtSlot(str)
    def _task_error(self, message: str) -> None:
        self._set_busy(False)
        self.active_task = None
        self.active_result_handler = None
        self.progress.setValue(0)
        self.status.setText("Falha na operação")
        QMessageBox.critical(self, "Erro de operação", message)

    @pyqtSlot(object)
    def _task_done(self, value) -> None:
        self._set_busy(False)
        self.active_task = None
        result_handler = self.active_result_handler
        self.active_result_handler = None
        self.progress.setValue(0)
        if result_handler:
            result_handler(value)
        else:
            self.status.setText(str(value))
            self.refresh()

    def check_klog(self) -> None:
        host = self.klog_host.text().strip()
        try:
            port = int(self.klog_port.text().strip())
        except ValueError:
            QMessageBox.warning(self, "Klog", "A porta precisa ser numérica.")
            return

        def show(result):
            running, report = result
            self.klog_status_label.setText(("Daemon ativo — " if running else "Daemon parado — ") + report)

        self._run_task("Consultando klog-center…", lambda _progress: klog_status(), show,
                       requires_connection=False)

    def start_klog_daemon(self) -> None:
        host = self.klog_host.text().strip()
        try:
            port = int(self.klog_port.text().strip())
            if not host or not 1 <= port <= 65535:
                raise ValueError
        except ValueError:
            QMessageBox.warning(self, "Klog", "Informe um host e uma porta entre 1 e 65535.")
            return

        def show(report):
            self.klog_status_label.setText(report)
            self.refresh_captures()

        self._run_task("Iniciando daemon compartilhado…",
                       lambda _progress: ensure_klog_daemon(host, port), show,
                       requires_connection=False)

    def begin_capture(self) -> None:
        name = self.capture_name.text().strip()
        build = self.capture_build.text().strip() or "unspecified"
        kind = self.capture_kind.currentData()
        filters = self.capture_filter.text().strip()
        host = self.klog_host.text().strip()
        try:
            port = int(self.klog_port.text().strip())
            if not name or not host or not 1 <= port <= 65535:
                raise ValueError
        except ValueError:
            QMessageBox.warning(self, "Klog", "Informe nome, host e porta válidos para a captura.")
            return

        def show(result):
            proc, path = result
            self.capture_processes[str(path)] = proc
            self.klog_status_label.setText(f"Captura ativa: {path}")
            self.refresh_captures()

        self._run_task("Iniciando captura independente…",
                       lambda _progress: start_klog_capture(name, build, kind, filters, host, port),
                       show, requires_connection=False)

    def refresh_captures(self) -> None:
        self.capture_list.clear()
        capture_dir = KLOG_ROOT / "captures"
        if not capture_dir.exists():
            return
        for path in sorted(capture_dir.glob("*.log"), key=lambda item: item.stat().st_mtime, reverse=True)[:100]:
            if path.name.endswith(".filtered.log"):
                continue
            active = str(path) in self.capture_processes and self.capture_processes[str(path)].poll() is None
            label = f"{'● ativa  ' if active else ''}{path.name}  ({self._size(path.stat().st_size)})"
            item = QListWidgetItem(label)
            item.setData(Qt.ItemDataRole.UserRole, str(path))
            self.capture_list.addItem(item)

    def _selected_capture_path(self) -> Path | None:
        item = self.capture_list.currentItem()
        return Path(item.data(Qt.ItemDataRole.UserRole)) if item else None

    def open_capture(self) -> None:
        path = self._selected_capture_path()
        if path is None:
            QMessageBox.information(self, "Klog", "Selecione uma captura.")
            return
        QDesktopServices.openUrl(QUrl.fromLocalFile(str(path)))

    def stop_capture(self) -> None:
        path = self._selected_capture_path()
        if path is None:
            QMessageBox.information(self, "Klog", "Selecione uma captura.")
            return
        proc = self.capture_processes.get(str(path))
        if proc is None:
            QMessageBox.information(self, "Klog", "Essa captura pertence a outra sessão. Peça ao processo que a iniciou para encerrá-la.")
            return

        def stop(_progress):
            stop_klog_capture(proc)
            return str(path)

        def done(stopped_path):
            self.capture_processes.pop(stopped_path, None)
            self.klog_status_label.setText(f"Captura encerrada: {stopped_path}")
            self.refresh_captures()

        self._run_task("Encerrando captura…", stop, done, requires_connection=False)

    def install(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Escolher PKG para instalar", str(Path.home()),
                                              "Pacotes PS4 (*.pkg)")
        if not path:
            return
        pkg = Path(path)
        if pkg.suffix.lower() != ".pkg":
            QMessageBox.warning(self, "Arquivo inválido", "Escolha um arquivo .pkg.")
            return
        try:
            ps4_host = self.host.text().strip()
            if not ps4_host:
                raise ValueError("Informe o endereço do PS4.")
            try:
                http_port = int(self.http_port.text().strip())
                if not 1 <= http_port <= 65535:
                    raise ValueError
            except ValueError:
                raise ValueError("A porta HTTP do PC precisa estar entre 1 e 65535.") from None
            self.settings = self._settings_from_ui()
            self._save_config()
        except (OSError, ValueError) as exc:
            QMessageBox.critical(self, "Conexão", str(exc))
            return
        answer = QMessageBox.warning(
            self, "Instalação experimental",
            "O app enviará este PKG ao Remote Package Installer (RPI) do PS4 pela porta 12800. "
            "O RPI precisa estar instalado e ativo. O PC servirá o arquivo pela porta HTTP "
            f"{http_port}; essa porta precisa estar acessível pelo PS4. O app iniciará uma "
            f"captura klog e registrará o progresso localmente.\n\nArquivo: {pkg}\n\nDeseja iniciar a instalação?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if answer != QMessageBox.StandardButton.Yes:
            return

        def action(progress):
            return install_pkg_experimental(pkg, ps4_host, progress=progress,
                                            klog_host=ps4_host, http_port=http_port,
                                            log=lambda line: None)

        def show(result):
            self.refresh_captures()
            log_file = result.get("log", "")
            klog_file = result.get("klog", "")
            state = result.get("status", "unknown")
            QMessageBox.information(
                self, "Instalação experimental",
                f"Estado: {state}\nTarefa RPI: {result.get('task_id', 'indisponível')}\n\n"
                f"Log da instalação:\n{log_file}\n\nCaptura klog:\n{klog_file}",
            )

        self._run_task(f"Enviando {pkg.name} ao RPI…", action, show, requires_connection=False)

    def refresh(self) -> None:
        def action(_progress):
            ftp = connect_ftp(self.settings)
            try:
                return list_pkg_files(ftp)
            finally:
                close_ftp(ftp)

        def show_files(files: dict[str, int]) -> None:
            self.files = files
            self.table.setRowCount(0)
            for name, size in sorted(files.items(), key=lambda pair: pair[0].casefold()):
                row = self.table.rowCount()
                self.table.insertRow(row)
                name_item = QTableWidgetItem(name)
                name_item.setData(Qt.ItemDataRole.UserRole, name)
                size_item = QTableWidgetItem(self._size(size))
                size_item.setTextAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
                self.table.setItem(row, 0, name_item)
                self.table.setItem(row, 1, size_item)
            self.count_label.setText(f"{len(files)} arquivo(s)")
            self.status.setText(f"Conectado — {self.settings[0]}:{self.settings[1]}")

        self._run_task("Conectando e lendo /data/pkg…", action, show_files)

    def _selected_names(self) -> list[str]:
        rows = sorted({index.row() for index in self.table.selectionModel().selectedRows()})
        return [self.table.item(row, 0).data(Qt.ItemDataRole.UserRole) for row in rows]

    @staticmethod
    def _size(size: int) -> str:
        value = float(size)
        for unit in ("B", "KiB", "MiB", "GiB"):
            if value < 1024 or unit == "GiB":
                return f"{int(value)} B" if unit == "B" else f"{value:.1f} {unit}"
            value /= 1024
        return f"{size} B"

    def upload(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Escolher arquivo para enviar", str(Path.home()),
                                              "Todos os arquivos (*)")
        if not path:
            return
        local = Path(path)
        if not local.is_file():
            QMessageBox.warning(self, "Arquivo inválido", "Escolha um arquivo regular.")
            return
        if any(char in local.name for char in ("\r", "\n", "/", "\\")):
            QMessageBox.warning(self, "Nome inválido", "O nome não pode conter barras ou quebras de linha.")
            return
        listed_exists = local.name in self.files
        replace_confirmed = False
        if listed_exists:
            answer = QMessageBox.question(self, "Substituir arquivo?", f"{local.name} já existe no PS4. Substituir?",
                                          QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                                          QMessageBox.StandardButton.No)
            if answer != QMessageBox.StandardButton.Yes:
                return
            replace_confirmed = True
        else:
            answer = QMessageBox.question(
                self, "Enviar arquivo",
                f"Enviar {local.name} para {self.remote_dir.text().strip() or DEFAULT_REMOTE_DIR}.\n\n"
                "Se já existir um arquivo com esse nome, permitir a substituição?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No,
            )
            replace_confirmed = answer == QMessageBox.StandardButton.Yes

        def action(progress):
            with console_operation_lock():
                ftp = connect_ftp(self.settings)
                temp_name = f".{local.name}.{uuid.uuid4().hex}.uploading"
                backup_name = f".{local.name}.{uuid.uuid4().hex}.replaced"
                old_moved = False
                try:
                    try:
                        existing_size = ftp.size(local.name)
                    except ftplib.error_perm as exc:
                        if not str(exc).startswith("550"):
                            raise
                        existing_size = None
                    if existing_size is not None and not replace_confirmed:
                        raise FileExistsError(f"{local.name} já existe no PS4. Atualize a lista e confirme a substituição.")
                    total = local.stat().st_size
                    sent = 0

                    def on_block(block: bytes) -> None:
                        nonlocal sent
                        sent += len(block)
                        progress(sent, total)

                    with local.open("rb") as stream:
                        ftp.storbinary(f"STOR {temp_name}", stream, blocksize=1024 * 1024, callback=on_block)
                    if ftp.size(temp_name) != total:
                        raise IOError("O tamanho do arquivo enviado não confere.")
                    if existing_size is not None:
                        ftp.rename(local.name, backup_name)
                        old_moved = True
                    try:
                        ftp.rename(temp_name, local.name)
                    except Exception:
                        if old_moved:
                            ftp.rename(backup_name, local.name)
                            old_moved = False
                        raise
                    if old_moved:
                        old_moved = False
                        try:
                            ftp.delete(backup_name)
                        except ftplib.all_errors:
                            pass
                        return f"Enviado: {local.name}; cópia anterior preservada como {backup_name}"
                    return f"Enviado para {self.settings[4]}: {local.name} ({self._size(total)})"
                except Exception:
                    try:
                        ftp.delete(temp_name)
                    except ftplib.all_errors:
                        pass
                    if old_moved:
                        try:
                            ftp.rename(backup_name, local.name)
                        except ftplib.all_errors:
                            pass
                    raise
                finally:
                    close_ftp(ftp)

        def done(message: str) -> None:
            self.status.setText(message)
            QMessageBox.information(self, "Arquivo enviado", message)
            if local.suffix.lower() == ".pkg":
                self.refresh()

        self._run_task(f"Enviando {local.name}…", action, done)

    def download(self) -> None:
        names = self._selected_names()
        if len(names) != 1:
            QMessageBox.information(self, "Baixar PKG", "Selecione um único PKG para baixar.")
            return
        name = names[0]
        target_name, _ = QFileDialog.getSaveFileName(self, "Salvar PKG", str(Path.home() / name), "Pacotes PS4 (*.pkg)")
        if not target_name:
            return
        target = Path(target_name)
        expected_size = self.files[name]

        def action(progress):
            ftp = connect_ftp(self.settings)
            partial = target.with_name(target.name + ".part")
            received = 0
            try:
                with partial.open("wb") as output:
                    def on_block(block: bytes) -> None:
                        nonlocal received
                        output.write(block)
                        received += len(block)
                        progress(received, expected_size)
                    ftp.retrbinary(f"RETR {name}", on_block, blocksize=1024 * 1024)
                if partial.stat().st_size != expected_size:
                    raise IOError("O tamanho baixado não confere com o arquivo no PS4.")
                partial.replace(target)
                return f"Baixado: {target}"
            except Exception:
                partial.unlink(missing_ok=True)
                raise
            finally:
                close_ftp(ftp)

        self._run_task(f"Baixando {name}…", action)

    def delete(self) -> None:
        names = self._selected_names()
        if not names:
            QMessageBox.information(self, "Excluir PKG", "Selecione um ou mais PKGs.")
            return
        preview = "\n".join(names[:8])
        if len(names) > 8:
            preview += f"\n… e mais {len(names) - 8} arquivo(s)"
        answer = QMessageBox.question(self, "Confirmar exclusão", f"Excluir permanentemente do PS4?\n\n{preview}",
                                      QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                                      QMessageBox.StandardButton.No)
        if answer != QMessageBox.StandardButton.Yes:
            return
        if any(name not in self.files or not name.lower().endswith(".pkg") or PurePosixPath(name).name != name for name in names):
            QMessageBox.critical(self, "Exclusão cancelada", "A seleção não corresponde a PKGs validados. Atualize a lista.")
            return

        def action(_progress):
            with console_operation_lock():
                ftp = connect_ftp(self.settings)
                deleted = []
                try:
                    for name in names:
                        ftp.delete(name)
                        deleted.append(name)
                    return f"Excluídos {len(deleted)} PKG(s)."
                finally:
                    close_ftp(ftp)

        self._run_task("Excluindo PKG(s)…", action)

    def clean_builds(self) -> None:
        try:
            self.settings = self._settings_from_ui()
            self._save_config()
        except (OSError, ValueError) as exc:
            QMessageBox.critical(self, "Conexão", str(exc))
            return

        remote_dir = self.settings[4]

        def inspect(_progress):
            with console_operation_lock():
                ftp = connect_ftp(self.settings)
                try:
                    return list_remote_files_preserving_directories(ftp, remote_dir)
                finally:
                    close_ftp(ftp)

        def confirm_and_clean(plan) -> None:
            files, directories = plan
            if not files:
                self.status.setText(f"Nenhum arquivo para limpar em {remote_dir}")
                QMessageBox.information(
                    self, "Limpar builds antigas",
                    f"Não há arquivos em {remote_dir}. As {len(directories)} pasta(s) permanecem intactas.",
                )
                return
            total_size = sum(size for _name, size in files)
            preview = "\n".join(name for name, _size in files[:12])
            if len(files) > 12:
                preview += f"\n… e mais {len(files) - 12} arquivo(s)"
            answer = QMessageBox.warning(
                self, "Limpar builds antigas",
                f"Isso excluirá permanentemente TODOS os {len(files)} arquivos de {remote_dir} "
                f"({self._size(total_size)}). As {len(directories)} pasta(s) e seu conteúdo não serão tocados.\n\n"
                f"Arquivos que serão excluídos:\n{preview}\n\nContinuar?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No,
            )
            if answer != QMessageBox.StandardButton.Yes:
                self.status.setText("Limpeza cancelada")
                return

            def delete_files(_progress):
                deleted = []
                with console_operation_lock():
                    ftp = connect_ftp(self.settings)
                    try:
                        for name, _expected_size in files:
                            # SIZE must positively identify a regular file; never
                            # issue DELE for a directory or an ambiguous entry.
                            original_dir = ftp.pwd()
                            try:
                                ftp.cwd(name)
                            except ftplib.error_perm:
                                ftp.cwd(original_dir)
                            else:
                                ftp.cwd(original_dir)
                                raise RuntimeError(f"{name} agora é uma pasta; limpeza interrompida sem removê-la.")
                            current_size = ftp.size(name)
                            if current_size is None:
                                raise RuntimeError(f"Não consegui confirmar {name} como arquivo; limpeza interrompida.")
                            ftp.delete(name)
                            deleted.append(name)
                    finally:
                        close_ftp(ftp)
                return deleted

            def cleaned(deleted: list[str]) -> None:
                message = f"Removidos {len(deleted)} arquivo(s) de {remote_dir}; pastas preservadas."
                self.status.setText(message)
                QMessageBox.information(self, "Limpeza concluída", message)
                self.refresh()

            self._run_task("Excluindo arquivos; preservando pastas…", delete_files,
                           cleaned, requires_connection=False)

        self._run_task(f"Listando arquivos de {remote_dir}…", inspect,
                       confirm_and_clean, requires_connection=False)

    def closeEvent(self, event) -> None:
        for proc in list(self.capture_processes.values()):
            stop_klog_capture(proc, timeout=2)
        self.capture_processes.clear()
        super().closeEvent(event)


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName(APP_NAME)
    window = Ps4Link()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
