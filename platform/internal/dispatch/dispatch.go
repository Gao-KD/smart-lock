// Package dispatch turns business requests (remote unlock, bulk credential
// sync, emergency open) into per-gateway command batches and tracks each
// command through its three acknowledgements:
//
//	queued -> accepted (gateway got it) -> delivered (lock ACKed on air)
//	       -> executed (lock's end-to-end result)    | failed / expired
//
// Commands for different gateways are fully independent, so a campus-wide
// operation fans out to every building gateway at once.
package dispatch

import (
	"context"
	"errors"
	"fmt"
	"sort"
	"sync"
	"sync/atomic"
	"time"
)

type Priority uint8

const (
	P0Emergency   Priority = 0
	P1Interactive Priority = 1
	P2Management  Priority = 2
	P3Background  Priority = 3
)

// Command codes, mirroring common/slp/include/slp/app.h.
const (
	CmdUnlock    uint8 = 0x01
	CmdLockdown  uint8 = 0x02
	CmdRelease   uint8 = 0x03
	CmdEmergency uint8 = 0x04
	CmdCredAdd   uint8 = 0x10
	CmdCredDel   uint8 = 0x11
	CmdStatus    uint8 = 0x50
)

// CmdNames are the Chinese labels shown in the console.
var CmdNames = map[uint8]string{
	CmdUnlock: "远程开门", CmdLockdown: "封锁", CmdRelease: "解除封锁", CmdEmergency: "应急常开",
	CmdCredAdd: "添加卡", CmdCredDel: "删除卡", CmdStatus: "状态查询",
}

const BroadcastGroup uint16 = 0xFFFF

type Status string

const (
	StatusQueued    Status = "queued"
	StatusAccepted  Status = "accepted"
	StatusDelivered Status = "delivered"
	StatusExecuted  Status = "executed"
	StatusFailed    Status = "failed"
	StatusExpired   Status = "expired"
)

// Item is one command inside a batch (proto: CommandBatch.Item).
type Item struct {
	MsgID      uint32    `json:"msg_id"`
	LockEUI    uint64    `json:"lock_eui,omitempty"`
	Group      uint16    `json:"group,omitempty"`
	Priority   Priority  `json:"priority"`
	ExpiresAt  time.Time `json:"expires_at"`
	AppPayload []byte    `json:"app_payload"` // sealed with the lock's K_app
}

// Batch is published to sl/v1/{tenant}/{gateway}/down/cmd.
type Batch struct {
	BatchID  uint64    `json:"batch_id"`
	Tenant   string    `json:"tenant"`
	Gateway  string    `json:"gateway"`
	IssuedAt time.Time `json:"issued_at"`
	Items    []Item    `json:"items"`
}

// LockInfo is what the registry knows about a lock.
type LockInfo struct {
	EUI     uint64
	Tenant  string
	Gateway string
}

// Registry resolves locks and buildings (backed by PostgreSQL).
type Registry interface {
	Lock(ctx context.Context, eui uint64) (LockInfo, error)
	GatewaysOfBuilding(ctx context.Context, tenant, building string) ([]string, error)
}

// Sealer builds and encrypts the end-to-end app message for a lock
// (K_app from the on-premise KMS). Group messages use K_grp.
type Sealer interface {
	Seal(ctx context.Context, eui uint64, cmd uint8, msgID uint32, expires time.Time, tlv []byte) ([]byte, error)
	SealGroup(ctx context.Context, tenant, gateway string, group uint16, cmd uint8, msgID uint32, expires time.Time, tlv []byte) ([]byte, error)
}

// Publisher sends a batch to the MQTT broker (QoS 1).
type Publisher interface {
	Publish(ctx context.Context, b Batch) error
}

var ErrUnknownLock = errors.New("dispatch: unknown lock")

type Config struct {
	MaxBatchItems  int           // per MQTT message, default 100
	InteractiveTTL time.Duration // remote unlock validity, default 30 s
	ManagementTTL  time.Duration // credential sync validity, default 24 h
}

