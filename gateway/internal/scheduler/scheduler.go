// Package scheduler decides, for each listen period, which command goes out
// in which (channel, slot) cell. See the design doc section "批量秒级下发方案".
//
// Rules:
//   - A lock can only be reached in its own cell, once per period.
//   - Higher priority first, FIFO within a priority.
//   - Group/broadcast commands at P0/P1 take a whole period on every
//     channel (every lock hears it once in its own slot). P2/P3 broadcasts
//     only go out when no P0/P1 unicast is waiting.
//   - P2/P3 may use at most MaxBackgroundShare of each channel per period,
//     so management traffic never starves interactive unlocks.
//   - Expired commands are dropped, unacked ones retried up to MaxAttempts.
package scheduler

import (
	"errors"
	"sort"
	"sync"
	"time"

	"github.com/Gao-KD/smart-lock/gateway/internal/locktable"
)

type Priority uint8

const (
	P0Emergency   Priority = 0 // fire-alarm open, building lockdown
	P1Interactive Priority = 1 // remote unlock, status query: a person is waiting
	P2Management  Priority = 2 // credentials, time windows, config
	P3Background  Priority = 3 // OTA, log pull
)

// Command is one end-to-end command from the platform. Payload is already
// encrypted with the lock's K_app; the gateway never decrypts it.
type Command struct {
	MsgID     uint32
	LockEUI   uint64 // unicast target, ignored when Group != 0
	Group     uint16 // 0 = unicast, 0xFFFF = broadcast, else group address
	Priority  Priority
	ExpiresAt time.Time
	Payload   []byte

	Attempts int
	seq      uint64
}

func (c *Command) IsGroup() bool { return c.Group != 0 }

// Tx is one planned transmission.
type Tx struct {
	Cell locktable.Cell
	Dst  uint16 // short address, group address or broadcast
	Cmd  *Command
}

type Config struct {
	MaxBackgroundShare float64 // default 0.7
	MaxAttempts        int     // default 3
}

var ErrUnknownLock = errors.New("scheduler: lock not in table")

type Scheduler struct {
	mu      sync.Mutex
	cfg     Config
	table   *locktable.Table
	pending []*Command
	seq     uint64
}

func New(table *locktable.Table, cfg Config) *Scheduler {
	if cfg.MaxBackgroundShare <= 0 || cfg.MaxBackgroundShare > 1 {
		cfg.MaxBackgroundShare = 0.7
	}
	if cfg.MaxAttempts <= 0 {
		cfg.MaxAttempts = 3
	}
	return &Scheduler{cfg: cfg, table: table}
}

// Enqueue accepts a command for later planning.
func (s *Scheduler) Enqueue(c *Command) error {
	if !c.IsGroup() {
		if _, err := s.table.Get(c.LockEUI); err != nil {
			return ErrUnknownLock
		}
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.seq++
	c.seq = s.seq
	s.pending = append(s.pending, c)
	return nil
}

// Pending returns the number of queued commands.
func (s *Scheduler) Pending() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.pending)
}

// PlanPeriod builds the transmissions for the next listen period. Planned
// commands leave the queue; report their outcome with Result.
func (s *Scheduler) PlanPeriod(now time.Time) (txs []Tx, expired []*Command) {
	s.mu.Lock()
	defer s.mu.Unlock()

	live := s.pending[:0]
	for _, c := range s.pending {
		if !c.ExpiresAt.IsZero() && now.After(c.ExpiresAt) {
			expired = append(expired, c)
			continue
		}
		live = append(live, c)
	}
	s.pending = live

	sort.SliceStable(s.pending, func(i, j int) bool {
		a, b := s.pending[i], s.pending[j]
		if a.Priority != b.Priority {
			return a.Priority < b.Priority
		}
		return a.seq < b.seq
	})

	if g := s.pickGroup(); g != nil {
		return s.planGroup(g), expired
	}
	return s.planUnicast(), expired
}

func (s *Scheduler) pickGroup() *Command {
	urgentUnicast := false
	for _, c := range s.pending {
		if c.IsGroup() {
			if c.Priority <= P1Interactive || !urgentUnicast {
				return c
			}
			return nil
		}
		if c.Priority <= P1Interactive {
			urgentUnicast = true
		}
	}
	return nil
}

func (s *Scheduler) planGroup(g *Command) []Tx {
	s.remove(g)
	var txs []Tx
	for ch := 0; ch < s.table.Channels(); ch++ {
		for sl := 0; sl < s.table.Slots(); sl++ {
			txs = append(txs, Tx{Cell: locktable.Cell{Channel: ch, Slot: sl}, Dst: g.Group, Cmd: g})
		}
	}
	return txs
}

func (s *Scheduler) planUnicast() []Tx {
	used := make(map[locktable.Cell]bool)
	bgUsed := make([]int, s.table.Channels())
	bgCap := int(float64(s.table.Slots()) * s.cfg.MaxBackgroundShare)

	var txs []Tx
	rest := s.pending[:0]
	for _, c := range s.pending {
		if c.IsGroup() {
			rest = append(rest, c)
			continue
		}
		l, err := s.table.Get(c.LockEUI)
		if err != nil {
			continue // lock left the gateway; drop
		}
		bg := c.Priority >= P2Management
		if used[l.Cell] || (bg && bgUsed[l.Cell.Channel] >= bgCap) {
			rest = append(rest, c)
			continue
		}
		used[l.Cell] = true
		if bg {
			bgUsed[l.Cell.Channel]++
		}
		txs = append(txs, Tx{Cell: l.Cell, Dst: l.Short, Cmd: c})
	}
	s.pending = rest
	return txs
}

func (s *Scheduler) remove(target *Command) {
	for i, c := range s.pending {
		if c == target {
			s.pending = append(s.pending[:i], s.pending[i+1:]...)
			return
		}
	}
}

// Outcome of a planned unicast transmission.
type Outcome int

const (
	Delivered Outcome = iota // link-layer ACK received
	Retrying                 // no ACK, re-queued for the next period
	Failed                   // gave up after MaxAttempts
)

// Result reports whether a planned unicast command was ACKed.
func (s *Scheduler) Result(c *Command, acked bool) Outcome {
	if acked {
		return Delivered
	}
	c.Attempts++
	if c.Attempts >= s.cfg.MaxAttempts {
		return Failed
	}
	s.mu.Lock()
	s.pending = append(s.pending, c) // keeps its original seq: stays ahead of newer work
	s.mu.Unlock()
	return Retrying
}

// TakeUrgent removes and returns P0/P1 unicast commands that can still go
// out in the current period: free(cell) reports whether the lock's cell is
// still ahead in this period and unused. This lets a remote unlock that
// arrives mid-period go out in the lock's next slot instead of waiting for
// the next period plan (latency ~ slot wait instead of up to 2 periods).
func (s *Scheduler) TakeUrgent(now time.Time, free func(locktable.Cell) bool) []Tx {
	s.mu.Lock()
	defer s.mu.Unlock()
	var txs []Tx
	rest := s.pending[:0]
	for _, c := range s.pending {
		if c.IsGroup() || c.Priority > P1Interactive || (!c.ExpiresAt.IsZero() && now.After(c.ExpiresAt)) {
			rest = append(rest, c)
			continue
		}
		l, err := s.table.Get(c.LockEUI)
		if err != nil || !free(l.Cell) {
			rest = append(rest, c)
			continue
		}
		txs = append(txs, Tx{Cell: l.Cell, Dst: l.Short, Cmd: c})
	}
	s.pending = rest
	return txs
}
