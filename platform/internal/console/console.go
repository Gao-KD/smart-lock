// Package console is the simulator edition of the platform: it wires the
// dispatch service to MQTT, decrypts lock uplinks, keeps the platform's view
// of every lock (state, battery, last seen), and serves the web console and
// the HTTP API used by the demo scripts.
package console

import (
	"context"
	"encoding/json"
	"fmt"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/Gao-KD/smart-lock/common/slpgo/simnet"
	"github.com/Gao-KD/smart-lock/common/slpgo/slp"
	"github.com/Gao-KD/smart-lock/platform/internal/dispatch"
)

// LockView is what the platform knows about a lock from its uplinks.
type LockView struct {
	EUI        string    `json:"eui"`
	Room       string    `json:"room"`
	Floor      int       `json:"floor"`
	RoomNo     int       `json:"room_no"`
	Gateway    string    `json:"gateway"`
	Joined     bool      `json:"joined"`
	State      string    `json:"state,omitempty"`
	BatteryMV  int       `json:"battery_mv,omitempty"`
	BattLevel  int       `json:"batt_level"`
	Cards      int       `json:"cards"`
	RSSI       int       `json:"rssi,omitempty"`
	LastSeen   time.Time `json:"last_seen,omitempty"`
	Online     bool      `json:"online"`
	LastAccess string    `json:"last_access,omitempty"`
	Sim        *SimLock  `json:"sim,omitempty"`

	// Door sensor, inside double lock and reader lockout (TLV FLAGS / EVT_DOOR).
	DoorOpen   bool `json:"door_open"`
	DoubleLock bool `json:"double_lock"`
	LockedOut  bool `json:"locked_out"`
	// Record log: highest LOG_SEQ the lock reported, records received, and
	// records known to exist but not received yet (pulled with READ_LOG).
	LogSeq     uint32         `json:"log_seq"`
	Records    int            `json:"records"`
	Missing    int            `json:"missing"`
	ReadLogs   int            `json:"read_logs"`
	Alarms     int            `json:"alarms"`
	LastAlarm  string         `json:"last_alarm,omitempty"`
	Request    *UnlockRequest `json:"request,omitempty"`
	AutolockS  int            `json:"autolock_s,omitempty"`
	Remote     *bool          `json:"remote_allowed,omitempty"`
	Reconnects int            `json:"reconnects"`

	upseq      uint32 // highest UPSEQ accepted
	upseqValid bool
	upseqWin   uint64 // bit i: UPSEQ upseq-i already accepted
	logSeen    map[uint32]bool
	missing    map[uint32]time.Time
	readLogAt  time.Time
	dedup      map[uint64]bool
	dedupOrder []uint64
	ofpIndex   map[uint32]uint8 // hour -> next offline password index
}

// UnlockRequest is a visitor's pending request (call button at the door).
type UnlockRequest struct {
	ID    uint16    `json:"id"`
	Until time.Time `json:"until"`
}

// Event is one line of the console's event log.
type Event struct {
	ID   uint64    `json:"id"`
	At   time.Time `json:"at"`
	Room string    `json:"room,omitempty"`
	Kind string    `json:"kind"` // access, alarm, battery, cmd, join, security, fw, system
	Text string    `json:"text"`
}

type GatewayView struct {
	ID          string    `json:"id"`
	Online      bool      `json:"online"`
	Locks       int       `json:"locks"`
	OnlineLocks int       `json:"online_locks"`
	Pending     int       `json:"pending"`
	Period      uint32    `json:"period"`
	Channels    int       `json:"channels"`
	Slots       int       `json:"slots"`
	PeriodMs    int       `json:"period_ms"`
	LastSeen    time.Time `json:"last_seen"`
}

type Console struct {
	Tenant       string
	Reg          Registry
	Svc          *dispatch.Service
	Sim          *SimCtl
	OfflineAfter time.Duration

	mu       sync.Mutex
	locks    map[uint64]*LockView
	gws      map[string]*GatewayView
	events   []Event
	eventSeq uint64
}