// Record tracks one command through its acknowledgements.
type Record struct {
	MsgID       uint32    `json:"msg_id"`
	LockEUI     uint64    `json:"-"`
	Lock        string    `json:"lock,omitempty"` // EUI as 16 hex digits (JSON numbers lose 64-bit precision)
	Group       uint16    `json:"group,omitempty"`
	Gateway     string    `json:"gateway"`
	Cmd         uint8     `json:"cmd"`
	CmdName     string    `json:"cmd_name"`
	Priority    Priority  `json:"priority"`
	Status      Status    `json:"status"`
	Result      string    `json:"result,omitempty"` // lock's end-to-end result
	Reason      string    `json:"reason,omitempty"` // gateway reason (failed, retrying)
	Attempts    int       `json:"attempts,omitempty"`
	Resends     int       `json:"resends,omitempty"`  // re-published after the lock came back
	Executed    int       `json:"executed,omitempty"` // group: locks that reported a result
	Expected    int       `json:"expected,omitempty"` // group: locks behind that gateway
	Job         uint64    `json:"job,omitempty"`
	QueuedAt    time.Time `json:"queued_at"`
	AcceptedAt  time.Time `json:"accepted_at,omitempty"`
	DeliveredAt time.Time `json:"delivered_at,omitempty"`
	ExecutedAt  time.Time `json:"executed_at,omitempty"`

	item   Item
	tenant string
}

// Job groups the commands of one bulk operation.
type Job struct {
	ID      uint64    `json:"id"`
	Name    string    `json:"name"`
	MsgIDs  []uint32  `json:"-"`
	Started time.Time `json:"started"`
}

type Service struct {
	cfg   Config
	reg   Registry
	seal  Sealer
	pub   Publisher
	now   func() time.Time
	msgID atomic.Uint32
	batch atomic.Uint64
	jobID atomic.Uint64

	mu     sync.Mutex
	recs   map[uint32]*Record
	order  []uint32
	jobs   map[uint64]*Job
	parked map[uint64][]uint32 // lock -> failed management commands to resend
}

func New(cfg Config, reg Registry, seal Sealer, pub Publisher) *Service {
	if cfg.MaxBatchItems <= 0 {
		cfg.MaxBatchItems = 100
	}
	if cfg.InteractiveTTL <= 0 {
		cfg.InteractiveTTL = 30 * time.Second
	}
	if cfg.ManagementTTL <= 0 {
		cfg.ManagementTTL = 24 * time.Hour
	}
	s := &Service{cfg: cfg, reg: reg, seal: seal, pub: pub, now: time.Now,
		recs: make(map[uint32]*Record), jobs: make(map[uint64]*Job), parked: make(map[uint64][]uint32)}
	// Start ids from the clock so a restarted platform never reuses one
	// (the lock de-duplicates by message id).
	s.msgID.Store(uint32(time.Now().Unix()-1_700_000_000) << 4)
	return s
}

// Request is one unicast command to build.
type Request struct {
	LockEUI  uint64
	Cmd      uint8
	TLV      []byte
	Priority Priority
}

// Unlock sends a remote unlock and returns its message id.
func (s *Service) Unlock(ctx context.Context, eui uint64, durationS uint16) (uint32, error) {
	var tlv []byte
	if durationS > 0 {
		tlv = []byte{0x02, 2, byte(durationS), byte(durationS >> 8)} // SLP_TLV_DURATION_S
	}
	ids, err := s.Submit(ctx, []Request{{LockEUI: eui, Cmd: CmdUnlock, TLV: tlv, Priority: P1Interactive}})
	if err != nil {
		return 0, err
	}
	return ids[0], nil
}

