// Package runtime is the gateway engine: it joins locks, runs one listen
// period at a time (scheduled frames on every data channel), applies the
// link-layer security (K_link / K_grp, anti-replay) and bridges command
// batches and lock uplinks to the platform.
//
// Synchronous wake-up (docs/radio-plan.md): the gateway sends no beacon and
// transmits only when it has data. Each lock's slot phase is refreshed by
// the authenticated ACK the gateway sends for every uplink; a downlink then
// gets a preamble long enough to cover the clock drift since that ACK.
// Locks whose phase is too old get a wake-up train instead, and so do
// broadcasts. Locks whose commands failed are probed with backoff until
// they answer.
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
	// LockOfflineAfter: a lock not heard for this long counts as offline in
	// the gateway status (default 95 min = 3 heartbeats of 30 min + margin).
	LockOfflineAfter time.Duration
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
	lastSync time.Time // last ACK / Join-Accept sent: the lock's phase reference
	div      uint8     // lock sniffs every div-th period (fake-wake-up guard)
	// suspect: a command got no ACK after all retries; probe with backoff
	suspect      bool
	probeAt      time.Time
	probeBackoff time.Duration
	probing      bool
}

const (
	probeMin = 5 * time.Second
	probeMax = 5 * time.Minute
)

type inflight struct {
	cmd      *scheduler.Command
	group    bool
	probe    *lockState
	deadline uint32 // period by whose end the TX result must have arrived
}

type Runtime struct {
	cfg   Config
	radio Radio
	pub   Publisher
	table *locktable.Table
	sched *scheduler.Scheduler

	mu         sync.Mutex
	stMu       sync.Mutex // guards planPeriod and lockState.div for the scheduler callback
	planPeriod uint32
	byEUI      map[uint64]*lockState
	byShort    map[uint16]*lockState
	inflight   map[uint32]*inflight
	ref        uint32
	fc         uint32
	seq        uint8
	period     uint32
	session    uint32
	// current period, for mid-period insertion of urgent commands
	periodStart time.Time
	usedCells   map[locktable.Cell]bool
	trainCh     map[int]bool // channels busy with a wake-up train this period
}

func New(cfg Config, radio Radio, pub Publisher) *Runtime {
	if cfg.LockOfflineAfter <= 0 {
		cfg.LockOfflineAfter = 95 * time.Minute
	}
	tb := locktable.New(cfg.Channels, cfg.Slots)
	r := &Runtime{
		cfg:      cfg,
		radio:    radio,
		pub:      pub,
		table:    tb,
		byShort:  make(map[uint16]*lockState),
		byEUI:    make(map[uint64]*lockState),
		inflight: make(map[uint32]*inflight),
		// Never reuse a frame counter across restarts (a real gateway
		// persists counter blocks like slp_txctr).
		fc:      uint32(time.Now().Unix()-1_700_000_000) * 64,
		session: uint32(time.Now().UnixNano()),
	}
	r.sched = scheduler.New(tb, scheduler.Config{MaxAttempts: cfg.MaxAttempts, Eligible: r.eligible})
	return r
}

// eligible: the lock sniffs in the period being planned (called by the
// scheduler with r.mu not held).
func (r *Runtime) eligible(eui uint64) bool {
	r.stMu.Lock()
	defer r.stMu.Unlock()
	st := r.byEUI[eui]
	return st == nil || st.div <= 1 || r.planPeriod%uint32(st.div) == 0
}

