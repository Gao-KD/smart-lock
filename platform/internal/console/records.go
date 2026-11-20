package console

import (
	"context"
	"encoding/hex"
	"fmt"
	"strings"
	"time"

	"github.com/Gao-KD/smart-lock/common/slpgo/slp"
	"github.com/Gao-KD/smart-lock/platform/internal/dispatch"
)

// Record handling: every access, alarm, door and request event carries the
// lock's LOG_SEQ. The platform keeps track of which sequence numbers it has
// seen; heartbeats tell it the newest one. A gap that stays open for a few
// seconds (records that overflowed the lock's queue during a long outage,
// or uplinks that gave up after their retries) is pulled with READ_LOG.

const (
	gapGrace      = 3 * time.Second  // events may still be on their way
	readLogRetry  = 10 * time.Second // between READ_LOG requests for one lock
	readLogMax    = 16               // records per READ_LOG (the lock's queue size)
	lockLogWindow = 128              // records the lock keeps (LOCK_LOG_SIZE)
)

// firstSight de-duplicates uplinks retransmitted after a lost ACK.
func (l *LockView) firstSight(key uint64) bool {
	if l.dedup == nil {
		l.dedup = map[uint64]bool{}
	}
	if l.dedup[key] {
		return false
	}
	l.dedup[key] = true
	l.dedupOrder = append(l.dedupOrder, key)
	if len(l.dedupOrder) > 64 {
		delete(l.dedup, l.dedupOrder[0])
		l.dedupOrder = l.dedupOrder[1:]
	}
	return true
}

// noteRecord registers a record's LOG_SEQ; false if it was seen before.
func (c *Console) noteRecord(l *LockView, m *slp.AppMsg) bool {
	s, ok := m.U32(slp.TLVLogSeq)
	if !ok || s == 0 {
		return true
	}
	if l.logSeen == nil {
		l.logSeen = map[uint32]bool{}
		l.missing = map[uint32]time.Time{}
	}
	if l.logSeen[s] {
		return false
	}
	l.logSeen[s] = true
	l.Records++
	delete(l.missing, s)
	c.noteHead(l, s)
	if len(l.logSeen) > 4*lockLogWindow {
		for k := range l.logSeen {
			if k+2*lockLogWindow < l.LogSeq {
				delete(l.logSeen, k)
			}
		}
	}
	l.Missing = len(l.missing)
	return true
}

// noteHead learns that records up to `head` exist.
func (c *Console) noteHead(l *LockView, head uint32) {
	if l.logSeen == nil {
		l.logSeen = map[uint32]bool{}
		l.missing = map[uint32]time.Time{}
	}
	if head < l.LogSeq {
		if head+lockLogWindow < l.LogSeq {
			// The lock's log restarted (factory reset / new board).
			l.LogSeq = head
			l.missing = map[uint32]time.Time{}
		}
		return
	}
	from := l.LogSeq + 1
	if head >= lockLogWindow && from+lockLogWindow <= head {
		from = head - lockLogWindow + 1 // older ones are gone from the lock too
	}
	now := time.Now()
	for s := from; s <= head; s++ {
		if !l.logSeen[s] {
			if _, ok := l.missing[s]; !ok {
				l.missing[s] = now
			}
		}
	}
	l.LogSeq = head
	l.Missing = len(l.missing)
}

func accessText(m *slp.AppMsg) string {
	res, _ := m.U8(slp.TLVResult)
	ct, _ := m.U8(slp.TLVCredType)
	id, _ := m.Find(slp.TLVCredID)
	user, _ := m.U32(slp.TLVUserID)
	var who string
	switch ct {
	case slp.CredCardUID:
		who = "刷卡 " + strings.ToUpper(hex.EncodeToString(id))
	case slp.CredOfflinePwd:
		who = "离线密码"
		if len(id) == 2 {
			kinds := map[byte]string{'0': "一次性", '9': "清除码"}
			k := kinds[id[0]]
			if k == "" {
				k = "限时"
			}
			who = fmt.Sprintf("离线密码(%s)", k)
		}
	default:
		who = slp.CredTypeNames[ct]
		if who == "" {
			who = fmt.Sprintf("凭证类型 %d", ct)
		}
	}
	txt := who + ": "
	if res == slp.ResultOK {
		if user != 0 {
			txt += fmt.Sprintf("开门 (用户 %d)", user)
		} else {
			txt += "开门"
		}
	} else {
		txt += "拒绝 (" + slp.ResultNames[res] + ")"
		if user != 0 {
			txt += fmt.Sprintf(" 用户 %d", user)
		}
	}
	return txt
}