// Submit builds, groups per gateway, batches and publishes unicast commands.
// Returns message ids in request order.
func (s *Service) Submit(ctx context.Context, reqs []Request) ([]uint32, error) {
	type key struct{ tenant, gw string }
	groups := make(map[key][]Item)
	ids := make([]uint32, len(reqs))
	now := s.now()

	for i, r := range reqs {
		info, err := s.reg.Lock(ctx, r.LockEUI)
		if err != nil {
			return nil, fmt.Errorf("%w: %016x", ErrUnknownLock, r.LockEUI)
		}
		id := s.msgID.Add(1)
		exp := now.Add(s.ttl(r.Priority))
		payload, err := s.seal.Seal(ctx, r.LockEUI, r.Cmd, id, exp, r.TLV)
		if err != nil {
			return nil, err
		}
		k := key{info.Tenant, info.Gateway}
		it := Item{MsgID: id, LockEUI: r.LockEUI, Priority: r.Priority, ExpiresAt: exp, AppPayload: payload}
		groups[k] = append(groups[k], it)
		s.track(&Record{MsgID: id, LockEUI: r.LockEUI, Gateway: info.Gateway, Cmd: r.Cmd, Priority: r.Priority, item: it, tenant: info.Tenant})
		ids[i] = id
	}

	keys := make([]key, 0, len(groups))
	for k := range groups {
		keys = append(keys, k)
	}
	sort.Slice(keys, func(i, j int) bool { return keys[i].gw < keys[j].gw })

	for _, k := range keys {
		items := groups[k]
		for start := 0; start < len(items); start += s.cfg.MaxBatchItems {
			end := min(start+s.cfg.MaxBatchItems, len(items))
			b := Batch{BatchID: s.batch.Add(1), Tenant: k.tenant, Gateway: k.gw, IssuedAt: now, Items: items[start:end]}
			if err := s.pub.Publish(ctx, b); err != nil {
				return nil, err
			}
			s.mark(b.Items, StatusQueued)
		}
	}
	return ids, nil
}

// EmergencyOpen broadcasts an emergency open to every gateway of a building.
func (s *Service) EmergencyOpen(ctx context.Context, tenant, building string) ([]uint32, error) {
	return s.Broadcast(ctx, tenant, building, CmdEmergency, P0Emergency)
}

// Broadcast sends one group command (emergency open, lockdown, release) to
// every lock of a building: one frame per gateway, heard by every lock in
// its own slot of the same listen period.
func (s *Service) Broadcast(ctx context.Context, tenant, building string, cmd uint8, prio Priority) ([]uint32, error) {
	gws, err := s.reg.GatewaysOfBuilding(ctx, tenant, building)
	if err != nil {
		return nil, err
	}
	now := s.now()
	var ids []uint32
	for _, gw := range gws {
		id := s.msgID.Add(1)
		exp := now.Add(s.cfg.InteractiveTTL)
		payload, err := s.seal.SealGroup(ctx, tenant, gw, BroadcastGroup, cmd, id, exp, nil)
		if err != nil {
			return nil, err
		}
		it := Item{MsgID: id, Group: BroadcastGroup, Priority: prio, ExpiresAt: exp, AppPayload: payload}
		s.track(&Record{MsgID: id, Group: BroadcastGroup, Gateway: gw, Cmd: cmd, Priority: prio, item: it, tenant: tenant})
		if err := s.pub.Publish(ctx, Batch{BatchID: s.batch.Add(1), Tenant: tenant, Gateway: gw, IssuedAt: now, Items: []Item{it}}); err != nil {
			return nil, err
		}
		s.mark([]Item{it}, StatusQueued)
		ids = append(ids, id)
	}
	return ids, nil
}

// NewJob records a bulk operation over the given message ids.
func (s *Service) NewJob(name string, ids []uint32) *Job {
	j := &Job{ID: s.jobID.Add(1), Name: name, MsgIDs: ids, Started: s.now()}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.jobs[j.ID] = j
	for _, id := range ids {
		if r := s.recs[id]; r != nil {
			r.Job = j.ID
		}
	}
	return j
}

// JobProgress summarises a job.
type JobProgress struct {
	Job
	Total      int            `json:"total"`
	Counts     map[Status]int `json:"counts"`
	Results    map[string]int `json:"results"`
	Done       bool           `json:"done"`
	LocksDone  int            `json:"locks_done"` // unicast: executed; group: locks that reported
	LocksTotal int            `json:"locks_total"`
	ElapsedMs  int64          `json:"elapsed_ms"` // until the last result (or now)
	P50Ms      int64          `json:"p50_ms"`
	P99Ms      int64          `json:"p99_ms"`
	MaxMs      int64          `json:"max_ms"`
}

