package locktable

import "testing"

func TestJoinBalancesCells(t *testing.T) {
	tb := New(4, 50)
	for i := 0; i < 200; i++ {
		if _, err := tb.Join(uint64(i + 1)); err != nil {
			t.Fatal(err)
		}
	}
	if tb.MaxCellLoad() != 1 {
		t.Fatalf("200 locks on 4x50 cells: max load %d, want 1", tb.MaxCellLoad())
	}
	l, _ := tb.Join(201)
	if tb.MaxCellLoad() != 2 || l.Cell != (Cell{Channel: 0, Slot: 0}) {
		t.Fatalf("201st lock got %+v", l.Cell)
	}
}

func TestJoinIsIdempotentAndRemoveFreesCell(t *testing.T) {
	tb := New(1, 2)
	a, _ := tb.Join(10)
	again, _ := tb.Join(10)
	if a.Short != again.Short || tb.Len() != 1 {
		t.Fatal("re-join should return the same entry")
	}
	b, _ := tb.Join(11)
	if a.Short == b.Short || a.Cell == b.Cell {
		t.Fatalf("a=%+v b=%+v", a, b)
	}
	tb.Remove(10)
	c, _ := tb.Join(12)
	if c.Cell != a.Cell {
		t.Fatalf("freed cell not reused: %+v", c.Cell)
	}
	if _, err := tb.Get(10); err == nil {
		t.Fatal("removed lock still present")
	}
}