// preamble picks the preamble for a unicast to st: synchronous (covering
// the drift since the last ACK) or a wake-up train.
func (r *Runtime) preamble(st *lockState, now time.Time) (preMs int, train bool) {
	if !st.lastSync.IsZero() {
		if p := simnet.SyncPreambleMs(now.Sub(st.lastSync).Milliseconds()); p > 0 {
			return p, false
		}
	}
	return r.cfg.PeriodMs + simnet.WakeTrainExtraMs, true
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
		if start < elapsed+guard || r.usedCells[c] || r.trainCh[c.Channel] {
			return false
		}
		r.usedCells[c] = true
		return true
	})
	if len(txs) == 0 {
		return
	}
	var entries []simnet.TxEntry
	now := time.Now()
	for _, tx := range txs {
		l, err := r.table.Get(tx.Cmd.LockEUI)
		st := r.byShort[l.Short]
		if err != nil || st == nil {
			continue
		}
		pre, train := r.preamble(st, now)
		if train || r.trainCh[tx.Cell.Channel] {
			r.sched.Requeue(tx.Cmd) // needs a whole period: next plan
			continue
		}
		frame, err := r.frame(tx.Dst, tx.Cmd.Payload, st.key, slp.KeyLink, true)
		if err != nil {
			continue
		}
		r.ref++
		r.inflight[r.ref] = &inflight{cmd: tx.Cmd, deadline: r.period}
		entries = append(entries, simnet.TxEntry{Channel: tx.Cell.Channel, Slot: tx.Cell.Slot, PreMs: pre, Ref: r.ref, Frame: frame})
	}
	if len(entries) > 0 {
		_ = r.radio.Send(simnet.EncodePeriodAdd(r.period, entries))
	}
}

func (r *Runtime) nextFC() uint32 {
	r.fc++
	return r.fc
}

func (r *Runtime) runPeriod(now time.Time) {
	r.mu.Lock()
	next := r.period + 1
	// Frames whose TX result is overdue: no ACK. Wake-up trains end in the
	// following period, so their deadline is one period later.
	var timedOut []*inflight
	for ref, inf := range r.inflight {
		if inf.deadline < next {
			if !inf.group {
				timedOut = append(timedOut, inf)
			}
			delete(r.inflight, ref)
		}
	}
	r.mu.Unlock()
	for _, inf := range timedOut {
		r.txOutcome(inf, false)
	}

	r.stMu.Lock()
	r.planPeriod = next
	r.stMu.Unlock()
	txs, expired := r.sched.PlanPeriod(now)
	for _, c := range expired {
		r.pub.Publish(topics.UpAck, Ack{MsgID: c.MsgID, State: "expired", LockEUI: HexEUI(c.LockEUI), AtMs: nowMs()}, false)
	}

	r.mu.Lock()
	defer r.mu.Unlock()
	r.period = next
	r.periodStart = now
	r.usedCells = make(map[locktable.Cell]bool)
	r.trainCh = make(map[int]bool)
	busyCh := make(map[int]bool) // channels with synchronous frames

	var entries []simnet.TxEntry
	groupFrames := map[*scheduler.Command][]byte{}
	groupCells := map[*scheduler.Command]int{}
	train := r.cfg.PeriodMs + simnet.WakeTrainExtraMs
	for _, tx := range txs {
		var frame []byte
		var err error
		pre := 0
		if tx.Cmd.IsGroup() {
			if f, ok := groupFrames[tx.Cmd]; ok {
				frame = f
			} else {
				frame, err = r.frame(tx.Dst, tx.Cmd.Payload, r.cfg.GrpKey, slp.KeyGrp, false)
				groupFrames[tx.Cmd] = frame
			}
			groupCells[tx.Cmd]++
			pre = train
		} else {
			l, gerr := r.table.Get(tx.Cmd.LockEUI)
			st := r.byShort[l.Short]
			if gerr != nil || st == nil {
				continue
			}
			var needTrain bool
			pre, needTrain = r.preamble(st, now)
			ch := tx.Cell.Channel
			if r.trainCh[ch] || (needTrain && busyCh[ch]) {
				r.sched.Requeue(tx.Cmd) // channel taken by a wake-up train this period
				continue
			}
			frame, err = r.frame(tx.Dst, tx.Cmd.Payload, st.key, slp.KeyLink, true)
		}
		if err != nil {
			log.Printf("frame for msg %d: %v", tx.Cmd.MsgID, err)
			continue
		}
		if pre >= r.cfg.PeriodMs {
			r.trainCh[tx.Cell.Channel] = true
		} else {
			busyCh[tx.Cell.Channel] = true
		}
		r.ref++
		dl := next
		if pre >= r.cfg.PeriodMs {
			dl = next + 1
		}
		r.inflight[r.ref] = &inflight{cmd: tx.Cmd, group: tx.Cmd.IsGroup(), deadline: dl}
		r.usedCells[tx.Cell] = true
		entries = append(entries, simnet.TxEntry{Channel: tx.Cell.Channel, Slot: tx.Cell.Slot, PreMs: pre, Ref: r.ref, Frame: frame})
	}
	entries = append(entries, r.probes(now, busyCh)...)
	if err := r.radio.Send(simnet.EncodePeriod(r.period, nil, entries)); err != nil {
		log.Printf("radio: %v", err)
	}
	for c, n := range groupCells {
		// Broadcasts are not ACKed per lock: every lock heard the wake-up
		// train on its channel. Results arrive as uplinks.
		r.pub.Publish(topics.UpAck, Ack{MsgID: c.MsgID, State: "delivered", Reason: "broadcast", Cells: n, AtMs: nowMs()}, false)
	}
}

