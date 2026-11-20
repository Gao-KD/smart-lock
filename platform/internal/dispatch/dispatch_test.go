package dispatch

import (
	"context"
	"errors"
	"testing"
	"time"
)

type fakeRegistry map[uint64]LockInfo

func (f fakeRegistry) Lock(_ context.Context, eui uint64) (LockInfo, error) {
	l, ok := f[eui]
	if !ok {
		return LockInfo{}, errors.New("not found")
	}
	return l, nil
}

func (f fakeRegistry) GatewaysOfBuilding(_ context.Context, tenant, building string) ([]string, error) {
	return []string{building + "-gw1", building + "-gw2", building + "-gw3"}, nil
}

type fakeSealer struct{}

func (fakeSealer) Seal(_ context.Context, _ uint64, cmd uint8, _ uint32, _ time.Time, tlv []byte) ([]byte, error) {
	return append([]byte{cmd}, tlv...), nil
}

func (fakeSealer) SealGroup(_ context.Context, _, _ string, _ uint16, cmd uint8, _ uint32, _ time.Time, _ []byte) ([]byte, error) {
	return []byte{cmd}, nil
}

type capture struct{ batches []Batch }

func (c *capture) Publish(_ context.Context, b Batch) error {
	c.batches = append(c.batches, b)
	return nil
}

// 800-lock building: locks spread over 3 gateways.
func building(n int) fakeRegistry {
	r := fakeRegistry{}
	for i := 0; i < n; i++ {
		gw := []string{"b1-gw1", "b1-gw2", "b1-gw3"}[i*3/n]
		r[uint64(i+1)] = LockInfo{EUI: uint64(i + 1), Tenant: "school-a", Gateway: gw}
	}
	return r
}

func TestUnlockBuildsInteractiveCommand(t *testing.T) {
	pub := &capture{}
	s := New(Config{}, building(10), fakeSealer{}, pub)
	now := time.Unix(1_800_000_000, 0)
	s.now = func() time.Time { return now }

	id, err := s.Unlock(context.Background(), 3, 8)
	if err != nil {
		t.Fatal(err)
	}
	if len(pub.batches) != 1 || len(pub.batches[0].Items) != 1 {
		t.Fatalf("batches = %+v", pub.batches)
	}
	it := pub.batches[0].Items[0]
	if it.MsgID != id || it.Priority != P1Interactive || !it.ExpiresAt.Equal(now.Add(30*time.Second)) {
		t.Fatalf("item = %+v", it)
	}
	if it.AppPayload[0] != CmdUnlock || it.AppPayload[3] != 8 {
		t.Fatalf("payload = %x", it.AppPayload)
	}
	if st, _ := s.Status(id); st != StatusQueued {
		t.Fatalf("status = %s", st)
	}
}

func TestBulkSubmitSplitsPerGatewayAndBatch(t *testing.T) {
	pub := &capture{}
	s := New(Config{MaxBatchItems: 100}, building(800), fakeSealer{}, pub)
	reqs := make([]Request, 800)
	for i := range reqs {
		reqs[i] = Request{LockEUI: uint64(i + 1), Cmd: CmdCredAdd, Priority: P2Management}
	}
	ids, err := s.Submit(context.Background(), reqs)
	if err != nil || len(ids) != 800 {
		t.Fatalf("ids=%d err=%v", len(ids), err)
	}
	perGw := map[string]int{}
	for _, b := range pub.batches {
		if len(b.Items) > 100 {
			t.Fatalf("batch too large: %d", len(b.Items))
		}
		for _, it := range b.Items {
			if want, _ := s.reg.Lock(context.Background(), it.LockEUI); want.Gateway != b.Gateway {
				t.Fatalf("lock %d sent to %s", it.LockEUI, b.Gateway)
			}
		}
		perGw[b.Gateway] += len(b.Items)
	}
	if len(perGw) != 3 || perGw["b1-gw1"]+perGw["b1-gw2"]+perGw["b1-gw3"] != 800 {
		t.Fatalf("per gateway = %v", perGw)
	}
}

func TestUnknownLock(t *testing.T) {
	s := New(Config{}, building(1), fakeSealer{}, &capture{})
	if _, err := s.Unlock(context.Background(), 999, 0); !errors.Is(err, ErrUnknownLock) {
		t.Fatalf("err = %v", err)
	}
}