func New(tenant string, layout simnet.Layout, svc *dispatch.Service, offlineAfter time.Duration) *Console {
	c := &Console{Tenant: tenant, Reg: Registry{Tenant: tenant, Layout: layout}, Svc: svc,
		OfflineAfter: offlineAfter, locks: map[uint64]*LockView{}, gws: map[string]*GatewayView{}}
	for f := 1; f <= layout.Floors; f++ {
		for r := 1; r <= layout.Rooms; r++ {
			eui := simnet.LockEUI(layout.Building, f, r)
			c.locks[eui] = &LockView{EUI: fmt.Sprintf("%016x", eui), Room: simnet.RoomName(f, r), Floor: f, RoomNo: r,
				Gateway: simnet.GatewayID(layout.Building, layout.GatewayOfFloor(f))}
		}
	}
	return c
}

func (c *Console) room(eui uint64) string {
	if l := c.locks[eui]; l != nil {
		return l.Room
	}
	return fmt.Sprintf("%016x", eui)
}

// AddEvent appends to the event log (caller must not hold c.mu).
func (c *Console) AddEvent(eui uint64, kind, text string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.addEventLocked(eui, kind, text)
}

func (c *Console) addEventLocked(eui uint64, kind, text string) {
	c.eventSeq++
	room := ""
	if eui != 0 {
		room = c.room(eui)
	}
	c.events = append(c.events, Event{ID: c.eventSeq, At: time.Now(), Room: room, Kind: kind, Text: text})
	if len(c.events) > 3000 {
		c.events = append([]Event(nil), c.events[len(c.events)-2000:]...)
	}
}

// OnFirmwareLog receives locksim log lines (what the lock firmware did).
func (c *Console) OnFirmwareLog(eui uint64, text string) {
	kind := "fw"
	if strings.Contains(text, "防重放") || strings.Contains(text, "MIC") || strings.Contains(text, "攻击者") {
		kind = "security"
	}
	c.AddEvent(eui, kind, text)
}

// ---- MQTT uplinks ----

type ackMsg struct {
	MsgID    uint32 `json:"msg_id"`
	State    string `json:"state"`
	LockEUI  string `json:"lock_eui"`
	Attempts int    `json:"attempts"`
	Reason   string `json:"reason"`
	Cells    int    `json:"cells"`
}

type upMsg struct {
	LockEUI    string `json:"lock_eui"`
	AppPayload []byte `json:"app_payload"`
	RSSI       int    `json:"rssi"`
}

type joinMsg struct {
	LockEUI string `json:"lock_eui"`
	Short   uint16 `json:"short"`
	Channel int    `json:"channel"`
	Slot    int    `json:"slot"`
}

// HandleUplink processes one MQTT message from sl/v1/{tenant}/{gw}/up/{kind}.
func (c *Console) HandleUplink(gateway, kind string, payload []byte) {
	switch kind {
	case "up/ack":
		var a ackMsg
		if json.Unmarshal(payload, &a) != nil {
			return
		}
		c.Svc.OnAck(dispatch.GatewayAck{MsgID: a.MsgID, State: a.State, Attempts: a.Attempts, Reason: a.Reason, Cells: a.Cells})
		if a.State == "failed" || a.State == "expired" || a.State == "retrying" {
			eui, _ := strconv.ParseUint(a.LockEUI, 16, 64)
			kind := "cmd"
			if a.State != "retrying" {
				kind = "alarm"
			}
			c.AddEvent(eui, kind, fmt.Sprintf("指令 %d: 网关报告 %s (%s, 已发送 %d 次)", a.MsgID, a.State, a.Reason, a.Attempts))
		}
	case "up/result", "up/event":
		var u upMsg
		if json.Unmarshal(payload, &u) != nil {
			return
		}
		eui, err := strconv.ParseUint(u.LockEUI, 16, 64)
		if err != nil {
			return
		}
		c.onLockUplink(eui, u)
	case "up/join":
		var j joinMsg
		if json.Unmarshal(payload, &j) != nil {
			return
		}
		eui, _ := strconv.ParseUint(j.LockEUI, 16, 64)
		c.mu.Lock()
		if l := c.locks[eui]; l != nil {
			l.Joined = true
			l.LastSeen = time.Now()
		}
		c.mu.Unlock()
	case "up/status":
		var s GatewayView
		if json.Unmarshal(payload, &s) != nil {
			return
		}
		c.mu.Lock()
		prev := c.gws[gateway]
		s.ID = gateway
		s.LastSeen = time.Now()
		c.gws[gateway] = &s
		if prev == nil || prev.Online != s.Online {
			state := "上线"
			if !s.Online {
				state = "离线"
			}
			c.addEventLocked(0, "system", fmt.Sprintf("网关 %s %s", gateway, state))
		}
		c.mu.Unlock()
	}
}

