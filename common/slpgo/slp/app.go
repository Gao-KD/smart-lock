package slp

import (
	"encoding/binary"
	"fmt"
)

// App commands / events (slp/app.h).
const (
	CmdUnlock    = 0x01
	CmdLockdown  = 0x02
	CmdRelease   = 0x03
	CmdEmergency = 0x04
	CmdCredAdd   = 0x10
	CmdCredDel   = 0x11
	CmdCredClear = 0x12
	CmdTimeSync  = 0x31
	CmdStatus    = 0x50

	RspResult    = 0x80
	EvtAccess    = 0x90
	EvtAlarm     = 0x91
	EvtBattery   = 0x92
	EvtHeartbeat = 0x93
)

// TLV types.
const (
	TLVResult    = 0x01
	TLVDurationS = 0x02
	TLVCredType  = 0x03
	TLVCredID    = 0x04
	TLVValidFrom = 0x05
	TLVValidTo   = 0x06
	TLVUnixTime  = 0x07
	TLVBatteryMV = 0x08
	TLVUserID    = 0x09
	TLVLockState = 0x0A
	TLVCredCount = 0x0B
	TLVBattLevel = 0x0C
)

const CredCardUID = 1

// Results.
const (
	ResultOK        = 0
	ResultExpired   = 1
	ResultDuplicate = 2
	ResultDenied    = 3
	ResultBadParam  = 4
	ResultFull      = 5
	ResultHWFault   = 6
	ResultUnknown   = 7
)

var ResultNames = map[uint8]string{
	ResultOK: "OK", ResultExpired: "EXPIRED", ResultDuplicate: "DUPLICATE", ResultDenied: "DENIED",
	ResultBadParam: "BAD_PARAM", ResultFull: "FULL", ResultHWFault: "HW_FAULT", ResultUnknown: "UNKNOWN",
}

// LockStateNames follows lock_state_t.
var LockStateNames = []string{"LOCKED", "UNLOCKED", "LOCKDOWN", "EMERGENCY", "FAULT"}

// AppMsg is a plaintext app message.
type AppMsg struct {
	Cmd       uint8
	MsgID     uint32
	ExpiresAt uint32
	TLV       []TLV
}

type TLV struct {
	Type  uint8
	Value []byte
}

func U8(t uint8, v uint8) TLV   { return TLV{t, []byte{v}} }
func U16(t uint8, v uint16) TLV { return TLV{t, binary.LittleEndian.AppendUint16(nil, v)} }
func U32(t uint8, v uint32) TLV { return TLV{t, binary.LittleEndian.AppendUint32(nil, v)} }

// EncodeTLV encodes TLVs only (used by the platform before sealing).
func EncodeTLV(tlvs []TLV) []byte {
	var b []byte
	for _, t := range tlvs {
		b = append(b, t.Type, byte(len(t.Value)))
		b = append(b, t.Value...)
	}
	return b
}

func (m *AppMsg) Header() []byte {
	h := []byte{m.Cmd}
	h = binary.LittleEndian.AppendUint32(h, m.MsgID)
	return binary.LittleEndian.AppendUint32(h, m.ExpiresAt)
}

// Encode returns HDR | TLV...
func (m *AppMsg) Encode() []byte { return append(m.Header(), EncodeTLV(m.TLV)...) }

// ParseApp parses a plaintext app message.
func ParseApp(b []byte) (*AppMsg, error) {
	if len(b) < AppHdrLen {
		return nil, fmt.Errorf("slp: app message too short")
	}
	m := &AppMsg{Cmd: b[0], MsgID: binary.LittleEndian.Uint32(b[1:]), ExpiresAt: binary.LittleEndian.Uint32(b[5:])}
	for p := b[AppHdrLen:]; len(p) > 0; {
		if len(p) < 2 || len(p) < 2+int(p[1]) {
			return nil, fmt.Errorf("slp: bad TLV")
		}
		m.TLV = append(m.TLV, TLV{p[0], p[2 : 2+int(p[1])]})
		p = p[2+int(p[1]):]
	}
	return m, nil
}