func TestEmergencyOpenBroadcastsToEveryGateway(t *testing.T) {
	pub := &capture{}
	s := New(Config{}, building(1), fakeSealer{}, pub)
	ids, err := s.EmergencyOpen(context.Background(), "school-a", "b7")
	if err != nil || len(ids) != 3 || len(pub.batches) != 3 {
		t.Fatalf("ids=%v batches=%d err=%v", ids, len(pub.batches), err)
	}
	for _, b := range pub.batches {
		it := b.Items[0]
		if it.Group != BroadcastGroup || it.Priority != P0Emergency {
			t.Fatalf("item = %+v", it)
		}
	}
}

func TestStatusOnlyMovesForward(t *testing.T) {
	s := New(Config{}, building(1), fakeSealer{}, &capture{})
	id, _ := s.Unlock(context.Background(), 1, 0)
	s.Update(id, StatusDelivered)
	s.Update(id, StatusAccepted) // late MQTT message
	if st, _ := s.Status(id); st != StatusDelivered {
		t.Fatalf("status = %s", st)
	}
	s.Update(id, StatusExecuted)
	s.Update(id, StatusFailed)
	if st, _ := s.Status(id); st != StatusExecuted {
		t.Fatalf("final status overwritten: %s", st)
	}
}

func TestAckResultAndParkedResend(t *testing.T) {
	pub := &capture{}
	s := New(Config{}, building(10), fakeSealer{}, pub)
	ids, err := s.Submit(context.Background(), []Request{
		{LockEUI: 1, Cmd: CmdCredAdd, Priority: P2Management},
		{LockEUI: 1, Cmd: CmdUnlock, Priority: P1Interactive},
	})
	if err != nil {
		t.Fatal(err)
	}
	card, unlock := ids[0], ids[1]
	for _, id := range ids {
		s.OnAck(GatewayAck{MsgID: id, State: "accepted"})
		s.OnAck(GatewayAck{MsgID: id, State: "failed", Attempts: 5, Reason: "no ACK"})
	}
	if st, _ := s.Status(card); st != StatusFailed {
		t.Fatalf("card status %s", st)
	}
	before := len(pub.batches)
	// The lock is heard again: only the management command is re-sent, with
	// its original message id (the lock de-duplicates by id).
	if n := s.LockSeen(context.Background(), 1); n != 1 {
		t.Fatalf("resent %d", n)
	}
	if len(pub.batches) != before+1 || pub.batches[before].Items[0].MsgID != card {
		t.Fatalf("resend batch = %+v", pub.batches[before:])
	}
	if st, _ := s.Status(unlock); st != StatusFailed {
		t.Fatalf("unlock must not be re-sent, status %s", st)
	}
	s.OnResult(1, card, "OK")
	s.OnResult(1, card, "DUPLICATE") // retransmission after a lost ACK
	r, _ := s.Record(card)
	if r.Status != StatusExecuted || r.Result != "OK" || r.Resends != 1 {
		t.Fatalf("record = %+v", r)
	}
	if s.LockSeen(context.Background(), 1) != 0 {
		t.Fatal("nothing should be parked any more")
	}
}

func TestBroadcastJobProgress(t *testing.T) {
	s := New(Config{}, building(10), fakeSealer{}, &capture{})
	ids, err := s.Broadcast(context.Background(), "school-a", "b1", CmdEmergency, P0Emergency)
	if err != nil {
		t.Fatal(err)
	}
	for _, id := range ids {
		s.SetExpected(id, 2)
	}
	j := s.NewJob("emergency", ids)
	for _, id := range ids {
		s.OnAck(GatewayAck{MsgID: id, State: "delivered", Reason: "broadcast", Cells: 150})
		s.OnResult(100, id, "OK")
	}
	p, _ := s.Job(j.ID)
	if p.Done || p.LocksTotal != 6 || p.LocksDone != 3 {
		t.Fatalf("progress = %+v", p)
	}
	for _, id := range ids {
		s.OnResult(101, id, "OK")
	}
	if p, _ = s.Job(j.ID); !p.Done || p.LocksDone != 6 {
		t.Fatalf("progress = %+v", p)
	}
}
