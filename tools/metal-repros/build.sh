#!/bin/sh
# Compiles a repro's kernel.metal the way Veach's build does (metal 3.1, macOS 14), then creates the pipeline.
# Usage: ./build.sh <repro directory> <function>
set -e
dir="$1"
xcrun -sdk macosx metal -std=metal3.1 -mmacosx-version-min=14.0 -c "$dir/kernel.metal" -o "$dir/kernel.air"
xcrun -sdk macosx metallib "$dir/kernel.air" -o "$dir/kernel.metallib"
xcrun swift "$(dirname "$0")/repro.swift" "$dir/kernel.metallib" "$2"
