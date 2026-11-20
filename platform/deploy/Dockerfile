# Build from the repository root (the platform imports common/slpgo):
#   docker build -f platform/deploy/Dockerfile .
FROM golang:1.24 AS build
WORKDIR /src
COPY common/slpgo common/slpgo
COPY platform platform
RUN cd platform && CGO_ENABLED=0 go build -o /out/platform ./cmd/platform

FROM gcr.io/distroless/static-debian12
COPY --from=build /out/platform /platform
EXPOSE 8080
ENTRYPOINT ["/platform", "-http", ":8080", "-mqtt", "tcp://emqx:1883", "-locksim", ""]