// probes plans one PROBE per suspect lock that is due, in its own cell
// (r.mu held). A lock that answers sends a RECONNECT heartbeat, which makes
// the platform re-send parked commands and pull missed records.
func (r *Runtime) probes(now time.Time, busyCh map[int]bool) []simnet.TxEntry {
	var out []simnet.TxEntry
	for short, st := range r.byShort {
		if !st.suspect || st.probing || now.Before(st.probeAt) {
			continue
		}
		r.stMu.Lock()
		div := st.div
		r.stMu.Unlock()
		if div > 1 && r.period%uint32(div) != 0 {
			continue
		}
		l, err := r.table.Get(st.eui)
		if err != nil || r.usedCells[l.Cell] || r.trainCh[l.Cell.Channel] {
			continue
		}
		pre, train := r.preamble(st, now)
		if train && busyCh[l.Cell.Channel] {
			continue
		}
		r.seq++
		f := &slp.Frame{Type: slp.FTMacCmd, Security: true, AckRequest: true, Seq: r.seq, PAN: r.cfg.PAN,
			DstMode: slp.AddrShort, Dst: uint64(short), SrcMode: slp.AddrShort, Src: slp.ShortGateway,
			FrameCounter: r.nextFC(), KeyID: slp.KeyLink, Payload: []byte{simnet.MacProbe}}
		b, err := f.EncodeSecure(st.key, r.cfg.EUI)
		if err != nil {
			continue
		}
		dl := r.period
		if train {
			r.trainCh[l.Cell.Channel] = true
			dl++
		} else {
			busyCh[l.Cell.Channel] = true
		}
		r.usedCells[l.Cell] = true
		st.probing = true
		r.ref++
		r.inflight[r.ref] = &inflight{probe: st, deadline: dl}
		out = append(out, simnet.TxEntry{Channel: l.Cell.Channel, Slot: l.Cell.Slot, PreMs: pre, Ref: r.ref, Frame: b})
	}
	return out
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
	r.txOutcome(inf, res.Acked)
}

func (r *Runtime) txOutcome(inf *inflight, acked bool) {
	if inf.probe != nil {
		r.mu.Lock()
		st := inf.probe
		st.probing = false
		if acked {
			st.suspect = false
			log.Printf("probe %016x: lock answered", st.eui)
		} else {
			st.probeBackoff *= 2
			if st.probeBackoff > probeMax {
				st.probeBackoff = probeMax
			}
			st.probeAt = time.Now().Add(st.probeBackoff)
		}
		r.mu.Unlock()
		return
	}
	r.outcome(inf.cmd, acked)
}

