package scheduler

import (
	"testing"
	"time"

	"github.com/Gao-KD/smart-lock/gateway/internal/locktable"
)

func newTable(t *testing.T, channels, locks int) *locktable.Table {
	t.Helper()
	tb := locktable.New(channels, 50)
	for i := 0; i < locks; i++ {
		if _, err := tb.Join(uint64(0x1000 + i)); err != nil {
			t.Fatal(err)
		}
	}
	return tb
}

// periodsToDrain runs the scheduler, ACKing everything, until the queue is empty.
func periodsToDrain(t *testing.T, s *Scheduler) int {
	t.Helper()
	now := time.Unix(1_800_000_000, 0)
	for p := 1; p <= 100; p++ {
		txs, _ := s.PlanPeriod(now)
		for _, tx := range txs {
			s.Result(tx.Cmd, true)
		}
		if s.Pending() == 0 {
			return p
		}
		now = now.Add(time.Second)
	}
	t.Fatal("queue never drained")
	return 0
}

// Design doc example: one of three gateways in a 20-floor building, about
// 270 locks on 3 channels -> everyone reached within 2 listen periods (~2 s).
func TestBulkUnlock270LocksIn2Periods(t *testing.T) {
	tb := newTable(t, 3, 270)
	if got := tb.MaxCellLoad(); got != 2 {
		t.Fatalf("max cell load = %d, want 2", got)
	}
	s := New(tb, Config{})
	for i := 0; i < 270; i++ {
		if err := s.Enqueue(&Command{MsgID: uint32(i), LockEUI: uint64(0x1000 + i), Priority: P1Interactive}); err != nil {
			t.Fatal(err)
		}
	}
	if p := periodsToDrain(t, s); p != 2 {
		t.Fatalf("drained in %d periods, want 2", p)
	}
}

func TestSmallBuilding200LocksOnePeriod(t *testing.T) {
	tb := newTable(t, 4, 200)
	s := New(tb, Config{})
	for i := 0; i < 200; i++ {
		_ = s.Enqueue(&Command{MsgID: uint32(i), LockEUI: uint64(0x1000 + i), Priority: P1Interactive})
	}
	if p := periodsToDrain(t, s); p != 1 {
		t.Fatalf("drained in %d periods, want 1", p)
	}
}

func TestInteractiveBeatsManagementInSameCell(t *testing.T) {
	tb := newTable(t, 1, 1)
	s := New(tb, Config{})
	_ = s.Enqueue(&Command{MsgID: 1, LockEUI: 0x1000, Priority: P2Management})
	_ = s.Enqueue(&Command{MsgID: 2, LockEUI: 0x1000, Priority: P1Interactive})
	txs, _ := s.PlanPeriod(time.Now())
	if len(txs) != 1 || txs[0].Cmd.MsgID != 2 {
		t.Fatalf("got %+v, want only msg 2", txs)
	}
}

func TestExpiredCommandsDropped(t *testing.T) {
	tb := newTable(t, 1, 2)
	s := New(tb, Config{})
	now := time.Unix(1_800_000_000, 0)
	_ = s.Enqueue(&Command{MsgID: 1, LockEUI: 0x1000, ExpiresAt: now.Add(-time.Second)})
	_ = s.Enqueue(&Command{MsgID: 2, LockEUI: 0x1001, ExpiresAt: now.Add(time.Minute)})
	txs, expired := s.PlanPeriod(now)
	if len(expired) != 1 || expired[0].MsgID != 1 {
		t.Fatalf("expired = %+v", expired)
	}
	if len(txs) != 1 || txs[0].Cmd.MsgID != 2 {
		t.Fatalf("txs = %+v", txs)
	}
}

func TestEmergencyBroadcastTakesWholePeriod(t *testing.T) {
	// One wake-up train per channel; nothing else in that period.
	tb := newTable(t, 3, 100)
	s := New(tb, Config{})
	_ = s.Enqueue(&Command{MsgID: 1, LockEUI: 0x1000, Priority: P1Interactive})
	_ = s.Enqueue(&Command{MsgID: 2, Group: 0xFFFF, Priority: P0Emergency})
	txs, _ := s.PlanPeriod(time.Now())
	if len(txs) != 3 {
		t.Fatalf("broadcast trains = %d, want one per channel (3)", len(txs))
	}
	for _, tx := range txs {
		if tx.Cmd.MsgID != 2 || tx.Dst != 0xFFFF {
			t.Fatalf("unexpected tx %+v", tx)
		}
	}
	txs, _ = s.PlanPeriod(time.Now())
	if len(txs) != 1 || txs[0].Cmd.MsgID != 1 {
		t.Fatalf("unicast should follow next period, got %+v", txs)
	}
}

