module github.com/Gao-KD/smart-lock/platform

go 1.24

require (
	github.com/Gao-KD/smart-lock/common/slpgo v0.0.0
	github.com/eclipse/paho.mqtt.golang v1.5.0
	github.com/mochi-mqtt/server/v2 v2.6.6
)

require (
	github.com/gorilla/websocket v1.5.3 // indirect
	github.com/rs/xid v1.4.0 // indirect
	golang.org/x/net v0.27.0 // indirect
	golang.org/x/sync v0.7.0 // indirect
	gopkg.in/yaml.v3 v3.0.1 // indirect
)

replace github.com/Gao-KD/smart-lock/common/slpgo => ../common/slpgo
