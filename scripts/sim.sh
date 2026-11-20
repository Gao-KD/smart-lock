#!/usr/bin/env bash
# 一键启动整套模拟平台：虚拟门锁(locksim) + 2 台网关 + 平台(含内置 MQTT)。
#
#   scripts/sim.sh            # 默认 12 层 x 30 间 = 360 把锁，每 6 层一台网关
#   FLOORS=20 ROOMS=40 FLOORS_PER_GW=7 scripts/sim.sh
#
# 打开 http://localhost:8080 。Ctrl+C 停止全部进程。日志在 .sim/logs/ 。
set -euo pipefail
cd "$(dirname "$0")/.."

BUILDING=${BUILDING:-1}
FLOORS=${FLOORS:-12}
ROOMS=${ROOMS:-30}
FLOORS_PER_GW=${FLOORS_PER_GW:-6}
HTTP_PORT=${HTTP_PORT:-8080}
MQTT_PORT=${MQTT_PORT:-1883}
RCP_PORT=${RCP_PORT:-9100}
CTRL_PORT=${CTRL_PORT:-9200}
HEARTBEAT_S=${HEARTBEAT_S:-30}
GATEWAYS=$(( (FLOORS + FLOORS_PER_GW - 1) / FLOORS_PER_GW ))

need() { command -v "$1" >/dev/null || { echo "缺少 $1，请先安装（见 README）"; exit 1; }; }
need cmake; need go; need cc

echo "==> 编译门锁固件(主机版)与 locksim"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j >/dev/null
echo "==> 编译网关与平台"
mkdir -p .sim/bin .sim/logs
(cd gateway && go build -o ../.sim/bin/gateway ./cmd/gateway)
(cd platform && go build -o ../.sim/bin/platform ./cmd/platform)

pids=()
cleanup() {
  echo; echo "==> 停止模拟平台"
  for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

./build/firmware/locksim --building "$BUILDING" --floors "$FLOORS" --rooms "$ROOMS" \
  --floors-per-gw "$FLOORS_PER_GW" --port "$RCP_PORT" --ctrl "$CTRL_PORT" \
  --heartbeat-s "$HEARTBEAT_S" > .sim/logs/locksim.log 2>&1 &
pids+=($!)

.sim/bin/platform -embedded-broker ":$MQTT_PORT" -mqtt "tcp://127.0.0.1:$MQTT_PORT" \
  -http ":$HTTP_PORT" -locksim "127.0.0.1:$CTRL_PORT" -building "$BUILDING" \
  -floors "$FLOORS" -rooms "$ROOMS" -floors-per-gw "$FLOORS_PER_GW" \
  -offline-after "$(( HEARTBEAT_S * 3 + 10 ))s" > .sim/logs/platform.log 2>&1 &
pids+=($!)
sleep 1

for i in $(seq 1 "$GATEWAYS"); do
  .sim/bin/gateway -building "$BUILDING" -index "$i" -mqtt "tcp://127.0.0.1:$MQTT_PORT" \
    -rcp "127.0.0.1:$(( RCP_PORT + i - 1 ))" > ".sim/logs/gateway-$i.log" 2>&1 &
  pids+=($!)
done

echo
echo "模拟平台已启动：$FLOORS 层 x $ROOMS 间 = $(( FLOORS * ROOMS )) 把锁，$GATEWAYS 台网关"
echo "  Web 控制台:  http://localhost:$HTTP_PORT"
echo "  功能演示:    另开一个终端运行  scripts/demo.sh all"
echo "  日志:        .sim/logs/"
echo "按 Ctrl+C 停止"
wait