func (s *Service) Job(id uint64) (JobProgress, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	j, ok := s.jobs[id]
	if !ok {
		return JobProgress{}, false
	}
	p := JobProgress{Job: *j, Total: len(j.MsgIDs), Counts: map[Status]int{}, Results: map[string]int{}, Done: true}
	var last time.Time
	var lat []int64
	for _, mid := range j.MsgIDs {
		r := s.recs[mid]
		if r == nil {
			continue
		}
		p.Counts[r.Status]++
		if r.Result != "" {
			p.Results[r.Result]++
		}
		if r.Group != 0 {
			p.LocksTotal += r.Expected
			p.LocksDone += r.Executed
			if r.Expected == 0 || r.Executed < r.Expected {
				p.Done = false
			}
		} else {
			p.LocksTotal++
			if r.Status == StatusExecuted {
				p.LocksDone++
			}
			if rank(r.Status) < 3 {
				p.Done = false
			}
		}
		if !r.ExecutedAt.IsZero() {
			if r.ExecutedAt.After(last) {
				last = r.ExecutedAt
			}
			lat = append(lat, r.ExecutedAt.Sub(r.QueuedAt).Milliseconds())
		}
	}
	if p.Done && !last.IsZero() {
		p.ElapsedMs = last.Sub(j.Started).Milliseconds()
	} else {
		p.ElapsedMs = s.now().Sub(j.Started).Milliseconds()
	}
	if len(lat) > 0 {
		sort.Slice(lat, func(a, b int) bool { return lat[a] < lat[b] })
		p.P50Ms = lat[len(lat)/2]
		p.P99Ms = lat[(len(lat)*99)/100]
		if p.P99Ms == 0 || len(lat) < 100 {
			p.P99Ms = lat[len(lat)-1]
		}
		p.MaxMs = lat[len(lat)-1]
	}
	return p, true
}

// SetExpected records how many locks should answer a group command.
func (s *Service) SetExpected(msgID uint32, n int) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if r := s.recs[msgID]; r != nil {
		r.Expected = n
	}
}

// GatewayAck is the gateway's up/ack message.
type GatewayAck struct {
	MsgID    uint32
	State    string // accepted, retrying, delivered, failed, expired
	Attempts int
	Reason   string
	Cells    int
}

// OnAck applies an up/ack from a gateway.
func (s *Service) OnAck(a GatewayAck) {
	s.mu.Lock()
	defer s.mu.Unlock()
	r := s.recs[a.MsgID]
	if r == nil {
		return
	}
	now := s.now()
	if a.Attempts > r.Attempts {
		r.Attempts = a.Attempts
	}
	switch a.State {
	case "accepted":
		if r.AcceptedAt.IsZero() {
			r.AcceptedAt = now
		}
		s.advance(r, StatusAccepted)
	case "retrying":
		r.Reason = fmt.Sprintf("第 %d 次发送未收到 ACK，下个周期重发", a.Attempts)
	case "delivered":
		if r.DeliveredAt.IsZero() {
			r.DeliveredAt = now
		}
		if a.Reason == "broadcast" {
			r.Reason = fmt.Sprintf("广播帧在 %d 个(信道,时隙)单元各发送一次", a.Cells)
		}
		s.advance(r, StatusDelivered)
	case "failed":
		r.Reason = a.Reason
		if rank(r.Status) < 3 {
			r.Status = StatusFailed
			if r.Priority >= P2Management && r.LockEUI != 0 {
				// Management commands stay valid for hours: park them and
				// re-send as soon as the lock is heard again.
				s.parked[r.LockEUI] = append(s.parked[r.LockEUI], r.MsgID)
				r.Reason += "；已挂起，锁重新上线后自动补发"
			}
		}
	case "expired":
		r.Reason = "过期未送达，网关已丢弃"
		if rank(r.Status) < 3 {
			r.Status = StatusExpired
		}
	}
}