func (c *Console) onLockUplink(eui uint64, u upMsg) {
	seq, m, err := slp.OpenUp(simnet.Key(simnet.TagApp, eui), eui, u.AppPayload)
	if err != nil {
		c.AddEvent(eui, "security", "上行消息端到端 MIC 校验失败，丢弃")
		return
	}
	c.mu.Lock()
	l := c.locks[eui]
	if l == nil {
		c.mu.Unlock()
		return
	}
	if !l.acceptUpseq(seq) {
		c.addEventLocked(eui, "security", fmt.Sprintf("上行防重放: 序号 %d 已用过或太旧 (最新 %d)，丢弃", seq, l.upseq))
		c.mu.Unlock()
		return
	}
	l.LastSeen = time.Now()
	l.Joined = true
	l.RSSI = u.RSSI
	if !l.firstSight(uint64(m.Cmd)<<32 | uint64(m.MsgID)) {
		// The lock retransmitted because our ACK got lost: already handled.
		c.mu.Unlock()
		return
	}
	if v, ok := m.U8(slp.TLVFlags); ok {
		l.DoorOpen, l.DoubleLock, l.LockedOut = v&slp.LFDoorOpen != 0, v&slp.LFDoubleLock != 0, v&slp.LFLockedOut != 0
	}
	if v, ok := m.U16(slp.TLVCfgAutolockS); ok {
		l.AutolockS = int(v)
	}
	if v, ok := m.U8(slp.TLVCfgRemote); ok {
		b := v != 0
		l.Remote = &b
	}
	if st, ok := m.U8(slp.TLVLockState); ok && int(st) < len(slp.LockStateNames) {
		l.State = slp.LockStateNames[st]
	}
	if v, ok := m.U16(slp.TLVBatteryMV); ok {
		l.BatteryMV = int(v)
	}
	if v, ok := m.U8(slp.TLVBattLevel); ok {
		l.BattLevel = int(v)
	}
	if v, ok := m.U16(slp.TLVCredCount); ok {
		l.Cards = int(v)
	}

	switch m.Cmd {
	case slp.RspResult:
		res, _ := m.U8(slp.TLVResult)
		if v, ok := m.U32(slp.TLVLogSeq); ok {
			c.noteHead(l, v)
		}
		name := slp.ResultNames[res]
		c.mu.Unlock()
		c.Svc.OnResult(eui, m.MsgID, name)
		rec, _ := c.Svc.Record(m.MsgID)
		if rec.Group == 0 {
			c.AddEvent(eui, "cmd", fmt.Sprintf("%s (指令 %d) 执行结果: %s", rec.CmdName, m.MsgID, name))
		}
		c.Svc.LockSeen(context.Background(), eui)
		return
	case slp.EvtAccess, slp.EvtAlarm, slp.EvtDoor, slp.EvtUnlockReq:
		if !c.noteRecord(l, m) {
			break // already have this record (READ_LOG re-sent it)
		}
		c.onRecord(eui, l, m)
	case slp.EvtBattery:
		if !c.noteRecord(l, m) {
			break
		}
		lv, _ := m.U8(slp.TLVBattLevel)
		mv, _ := m.U16(slp.TLVBatteryMV)
		names := []string{"恢复正常", "低电量，请尽快更换电池", "严重低电，请立即更换电池"}
		kind := "battery"
		if lv > 0 {
			kind = "alarm"
		}
		if int(lv) < len(names) {
			c.addEventLocked(eui, kind, fmt.Sprintf("电池告警: %d mV，%s", mv, names[lv]))
		}
	case slp.EvtHeartbeat:
		if v, ok := m.U32(slp.TLVLogSeq); ok {
			c.noteHead(l, v)
		}
		if r, _ := m.U8(slp.TLVHBReason); r == slp.HBReconnect {
			l.Reconnects++
			c.addEventLocked(eui, "system", "门锁重新连上网关（重连心跳），平台补发挂起的指令并检查记录是否缺失")
		}
	}
	c.mu.Unlock()
	c.Svc.LockSeen(context.Background(), eui)
}

// ---- views for the web console ----