func TestBackgroundBroadcastWaitsForInteractive(t *testing.T) {
	tb := newTable(t, 1, 10)
	s := New(tb, Config{})
	_ = s.Enqueue(&Command{MsgID: 1, Group: 0xFFFF, Priority: P2Management}) // e.g. time sync
	_ = s.Enqueue(&Command{MsgID: 2, LockEUI: 0x1003, Priority: P1Interactive})
	txs, _ := s.PlanPeriod(time.Now())
	if len(txs) != 1 || txs[0].Cmd.MsgID != 2 {
		t.Fatalf("got %+v, want the unlock first", txs)
	}
	txs, _ = s.PlanPeriod(time.Now())
	if len(txs) != 1 || txs[0].Cmd.MsgID != 1 {
		t.Fatalf("broadcast should follow, got %d txs", len(txs))
	}
}

func TestBackgroundShareCapped(t *testing.T) {
	tb := newTable(t, 1, 50)
	s := New(tb, Config{MaxBackgroundShare: 0.7})
	for i := 0; i < 50; i++ {
		_ = s.Enqueue(&Command{MsgID: uint32(i), LockEUI: uint64(0x1000 + i), Priority: P2Management})
	}
	txs, _ := s.PlanPeriod(time.Now())
	if len(txs) != 35 {
		t.Fatalf("background txs = %d, want 35 (70%% of 50)", len(txs))
	}
}

func TestRetryThenFail(t *testing.T) {
	tb := newTable(t, 1, 1)
	s := New(tb, Config{MaxAttempts: 3})
	c := &Command{MsgID: 9, LockEUI: 0x1000, Priority: P1Interactive}
	_ = s.Enqueue(c)
	var last Outcome
	for i := 0; i < 3; i++ {
		txs, _ := s.PlanPeriod(time.Now())
		if len(txs) != 1 {
			t.Fatalf("attempt %d: %d txs", i, len(txs))
		}
		last = s.Result(txs[0].Cmd, false)
	}
	if last != Failed || s.Pending() != 0 {
		t.Fatalf("outcome %v pending %d", last, s.Pending())
	}
}

func TestUnknownLockRejected(t *testing.T) {
	s := New(newTable(t, 1, 1), Config{})
	if err := s.Enqueue(&Command{LockEUI: 0xBAD}); err != ErrUnknownLock {
		t.Fatalf("err = %v", err)
	}
}

func TestTakeUrgentOnlyInteractiveAndFreeCells(t *testing.T) {
	tb := newTable(t, 3, 6)
	s := New(tb, Config{})
	now := time.Unix(1_800_000_000, 0)
	for i := 0; i < 6; i++ {
		p := P1Interactive
		if i%2 == 1 {
			p = P2Management
		}
		_ = s.Enqueue(&Command{MsgID: uint32(i + 1), LockEUI: uint64(0x1000 + i), Priority: p})
	}
	used := map[locktable.Cell]bool{}
	first, _ := tb.Get(0x1000)
	used[first.Cell] = true // already taken this period
	txs := s.TakeUrgent(now, func(c locktable.Cell) bool {
		if used[c] {
			return false
		}
		used[c] = true
		return true
	})
	if len(txs) != 2 { // locks 2 and 4 (P1); lock 0's cell is used; odd ones are P2
		t.Fatalf("took %d", len(txs))
	}
	for _, tx := range txs {
		if tx.Cmd.Priority != P1Interactive {
			t.Fatal("took a management command")
		}
	}
	if s.Pending() != 4 {
		t.Fatalf("pending = %d", s.Pending())
	}
}

func TestIneligibleLockWaits(t *testing.T) {
	tb := newTable(t, 1, 10)
	listening := false
	s := New(tb, Config{Eligible: func(uint64) bool { return listening }})
	_ = s.Enqueue(&Command{MsgID: 1, LockEUI: 0x1003, Priority: P1Interactive})
	if txs, _ := s.PlanPeriod(time.Now()); len(txs) != 0 {
		t.Fatalf("lock not sniffing this period, got %+v", txs)
	}
	listening = true
	if txs, _ := s.PlanPeriod(time.Now()); len(txs) != 1 {
		t.Fatalf("want the command once the lock listens, got %+v", txs)
	}
}
