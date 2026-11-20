# 模拟平台一体化镜像：门锁固件(主机版) + locksim + 网关 + 平台，Windows/macOS 用 Docker Desktop 即可运行。
#   docker compose -f deploy/sim/docker-compose.yml up --build
FROM golang:1.24-bookworm AS build
WORKDIR /src
COPY common/slpgo/go.mod common/slpgo/
COPY gateway/go.mod gateway/go.sum gateway/
COPY platform/go.mod platform/go.sum platform/
RUN cd gateway && go mod download && cd ../platform && go mod download
COPY . .
# locksim = lock firmware logic + SLP library + host crypto + simulator port
# (same sources as the CMake target; gcc ships with the golang image).
RUN mkdir -p /out && F=firmware/lock && gcc -std=c99 -O2 -Wall -o /out/locksim \
      -Icommon/slp/include -I$F/hal -I$F/core -I$F/credential -I$F/app -I$F/mac -I$F/port/sim \
      common/slp/src/*.c common/slp/port/host/crypto_host.c \
      $F/core/lock_core.c $F/credential/credential.c $F/app/lock_app.c $F/mac/lock_mac.c \
      $F/port/sim/locksim.c $F/port/sim/hal_sim.c $F/port/sim/sim_net.c \
 && cd gateway && CGO_ENABLED=0 go build -o /out/gateway ./cmd/gateway \
 && cd ../platform && CGO_ENABLED=0 go build -o /out/platform ./cmd/platform && CGO_ENABLED=0 go build -o /out/demo ./cmd/demo

FROM debian:bookworm-slim
COPY --from=build /out/ /usr/local/bin/
COPY deploy/sim/run.sh /usr/local/bin/run-sim
ENV FLOORS=12 ROOMS=30 FLOORS_PER_GW=6 HEARTBEAT_S=30
EXPOSE 8080 1883
CMD ["run-sim"]
