package console

import (
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/Gao-KD/smart-lock/common/slpgo/simnet"
	"github.com/Gao-KD/smart-lock/common/slpgo/slp"
	"github.com/Gao-KD/smart-lock/platform/internal/dispatch"
)

// credBody is the common body of card and password requests.
type credBody struct {
	UID        string `json:"uid"`
	PIN        string `json:"pin"`
	UserID     uint32 `json:"user_id"`
	ValidDays  int    `json:"valid_days"`
	ValidHours int    `json:"valid_hours"`
	Uses       uint16 `json:"uses"`  // 1 = one-time
	Days       []int  `json:"days"`  // weekly schedule, 1 = Monday .. 7 = Sunday
	Start      string `json:"start"` // "08:00"
	End        string `json:"end"`   // "10:00"
	Duress     bool   `json:"duress"`
	Admin      bool   `json:"admin"`
}

func parseHHMM(s string) (uint16, error) {
	var h, m int
	if _, err := fmt.Sscanf(s, "%d:%d", &h, &m); err != nil || h < 0 || h > 24 || m < 0 || m > 59 || h*60+m > 1440 {
		return 0, fmt.Errorf("time must be HH:MM, got %q", s)
	}
	return uint16(h*60 + m), nil
}

// credTLV builds CRED_ADD / CRED_DEL TLVs.
func credTLV(typ uint8, id []byte, b credBody) ([]byte, error) {
	t := []slp.TLV{slp.U8(slp.TLVCredType, typ), {Type: slp.TLVCredID, Value: id}}
	if b.UserID != 0 {
		t = append(t, slp.U32(slp.TLVUserID, b.UserID))
	}
	switch {
	case b.ValidHours > 0:
		t = append(t, slp.U32(slp.TLVValidTo, uint32(time.Now().Add(time.Duration(b.ValidHours)*time.Hour).Unix())))
	case b.ValidDays > 0:
		t = append(t, slp.U32(slp.TLVValidTo, uint32(time.Now().Add(time.Duration(b.ValidDays)*24*time.Hour).Unix())))
	}
	if b.Uses > 0 {
		t = append(t, slp.U16(slp.TLVUses, b.Uses))
	}
	var flags uint8
	if b.Duress {
		flags |= slp.CredFDuress
	}
	if b.Admin {
		flags |= slp.CredFAdmin
	}
	if flags != 0 {
		t = append(t, slp.U8(slp.TLVCredFlags, flags))
	}
	if len(b.Days) > 0 || b.Start != "" || b.End != "" {
		var mask uint8
		for _, d := range b.Days {
			if d < 1 || d > 7 {
				return nil, fmt.Errorf("days are 1 (Monday) .. 7 (Sunday)")
			}
			mask |= 1 << (d - 1)
		}
		var start, end uint16
		var err error
		if b.Start != "" || b.End != "" {
			if start, err = parseHHMM(b.Start); err != nil {
				return nil, err
			}
			if end, err = parseHHMM(b.End); err != nil {
				return nil, err
			}
		}
		t = append(t, slp.TLV{Type: slp.TLVSchedule, Value: slp.Schedule(mask, start, end)})
	}
	return slp.EncodeTLV(t), nil
}

func validPIN(p string) bool {
	if len(p) < 6 || len(p) > 10 {
		return false
	}
	for _, c := range p {
		if c < '0' || c > '9' {
			return false
		}
	}
	return true
}

func decode(w http.ResponseWriter, r *http.Request, v any) bool {
	if err := json.NewDecoder(r.Body).Decode(v); err != nil && err.Error() != "EOF" {
		http.Error(w, "bad body: "+err.Error(), http.StatusBadRequest)
		return false
	}
	return true
}

