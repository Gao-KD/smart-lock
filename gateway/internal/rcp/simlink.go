package rcp

import (
	"encoding/binary"
	"net"
	"time"

	"github.com/Gao-KD/smart-lock/common/slpgo/simnet"
)

// SimLink talks to the simulated radio co-processors in locksim over UDP
// (wire format: common/slpgo/simnet). It stands in for the serial driver.
type SimLink struct {
	conn    *net.UDPConn
	results chan TxResult
	uplinks chan RxFrame
}

// DialSim connects to the locksim port of this gateway.
func DialSim(addr string) (*SimLink, error) {
	ra, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		return nil, err
	}
	c, err := net.DialUDP("udp", nil, ra)
	if err != nil {
		return nil, err
	}
	_ = c.SetReadBuffer(1 << 20)
	l := &SimLink{conn: c, results: make(chan TxResult, 4096), uplinks: make(chan RxFrame, 4096)}
	go l.read()
	return l, nil
}

func (l *SimLink) read() {
	buf := make([]byte, 65536)
	for {
		n, err := l.conn.Read(buf)
		if err != nil {
			// locksim not up yet (ICMP port unreachable): keep listening.
			time.Sleep(200 * time.Millisecond)
			continue
		}
		m := buf[:n]
		switch {
		case m[0] == simnet.MsgTxResult && n >= 7:
			l.results <- TxResult{Ref: uint64(binary.LittleEndian.Uint32(m[1:])), Acked: m[5] == 1, RSSI: int8(m[6])}
		case m[0] == simnet.MsgRx && n >= 4 && n >= 4+int(m[3]):
			f := append([]byte(nil), m[4:4+int(m[3])]...)
			l.uplinks <- RxFrame{Channel: int(m[1]), RSSI: int8(m[2]), Frame: f}
		}
	}
}

func (l *SimLink) Results() <-chan TxResult { return l.results }
func (l *SimLink) Uplinks() <-chan RxFrame  { return l.uplinks }

func (l *SimLink) Send(b []byte) error {
	_, err := l.conn.Write(b)
	return err
}
