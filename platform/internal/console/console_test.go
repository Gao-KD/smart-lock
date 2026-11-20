package console

import (
	"testing"

	"github.com/Gao-KD/smart-lock/common/slpgo/slp"
)

func TestUpseqSlidingWindow(t *testing.T) {
	var l LockView
	for _, c := range []struct {
		seq uint32
		ok  bool
	}{{100, true}, {102, true}, {101, true}, {101, false}, {102, false}, {170, true}, {106, false}, {107, true}, {107, false}} {
		if got := l.acceptUpseq(c.seq); got != c.ok {
			t.Fatalf("upseq %d: got %v want %v", c.seq, got, c.ok)
		}
	}
}

func TestRecordGapsAndDuplicates(t *testing.T) {
	c := &Console{}
	var l LockView
	rec := func(seq uint32) bool {
		return c.noteRecord(&l, &slp.AppMsg{Cmd: slp.EvtAccess, TLV: []slp.TLV{slp.U32(slp.TLVLogSeq, seq)}})
	}
	if !rec(1) || !rec(2) || rec(2) {
		t.Fatal("duplicate record accepted")
	}
	c.noteHead(&l, 6) // heartbeat: records 3..6 exist
	if l.Missing != 4 {
		t.Fatalf("missing %d, want 4", l.Missing)
	}
	rec(4)
	rec(3)
	if l.Missing != 2 || l.Records != 4 {
		t.Fatalf("missing %d records %d", l.Missing, l.Records)
	}
	// Only the last 128 records can still be pulled from the lock.
	c.noteHead(&l, 1000)
	if l.Missing != 2+128 {
		t.Fatalf("missing %d, want %d", l.Missing, 2+128)
	}
}

func TestCredTLVSchedule(t *testing.T) {
	b, err := credTLV(slp.CredPassword, []byte("246810"), credBody{UserID: 7, Uses: 1, Days: []int{1, 5}, Start: "08:00", End: "10:30", Duress: true})
	if err != nil {
		t.Fatal(err)
	}
	m, err := slp.ParseApp(append(make([]byte, slp.AppHdrLen), b...))
	if err != nil {
		t.Fatal(err)
	}
	sch, _ := m.Find(slp.TLVSchedule)
	if len(sch) != 5 || sch[0] != 0x11 || sch[1] != 0xE0 || sch[3] != 0x76 {
		t.Fatalf("schedule % x", sch)
	}
	if f, _ := m.U8(slp.TLVCredFlags); f != slp.CredFDuress {
		t.Fatalf("flags %d", f)
	}
	if u, _ := m.U16(slp.TLVUses); u != 1 {
		t.Fatal("uses")
	}
	if _, err := credTLV(slp.CredPassword, nil, credBody{Days: []int{8}}); err == nil {
		t.Fatal("day 8 accepted")
	}
	if _, err := credTLV(slp.CredPassword, nil, credBody{Start: "25:00", End: "10:00"}); err == nil {
		t.Fatal("25:00 accepted")
	}
}
