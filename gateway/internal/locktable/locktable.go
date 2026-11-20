// Package locktable keeps the gateway's view of its locks: short address,
// home channel, listen slot and online state. Channel/slot assignment at
// join time balances locks over the (channel x slot) grid, which is what
// lets one listen period serve up to channels*slots locks in parallel.
package locktable

import (
	"errors"
	"fmt"
	"sync"
	"time"
)

// Cell is one (channel, slot) position in the listen period.
type Cell struct {
	Channel int // index into the gateway's data channel list
	Slot    int
}

// Lock is one joined lock.
type Lock struct {
	EUI      uint64
	Short    uint16
	Cell     Cell
	Groups   []uint16
	LastSeen time.Time
	Online   bool
}

var (
	ErrFull    = errors.New("locktable: no short address left")
	ErrUnknown = errors.New("locktable: unknown lock")
)

// Table is safe for concurrent use.
type Table struct {
	mu        sync.RWMutex
	channels  int
	slots     int
	byEUI     map[uint64]*Lock
	byShort   map[uint16]*Lock
	occupancy [][]int // [channel][slot] -> number of locks
	nextShort uint16
}

// New creates a table for a gateway with `channels` data channels and
// `slots` listen slots per period.
func New(channels, slots int) *Table {
	occ := make([][]int, channels)
	for i := range occ {
		occ[i] = make([]int, slots)
	}
	return &Table{
		channels:  channels,
		slots:     slots,
		byEUI:     make(map[uint64]*Lock),
		byShort:   make(map[uint16]*Lock),
		occupancy: occ,
		nextShort: 1,
	}
}

func (t *Table) Channels() int { return t.channels }
func (t *Table) Slots() int    { return t.slots }

// Join assigns (or returns the existing) short address and cell for a lock.
func (t *Table) Join(eui uint64) (*Lock, error) {
	t.mu.Lock()
	defer t.mu.Unlock()
	if l, ok := t.byEUI[eui]; ok {
		return l, nil
	}
	short, err := t.allocShort()
	if err != nil {
		return nil, err
	}
	cell := t.leastLoadedCell()
	t.occupancy[cell.Channel][cell.Slot]++
	l := &Lock{EUI: eui, Short: short, Cell: cell, Online: true, LastSeen: time.Now()}
	t.byEUI[eui] = l
	t.byShort[short] = l
	return l, nil
}

// Remove forgets a lock (e.g. moved to another gateway).
func (t *Table) Remove(eui uint64) {
	t.mu.Lock()
	defer t.mu.Unlock()
	l, ok := t.byEUI[eui]
	if !ok {
		return
	}
	t.occupancy[l.Cell.Channel][l.Cell.Slot]--
	delete(t.byEUI, eui)
	delete(t.byShort, l.Short)
}

// Get returns a copy of the lock entry.
func (t *Table) Get(eui uint64) (Lock, error) {
	t.mu.RLock()
	defer t.mu.RUnlock()
	l, ok := t.byEUI[eui]
	if !ok {
		return Lock{}, fmt.Errorf("%w: %016x", ErrUnknown, eui)
	}
	return *l, nil
}

// Len returns the number of joined locks.
func (t *Table) Len() int {
	t.mu.RLock()
	defer t.mu.RUnlock()
	return len(t.byEUI)
}

// MaxCellLoad returns the largest number of locks sharing one cell.
func (t *Table) MaxCellLoad() int {
	t.mu.RLock()
	defer t.mu.RUnlock()
	m := 0
	for _, ch := range t.occupancy {
		for _, n := range ch {
			if n > m {
				m = n
			}
		}
	}
	return m
}

func (t *Table) allocShort() (uint16, error) {
	// 0x0001..0xEFFF are unicast; 0xF000.. are groups/broadcast.
	for i := 0; i < 0xEFFF; i++ {
		s := t.nextShort
		t.nextShort++
		if t.nextShort >= 0xF000 {
			t.nextShort = 1
		}
		if _, used := t.byShort[s]; !used {
			return s, nil
		}
	}
	return 0, ErrFull
}

// leastLoadedCell walks slot-major so consecutive joins land on different
// channels first, then different slots.
func (t *Table) leastLoadedCell() Cell {
	best := Cell{}
	bestN := int(^uint(0) >> 1)
	for s := 0; s < t.slots; s++ {
		for c := 0; c < t.channels; c++ {
			if n := t.occupancy[c][s]; n < bestN {
				best, bestN = Cell{Channel: c, Slot: s}, n
			}
		}
	}
	return best
}
