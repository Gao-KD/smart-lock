// Package simnet holds the conventions shared by the simulator processes
// (C locksim, Go gateway, Go platform): building layout, how simulated
// devices are addressed, the simulation-only key derivation, and the UDP
// wire format between the gateway host and its (simulated) radio
// co-processors. The C side of the same rules is
// firmware/lock/port/sim/sim_net.h; keep both in sync.
//
// NOTHING here is a production design: real locks get K_dev at the factory
// and the platform's KMS derives K_app/K_link from it.
package simnet

import (
	"crypto/aes"
	"encoding/binary"
	"fmt"
)

// Layout of one simulated building.
type Layout struct {
	Building    int // 1..15
	Floors      int
	Rooms       int // rooms per floor
	FloorsPerGW int
}

func (l Layout) Gateways() int { return (l.Floors + l.FloorsPerGW - 1) / l.FloorsPerGW }

// LockEUI = 0x5A00_0000_0000_0000 | building<<16 | floor<<8 | room.
func LockEUI(building, floor, room int) uint64 {
	return 0x5A00000000000000 | uint64(building)<<16 | uint64(floor)<<8 | uint64(room)
}

// SplitLockEUI is the inverse of LockEUI.
func SplitLockEUI(eui uint64) (building, floor, room int, ok bool) {
	if eui>>56 != 0x5A {
		return 0, 0, 0, false
	}
	return int(eui >> 16 & 0xFF), int(eui >> 8 & 0xFF), int(eui & 0xFF), true
}

// GatewayEUI = 0x6A00_0000_0000_0000 | building<<8 | index (1-based).
func GatewayEUI(building, gw int) uint64 {
	return 0x6A00000000000000 | uint64(building)<<8 | uint64(gw)
}

// GatewayID is the MQTT gateway id.
func GatewayID(building, gw int) string { return fmt.Sprintf("gw-%d-%d", building, gw) }

// PAN of one gateway's network.
func PAN(building, gw int) uint16 { return uint16(0x5A00 + building*16 + gw) }

// GatewayOfFloor returns the 1-based gateway index serving a floor.
func (l Layout) GatewayOfFloor(floor int) int { return (floor-1)/l.FloorsPerGW + 1 }

// RoomName is the door label, e.g. "1203" for floor 12 room 3.
func RoomName(floor, room int) string { return fmt.Sprintf("%d%02d", floor, room) }

// Simulation master key ("smartlock-sim-01"). Simulation only.
var MasterKey = []byte("smartlock-sim-01")

// Key tags.
const (
	TagLink    = 'L' // K_link(lock)
	TagApp     = 'A' // K_app(lock)
	TagGrpLink = 'G' // K_grp link key (gateway EUI)
	TagGrpApp  = 'g' // K_grp app key (gateway EUI)
)

// Key = AES-128_master( tag | 0^7 | EUI (big-endian) ).
func Key(tag byte, eui uint64) []byte {
	b, _ := aes.NewCipher(MasterKey)
	in := make([]byte, 16)
	in[0] = tag
	binary.BigEndian.PutUint64(in[8:], eui)
	out := make([]byte, 16)
	b.Encrypt(out, in)
	return out
}

// ---- gateway host <-> radio co-processor wire format (UDP) ----
//
// The real link is UART/SPI to EFR32 co-processors (gateway/internal/rcp);
// in the simulator the "co-processors" and the 433 MHz air live in locksim.
// All integers little-endian.
const (
	// host -> RCP
	MsgHello  = 0x01 // gwEUI u64 | session u32 | channels u8 | slots u8 | period_ms u16 | slot_ms u16 | pan u16
	MsgPeriod = 0x02 // period u32 | beacon_len u8 (always 0: no beacon on the air) | beacon | n u16 | n x (ch u8 | slot u8 | pre_ms u16 | ref u32 | len u8 | frame)
	MsgTxNow  = 0x03 // ref u32 | len u8 | frame   (control channel, immediate: join accept)
	MsgAdd    = 0x04 // period u32 | n u16 | entries: more frames for the current period
	// RCP -> host
	MsgTxResult = 0x81 // ref u32 | acked u8 | rssi i8
	MsgRx       = 0x82 // ch u8 | rssi i8 | len u8 | frame
)

