#!/usr/bin/env bash
# Creates a virtual CAN interface for harness/tests/test_vcan.py (requires root).
set -euo pipefail
modprobe vcan
ip link show vcan0 >/dev/null 2>&1 || ip link add dev vcan0 type vcan
ip link set up vcan0
echo "vcan0 ready"
