package slp

import (
	"encoding/binary"
	"errors"
)

// Frame types, address modes (slp/frame.h).
const (
	FTBeacon = 0
	FTWakeup = 1
	FTData   = 2
	FTAck    = 3
	FTMacCmd = 4

	AddrNone  = 0
	AddrShort = 2
	AddrLong  = 3

	MaxFrameLen = 127
	LinkMICLen  = 4
	AppMICLen   = 8
	AppHdrLen   = 9
	UpSeqLen    = 4

	ShortBroadcast = 0xFFFF
	ShortGateway   = 0x0000

	KeyLink = 1
	KeyGrp  = 3
)

var (
	ErrFormat = errors.New("slp: bad frame format")
	ErrCRC    = errors.New("slp: bad CRC")
)

// Frame mirrors slp_frame_t.
type Frame struct {
	Type         uint8
	Security     bool
	FramePending bool
	AckRequest   bool
	Seq          uint8
	PAN          uint16
	DstMode      uint8
	Dst          uint64
	SrcMode      uint8
	Src          uint64
	FrameCounter uint32
	KeyID        uint8
	Payload      []byte
}

func addrLen(mode uint8) int {
	switch mode {
	case AddrNone:
		return 0
	case AddrShort:
		return 2
	case AddrLong:
		return 8
	}
	return -1
}

// CRC16 is CRC-16/CCITT-FALSE.
func CRC16(data []byte) uint16 {
	crc := uint16(0xFFFF)
	for _, b := range data {
		crc ^= uint16(b) << 8
		for i := 0; i < 8; i++ {
			if crc&0x8000 != 0 {
				crc = crc<<1 ^ 0x1021
			} else {
				crc <<= 1
			}
		}
	}
	return crc
}

func putLE(b []byte, v uint64, n int) {
	for i := 0; i < n; i++ {
		b[i] = byte(v >> (8 * i))
	}
}

func getLE(b []byte, n int) uint64 {
	var v uint64
	for i := 0; i < n; i++ {
		v |= uint64(b[i]) << (8 * i)
	}
	return v
}

// Header returns the encoded MAC header (the CCM additional data).
func (f *Frame) Header() ([]byte, error) {
	dl, sl := addrLen(f.DstMode), addrLen(f.SrcMode)
	if dl < 0 || sl < 0 {
		return nil, ErrFormat
	}
	fcf := uint16(f.Type & 7)
	if f.Security {
		fcf |= 1 << 3
	}
	if f.FramePending {
		fcf |= 1 << 4
	}
	if f.AckRequest {
		fcf |= 1 << 5
	}
	fcf |= uint16(f.DstMode&3) << 8
	fcf |= uint16(f.SrcMode&3) << 10
	h := make([]byte, 5+dl+sl)
	binary.LittleEndian.PutUint16(h, fcf)
	h[2] = f.Seq
	binary.LittleEndian.PutUint16(h[3:], f.PAN)
	putLE(h[5:], f.Dst, dl)
	putLE(h[5+dl:], f.Src, sl)
	if f.Security {
		sec := make([]byte, 5)
		binary.LittleEndian.PutUint32(sec, f.FrameCounter)
		sec[4] = f.KeyID
		h = append(h, sec...)
	}
	return h, nil
}

// Encode returns header | payload | CRC16.
func (f *Frame) Encode() ([]byte, error) {
	h, err := f.Header()
	if err != nil {
		return nil, err
	}
	out := append(h, f.Payload...)
	if len(out)+2 > MaxFrameLen {
		return nil, ErrFormat
	}
	crc := CRC16(out)
	return append(out, byte(crc), byte(crc>>8)), nil
}

// EncodeSecure encrypts f.Payload with the link key (MIC 4) and encodes.
func (f *Frame) EncodeSecure(key []byte, senderEUI uint64) ([]byte, error) {
	g := *f
	g.Security = true
	h, err := g.Header()
	if err != nil {
		return nil, err
	}
	g.Payload = CCMSeal(key, Nonce(senderEUI, g.FrameCounter, SecEncMIC32), h, f.Payload, LinkMICLen)
	return g.Encode()
}

// Decode parses and CRC-checks a frame. Payload aliases buf.
func Decode(buf []byte) (*Frame, int, error) {
	if len(buf) < 7 || len(buf) > MaxFrameLen {
		return nil, 0, ErrFormat
	}
	if getLE(buf[len(buf)-2:], 2) != uint64(CRC16(buf[:len(buf)-2])) {
		return nil, 0, ErrCRC
	}
	fcf := binary.LittleEndian.Uint16(buf)
	f := &Frame{
		Type:         uint8(fcf & 7),
		Security:     fcf>>3&1 == 1,
		FramePending: fcf>>4&1 == 1,
		AckRequest:   fcf>>5&1 == 1,
		DstMode:      uint8(fcf >> 8 & 3),
		SrcMode:      uint8(fcf >> 10 & 3),
	}
	if fcf>>12&3 != 0 {
		return nil, 0, ErrFormat
	}
	dl, sl := addrLen(f.DstMode), addrLen(f.SrcMode)
	hl := 5 + dl + sl
	if f.Security {
		hl += 5
	}
	if dl < 0 || sl < 0 || hl+2 > len(buf) {
		return nil, 0, ErrFormat
	}
	f.Seq = buf[2]
	f.PAN = binary.LittleEndian.Uint16(buf[3:])
	f.Dst = getLE(buf[5:], dl)
	f.Src = getLE(buf[5+dl:], sl)
	if f.Security {
		f.FrameCounter = binary.LittleEndian.Uint32(buf[5+dl+sl:])
		f.KeyID = buf[9+dl+sl]
	}
	f.Payload = buf[hl : len(buf)-2]
	return f, hl, nil
}

// DecodeSecure decodes and decrypts a secured frame. It does not check the
// frame counter; the caller enforces anti-replay afterwards.
func DecodeSecure(buf []byte, key func(f *Frame) ([]byte, uint64, error)) (*Frame, error) {
	f, hl, err := Decode(buf)
	if err != nil {
		return nil, err
	}
	if !f.Security {
		return nil, ErrFormat
	}
	k, sender, err := key(f)
	if err != nil {
		return nil, err
	}
	pt, err := CCMOpen(k, Nonce(sender, f.FrameCounter, SecEncMIC32), buf[:hl], f.Payload, LinkMICLen)
	if err != nil {
		return nil, err
	}
	f.Payload = pt
	return f, nil
}
