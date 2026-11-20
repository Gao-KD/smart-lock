// Package runtime is the gateway engine: it joins locks, runs one listen
// period at a time (beacon + scheduled frames on every data channel),
// applies the link-layer security (K_link / K_grp, anti-replay) and bridges
// command batches and lock uplinks to the platform.
//
// The app payloads stay end-to-end sealed: the gateway reads only the clear
// header byte that says whether an uplink is a result or an event.
package runtime

import (
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"log"
	"strconv"
	"sync"
	"time"

	"github.com/Gao-KD/smart-lock/common/slpgo/simnet"
	"github.com/Gao-KD/smart-lock/common/slpgo/slp"
	"github.com/Gao-KD/smart-lock/gateway/internal/locktable"
	"github.com/Gao-KD/smart-lock/gateway/internal/rcp"
	"github.com/Gao-KD/smart-lock/gateway/internal/scheduler"
	"github.com/Gao-KD/smart-lock/gateway/internal/topics"
)

// Radio is the co-processor link (rcp.SimLink in the simulator).
type Radio interface {
	Send(b []byte) error
	Results() <-chan rcp.TxResult
	Uplinks() <-chan rcp.RxFrame
}

// Publisher sends JSON messages to the platform (MQTT QoS 1).
type Publisher interface {
	Publish(kind topics.Kind, v any, retained bool)
}

type Config struct {
	Tenant      string
	ID          string
	EUI         uint64
	PAN         uint16
	Channels    int
	Slots       int
	PeriodMs    int
	SlotMs      int
	MaxAttempts int
	// LinkKey returns K_link for a lock (from the platform's KMS in
	// production, derived locally in the simulator).
	LinkKey func(eui uint64) []byte
	GrpKey  []byte
}

// ---- messages to/from the platform (JSON; see proto/gateway/v1) ----

type BatchItem struct {
	MsgID      uint32    `json:"msg_id"`
	LockEUI    HexEUI    `json:"lock_eui,omitempty"`
	Group      uint16    `json:"group,omitempty"`
	Priority   uint8     `json:"priority"`
	ExpiresAt  time.Time `json:"expires_at"`
	AppPayload []byte    `json:"app_payload"`
}

type Batch struct {
	BatchID uint64      `json:"batch_id"`
	Items   []BatchItem `json:"items"`
}

type Ack struct {
	MsgID    uint32 `json:"msg_id"`
	State    string `json:"state"` // accepted, retrying, delivered, failed, expired
	LockEUI  HexEUI `json:"lock_eui,omitempty"`
	Attempts int    `json:"attempts,omitempty"`
	Reason   string `json:"reason,omitempty"`
	Cells    int    `json:"cells,omitempty"`
	AtMs     int64  `json:"at_ms"`
}

type Uplink struct {
	LockEUI    HexEUI `json:"lock_eui"`
	AppPayload []byte `json:"app_payload"`
	RSSI       int8   `json:"rssi"`
	AtMs       int64  `json:"at_ms"`
}

type JoinInfo struct {
	LockEUI HexEUI `json:"lock_eui"`
	Short   uint16 `json:"short"`
	Channel int    `json:"channel"`
	Slot    int    `json:"slot"`
	AtMs    int64  `json:"at_ms"`
}

type Status struct {
	Gateway     string `json:"gateway"`
	Online      bool   `json:"online"`
	Locks       int    `json:"locks"`
	OnlineLocks int    `json:"online_locks"`
	Pending     int    `json:"pending"`
	Period      uint32 `json:"period"`
	Channels    int    `json:"channels"`
	Slots       int    `json:"slots"`
	PeriodMs    int    `json:"period_ms"`
	AtMs        int64  `json:"at_ms"`
}

// HexEUI marshals as a 16-digit hex string.
type HexEUI uint64

func (e HexEUI) MarshalText() ([]byte, error) { return []byte(fmt.Sprintf("%016x", uint64(e))), nil }
func (e *HexEUI) UnmarshalText(b []byte) error {
	v, err := strconv.ParseUint(string(b), 16, 64)
	*e = HexEUI(v)
	return err
}

type lockState struct {
	eui      uint64
	key      []byte
	rxValid  bool
	rxLast   uint32
	lastSeen time.Time
}

type inflight struct {
	cmd   *scheduler.Command
	group bool
}

type Runtime struct {
	cfg   Config
	radio Radio
	pub   Publisher
	table *locktable.Table
	sched *scheduler.Scheduler

	mu       sync.Mutex
	byShort  map[uint16]*lockState
	inflight map[uint32]*inflight
	ref      uint32
	fc       uint32
	seq      uint8
	period   uint32
	session  uint32
	// current period, for mid-period insertion of urgent commands
	periodStart time.Time
	usedCells   map[locktable.Cell]bool
}