// registerFeatures adds the API of the lock features beyond cards.
func (c *Console) registerFeatures(mux *http.ServeMux) {
	mux.HandleFunc("POST /api/locks/{lock}/passwords", c.withLock(func(w http.ResponseWriter, r *http.Request, eui uint64) {
		var b credBody
		if !decode(w, r, &b) {
			return
		}
		if !validPIN(b.PIN) {
			http.Error(w, "pin must be 6..10 digits", http.StatusBadRequest)
			return
		}
		tlv, err := credTLV(slp.CredPassword, []byte(b.PIN), b)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		c.submit(w, eui, dispatch.CmdCredAdd, tlv, dispatch.P2Management)
	}))
	mux.HandleFunc("DELETE /api/locks/{lock}/passwords/{pin}", c.withLock(func(w http.ResponseWriter, r *http.Request, eui uint64) {
		tlv, _ := credTLV(slp.CredPassword, []byte(r.PathValue("pin")), credBody{})
		c.submit(w, eui, dispatch.CmdCredDel, tlv, dispatch.P2Management)
	}))

	// Member management: move-out removes every credential, rent arrears
	// freezes them.
	mux.HandleFunc("DELETE /api/locks/{lock}/users/{user}", c.withLock(func(w http.ResponseWriter, r *http.Request, eui uint64) {
		u, err := strconv.ParseUint(r.PathValue("user"), 10, 32)
		if err != nil || u == 0 {
			http.Error(w, "bad user id", http.StatusBadRequest)
			return
		}
		c.submit(w, eui, dispatch.CmdUserDel, slp.EncodeTLV([]slp.TLV{slp.U32(slp.TLVUserID, uint32(u))}), dispatch.P2Management)
	}))
	mux.HandleFunc("POST /api/locks/{lock}/users/{user}/freeze", c.withLock(func(w http.ResponseWriter, r *http.Request, eui uint64) {
		u, err := strconv.ParseUint(r.PathValue("user"), 10, 32)
		var b struct {
			Freeze *bool `json:"freeze"`
		}
		if err != nil || u == 0 || !decode(w, r, &b) {
			http.Error(w, "bad request", http.StatusBadRequest)
			return
		}
		f := uint8(1)
		if b.Freeze != nil && !*b.Freeze {
			f = 0
		}
		c.submit(w, eui, dispatch.CmdUserFreeze, slp.EncodeTLV([]slp.TLV{slp.U32(slp.TLVUserID, uint32(u)), slp.U8(slp.TLVFreeze, f)}), dispatch.P2Management)
	}))

	mux.HandleFunc("POST /api/locks/{lock}/config", c.withLock(func(w http.ResponseWriter, r *http.Request, eui uint64) {
		tlv, ok := c.configTLV(w, r)
		if ok {
			c.submit(w, eui, dispatch.CmdConfig, tlv, dispatch.P2Management)
		}
	}))
	mux.HandleFunc("POST /api/locks/{lock}/read-log", c.withLock(func(w http.ResponseWriter, r *http.Request, eui uint64) {
		var b struct {
			From  uint32 `json:"from"`
			Count uint16 `json:"count"`
		}
		if !decode(w, r, &b) {
			return
		}
		if b.Count == 0 {
			b.Count = readLogMax
		}
		c.submit(w, eui, dispatch.CmdReadLog, slp.EncodeTLV([]slp.TLV{slp.U32(slp.TLVLogSeq, b.From), slp.U16(slp.TLVCount, b.Count)}), dispatch.P2Management)
	}))

	// Visitor's unlock request answered by the resident / dorm keeper.
	mux.HandleFunc("POST /api/locks/{lock}/requests/approve", c.withLock(func(w http.ResponseWriter, r *http.Request, eui uint64) {
		c.mu.Lock()
		l := c.locks[eui]
		req := l.Request
		l.Request = nil
		c.mu.Unlock()
		if req == nil || time.Now().After(req.Until) {
			http.Error(w, "no pending unlock request", http.StatusConflict)
			return
		}
		tlv := slp.EncodeTLV([]slp.TLV{slp.U16(slp.TLVReqID, req.ID)})
		c.submit(w, eui, dispatch.CmdUnlock, tlv, dispatch.P1Interactive)
	}))

	// Offline password: computed here, nothing is sent to the lock.
	mux.HandleFunc("POST /api/locks/{lock}/offline-password", c.withLock(func(w http.ResponseWriter, r *http.Request, eui uint64) {
		var b struct {
			Kind  string `json:"kind"` // once | timed | clear
			Hours int    `json:"hours"`
		}
		if !decode(w, r, &b) {
			return
		}
		var kind uint8
		var desc string
		switch b.Kind {
		case "", "once":
			kind, desc = slp.OfflineOnce, "一次性，6 小时内使用一次"
		case "clear":
			kind, desc = slp.OfflineClear, "清除码：输入后之前发出的离线密码全部作废"
		case "timed":
			k, err := slp.OfflineKindFor(max(b.Hours, 1))
			if err != nil {
				http.Error(w, err.Error(), http.StatusBadRequest)
				return
			}
			kind, desc = k, fmt.Sprintf("限时 %d 小时内可反复使用", slp.OfflineClassHours[k])
		default:
			http.Error(w, "kind must be once, timed or clear", http.StatusBadRequest)
			return
		}
		hour := uint32(time.Now().Unix() / 3600)
		c.mu.Lock()
		l := c.locks[eui]
		if l.ofpIndex == nil {
			l.ofpIndex = map[uint32]uint8{}
		}
		idx := l.ofpIndex[hour]
		if idx > 9 {
			c.mu.Unlock()
			http.Error(w, "at most 10 offline passwords per lock per hour", http.StatusTooManyRequests)
			return
		}
		l.ofpIndex[hour] = idx + 1
		c.mu.Unlock()
		code := slp.OfflinePassword(simnet.Key(simnet.TagApp, eui), eui, kind, idx, hour)
		until := slp.OfflineWindow(kind, hour)
		c.AddEvent(eui, "cmd", fmt.Sprintf("平台生成离线密码 (%s，有效至 %s)，不经过网络下发", desc, until.Format("01-02 15:04")))
		writeJSON(w, http.StatusOK, map[string]any{"password": code, "kind": b.Kind, "desc": desc, "valid_until": until})
	}))

	// BLE key for a phone: works at the door even with no gateway.
	mux.HandleFunc("POST /api/locks/{lock}/ble-key", c.withLock(func(w http.ResponseWriter, r *http.Request, eui uint64) {
		var b struct {
			UserID uint32 `json:"user_id"`
			Hours  int    `json:"hours"`
			Admin  bool   `json:"admin"`
		}
		if !decode(w, r, &b) {
			return
		}
		if b.Hours <= 0 {
			b.Hours = 24
		}
		var flags uint8
		if b.Admin {
			flags = slp.CredFAdmin
		}
		to := time.Now().Add(time.Duration(b.Hours) * time.Hour)
		tok := slp.BLEToken(simnet.Key(simnet.TagApp, eui), eui, b.UserID, to, flags)
		c.AddEvent(eui, "cmd", fmt.Sprintf("平台给用户 %d 签发手机蓝牙钥匙 (有效至 %s)", b.UserID, to.Format("01-02 15:04")))
		writeJSON(w, http.StatusOK, map[string]any{"token": strings.ToUpper(hex.EncodeToString(tok)), "valid_to": to})
	}))
}

