package slp

import (
	"bytes"
	"encoding/hex"
	"testing"
)

func h(s string) []byte { b, _ := hex.DecodeString(s); return b }

// Same vector as common/slp/tests/test_secure.c.
func TestCCMRFC3610Vector1(t *testing.T) {
	key := h("c0c1c2c3c4c5c6c7c8c9cacbcccdcecf")
	nonce := h("00000003020100a0a1a2a3a4a5")
	aad := h("0001020304050607")
	pt := h("08090a0b0c0d0e0f101112131415161718191a1b1c1d1e")
	got := CCMSeal(key, nonce, aad, pt, 8)
	if want := h("588c979a61c663d2f066d0c2c0f989806d5f6b61dac38417e8d12cfdf926e0"); !bytes.Equal(got, want) {
		t.Fatalf("seal = %x", got)
	}
	back, err := CCMOpen(key, nonce, aad, got, 8)
	if err != nil || !bytes.Equal(back, pt) {
		t.Fatalf("open: %v", err)
	}
	got[2] ^= 1
	if _, err := CCMOpen(key, nonce, aad, got, 8); err != ErrAuth {
		t.Fatal("tampered message accepted")
	}
}

func TestCRC16(t *testing.T) {
	if CRC16([]byte("123456789")) != 0x29B1 {
		t.Fatal("crc")
	}
}

func TestSecureFrameRoundtrip(t *testing.T) {
	key := make([]byte, 16)
	key[0] = 9
	f := &Frame{Type: FTData, AckRequest: true, Seq: 3, PAN: 0x5A01, DstMode: AddrShort, Dst: 0x21,
		SrcMode: AddrShort, Src: 0, FrameCounter: 100, KeyID: KeyLink, Payload: []byte{1, 2, 3, 4, 5, 6, 7}}
	buf, err := f.EncodeSecure(key, 0xAA)
	if err != nil {
		t.Fatal(err)
	}
	g, err := DecodeSecure(buf, func(*Frame) ([]byte, uint64, error) { return key, 0xAA, nil })
	if err != nil || !bytes.Equal(g.Payload, f.Payload) || g.Dst != 0x21 || g.FrameCounter != 100 {
		t.Fatalf("%v %+v", err, g)
	}
}

func TestAppSealRoundtrip(t *testing.T) {
	key := make([]byte, 16)
	m := &AppMsg{Cmd: CmdUnlock, MsgID: 42, TLV: []TLV{U16(TLVDurationS, 9)}}
	b := SealDown(key, 0x1122, m)
	got, err := OpenDown(key, 0x1122, b)
	if err != nil || got.MsgID != 42 {
		t.Fatal(err)
	}
	if d, _ := got.U16(TLVDurationS); d != 9 {
		t.Fatal("tlv")
	}
	up := SealUp(key, 0x1122, 5, &AppMsg{Cmd: RspResult, MsgID: 42, TLV: []TLV{U8(TLVResult, 0)}})
	seq, r, err := OpenUp(key, 0x1122, up)
	if err != nil || seq != 5 || r.Cmd != RspResult {
		t.Fatal(err)
	}
	if _, _, err := OpenUp(key, 0x1123, up); err == nil {
		t.Fatal("wrong lock accepted")
	}
}
