package console

import (
	"fmt"
	"net"
	"strconv"
	"strings"
	"sync"
	"time"
)

// SimLock is locksim's ground truth for one virtual lock.
type SimLock struct {
	State     int  `json:"state"`
	BatteryMV int  `json:"battery_mv"`
	RF        bool `json:"rf"`
	Joined    bool `json:"joined"`
	Cards     int  `json:"cards"`
	BattLevel int  `json:"batt_level"`
	Clutch    bool `json:"clutch"`
	Ind       int  `json:"ind"`
	IndAgeMs  int  `json:"ind_age_ms"`
	Channel   int  `json:"channel"`
	Slot      int  `json:"slot"`
	Short     int  `json:"short"`
	Flags     int  `json:"flags"`      // SLP_LF_*: door open, double lock, locked out
	LogSeq    int  `json:"log_seq"`    // newest record in the lock's log
	UpPending int  `json:"up_pending"` // uplinks waiting in the lock (offline / no ACK)
	ClockOffs int  `json:"clock_offset_s"`
	AutolockS int  `json:"autolock_s"`
	Remote    int  `json:"remote_allowed"`
}

// SimCtl talks to locksim's control port (text over UDP).
type SimCtl struct {
	addr  *net.UDPAddr
	mu    sync.Mutex
	snap  map[uint64]SimLock
	loss  int
	ok    bool
	onLog func(eui uint64, text string)
}

func NewSimCtl(addr string, onLog func(uint64, string)) (*SimCtl, error) {
	a, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		return nil, err
	}
	return &SimCtl{addr: a, snap: map[uint64]SimLock{}, onLog: onLog}, nil
}

// Do sends one command and waits for the reply.
func (s *SimCtl) Do(cmd string) (string, error) {
	c, err := net.DialUDP("udp", nil, s.addr)
	if err != nil {
		return "", err
	}
	defer c.Close()
	if _, err := c.Write([]byte(cmd)); err != nil {
		return "", err
	}
	_ = c.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 65536)
	n, err := c.Read(buf)
	if err != nil {
		return "", fmt.Errorf("locksim not reachable at %s: %w", s.addr, err)
	}
	r := string(buf[:n])
	if strings.HasPrefix(r, "ERR") {
		return r, fmt.Errorf("%s", r)
	}
	return r, nil
}

// Run polls snapshots and keeps a log subscription.
func (s *SimCtl) Run() {
	go s.logs()
	for {
		r, err := s.Do("snapshot")
		s.mu.Lock()
		s.ok = err == nil
		if err == nil {
			s.parse(r)
		}
		s.mu.Unlock()
		time.Sleep(250 * time.Millisecond)
	}
}

func (s *SimCtl) parse(r string) {
	lines := strings.Split(r, "\n")
	if len(lines) == 0 {
		return
	}
	var n, b, f, rooms int
	fmt.Sscanf(lines[0], "SNAP %d %d %d %d %d", &n, &b, &f, &rooms, &s.loss)
	for _, l := range lines[1:] {
		var e uint64
		var x SimLock
		var rf, joined, clutch int
		if _, err := fmt.Sscanf(l, "%x %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d", &e, &x.State, &x.BatteryMV, &rf, &joined,
			&x.Cards, &x.BattLevel, &clutch, &x.Ind, &x.IndAgeMs, &x.Channel, &x.Slot, &x.Short,
			&x.Flags, &x.LogSeq, &x.UpPending, &x.ClockOffs, &x.AutolockS, &x.Remote); err != nil {
			continue
		}
		x.RF, x.Joined, x.Clutch = rf == 1, joined == 1, clutch == 1
		s.snap[e] = x
	}
}

func (s *SimCtl) logs() {
	c, err := net.DialUDP("udp", nil, s.addr)
	if err != nil {
		return
	}
	go func() {
		for {
			_, _ = c.Write([]byte("subscribe"))
			time.Sleep(3 * time.Second)
		}
	}()
	buf := make([]byte, 4096)
	for {
		n, err := c.Read(buf)
		if err != nil {
			time.Sleep(200 * time.Millisecond)
			continue
		}
		line := string(buf[:n])
		if !strings.HasPrefix(line, "LOG ") {
			continue
		}
		parts := strings.SplitN(line[4:], " ", 2)
		if len(parts) != 2 {
			continue
		}
		eui, _ := strconv.ParseUint(parts[0], 16, 64)
		if s.onLog != nil {
			s.onLog(eui, parts[1])
		}
	}
}

// Snapshot returns a copy of the latest snapshot.
func (s *SimCtl) Snapshot() (map[uint64]SimLock, int, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make(map[uint64]SimLock, len(s.snap))
	for k, v := range s.snap {
		out[k] = v
	}
	return out, s.loss, s.ok
}