// MAC command ids inside FTMacCmd payloads.
const (
	MacJoinReq    = 0x01 // (empty)
	MacJoinAccept = 0x02 // short u16 | channel u8 | slot u16 | period_ms u16 | slot_ms u16 | phase_ms u16 | period u32
	MacProbe      = 0x03 // gateway -> lock, secured, ACK requested: lock answers with a RECONNECT heartbeat
	MacSniffDiv   = 0x04 // lock -> gateway, secured: div u8 (fake-wake-up guard)
)

// Synchronous wake-up (docs/radio-plan.md, C: slp/slot.h). Keep in sync.
const (
	DriftPPM          = 40  // lock + gateway crystals, worst case
	SyncPreambleMinMs = 8   // sniff + radio turnaround margin
	SyncPreambleMaxMs = 250 // longer: use a wake-up train
	WakeTrainExtraMs  = 50  // wake-up train = period + this
	AckPayloadLen     = 10  // phase_ms u16 | period u32 | unix u32
)

// SyncPreambleMs is the preamble that reaches a lock whose last ACK was
// sinceSyncMs ago, or 0 when a wake-up train is needed.
func SyncPreambleMs(sinceSyncMs int64) int {
	if sinceSyncMs < 0 {
		sinceSyncMs = 0
	}
	drift := (sinceSyncMs*DriftPPM + 999_999) / 1_000_000
	pre := 2*drift + SyncPreambleMinMs
	if pre > SyncPreambleMaxMs {
		return 0
	}
	return int(pre)
}

// AckPayload is the authenticated payload of a gateway ACK: where the
// gateway is in its period, the period number and the time. It replaces
// the beacon.
func AckPayload(phaseMs int, period uint32, unix uint32) []byte {
	b := binary.LittleEndian.AppendUint16(nil, uint16(phaseMs))
	b = binary.LittleEndian.AppendUint32(b, period)
	return binary.LittleEndian.AppendUint32(b, unix)
}

// TxEntry is one planned frame in a period.
type TxEntry struct {
	Channel, Slot int
	PreMs         int // preamble length; > period = wake-up train
	Ref           uint32
	Frame         []byte
}

func EncodeHello(gwEUI uint64, session uint32, channels, slots, periodMs, slotMs int, pan uint16) []byte {
	b := []byte{MsgHello}
	b = binary.LittleEndian.AppendUint64(b, gwEUI)
	b = binary.LittleEndian.AppendUint32(b, session)
	b = append(b, byte(channels), byte(slots))
	b = binary.LittleEndian.AppendUint16(b, uint16(periodMs))
	b = binary.LittleEndian.AppendUint16(b, uint16(slotMs))
	return binary.LittleEndian.AppendUint16(b, pan)
}

func EncodePeriod(period uint32, beacon []byte, txs []TxEntry) []byte {
	b := []byte{MsgPeriod}
	b = binary.LittleEndian.AppendUint32(b, period)
	b = append(b, byte(len(beacon)))
	b = append(b, beacon...)
	b = binary.LittleEndian.AppendUint16(b, uint16(len(txs)))
	for _, t := range txs {
		b = append(b, byte(t.Channel), byte(t.Slot))
		b = binary.LittleEndian.AppendUint16(b, uint16(t.PreMs))
		b = binary.LittleEndian.AppendUint32(b, t.Ref)
		b = append(b, byte(len(t.Frame)))
		b = append(b, t.Frame...)
	}
	return b
}

// EncodePeriodAdd adds frames to the period that is already running.
func EncodePeriodAdd(period uint32, txs []TxEntry) []byte {
	b := EncodePeriod(period, nil, txs)
	b[0] = MsgAdd
	// drop the (empty) beacon length byte
	return append(b[:5], b[6:]...)
}

func EncodeTxNow(ref uint32, frame []byte) []byte {
	b := []byte{MsgTxNow}
	b = binary.LittleEndian.AppendUint32(b, ref)
	b = append(b, byte(len(frame)))
	return append(b, frame...)
}
