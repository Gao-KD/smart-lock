// Command gateway is the building gateway host process.
//
// Simulator mode (default): connects to the platform's MQTT broker and to
// the simulated radio co-processors in locksim over UDP:
//
//	go run ./cmd/gateway -building 1 -index 1 -mqtt tcp://127.0.0.1:1883 -rcp 127.0.0.1:9100
//
// Dry run: the original scheduling-only simulation, no MQTT/radio needed:
//
//	go run ./cmd/gateway -dry-run -locks 270 -channels 3
//
// TODO for hardware: RCP serial driver (gateway/internal/rcp), TLS client
// certificate, persistent command queue, fire-alarm dry contact input.
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"

	"github.com/Gao-KD/smart-lock/common/slpgo/simnet"
	"github.com/Gao-KD/smart-lock/gateway/internal/locktable"
	"github.com/Gao-KD/smart-lock/gateway/internal/rcp"
	"github.com/Gao-KD/smart-lock/gateway/internal/runtime"
	"github.com/Gao-KD/smart-lock/gateway/internal/scheduler"
	"github.com/Gao-KD/smart-lock/gateway/internal/topics"
)

func main() {
	dry := flag.Bool("dry-run", false, "scheduling-only simulation (no MQTT, no radio)")
	locks := flag.Int("locks", 270, "dry run: number of locks on this gateway")
	channels := flag.Int("channels", 3, "data channels (radio co-processors)")
	slots := flag.Int("slots", 50, "listen slots per period")
	periodMs := flag.Int("period-ms", 1000, "listen period")
	tenant := flag.String("tenant", "school-demo", "tenant (school) id")
	building := flag.Int("building", 1, "simulator: building number")
	index := flag.Int("index", 1, "simulator: gateway index in the building (1-based)")
	broker := flag.String("mqtt", "tcp://127.0.0.1:1883", "MQTT broker URL")
	rcpAddr := flag.String("rcp", "127.0.0.1:9100", "simulator: locksim UDP address of this gateway's radios")
	maxAttempts := flag.Int("max-attempts", 5, "transmissions per unicast command before giving up")
	flag.Parse()

	if *dry {
		dryRun(*locks, *channels, *slots, *periodMs, *tenant)
		return
	}

	id := simnet.GatewayID(*building, *index)
	eui := simnet.GatewayEUI(*building, *index)
	radio, err := rcp.DialSim(*rcpAddr)
	if err != nil {
		log.Fatal(err)
	}
	pub := &mqttPublisher{tenant: *tenant, gw: id}
	rt := runtime.New(runtime.Config{
		Tenant: *tenant, ID: id, EUI: eui, PAN: simnet.PAN(*building, *index),
		Channels: *channels, Slots: *slots, PeriodMs: *periodMs, SlotMs: *periodMs / *slots,
		MaxAttempts: *maxAttempts,
		LinkKey:     func(e uint64) []byte { return simnet.Key(simnet.TagLink, e) },
		GrpKey:      simnet.Key(simnet.TagGrpLink, eui),
	}, radio, pub)

	lwt, _ := json.Marshal(runtime.Status{Gateway: id, Online: false})
	opts := mqtt.NewClientOptions().AddBroker(*broker).SetClientID(id).
		SetAutoReconnect(true).SetConnectRetry(true).SetConnectRetryInterval(time.Second).
		SetCleanSession(false).
		SetWill(topics.Build(*tenant, id, topics.UpStatus), string(lwt), 1, true)
	opts.SetOnConnectHandler(func(c mqtt.Client) {
		log.Printf("%s: connected to %s", id, *broker)
		c.Subscribe(topics.Build(*tenant, id, topics.DownCmd), 1, func(_ mqtt.Client, m mqtt.Message) {
			var b runtime.Batch
			if err := json.Unmarshal(m.Payload(), &b); err != nil {
				log.Printf("bad batch: %v", err)
				return
			}
			rt.HandleBatch(b)
		})
	})
	client := mqtt.NewClient(opts)
	pub.client = client
	client.Connect()

	log.Printf("gateway %s (EUI %016x): %d channels x %d slots, period %d ms, radios at %s",
		id, eui, *channels, *slots, *periodMs, *rcpAddr)
	stop := make(chan struct{})
	go func() {
		sig := make(chan os.Signal, 1)
		signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
		<-sig
		close(stop)
	}()
	rt.Run(stop)
	pub.Publish(topics.UpStatus, runtime.Status{Gateway: id, Online: false}, true)
	client.Disconnect(500)
}

type mqttPublisher struct {
	client mqtt.Client
	tenant string
	gw     string
}

func (p *mqttPublisher) Publish(kind topics.Kind, v any, retained bool) {
	b, err := json.Marshal(v)
	if err != nil {
		return
	}
	if p.client == nil || !p.client.IsConnected() {
		return // a real gateway would keep it in its persistent queue
	}
	p.client.Publish(topics.Build(p.tenant, p.gw, kind), 1, retained, b)
}

// dryRun is the scheduling-only simulation of one gateway.
func dryRun(locks, channels, slots, periodMs int, tenant string) {
	gwID := "gw-demo-01"
	tb := locktable.New(channels, slots)
	for i := 0; i < locks; i++ {
		if _, err := tb.Join(uint64(0xA0000000 + i)); err != nil {
			log.Fatal(err)
		}
	}
	s := scheduler.New(tb, scheduler.Config{})
	for i := 0; i < locks; i++ {
		_ = s.Enqueue(&scheduler.Command{MsgID: uint32(i + 1), LockEUI: uint64(0xA0000000 + i), Priority: scheduler.P1Interactive})
	}
	fmt.Printf("gateway %s subscribes to %v\n", gwID, topics.GatewaySubscriptions(tenant, gwID))
	fmt.Printf("%d locks on %d channels x %d slots, max %d locks per cell\n", tb.Len(), channels, slots, tb.MaxCellLoad())
	now := time.Now()
	for period := 1; s.Pending() > 0; period++ {
		txs, _ := s.PlanPeriod(now)
		for _, tx := range txs {
			s.Result(tx.Cmd, true)
		}
		fmt.Printf("period %d: sent %d commands, %d left\n", period, len(txs), s.Pending())
		now = now.Add(time.Duration(periodMs) * time.Millisecond)
	}
}