// configTLV reads a CONFIG body; only the given fields are changed.
func (c *Console) configTLV(w http.ResponseWriter, r *http.Request) ([]byte, bool) {
	var b struct {
		AutolockS  *uint16 `json:"autolock_s"`
		Volume     *uint8  `json:"volume"`
		Remote     *bool   `json:"remote_allowed"`
		WrongLimit *uint8  `json:"wrong_limit"`
		LockoutS   *uint16 `json:"lockout_s"`
		HeartbeatS *uint16 `json:"heartbeat_s"`
		DoorAlarmS *uint16 `json:"door_alarm_s"`
	}
	if !decode(w, r, &b) {
		return nil, false
	}
	var t []slp.TLV
	if b.AutolockS != nil {
		t = append(t, slp.U16(slp.TLVCfgAutolockS, *b.AutolockS))
	}
	if b.Volume != nil {
		t = append(t, slp.U8(slp.TLVCfgVolume, *b.Volume))
	}
	if b.Remote != nil {
		v := uint8(0)
		if *b.Remote {
			v = 1
		}
		t = append(t, slp.U8(slp.TLVCfgRemote, v))
	}
	if b.WrongLimit != nil {
		t = append(t, slp.U8(slp.TLVCfgWrongLimit, *b.WrongLimit))
	}
	if b.LockoutS != nil {
		t = append(t, slp.U16(slp.TLVCfgLockoutS, *b.LockoutS))
	}
	if b.HeartbeatS != nil {
		t = append(t, slp.U16(slp.TLVCfgHeartbeatS, *b.HeartbeatS))
	}
	if b.DoorAlarmS != nil {
		t = append(t, slp.U16(slp.TLVCfgDoorAlarmS, *b.DoorAlarmS))
	}
	if len(t) == 0 {
		http.Error(w, "no config field given", http.StatusBadRequest)
		return nil, false
	}
	return slp.EncodeTLV(t), true
}
