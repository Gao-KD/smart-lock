#!/bin/sh
# Starts locksim, the platform (embedded MQTT broker) and one gateway per
# FLOORS_PER_GW floors inside one container. Logs go to stdout.
set -e
B=${BUILDING:-1}
G=$(( (FLOORS + FLOORS_PER_GW - 1) / FLOORS_PER_GW ))
locksim --building "$B" --floors "$FLOORS" --rooms "$ROOMS" --floors-per-gw "$FLOORS_PER_GW" \
  --heartbeat-s "$HEARTBEAT_S" --port 9100 --ctrl 9200 | sed -u 's/^/[locksim] /' &
platform -embedded-broker :1883 -http :8080 -locksim 127.0.0.1:9200 -building "$B" \
  -floors "$FLOORS" -rooms "$ROOMS" -floors-per-gw "$FLOORS_PER_GW" \
  -offline-after "$(( HEARTBEAT_S * 3 + 10 ))s" 2>&1 | sed -u 's/^/[platform] /' &
sleep 1
i=1
while [ "$i" -le "$G" ]; do
  gateway -lock-offline-after 100s -building "$B" -index "$i" -rcp "127.0.0.1:$(( 9100 + i - 1 ))" 2>&1 | sed -u "s/^/[gw-$i] /" &
  i=$(( i + 1 ))
done
echo "模拟平台已启动: http://localhost:8080  ($FLOORS 层 x $ROOMS 间, $G 台网关)"
echo "功能演示: docker compose -f deploy/sim/docker-compose.yml exec sim demo all"
wait
