#!/usr/bin/env bash
# Compiles shaders/*.vert|.frag with glslc and regenerates ../table_shader_spv.h.
set -euo pipefail
cd "$(dirname "$0")"

shaders=(table.vert table.frag fullscreen.vert bright.frag blur.frag composite.frag)

for f in "${shaders[@]}"; do
	glslc "$f" -o "$f.spv"
done

python3 - "${shaders[@]}" <<'PY'
import sys

specs = [
	("table.vert", "table_vert"),
	("table.frag", "table_frag"),
	("fullscreen.vert", "fullscreen_vert"),
	("bright.frag", "bright_frag"),
	("blur.frag", "blur_frag"),
	("composite.frag", "composite_frag"),
]

out = ["// Auto-generated from shaders/*.vert|.frag using glslc. Do not edit by hand.",
       "#pragma once", ""]
for src, sym in specs:
	with open(src + ".spv", "rb") as fh:
		data = fh.read()
	out.append(f"unsigned char {sym}_spv[] = {{")
	for i in range(0, len(data), 12):
		chunk = data[i:i + 12]
		out.append("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
	out.append("};")
	out.append(f"unsigned int {sym}_spv_len = {len(data)};")
	out.append("")

with open("../table_shader_spv.h", "w") as fh:
	fh.write("\n".join(out))
PY

echo "shaders built -> ../table_shader_spv.h"
