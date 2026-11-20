FROM golang:1.24 AS build
WORKDIR /src
COPY go.mod ./
COPY . .
RUN CGO_ENABLED=0 go build -o /out/dispatch ./cmd/dispatch

FROM gcr.io/distroless/static-debian12
COPY --from=build /out/dispatch /dispatch
EXPOSE 8080
ENTRYPOINT ["/dispatch", "-addr", ":8080"]