func New(cfg Config, radio Radio, pub Publisher) *Runtime {
	tb := locktable.New(cfg.Channels, cfg.Slots)
	return &Runtime{
		cfg:      cfg,
		radio:    radio,
		pub:      pub,
		table:    tb,
		sched:    scheduler.New(tb, scheduler.Config{MaxAttempts: cfg.MaxAttempts}),
		byShort:  make(map[uint16]*lockState),
		inflight: make(map[uint32]*inflight),
		// Never reuse a frame counter across restarts (a real gateway
		// persists counter blocks like slp_txctr).
		fc:      uint32(time.Now().Unix()-1_700_000_000) * 64,
		session: uint32(time.Now().UnixNano()),
	}
}

func nowMs() int64 { return time.Now().UnixMilli() }

func (r *Runtime) hello() []byte {
	return simnet.EncodeHello(r.cfg.EUI, r.session, r.cfg.Channels, r.cfg.Slots, r.cfg.PeriodMs, r.cfg.SlotMs, r.cfg.PAN)
}

// Run drives periods until stop is closed.
func (r *Runtime) Run(stop <-chan struct{}) {
	_ = r.radio.Send(r.hello())
	period := time.NewTicker(time.Duration(r.cfg.PeriodMs) * time.Millisecond)
	status := time.NewTicker(5 * time.Second)
	defer period.Stop()
	defer status.Stop()
	r.publishStatus(true)
	for {
		select {
		case <-stop:
			return
		case <-period.C:
			r.runPeriod(time.Now())
		case <-status.C:
			_ = r.radio.Send(r.hello())
			r.publishStatus(true)
		case res := <-r.radio.Results():
			r.onResult(res)
		case up := <-r.radio.Uplinks():
			r.onUplink(up)
		}
	}
}

// HandleBatch queues a command batch from down/cmd.
func (r *Runtime) HandleBatch(b Batch) {
	for _, it := range b.Items {
		c := &scheduler.Command{
			MsgID: it.MsgID, LockEUI: uint64(it.LockEUI), Group: it.Group,
			Priority: scheduler.Priority(it.Priority), ExpiresAt: it.ExpiresAt, Payload: it.AppPayload,
		}
		ack := Ack{MsgID: it.MsgID, LockEUI: it.LockEUI, AtMs: nowMs()}
		if err := r.sched.Enqueue(c); err != nil {
			ack.State, ack.Reason = "failed", "lock not joined to this gateway"
		} else {
			ack.State = "accepted"
		}
		r.pub.Publish(topics.UpAck, ack, false)
	}
	r.lateInsert()
}

// lateInsert sends urgent (P0/P1) unicast commands in the current period
// when the target lock's slot is still ahead and its cell is free.
func (r *Runtime) lateInsert() {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.periodStart.IsZero() || r.usedCells == nil {
		return
	}
	const guard = 40 * time.Millisecond // UDP/serial + co-processor setup
	elapsed := time.Since(r.periodStart)
	txs := r.sched.TakeUrgent(time.Now(), func(c locktable.Cell) bool {
		start := time.Duration(c.Slot*r.cfg.SlotMs) * time.Millisecond
		if start < elapsed+guard || r.usedCells[c] {
			return false
		}
		r.usedCells[c] = true
		return true
	})
	if len(txs) == 0 {
		return
	}
	var entries []simnet.TxEntry
	for _, tx := range txs {
		l, err := r.table.Get(tx.Cmd.LockEUI)
		st := r.byShort[l.Short]
		if err != nil || st == nil {
			continue
		}
		frame, err := r.frame(tx.Dst, tx.Cmd.Payload, st.key, slp.KeyLink, true)
		if err != nil {
			continue
		}
		r.ref++
		r.inflight[r.ref] = &inflight{cmd: tx.Cmd}
		entries = append(entries, simnet.TxEntry{Channel: tx.Cell.Channel, Slot: tx.Cell.Slot, Ref: r.ref, Frame: frame})
	}
	_ = r.radio.Send(simnet.EncodePeriodAdd(r.period, entries))
}

func (r *Runtime) nextFC() uint32 {
	r.fc++
	return r.fc
}

