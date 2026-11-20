// Package rcp is the link between the Linux host and the radio
// co-processors (one EFR32FG28 per data channel, plus one for the control
// channel), over UART/SPI.
//
// The co-processor owns millisecond timing: the host hands it a frame and a
// target slot for the next period, the co-processor transmits at the slot
// start, waits for the link-layer ACK and reports back.
//
// Wire framing (TODO): HDLC-like, CRC-16, sequence numbers.
package rcp

import (
	"context"

	"github.com/Gao-KD/smart-lock/gateway/internal/locktable"
)

// TxRequest asks a co-processor to send one MAC frame in a slot.
type TxRequest struct {
	Ref   uint64 // host reference, echoed in TxResult
	Slot  int
	Frame []byte // complete MAC frame, link-layer encrypted by the host
}

type TxResult struct {
	Ref   uint64
	Acked bool
	RSSI  int8
}

// RxFrame is an uplink frame heard by a co-processor.
type RxFrame struct {
	Channel int
	Frame   []byte
	RSSI    int8
	AtMs    uint32 // co-processor clock
}

// Link is implemented by the serial driver and by a simulator for tests.
type Link interface {
	// Submit queues transmissions for the next period on one channel.
	Submit(ctx context.Context, channel int, reqs []TxRequest) error
	// Results delivers TX outcomes.
	Results() <-chan TxResult
	// Uplinks delivers received frames from all channels.
	Uplinks() <-chan RxFrame
	// Beacon schedules a beacon on every channel at the next period start.
	Beacon(ctx context.Context, payload []byte) error
}

// Plan groups cell-addressed frames per channel, ready for Submit.
func Plan(frames map[locktable.Cell][]byte, refs map[locktable.Cell]uint64) map[int][]TxRequest {
	out := make(map[int][]TxRequest)
	for cell, f := range frames {
		out[cell.Channel] = append(out[cell.Channel], TxRequest{Ref: refs[cell], Slot: cell.Slot, Frame: f})
	}
	return out
}
