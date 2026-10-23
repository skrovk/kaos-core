#!/usr/bin/env bash
set -euo pipefail
source /home/karolina/esp/esp-idf/export.sh
idf.py -C /home/karolina/Research/Code/kaos_core/verification/graph_core/espidf_p4 \
    -B /tmp/kaos-p4-session/esp32 build