func (r *Runtime) runPeriod(now time.Time) {
	r.mu.Lock()
	// Frames of the previous period that got no TX result: no ACK.
	var timedOut []*scheduler.Command
	for ref, inf := range r.inflight {
		if !inf.group {
			timedOut = append(timedOut, inf.cmd)
		}
		delete(r.inflight, ref)
	}
	r.mu.Unlock()
	for _, c := range timedOut {
		r.outcome(c, false)
	}

	txs, expired := r.sched.PlanPeriod(now)
	for _, c := range expired {
		r.pub.Publish(topics.UpAck, Ack{MsgID: c.MsgID, State: "expired", LockEUI: HexEUI(c.LockEUI), AtMs: nowMs()}, false)
	}

	r.mu.Lock()
	defer r.mu.Unlock()
	r.period++
	r.periodStart = now
	r.usedCells = make(map[locktable.Cell]bool)
	beacon := &slp.Frame{Type: slp.FTBeacon, PAN: r.cfg.PAN, DstMode: slp.AddrShort, Dst: slp.ShortBroadcast,
		SrcMode: slp.AddrShort, Src: slp.ShortGateway,
		Payload: binary.LittleEndian.AppendUint32(binary.LittleEndian.AppendUint32(nil, r.period), uint32(now.Unix()))}
	bf, _ := beacon.Encode()

	var entries []simnet.TxEntry
	groupFrames := map[*scheduler.Command][]byte{}
	groupCells := map[*scheduler.Command]int{}
	for _, tx := range txs {
		var frame []byte
		var err error
		if tx.Cmd.IsGroup() {
			if f, ok := groupFrames[tx.Cmd]; ok {
				frame = f
			} else {
				frame, err = r.frame(tx.Dst, tx.Cmd.Payload, r.cfg.GrpKey, slp.KeyGrp, false)
				groupFrames[tx.Cmd] = frame
			}
			groupCells[tx.Cmd]++
		} else {
			l, gerr := r.table.Get(tx.Cmd.LockEUI)
			st := r.byShort[l.Short]
			if gerr != nil || st == nil {
				continue
			}
			frame, err = r.frame(tx.Dst, tx.Cmd.Payload, st.key, slp.KeyLink, true)
		}
		if err != nil {
			log.Printf("frame for msg %d: %v", tx.Cmd.MsgID, err)
			continue
		}
		r.ref++
		r.inflight[r.ref] = &inflight{cmd: tx.Cmd, group: tx.Cmd.IsGroup()}
		r.usedCells[tx.Cell] = true
		entries = append(entries, simnet.TxEntry{Channel: tx.Cell.Channel, Slot: tx.Cell.Slot, Ref: r.ref, Frame: frame})
	}
	if err := r.radio.Send(simnet.EncodePeriod(r.period, bf, entries)); err != nil {
		log.Printf("radio: %v", err)
	}
	for c, n := range groupCells {
		// Broadcasts are not ACKed per lock: every lock heard it in its own
		// slot of this period. Results arrive as uplinks.
		r.pub.Publish(topics.UpAck, Ack{MsgID: c.MsgID, State: "delivered", Reason: "broadcast", Cells: n, AtMs: nowMs()}, false)
	}
}

func (r *Runtime) frame(dst uint16, payload, key []byte, keyID uint8, ack bool) ([]byte, error) {
	r.seq++
	f := &slp.Frame{Type: slp.FTData, Security: true, AckRequest: ack, Seq: r.seq, PAN: r.cfg.PAN,
		DstMode: slp.AddrShort, Dst: uint64(dst), SrcMode: slp.AddrShort, Src: slp.ShortGateway,
		FrameCounter: r.nextFC(), KeyID: keyID, Payload: payload}
	return f.EncodeSecure(key, r.cfg.EUI)
}

func (r *Runtime) onResult(res rcp.TxResult) {
	r.mu.Lock()
	inf, ok := r.inflight[uint32(res.Ref)]
	delete(r.inflight, uint32(res.Ref))
	r.mu.Unlock()
	if !ok || inf.group {
		return
	}
	r.outcome(inf.cmd, res.Acked)
}

func (r *Runtime) outcome(c *scheduler.Command, acked bool) {
	switch r.sched.Result(c, acked) {
	case scheduler.Delivered:
		r.pub.Publish(topics.UpAck, Ack{MsgID: c.MsgID, State: "delivered", LockEUI: HexEUI(c.LockEUI), Attempts: c.Attempts + 1, AtMs: nowMs()}, false)
	case scheduler.Retrying:
		r.pub.Publish(topics.UpAck, Ack{MsgID: c.MsgID, State: "retrying", LockEUI: HexEUI(c.LockEUI), Attempts: c.Attempts, Reason: "no ACK", AtMs: nowMs()}, false)
	case scheduler.Failed:
		r.pub.Publish(topics.UpAck, Ack{MsgID: c.MsgID, State: "failed", LockEUI: HexEUI(c.LockEUI), Attempts: c.Attempts, Reason: "no ACK after retries (lock offline?)", AtMs: nowMs()}, false)
	}
}

