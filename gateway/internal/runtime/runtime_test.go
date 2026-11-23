package runtime

import (
	"testing"

	"github.com/Gao-KD/smart-lock/common/slpgo/simnet"
	"github.com/Gao-KD/smart-lock/common/slpgo/slp"
	"github.com/Gao-KD/smart-lock/gateway/internal/rcp"
	"github.com/Gao-KD/smart-lock/gateway/internal/topics"
)

type fakeRadio struct{ sent [][]byte }

func (f *fakeRadio) Send(b []byte) error {
	f.sent = append(f.sent, append([]byte(nil), b...))
	return nil
}
func (f *fakeRadio) Results() <-chan rcp.TxResult { return nil }
func (f *fakeRadio) Uplinks() <-chan rcp.RxFrame  { return nil }

type fakePub struct{ kinds []topics.Kind }

func (p *fakePub) Publish(k topics.Kind, _ any, _ bool) { p.kinds = append(p.kinds, k) }

func TestUplinkIsAcknowledged(t *testing.T) {
	const lockEUI = 0x5A00000000010203
	radio, pub := &fakeRadio{}, &fakePub{}
	rt := New(Config{ID: "gw-1-1", EUI: simnet.GatewayEUI(1, 1), PAN: 0x5A11, Channels: 3, Slots: 50,
		PeriodMs: 1000, SlotMs: 20, MaxAttempts: 5,
		LinkKey: func(eui uint64) []byte { return simnet.Key(simnet.TagLink, eui) }}, radio, pub)
	rt.onJoin(lockEUI)
	l, err := rt.table.Get(lockEUI)
	if err != nil {
		t.Fatal(err)
	}
	radio.sent = nil

	app := slp.SealUp(simnet.Key(simnet.TagApp, lockEUI), lockEUI, 1, &slp.AppMsg{Cmd: slp.EvtHeartbeat, MsgID: 1})
	up := &slp.Frame{Type: slp.FTData, Security: true, AckRequest: true, Seq: 77, PAN: 0x5A11,
		DstMode: slp.AddrShort, Dst: slp.ShortGateway, SrcMode: slp.AddrShort, Src: uint64(l.Short),
		FrameCounter: 10, KeyID: slp.KeyLink, Payload: app}
	b, err := up.EncodeSecure(simnet.Key(simnet.TagLink, lockEUI), lockEUI)
	if err != nil {
		t.Fatal(err)
	}
	rt.onUplink(rcp.RxFrame{Frame: b})
	if len(radio.sent) != 1 {
		t.Fatalf("want one ACK transmission, got %d", len(radio.sent))
	}
	m := radio.sent[0]
	ack, _, err := slp.Decode(m[6:]) // TXNOW: type(1) ref(4) len(1) frame
	if err != nil || ack.Type != slp.FTAck || ack.Seq != 77 || uint16(ack.Dst) != l.Short || !ack.Security {
		t.Fatalf("bad ACK %+v %v", ack, err)
	}
	// The ACK is authenticated with K_link and replaces the beacon: it
	// carries the period phase, the period number and the time.
	open, err := slp.DecodeSecure(m[6:], func(*slp.Frame) ([]byte, uint64, error) {
		return simnet.Key(simnet.TagLink, lockEUI), simnet.GatewayEUI(1, 1), nil
	})
	if err != nil || len(open.Payload) != simnet.AckPayloadLen {
		t.Fatalf("ACK payload: %v %x", err, open.Payload)
	}
	if len(pub.kinds) == 0 || pub.kinds[len(pub.kinds)-1] != topics.UpEvent {
		t.Fatalf("uplink not forwarded: %v", pub.kinds)
	}

	// A replayed frame is neither forwarded nor acknowledged.
	radio.sent = nil
	rt.onUplink(rcp.RxFrame{Frame: b})
	if len(radio.sent) != 0 {
		t.Fatal("replayed uplink was acknowledged")
	}
}
