// Command platform is the on-premise platform, simulator edition: command
// dispatch + uplink processing + web console, with an optional embedded
// MQTT broker so the whole system runs on one PC without Docker.
//
//	go run ./cmd/platform -embedded-broker :1883 -http :8080 -locksim 127.0.0.1:9200
//
// Production replaces the simulator pieces: PostgreSQL registry instead of
// the layout registry, KMS-backed sealer instead of simulation keys, EMQX
// instead of the embedded broker, TLS + authentication on every port.
package main

import (
	"context"
	"flag"
	"log"
	"net/http"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
	mochi "github.com/mochi-mqtt/server/v2"
	"github.com/mochi-mqtt/server/v2/hooks/auth"
	"github.com/mochi-mqtt/server/v2/listeners"

	"github.com/Gao-KD/smart-lock/common/slpgo/simnet"
	"github.com/Gao-KD/smart-lock/platform/internal/console"
	"github.com/Gao-KD/smart-lock/platform/internal/dispatch"
)

func main() {
	httpAddr := flag.String("http", ":8080", "web console / API listen address")
	brokerURL := flag.String("mqtt", "tcp://127.0.0.1:1883", "MQTT broker URL")
	embedded := flag.String("embedded-broker", "", "run an embedded MQTT broker on this address (e.g. :1883)")
	locksim := flag.String("locksim", "127.0.0.1:9200", "locksim control address (empty: no simulator)")
	tenant := flag.String("tenant", "school-demo", "tenant (school) id")
	building := flag.Int("building", 1, "building number")
	floors := flag.Int("floors", 12, "floors")
	rooms := flag.Int("rooms", 30, "rooms per floor")
	floorsPerGW := flag.Int("floors-per-gw", 6, "floors served by one gateway")
	offline := flag.Duration("offline-after", 95*time.Minute, "lock shown offline after this long without uplink (3 heartbeats + margin)")
	flag.Parse()

	if *embedded != "" {
		srv := mochi.New(&mochi.Options{InlineClient: false})
		_ = srv.AddHook(new(auth.AllowHook), nil) // simulator only: no auth
		if err := srv.AddListener(listeners.NewTCP(listeners.Config{ID: "tcp", Address: *embedded})); err != nil {
			log.Fatal(err)
		}
		go func() {
			if err := srv.Serve(); err != nil {
				log.Fatal(err)
			}
		}()
		log.Printf("embedded MQTT broker on %s", *embedded)
	}

	layout := simnet.Layout{Building: *building, Floors: *floors, Rooms: *rooms, FloorsPerGW: *floorsPerGW}
	pub := &mqttPublisher{}
	reg := console.Registry{Tenant: *tenant, Layout: layout}
	svc := dispatch.New(dispatch.Config{}, reg, console.Sealer{}, pub)
	con := console.New(*tenant, layout, svc, *offline)
	go con.RunBackground(context.Background()) // pulls missing lock records (READ_LOG)

	if *locksim != "" {
		sc, err := console.NewSimCtl(*locksim, con.OnFirmwareLog)
		if err != nil {
			log.Fatal(err)
		}
		con.Sim = sc
		go sc.Run()
	}

	opts := mqtt.NewClientOptions().AddBroker(*brokerURL).SetClientID("platform-" + *tenant).
		SetAutoReconnect(true).SetConnectRetry(true).SetConnectRetryInterval(time.Second).SetOrderMatters(false)
	opts.SetOnConnectHandler(func(c mqtt.Client) {
		log.Printf("platform connected to %s", *brokerURL)
		// Production: shared subscription ($share/platform/...) over several
		// instances, see gateway/internal/topics.PlatformSubscription.
		c.Subscribe("sl/v1/"+*tenant+"/+/up/#", 1, func(_ mqtt.Client, m mqtt.Message) {
			gw, kind, ok := splitTopic(m.Topic())
			if ok {
				con.HandleUplink(gw, kind, m.Payload())
			}
		})
	})
	client := mqtt.NewClient(opts)
	pub.client = client
	client.Connect()

	con.AddEvent(0, "system", "平台启动")
	log.Printf("web console: http://localhost%s  (building %d: %d floors x %d rooms, %d gateways)",
		*httpAddr, *building, *floors, *rooms, layout.Gateways())
	srv := &http.Server{Addr: *httpAddr, Handler: con.Handler(), ReadHeaderTimeout: 5 * time.Second}
	log.Fatal(srv.ListenAndServe())
}

// splitTopic: sl/v1/{tenant}/{gateway}/up/{x}
func splitTopic(t string) (gw, kind string, ok bool) {
	var parts []string
	start := 0
	for i := 0; i <= len(t); i++ {
		if i == len(t) || t[i] == '/' {
			parts = append(parts, t[start:i])
			start = i + 1
		}
	}
	if len(parts) != 6 {
		return "", "", false
	}
	return parts[3], parts[4] + "/" + parts[5], true
}

type mqttPublisher struct{ client mqtt.Client }

func (p *mqttPublisher) Publish(_ context.Context, b dispatch.Batch) error {
	data, err := jsonMarshal(b)
	if err != nil {
		return err
	}
	tok := p.client.Publish("sl/v1/"+b.Tenant+"/"+b.Gateway+"/down/cmd", 1, false, data)
	if !tok.WaitTimeout(3 * time.Second) {
		return context.DeadlineExceeded
	}
	return tok.Error()
}