type StateView struct {
	Layout   simnet.Layout     `json:"layout"`
	Tenant   string            `json:"tenant"`
	Gateways []*GatewayView    `json:"gateways"`
	Locks    []LockView        `json:"locks"`
	Events   []Event           `json:"events"`
	Commands []dispatch.Record `json:"commands"`
	SimOK    bool              `json:"sim_ok"`
	Loss     int               `json:"loss"`
	Now      time.Time         `json:"now"`
}

func (c *Console) State(sinceEvent uint64, withLocks bool) StateView {
	var snap map[uint64]SimLock
	var loss int
	var ok bool
	if c.Sim != nil {
		snap, loss, ok = c.Sim.Snapshot()
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	v := StateView{Layout: c.Reg.Layout, Tenant: c.Tenant, SimOK: ok, Loss: loss, Now: time.Now()}
	for _, g := range c.gws {
		cp := *g
		if time.Since(g.LastSeen) > 15*time.Second {
			cp.Online = false
		}
		v.Gateways = append(v.Gateways, &cp)
	}
	sort.Slice(v.Gateways, func(i, j int) bool { return v.Gateways[i].ID < v.Gateways[j].ID })
	if withLocks {
		for eui, l := range c.locks {
			cp := *l
			cp.Online = !l.LastSeen.IsZero() && time.Since(l.LastSeen) < c.OfflineAfter
			if s, ok := snap[eui]; ok {
				s := s
				cp.Sim = &s
			}
			v.Locks = append(v.Locks, cp)
		}
		sort.Slice(v.Locks, func(i, j int) bool { return v.Locks[i].EUI < v.Locks[j].EUI })
	}
	for _, e := range c.events {
		if e.ID > sinceEvent {
			v.Events = append(v.Events, e)
		}
	}
	if len(v.Events) > 400 {
		v.Events = v.Events[len(v.Events)-400:]
	}
	v.Commands = c.Svc.Recent(40) // dispatch never calls back into the console
	return v
}

// Lock returns the platform view of one lock.
func (c *Console) Lock(eui uint64) (LockView, bool) {
	var snap map[uint64]SimLock
	if c.Sim != nil {
		snap, _, _ = c.Sim.Snapshot()
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	l, ok := c.locks[eui]
	if !ok {
		return LockView{}, false
	}
	cp := *l
	cp.Online = !l.LastSeen.IsZero() && time.Since(l.LastSeen) < c.OfflineAfter
	if s, ok := snap[eui]; ok {
		cp.Sim = &s
	}
	return cp, true
}

// CardTLV builds the CRED_ADD / CRED_DEL TLVs for a campus card UID.
func CardTLV(uid []byte, userID uint32, validTo time.Time) []byte {
	t := []slp.TLV{slp.U8(slp.TLVCredType, slp.CredCardUID), {Type: slp.TLVCredID, Value: uid}}
	if userID != 0 {
		t = append(t, slp.U32(slp.TLVUserID, userID))
	}
	if !validTo.IsZero() {
		t = append(t, slp.U32(slp.TLVValidTo, uint32(validTo.Unix())))
	}
	return slp.EncodeTLV(t)
}

// RoomCardUID is the demo card of a room: 4-byte UID C0 BB FF RR.
func RoomCardUID(building, floor, room int) []byte {
	return []byte{0xC0 | byte(building&0x0F), byte(building >> 4), byte(floor), byte(room)}
}

func durationTLV(s uint16) []byte {
	if s == 0 {
		return nil
	}
	return []byte{slp.TLVDurationS, 2, byte(s), byte(s >> 8)}
}

// acceptUpseq is the uplink anti-replay check with a 64-message sliding
// window (as in IPsec): results and events travel on different MQTT topics
// and may be handled out of order, so a strictly increasing check would
// drop genuine messages. Each UPSEQ is still accepted at most once.
func (l *LockView) acceptUpseq(seq uint32) bool {
	if !l.upseqValid || seq > l.upseq {
		if l.upseqValid {
			if d := seq - l.upseq; d < 64 {
				l.upseqWin <<= d
			} else {
				l.upseqWin = 0
			}
		}
		l.upseqWin |= 1
		l.upseq, l.upseqValid = seq, true
		return true
	}
	d := l.upseq - seq
	if d >= 64 || l.upseqWin&(1<<d) != 0 {
		return false
	}
	l.upseqWin |= 1 << d
	return true
}
