package simnet

import (
	"encoding/hex"
	"testing"
)

func TestLayout(t *testing.T) {
	l := Layout{Building: 1, Floors: 12, Rooms: 30, FloorsPerGW: 6}
	if l.Gateways() != 2 || l.GatewayOfFloor(6) != 1 || l.GatewayOfFloor(7) != 2 {
		t.Fatal("gateways")
	}
	e := LockEUI(1, 12, 3)
	if b, f, r, ok := SplitLockEUI(e); !ok || b != 1 || f != 12 || r != 3 {
		t.Fatal("split")
	}
	if RoomName(12, 3) != "1203" {
		t.Fatal("name")
	}
}

// Pinned so the C implementation (sim_net.c) can be checked against it.
func TestKeyVector(t *testing.T) {
	got := hex.EncodeToString(Key(TagApp, LockEUI(1, 1, 1)))
	if got != "20e3778dea1708a897213045be9b2f3e" {
		t.Fatal(got)
	}
}
