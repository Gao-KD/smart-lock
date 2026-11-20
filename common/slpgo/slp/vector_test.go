package slp

import (
	"encoding/hex"
	"testing"
)

// Cross-language vector: firmware/lock/tests/test_sim_net.c seals the same
// message in C and must produce these bytes.
func TestInteropVector(t *testing.T) {
	key := []byte("0123456789abcdef")
	got := hex.EncodeToString(SealDownRaw(key, 0x5A00000000010101, CmdUnlock, 0x01020304, 0x6A000000,
		EncodeTLV([]TLV{U16(TLVDurationS, 5)})))
	const want = "01040302010000006af94db7bbdb4faa19b8e75475"
	if got != want {
		t.Fatalf("got %s", got)
	}
}
