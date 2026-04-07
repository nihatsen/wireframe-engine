#!/bin/bash
# Quick repack without rebuilding (for config changes)
cd "$(dirname "$0")/module"
ZIP="wireframe-engine-v1.0.0.zip"
rm -f "../out/$ZIP"
mkdir -p ../out
zip -r9 "../out/$ZIP" . -x "*.DS_Store"
echo "Packed: out/$ZIP"