func (r *Runtime) outcome(c *scheduler.Command, acked bool) {
	out := r.sched.Result(c, acked)
	if out == scheduler.Failed {
		// The lock is unreachable: probe it with backoff until it answers.
		r.mu.Lock()
		if l, err := r.table.Get(c.LockEUI); err == nil {
			if st := r.byShort[l.Short]; st != nil && !st.suspect {
				st.suspect, st.probeBackoff = true, probeMin
				st.probeAt = time.Now().Add(probeMin)
			}
		}
		r.mu.Unlock()
	}
	switch out {
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
	if (f.Type != slp.FTData && f.Type != slp.FTMacCmd) || !f.Security || f.SrcMode != slp.AddrShort || f.PAN != r.cfg.PAN {
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
	st.suspect = false // it is back
	r.mu.Unlock()
	if g.AckRequest {
		r.sendAck(st, uint16(f.Src), g.Seq)
	}
	if g.Type == slp.FTMacCmd {
		if len(g.Payload) >= 2 && g.Payload[0] == simnet.MacSniffDiv {
			div := g.Payload[1]
			if div == 0 {
				div = 1
			}
			r.stMu.Lock()
			st.div = div
			r.stMu.Unlock()
			log.Printf("lock %016x now sniffs every %d period(s) (fake-wake-up guard)", st.eui, div)
		}
		return
	}

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

// sendAck answers an uplink right away: the lock keeps its receiver on for
// a few milliseconds after transmitting and retries with back-off (300 ms,
// 600 ms, ...) when the ACK does not come. The ACK is authenticated with
// K_link and carries where the gateway is in its period, the period number
// and the time: this replaces the beacon (slot phase, time sync), so the
// gateway never transmits when it has nothing to say.
func (r *Runtime) sendAck(st *lockState, dst uint16, seq uint8) {
	r.mu.Lock()
	now := time.Now()
	phase := 0
	if !r.periodStart.IsZero() {
		phase = int(now.Sub(r.periodStart).Milliseconds())
		if phase >= r.cfg.PeriodMs {
			phase = r.cfg.PeriodMs - 1
		}
	}
	f := &slp.Frame{Type: slp.FTAck, Security: true, PAN: r.cfg.PAN, Seq: seq, DstMode: slp.AddrShort, Dst: uint64(dst),
		SrcMode: slp.AddrShort, Src: slp.ShortGateway, FrameCounter: r.nextFC(), KeyID: slp.KeyLink,
		Payload: simnet.AckPayload(phase, r.period, uint32(now.Unix()))}
	b, err := f.EncodeSecure(st.key, r.cfg.EUI)
	if err != nil {
		r.mu.Unlock()
		return
	}
	st.lastSync = now
	r.ref++
	ref := r.ref
	r.mu.Unlock()
	_ = r.radio.Send(simnet.EncodeTxNow(ref, b))
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
	r.stMu.Lock()
	r.byEUI[eui] = st
	st.div = 1
	r.stMu.Unlock()
	now := time.Now()
	st.rxValid = false // the lock restarted its MAC
	st.lastSeen, st.lastSync, st.suspect = now, now, false
	phase := 0
	if !r.periodStart.IsZero() {
		phase = int(now.Sub(r.periodStart).Milliseconds()) % r.cfg.PeriodMs
	}
	period := r.period
	r.mu.Unlock()

	pl := []byte{simnet.MacJoinAccept}
	pl = binary.LittleEndian.AppendUint16(pl, l.Short)
	pl = append(pl, byte(l.Cell.Channel))
	pl = binary.LittleEndian.AppendUint16(pl, uint16(l.Cell.Slot))
	pl = binary.LittleEndian.AppendUint16(pl, uint16(r.cfg.PeriodMs))
	pl = binary.LittleEndian.AppendUint16(pl, uint16(r.cfg.SlotMs))
	pl = binary.LittleEndian.AppendUint16(pl, uint16(phase))
	pl = binary.LittleEndian.AppendUint32(pl, period)
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
		if time.Since(st.lastSeen) < r.cfg.LockOfflineAfter {
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