// OnResult applies a lock's end-to-end result (already decrypted).
// DUPLICATE means the lock had executed this message id before, which only
// happens when an ACK got lost: it counts as success.
func (s *Service) OnResult(lockEUI uint64, msgID uint32, result string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	r := s.recs[msgID]
	if r == nil {
		return
	}
	now := s.now()
	if r.Group != 0 {
		r.Executed++
		if r.ExecutedAt.IsZero() || now.After(r.ExecutedAt) {
			r.ExecutedAt = now
		}
		r.Result = "OK"
		return
	}
	if r.LockEUI != lockEUI {
		return
	}
	if r.Result == "" || result != "DUPLICATE" {
		r.Result = result
	}
	if r.ExecutedAt.IsZero() {
		r.ExecutedAt = now
	}
	r.Status = StatusExecuted
}

// LockSeen is called for every uplink from a lock. Parked management
// commands are re-published with their original message id, so a lock that
// did get the first copy answers DUPLICATE instead of executing twice.
func (s *Service) LockSeen(ctx context.Context, eui uint64) int {
	s.mu.Lock()
	ids := s.parked[eui]
	delete(s.parked, eui)
	var batches []Batch
	now := s.now()
	for _, id := range ids {
		r := s.recs[id]
		if r == nil || r.Status != StatusFailed || now.After(r.item.ExpiresAt) {
			continue
		}
		r.Status = StatusQueued
		r.Resends++
		r.Reason = "锁已重新上线，自动补发"
		batches = append(batches, Batch{BatchID: s.batch.Add(1), Tenant: r.tenant, Gateway: r.Gateway, IssuedAt: now, Items: []Item{r.item}})
	}
	s.mu.Unlock()
	for _, b := range batches {
		_ = s.pub.Publish(ctx, b)
	}
	return len(batches)
}

// Record returns a copy of a command record.
func (s *Service) Record(msgID uint32) (Record, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	r, ok := s.recs[msgID]
	if !ok {
		return Record{}, false
	}
	return *r, true
}

// Recent returns the newest n records, newest first.
func (s *Service) Recent(n int) []Record {
	s.mu.Lock()
	defer s.mu.Unlock()
	var out []Record
	for i := len(s.order) - 1; i >= 0 && len(out) < n; i-- {
		if r := s.recs[s.order[i]]; r != nil {
			out = append(out, *r)
		}
	}
	return out
}

func (s *Service) track(r *Record) {
	r.QueuedAt = s.now()
	r.Status = StatusQueued
	r.CmdName = CmdNames[r.Cmd]
	if r.LockEUI != 0 {
		r.Lock = fmt.Sprintf("%016x", r.LockEUI)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.recs[r.MsgID] = r
	s.order = append(s.order, r.MsgID)
	const keep = 20000
	if len(s.order) > keep {
		for _, old := range s.order[:len(s.order)-keep] {
			delete(s.recs, old)
		}
		s.order = append([]uint32(nil), s.order[len(s.order)-keep:]...)
	}
}

func (s *Service) advance(r *Record, st Status) {
	if rank(st) > rank(r.Status) {
		r.Status = st
	}
}

// Update records an acknowledgement from up/ack or up/result. Status only
// moves forward, so late or duplicated MQTT messages are harmless.
func (s *Service) Update(msgID uint32, st Status) {
	s.mu.Lock()
	defer s.mu.Unlock()
	r, ok := s.recs[msgID]
	if !ok {
		r = &Record{MsgID: msgID, Status: st}
		s.recs[msgID] = r
		return
	}
	s.advance(r, st)
}

// Status returns the current status of a message.
func (s *Service) Status(msgID uint32) (Status, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	r, ok := s.recs[msgID]
	if !ok {
		return "", false
	}
	return r.Status, true
}

func (s *Service) ttl(p Priority) time.Duration {
	if p <= P1Interactive {
		return s.cfg.InteractiveTTL
	}
	return s.cfg.ManagementTTL
}

func (s *Service) mark(items []Item, st Status) {
	for _, it := range items {
		s.Update(it.MsgID, st)
	}
}

func rank(s Status) int {
	switch s {
	case StatusQueued:
		return 0
	case StatusAccepted:
		return 1
	case StatusDelivered:
		return 2
	default: // executed, failed, expired are final
		return 3
	}
}
