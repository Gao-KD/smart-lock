// Command gateway is the building gateway host process.
//
// Today it runs a self-contained simulation of one gateway so the
// scheduling behaviour can be checked without hardware:
//
//	go run ./cmd/gateway -locks 270 -channels 3
//
// TODO (in order): MQTT client (persistent session, TLS client cert),
// RCP serial driver, link-layer AES-CCM, join forwarding, persistent
// command queue, fire-alarm dry contact input, local web/CLI.
package main

import (
	"flag"
	"fmt"
	"log"
	"time"

	"github.com/Gao-KD/smart-lock/gateway/internal/locktable"
	"github.com/Gao-KD/smart-lock/gateway/internal/scheduler"
	"github.com/Gao-KD/smart-lock/gateway/internal/topics"
)

func main() {
	locks := flag.Int("locks", 270, "number of locks on this gateway")
	channels := flag.Int("channels", 3, "data channels (radio co-processors)")
	slots := flag.Int("slots", 50, "listen slots per period")
	periodMs := flag.Int("period-ms", 1000, "listen period")
	tenant := flag.String("tenant", "school-demo", "tenant (school) id")
	gwID := flag.String("id", "gw-demo-01", "gateway id")
	flag.Parse()

	tb := locktable.New(*channels, *slots)
	for i := 0; i < *locks; i++ {
		if _, err := tb.Join(uint64(0xA0000000 + i)); err != nil {
			log.Fatal(err)
		}
	}
	s := scheduler.New(tb, scheduler.Config{})
	for i := 0; i < *locks; i++ {
		_ = s.Enqueue(&scheduler.Command{MsgID: uint32(i + 1), LockEUI: uint64(0xA0000000 + i), Priority: scheduler.P1Interactive})
	}

	fmt.Printf("gateway %s subscribes to %v\n", *gwID, topics.GatewaySubscriptions(*tenant, *gwID))
	fmt.Printf("%d locks on %d channels x %d slots, max %d locks per cell\n", tb.Len(), *channels, *slots, tb.MaxCellLoad())

	now := time.Now()
	for period := 1; s.Pending() > 0; period++ {
		txs, _ := s.PlanPeriod(now)
		for _, tx := range txs {
			s.Result(tx.Cmd, true) // simulation: every frame is ACKed
		}
		fmt.Printf("period %d: sent %d commands, %d left\n", period, len(txs), s.Pending())
		now = now.Add(time.Duration(*periodMs) * time.Millisecond)
	}
}
