#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the NV12 Vulkan shaders and embed their SPIR-V in the PS4 binary."""
from pathlib import Path
import argparse
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "ps4-homebrew" / "shaders"
OUTPUT = ROOT / "ps4-homebrew" / "gpu_video_shaders.h"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=shutil.which("glslangValidator") or "glslangValidator")
    args = parser.parse_args()
    blocks = []
    with tempfile.TemporaryDirectory(prefix="psbox-shaders-") as temp:
        for variable, filename in (("kNv12VideoVertSpv", "nv12_video.vert"),
                                   ("kNv12VideoFragSpv", "nv12_video.frag"),
                                   ("kRcasFragSpv", "rcas.frag"),
                                   ("kUiVertSpv", "ui.vert"), ("kUiFragSpv", "ui.frag")):
            binary = Path(temp) / (filename + ".spv")
            subprocess.run([args.compiler, "-V", "--target-env", "vulkan1.1",
                            "-I" + str(ROOT / "extern" / "fsr1"), "-o", str(binary), str(SHADERS / filename)], check=True)
            data = binary.read_bytes()
            if len(data) % 4:
                raise SystemExit(f"invalid SPIR-V word size: {filename}")
            words = struct.unpack("<" + "I" * (len(data) // 4), data)
            lines = ["    " + ", ".join(f"0x{word:08x}u" for word in words[i:i + 8]) + ","
                     for i in range(0, len(words), 8)]
            blocks.append(f"static constexpr uint32_t {variable}[] = {{\n" +
                          "\n".join(lines) + "\n};\n")
    OUTPUT.write_text(
        "// SPDX-License-Identifier: GPL-3.0-or-later\n"
        "// Generated from shaders/nv12_video.vert and nv12_video.frag.\n"
        "#pragma once\n#include <cstdint>\nnamespace xc::ps4 {\n" +
        "".join(blocks) + "}\n"
    )


if __name__ == "__main__":
    main()
