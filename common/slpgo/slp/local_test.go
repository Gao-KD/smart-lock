package slp

import (
	"testing"
	"time"
)

// Same key and EUI as firmware/lock/tests/test_lock.c (HAL_HOST_APP_KEY,
// HAL_HOST_EUI): the C test prints the same value.
var testAppKey = []byte{0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F}

const testEUI = 0x5A00000000010101

func TestOfflinePasswordMatchesFirmware(t *testing.T) {
	if got := OfflinePassword(testAppKey, testEUI, 3, 1, 500000); got != "3118455381" {
		t.Fatalf("offline password = %s, want 3118455381 (C firmware)", got)
	}
}

func TestOfflineKindAndWindow(t *testing.T) {
	for _, c := range []struct {
		h    int
		kind uint8
	}{{1, 1}, {2, 2}, {8, 3}, {9, 4}, {24, 4}, {48, 5}, {168, 6}, {720, 7}} {
		k, err := OfflineKindFor(c.h)
		if err != nil || k != c.kind {
			t.Fatalf("kind for %d h = %d, %v", c.h, k, err)
		}
	}
	if _, err := OfflineKindFor(721); err == nil {
		t.Fatal("721 h accepted")
	}
	if w := OfflineWindow(3, 10); w.Unix() != 18*3600 {
		t.Fatalf("window %v", w)
	}
}

func TestBLETokenLayout(t *testing.T) {
	tok := BLEToken(testAppKey, testEUI, 9001, time.Unix(1800003600, 0), CredFAdmin)
	if len(tok) != 17 || tok[8] != CredFAdmin || tok[0] != 0x29 || tok[1] != 0x23 {
		t.Fatalf("token % x", tok)
	}
	tok2 := BLEToken(testAppKey, testEUI+1, 9001, time.Unix(1800003600, 0), CredFAdmin)
	if string(tok[9:]) == string(tok2[9:]) {
		t.Fatal("token MAC not bound to the lock")
	}
}
