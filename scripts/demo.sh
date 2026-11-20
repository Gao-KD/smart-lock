#!/usr/bin/env bash
# 逐个功能演示（需要先运行 scripts/sim.sh）。
#   scripts/demo.sh            # 列出所有演示
#   scripts/demo.sh all        # 依次运行全部
#   scripts/demo.sh card-unlock
set -euo pipefail
cd "$(dirname "$0")/../platform"
exec go run ./cmd/demo -api "${API:-http://localhost:8080}" "$@"
