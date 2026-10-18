#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Desktop viewer for the production PS4 dock renderer; settings are demo-only."""

import ctypes
import time
import sys
from pathlib import Path

from PyQt6 import sip
from PyQt6.QtCore import Qt, QTimer
from PyQt6.QtGui import QImage, QPixmap, QKeyEvent
from PyQt6.QtWidgets import QApplication, QLabel, QMainWindow, QVBoxLayout, QWidget


class PreviewWindow(QMainWindow):
    # IDs must match StreamMenuUiState; traversal follows the console's visible order.
    dock_order = (0, 2, 3, 1)  # Imagem, Volume, Configurações, Gamepad
    card_names = {"settings": 0, "controls": 1, "volume": 2, "configuration": 3}
    rows = {0: (0, 2, 7, 1, 4), 3: (6, 7, 8, 5, 3)}
    stats_row_counts = (7, 6, 5, 7, 4, 8)
    stats_rows = (
        ("variant", "anchor", "subtitles", "titles", "header", "scale_percent", "font_size"),
        ("line_height", "margin_x", "margin_y", "padding", "background", "outline"),
        ("accent_text", "font_face", "accent_r", "accent_g", "accent_b"),
        ("metric_rtt", "metric_br", "metric_loss", "rtt_warn", "rtt_bad", "loss_warn", "loss_bad"),
        ("metric_fps", "metric_dec", "metric_drop", "metric_cad"),
        ("fps_warn", "fps_bad", "drop_warn", "drop_bad", "decode_warn", "decode_bad", "jitter_warn", "jitter_bad"),
    )
    bitrates = (3, 4, 6, 8, 10, 12, 15, 20, 25, 30)

    def __init__(self, root: Path, renderer: Path, initial: str):
        super().__init__()
        self.root, self.renderer = root, renderer
        self.card = self.card_names.get(initial, 0)
        self.lib = ctypes.CDLL(str(renderer))
        self.lib.psbox_preview_create.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
        self.lib.psbox_preview_create.restype = ctypes.c_void_p
        self.lib.psbox_preview_destroy.argtypes = [ctypes.c_void_p]
        self.lib.psbox_preview_active.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
        self.lib.psbox_preview_render.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32), ctypes.c_size_t, ctypes.c_uint64]
        self.lib.psbox_preview_render.restype = ctypes.c_void_p
        self.width_px, self.height_px = 1920, 1080
        self.handle = self.lib.psbox_preview_create(str(root).encode(), self.width_px, self.height_px)
        if not self.handle:
            raise RuntimeError("Não foi possível criar a prévia C++ ou carregar as fontes")
        self.dirty = True
        self.values = dict(row=0, tier=720, fps=60, bitrate=3, bitrate_custom=0, filter=1, volume=80,
                           stats=2, fsr=0, fsr_row=0, sharpness=4, scale=2, reset=0, pacing=0)
        self.preview_condition = 0
        self.stats_enabled = 1
        self.stats_open = False
        self.stats_page = self.stats_row = 0
        self.stats_backup = None
        self.stats_config = dict(variant=0, anchor=0, subtitles=1, titles=1, header=1,
            accent_text=0, font_face=2, scale_percent=120, font_size=13, line_height=100,
            margin_x=8, margin_y=6, padding=8, background=90, outline=2,
            accent_r=168, accent_g=196, accent_b=207,
            metric_fps=1, metric_rtt=1, metric_br=1, metric_loss=1, metric_dec=1,
            metric_drop=2, metric_cad=2, fps_warn=55, fps_bad=45, rtt_warn=60, rtt_bad=100,
            loss_warn=5, loss_bad=20, drop_warn=1, drop_bad=4, decode_warn=5, decode_bad=9,
            jitter_warn=4, jitter_bad=8)
        self.panel_open = True
        # Start on the dock so arrow keys immediately select its icons.
        self.in_panel = False
        self.previous_volume = 80
        self.last_custom_bitrate = 8
        self.pixmap = QPixmap()
        self.setWindowTitle("PSBOX · Prévia da dock C++ · Valores demonstrativos")
        self.setMinimumSize(900, 680)
        central = QWidget(self)
        layout = QVBoxLayout(central)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        self.hint = QLabel()
        self.hint.setWordWrap(True)
        self.hint.setStyleSheet("color:#b1b5b1; font:13px sans-serif; padding:4px")
        self.image = QLabel(alignment=Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignTop)
        self.image.setStyleSheet("background:#111111")
        layout.addWidget(self.hint)
        layout.addWidget(self.image, 1)
        self.setCentralWidget(central)
        self.setStyleSheet("background:#111111")
        self.timer = QTimer(self)
        self.timer.setTimerType(Qt.TimerType.PreciseTimer)
        self.timer.setInterval(16)
        self.timer.timeout.connect(self.tick)
        self.timer.start()
        self.render_card()

    def render_card(self):
        rows = self.rows.get(self.card, (0,))
        if self.card == 0 and self.values["bitrate_custom"]:
            rows = (0, 2, 7, 1, 4)
        elif self.card == 0:
            rows = (0, 2, 7, 4)
        if self.values["row"] not in rows:
            self.values["row"] = rows[0]
        self.dirty = True
        # Keep keyboard/debug labels out of the simulated console UI.
        # PS4 button glyphs are rendered by the shared C++ module.
        status = ("Vídeo normal", "Vídeo parado", "Conexão perdida")[self.preview_condition]
        self.hint.setText(f"Dados simulados · {status} · F5 normal / F6 vídeo parado / F7 desconectado · Setas navegam · Enter entra · Esc volta")
        self.hint.show()
        self.tick()

    def tick(self):
        now = time.monotonic_ns() // 1_000_000
        if not self.handle or not (self.dirty or self.lib.psbox_preview_active(self.handle, now)):
            return
        c = self.stats_config
        values = [self.card, self.values["row"], self.values["tier"], self.values["fps"],
            self.values["bitrate"], self.values["bitrate_custom"], self.values["filter"], self.values["volume"],
            self.values["stats"], self.values["fsr"], self.values["fsr_row"], self.values["sharpness"],
            self.values["scale"], self.values["reset"], self.values["pacing"], int(self.panel_open), int(self.in_panel),
            self.stats_enabled, int(self.stats_open), self.stats_page, self.stats_row,
            c["variant"],c["anchor"]&1,c["subtitles"],c["titles"],c["header"],c["accent_text"],c["font_face"],
            c["scale_percent"],c["font_size"],c["line_height"],c["margin_x"],c["margin_y"],c["padding"],
            c["background"],c["outline"],c["accent_r"],c["accent_g"],c["accent_b"],
            c["metric_fps"],c["metric_rtt"],c["metric_br"],c["metric_loss"],c["metric_dec"],c["metric_drop"],c["metric_cad"],
            c["fps_warn"],c["fps_bad"],c["rtt_warn"],c["rtt_bad"],c["loss_warn"],c["loss_bad"],
            c["drop_warn"],c["drop_bad"],c["decode_warn"],c["decode_bad"],c["jitter_warn"],c["jitter_bad"],self.preview_condition,(c["anchor"]>>1)&1]
        state = (ctypes.c_uint32 * len(values))(*values)
        address = self.lib.psbox_preview_render(self.handle, state, len(values), now)
        if not address:
            self.hint.show()
            self.hint.setText("Falha ao desenhar a prévia C++")
            self.timer.stop()
            return
        image = QImage(sip.voidptr(address), self.width_px, self.height_px,
                       self.width_px * 4, QImage.Format.Format_RGBA8888)
        self.pixmap = QPixmap.fromImage(image.copy())
        self.dirty = False
        self.resize_image()

    def closeEvent(self, event):
        self.timer.stop()
        if self.handle:
            self.lib.psbox_preview_destroy(self.handle)
            self.handle = None
        super().closeEvent(event)

    def move_card(self, direction: int):
        try:
            index = self.dock_order.index(self.card)
        except ValueError:
            index = 0
        self.card = self.dock_order[(index + (1 if direction > 0 else -1)) % len(self.dock_order)]

    def resize_image(self):
        if not self.pixmap.isNull():
            self.image.setPixmap(self.pixmap.scaled(
                self.image.size(), Qt.AspectRatioMode.KeepAspectRatio,
                Qt.TransformationMode.SmoothTransformation,
            ))

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self.resize_image()

    def change_value(self, step):
        v = self.values
        if self.card == 2:
            v["volume"] = max(0, min(100, v["volume"] + step * 5))
        elif self.card in self.rows:
            if v["fsr"] and self.card == 0:
                key = ("sharpness", "scale", "filter")[v["fsr_row"]]
                v[key] = (v[key] + step) % 5 if key == "sharpness" else (v[key] + step) % 3 if key == "filter" else 3-v[key]
                return
            key = {0: "tier", 1: "bitrate", 2: "fps", 3: "reset", 4: "filter", 5: "pacing", 6: "stats", 7: "new_stats", 8: "configure_stats", 9: "bitrate_mode"}.get(v["row"], "")
            if self.card == 0 and v["row"] == 7:
                key = "bitrate_mode"
            if key == "tier": v[key] = 1080 if v[key] == 720 else 720
            elif key == "fps": v[key] = 30 if v[key] == 60 else 60
            elif key == "bitrate_mode":
                modes = (3, 10, 30, "custom")
                current = "custom" if v["bitrate_custom"] else v["bitrate"]
                index = modes.index(current) if current in modes else 0
                selected = modes[(index + step) % len(modes)]
                v["bitrate_custom"] = int(selected == "custom")
                if selected == "custom": v["bitrate"] = self.last_custom_bitrate
                else: v["bitrate"] = selected
                self.render_card()
            elif key == "bitrate":
                v[key] = self.bitrates[(self.bitrates.index(v[key]) + step) % len(self.bitrates)]
                v["bitrate_custom"] = 1
                self.last_custom_bitrate = v[key]
            elif key == "filter": v[key] = (v[key] + step) % 3
            elif key == "stats": v[key] = (v[key] + step) % 3
            elif key == "new_stats":
                self.apply_stats_preset((self.stats_config["variant"]+step)%3)
                self.stats_enabled=1
            elif key == "configure_stats":
                self.stats_open = True
                self.stats_page = self.stats_row = 0
                self.stats_backup = self.stats_config.copy()
            else: v[key] = 1 - v[key]

    def apply_stats_preset(self, variant):
        card=variant==0
        self.stats_config.update(variant=variant, scale_percent=120 if card else 100,
            font_size=13 if card else 16, line_height=100 if card else 120,
            margin_x=8 if card else 12, margin_y=6 if card else 12,
            padding=8 if card else 6, background=90 if card else 0,
            outline=2 if card else 1, font_face=2)

    def change_stats_setting(self, step):
        key = self.stats_rows[self.stats_page][self.stats_row]
        c = self.stats_config
        bounds = {
            "variant":(0,2,1),"anchor":(0,3,1),"subtitles":(0,1,1),"titles":(0,1,1),"header":(0,1,1),
            "scale_percent":(60,180,10),"font_size":(9,24,1),"line_height":(80,180,5),
            "margin_x":(0,80,1),"margin_y":(0,80,1),"padding":(2,24,1),"background":(0,100,5),"outline":(0,5,1),
            "accent_text":(0,1,1),"font_face":(0,2,1),"accent_r":(0,255,1),"accent_g":(0,255,1),"accent_b":(0,255,1),
            "metric_fps":(0,2,1),"metric_rtt":(0,2,1),"metric_br":(0,2,1),"metric_loss":(0,2,1),
            "metric_dec":(0,2,1),"metric_drop":(0,2,1),"metric_cad":(0,2,1),
            "fps_warn":(10,120,1),"fps_bad":(5,110,1),"rtt_warn":(10,500,5),"rtt_bad":(20,1000,5),
            "loss_warn":(1,100,1),"loss_bad":(2,100,1),"drop_warn":(0,60,1),"drop_bad":(1,120,1),
            "decode_warn":(1,50,1),"decode_bad":(2,100,1),"jitter_warn":(1,50,1),"jitter_bad":(2,100,1)}
        low, high, amount = bounds[key]
        c[key] = low + ((c[key] - low + step * amount) % (high - low + amount))
        if key=="variant":
            self.apply_stats_preset(c[key])
            self.stats_enabled=1

    def keyPressEvent(self, event: QKeyEvent):
        key = event.key()
        v = self.values
        if key in (Qt.Key.Key_F5, Qt.Key.Key_F6, Qt.Key.Key_F7):
            self.preview_condition = key - Qt.Key.Key_F5
        elif self.stats_open and key in (Qt.Key.Key_BracketLeft, Qt.Key.Key_BracketRight,
                                       Qt.Key.Key_Q, Qt.Key.Key_E):
            forward = key in (Qt.Key.Key_BracketRight, Qt.Key.Key_E)
            self.stats_page = (self.stats_page + (1 if forward else -1)) % 6
            self.stats_row = 0
        elif self.stats_open and key in (Qt.Key.Key_Up, Qt.Key.Key_Down):
            n = self.stats_row_counts[self.stats_page]
            self.stats_row = (self.stats_row + (1 if key == Qt.Key.Key_Down else -1)) % n
        elif self.stats_open and key in (Qt.Key.Key_Left, Qt.Key.Key_Right):
            self.change_stats_setting(1 if key == Qt.Key.Key_Right else -1)
        elif key in (Qt.Key.Key_Q, Qt.Key.Key_E):
            self.move_card(1 if key == Qt.Key.Key_E else -1)
            self.panel_open, self.in_panel, v["fsr"] = True, True, 0
        elif Qt.Key.Key_1 <= key <= Qt.Key.Key_4:
            self.card = self.dock_order[key - Qt.Key.Key_1]
            self.panel_open, self.in_panel, v["fsr"] = True, True, 0
        elif key in (Qt.Key.Key_Left, Qt.Key.Key_Right):
            direction = 1 if key == Qt.Key.Key_Right else -1
            if self.in_panel:
                self.change_value(direction)
            else:
                self.move_card(direction)
                self.panel_open, v["fsr"] = True, 0
        elif key in (Qt.Key.Key_Up, Qt.Key.Key_Down):
            if not self.in_panel:
                self.panel_open, self.in_panel = True, True
            elif self.card in self.rows:
                if v["fsr"]:
                    v["fsr_row"] = (v["fsr_row"] + (1 if key == Qt.Key.Key_Down else -1)) % 3
                else:
                    rows = ((0, 2, 7, 1, 4) if v["bitrate_custom"] else (0, 2, 7, 4)) if self.card == 0 else self.rows[self.card]
                    v["row"] = rows[(rows.index(v["row"]) + (1 if key == Qt.Key.Key_Down else -1)) % len(rows)]
        elif key == Qt.Key.Key_T and self.card == 0:
            v["fsr"] = 1 - v["fsr"]
            self.panel_open, self.in_panel = True, True
        elif key in (Qt.Key.Key_Return, Qt.Key.Key_Enter, Qt.Key.Key_Space):
            if self.stats_open:
                self.stats_backup = None
                self.stats_open = False
            elif not self.in_panel:
                self.panel_open, self.in_panel = True, True
            elif self.card == 2:
                if v["volume"]:
                    self.previous_volume, v["volume"] = v["volume"], 0
                else:
                    v["volume"] = self.previous_volume
            elif self.card in self.rows:
                self.change_value(1)
        elif key == Qt.Key.Key_Escape:
            if self.stats_open:
                if self.stats_backup is not None:
                    self.stats_config = self.stats_backup
                self.stats_backup = None
                self.stats_open = False
            elif v["fsr"]:
                v["fsr"] = 0
            elif self.in_panel:
                self.in_panel = False
            elif self.panel_open:
                self.panel_open = False
            else:
                self.close()
                return
        else:
            super().keyPressEvent(event)
            return
        self.render_card()


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: run-native-preview.py PROJECT_ROOT RENDERER [settings|controls|volume|configuration]")
    root, renderer = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    initial = sys.argv[3] if len(sys.argv) > 3 else "settings"
    app = QApplication(sys.argv[:1])
    window = PreviewWindow(root, renderer, initial)
    if len(sys.argv)>4 and sys.argv[4] in ("off","compact","full"):
        window.values["stats"]={"off":0,"compact":1,"full":2}[sys.argv[4]]
        window.render_card()
    window.showMaximized()
    raise SystemExit(app.exec())


if __name__ == "__main__":
    main()
