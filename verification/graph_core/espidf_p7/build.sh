#!/usr/bin/env bash
set -euo pipefail
source /home/karolina/esp/esp-idf/export.sh
idf.py -C /home/karolina/Research/Code/kaos_core/verification/graph_core/espidf_p7 \
    -B /tmp/kaos-p7-session/esp32 build