func (r *Runtime) onUplink(up rcp.RxFrame) {
	f, _, err := slp.Decode(up.Frame)
	if err != nil {
		return
	}
	if f.Type == slp.FTMacCmd && f.SrcMode == slp.AddrLong && len(f.Payload) >= 1 && f.Payload[0] == simnet.MacJoinReq {
		r.onJoin(f.Src)
		return
	}
	if f.Type != slp.FTData || !f.Security || f.SrcMode != slp.AddrShort || f.PAN != r.cfg.PAN {
		return
	}
	r.mu.Lock()
	st := r.byShort[uint16(f.Src)]
	r.mu.Unlock()
	if st == nil {
		return
	}
	g, err := slp.DecodeSecure(up.Frame, func(*slp.Frame) ([]byte, uint64, error) { return st.key, st.eui, nil })
	if err != nil {
		log.Printf("uplink from %016x: link MIC failed, dropped", st.eui)
		return
	}
	r.mu.Lock()
	if st.rxValid && g.FrameCounter <= st.rxLast {
		r.mu.Unlock()
		log.Printf("uplink from %016x: replayed frame counter %d, dropped", st.eui, g.FrameCounter)
		return
	}
	st.rxValid, st.rxLast, st.lastSeen = true, g.FrameCounter, time.Now()
	r.mu.Unlock()

	if len(g.Payload) < slp.UpSeqLen+slp.AppHdrLen {
		return
	}
	msg := Uplink{LockEUI: HexEUI(st.eui), AppPayload: append([]byte(nil), g.Payload...), RSSI: up.RSSI, AtMs: nowMs()}
	if g.Payload[slp.UpSeqLen] == slp.RspResult {
		r.pub.Publish(topics.UpResult, msg, false)
	} else {
		r.pub.Publish(topics.UpEvent, msg, false)
	}
}

func (r *Runtime) onJoin(eui uint64) {
	// Production: the join request carries a MIC with K_dev and is
	// forwarded to the platform, which answers with the wrapped K_link.
	l, err := r.table.Join(eui)
	if err != nil {
		log.Printf("join %016x: %v", eui, err)
		return
	}
	r.mu.Lock()
	st := r.byShort[l.Short]
	if st == nil {
		st = &lockState{eui: eui, key: r.cfg.LinkKey(eui)}
		r.byShort[l.Short] = st
	}
	st.rxValid = false // the lock restarted its MAC
	st.lastSeen = time.Now()
	r.mu.Unlock()

	pl := []byte{simnet.MacJoinAccept}
	pl = binary.LittleEndian.AppendUint16(pl, l.Short)
	pl = append(pl, byte(l.Cell.Channel))
	pl = binary.LittleEndian.AppendUint16(pl, uint16(l.Cell.Slot))
	pl = binary.LittleEndian.AppendUint16(pl, uint16(r.cfg.PeriodMs))
	pl = binary.LittleEndian.AppendUint16(pl, uint16(r.cfg.SlotMs))
	f := &slp.Frame{Type: slp.FTMacCmd, PAN: r.cfg.PAN, DstMode: slp.AddrLong, Dst: eui, SrcMode: slp.AddrShort, Src: slp.ShortGateway, Payload: pl}
	b, err := f.Encode()
	if err != nil {
		return
	}
	r.mu.Lock()
	r.ref++
	ref := r.ref
	r.mu.Unlock()
	_ = r.radio.Send(simnet.EncodeTxNow(ref, b))
	r.pub.Publish(topics.UpJoin, JoinInfo{LockEUI: HexEUI(eui), Short: l.Short, Channel: l.Cell.Channel, Slot: l.Cell.Slot, AtMs: nowMs()}, false)
}

func (r *Runtime) publishStatus(online bool) {
	r.mu.Lock()
	n, on := len(r.byShort), 0
	for _, st := range r.byShort {
		if time.Since(st.lastSeen) < 3*time.Minute {
			on++
		}
	}
	p := r.period
	r.mu.Unlock()
	r.pub.Publish(topics.UpStatus, Status{Gateway: r.cfg.ID, Online: online, Locks: n, OnlineLocks: on,
		Pending: r.sched.Pending(), Period: p, Channels: r.cfg.Channels, Slots: r.cfg.Slots, PeriodMs: r.cfg.PeriodMs, AtMs: nowMs()}, true)
}

// EUIString formats an EUI for logs.
func EUIString(e uint64) string { return hex.EncodeToString(binary.BigEndian.AppendUint64(nil, e)) }