// onRecord turns a new record into the platform's view and event log.
func (c *Console) onRecord(eui uint64, l *LockView, m *slp.AppMsg) {
	prefix := ""
	if v, _ := m.U8(slp.TLVReplayed); v == 1 {
		prefix = "[补传] "
	}
	switch m.Cmd {
	case slp.EvtAccess:
		txt := accessText(m)
		l.LastAccess = txt
		c.addEventLocked(eui, "access", prefix+txt)
	case slp.EvtAlarm:
		a, _ := m.U8(slp.TLVAlarm)
		txt := slp.AlarmNames[a]
		if user, ok := m.U32(slp.TLVUserID); ok {
			txt += fmt.Sprintf("，用户 %d", user)
		}
		if a == slp.AlarmWrongAttempts {
			l.LockedOut = true
		}
		l.Alarms++
		l.LastAlarm = txt
		c.addEventLocked(eui, "alarm", prefix+"告警: "+txt)
	case slp.EvtDoor:
		door, _ := m.U8(slp.TLVDoor)
		dl, _ := m.U8(slp.TLVDoubleLck)
		var txt string
		switch {
		case (dl == 1) != l.DoubleLock:
			txt = map[bool]string{true: "室内已反锁", false: "已解除反锁"}[dl == 1]
		default:
			txt = map[bool]string{true: "门已打开", false: "门已关上"}[door == 1]
		}
		if prefix == "" {
			l.DoorOpen, l.DoubleLock = door == 1, dl == 1
		}
		c.addEventLocked(eui, "door", prefix+txt)
	case slp.EvtUnlockReq:
		id, _ := m.U16(slp.TLVReqID)
		win, _ := m.U16(slp.TLVDurationS)
		at, _ := m.U32(slp.TLVUnixTime)
		until := time.Unix(int64(at)+int64(win), 0)
		if prefix == "" && time.Now().Before(until) {
			l.Request = &UnlockRequest{ID: id, Until: until}
		}
		c.addEventLocked(eui, "request", fmt.Sprintf("%s访客按呼叫键请求开门 (请求 %d，%d 秒内可批准)", prefix, id, win))
	}
}

// RunBackground pulls missing records. Call once.
func (c *Console) RunBackground(ctx context.Context) {
	t := time.NewTicker(time.Second)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			c.pullGaps(ctx)
		}
	}
}

func (c *Console) pullGaps(ctx context.Context) {
	type pull struct {
		eui   uint64
		from  uint32
		count int
	}
	var todo []pull
	now := time.Now()
	c.mu.Lock()
	for eui, l := range c.locks {
		if len(l.missing) == 0 || now.Sub(l.readLogAt) < readLogRetry {
			continue
		}
		var from uint32
		for s, at := range l.missing {
			if now.Sub(at) < gapGrace {
				continue
			}
			if from == 0 || s < from {
				from = s
			}
		}
		if from == 0 {
			continue
		}
		last := from
		for s, at := range l.missing {
			if s > last && s < from+readLogMax && now.Sub(at) >= gapGrace {
				last = s
			}
		}
		l.readLogAt = now
		l.ReadLogs++
		todo = append(todo, pull{eui, from, int(last-from) + 1})
	}
	c.mu.Unlock()
	for _, p := range todo {
		tlv := slp.EncodeTLV([]slp.TLV{slp.U32(slp.TLVLogSeq, p.from), slp.U16(slp.TLVCount, uint16(p.count))})
		ids, err := c.Svc.Submit(ctx, []dispatch.Request{{LockEUI: p.eui, Cmd: dispatch.CmdReadLog, TLV: tlv, Priority: dispatch.P2Management}})
		if err == nil {
			c.AddEvent(p.eui, "cmd", fmt.Sprintf("发现缺失的开门/告警记录 #%d 起 %d 条，下发补传 (指令 %d)", p.from, p.count, ids[0]))
		}
	}
}