func (m *AppMsg) Find(t uint8) ([]byte, bool) {
	for _, x := range m.TLV {
		if x.Type == t {
			return x.Value, true
		}
	}
	return nil, false
}

func (m *AppMsg) U8(t uint8) (uint8, bool) {
	v, ok := m.Find(t)
	if !ok || len(v) != 1 {
		return 0, false
	}
	return v[0], true
}

func (m *AppMsg) U16(t uint8) (uint16, bool) {
	v, ok := m.Find(t)
	if !ok || len(v) != 2 {
		return 0, false
	}
	return binary.LittleEndian.Uint16(v), true
}

func (m *AppMsg) U32(t uint8) (uint32, bool) {
	v, ok := m.Find(t)
	if !ok || len(v) != 4 {
		return 0, false
	}
	return binary.LittleEndian.Uint32(v), true
}

// SealDown seals a downlink message: HDR | ENC(TLV) | MIC8 (slp/secure.h).
// nonceEUI is the lock EUI (unicast) or the gateway EUI (group).
func SealDown(key []byte, nonceEUI uint64, m *AppMsg) []byte {
	h := m.Header()
	ct := CCMSeal(key, Nonce(nonceEUI, m.MsgID, SecEncMIC64|DirDown), h, EncodeTLV(m.TLV), AppMICLen)
	return append(h, ct...)
}

// OpenUp opens an uplink: UPSEQ(4) | HDR | ENC(TLV) | MIC8.
func OpenUp(key []byte, lockEUI uint64, b []byte) (upseq uint32, m *AppMsg, err error) {
	aad := UpSeqLen + AppHdrLen
	if len(b) < aad+AppMICLen {
		return 0, nil, ErrAuth
	}
	upseq = binary.LittleEndian.Uint32(b)
	pt, err := CCMOpen(key, Nonce(lockEUI, upseq, SecEncMIC64|DirUp), b[:aad], b[aad:], AppMICLen)
	if err != nil {
		return 0, nil, err
	}
	m, err = ParseApp(append(append([]byte(nil), b[UpSeqLen:aad]...), pt...))
	return upseq, m, err
}

// SealUp is the lock-side uplink seal (used by Go tests and tools).
func SealUp(key []byte, lockEUI uint64, upseq uint32, m *AppMsg) []byte {
	pre := binary.LittleEndian.AppendUint32(nil, upseq)
	aad := append(pre, m.Header()...)
	ct := CCMSeal(key, Nonce(lockEUI, upseq, SecEncMIC64|DirUp), aad, EncodeTLV(m.TLV), AppMICLen)
	return append(aad, ct...)
}

// OpenDown is the lock-side downlink open (used by Go tests and tools).
func OpenDown(key []byte, nonceEUI uint64, b []byte) (*AppMsg, error) {
	if len(b) < AppHdrLen+AppMICLen {
		return nil, ErrAuth
	}
	msgID := binary.LittleEndian.Uint32(b[1:])
	pt, err := CCMOpen(key, Nonce(nonceEUI, msgID, SecEncMIC64|DirDown), b[:AppHdrLen], b[AppHdrLen:], AppMICLen)
	if err != nil {
		return nil, err
	}
	return ParseApp(append(append([]byte(nil), b[:AppHdrLen]...), pt...))
}

// SealDownRaw seals HDR(cmd, msgID, expires) | tlv for a lock (or a group,
// with the gateway EUI as nonceEUI).
func SealDownRaw(key []byte, nonceEUI uint64, cmd uint8, msgID, expiresAt uint32, tlv []byte) []byte {
	m := &AppMsg{Cmd: cmd, MsgID: msgID, ExpiresAt: expiresAt}
	h := m.Header()
	ct := CCMSeal(key, Nonce(nonceEUI, msgID, SecEncMIC64|DirDown), h, tlv, AppMICLen)
	return append(h, ct...)
}
