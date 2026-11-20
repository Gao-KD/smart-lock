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
)

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

type Service struct {
	cfg   Config
	reg   Registry
	seal  Sealer
	pub   Publisher
	now   func() time.Time
	msgID atomic.Uint32
	batch atomic.Uint64

	mu     sync.Mutex
	status map[uint32]Status
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
	return &Service{cfg: cfg, reg: reg, seal: seal, pub: pub, now: time.Now, status: make(map[uint32]Status)}
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
		groups[k] = append(groups[k], Item{MsgID: id, LockEUI: r.LockEUI, Priority: r.Priority, ExpiresAt: exp, AppPayload: payload})
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
	gws, err := s.reg.GatewaysOfBuilding(ctx, tenant, building)
	if err != nil {
		return nil, err
	}
	now := s.now()
	var ids []uint32
	for _, gw := range gws {
		id := s.msgID.Add(1)
		exp := now.Add(s.cfg.InteractiveTTL)
		payload, err := s.seal.SealGroup(ctx, tenant, gw, BroadcastGroup, CmdEmergency, id, exp, nil)
		if err != nil {
			return nil, err
		}
		it := Item{MsgID: id, Group: BroadcastGroup, Priority: P0Emergency, ExpiresAt: exp, AppPayload: payload}
		if err := s.pub.Publish(ctx, Batch{BatchID: s.batch.Add(1), Tenant: tenant, Gateway: gw, IssuedAt: now, Items: []Item{it}}); err != nil {
			return nil, err
		}
		s.mark([]Item{it}, StatusQueued)
		ids = append(ids, id)
	}
	return ids, nil
}

// Update records an acknowledgement from up/ack or up/result. Status only
// moves forward, so late or duplicated MQTT messages are harmless.
func (s *Service) Update(msgID uint32, st Status) {
	s.mu.Lock()
	defer s.mu.Unlock()
	cur, ok := s.status[msgID]
	if !ok || rank(st) > rank(cur) {
		s.status[msgID] = st
	}
}

// Status returns the current status of a message.
func (s *Service) Status(msgID uint32) (Status, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	st, ok := s.status[msgID]
	return st, ok
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
