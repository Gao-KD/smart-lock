package console

import (
	"embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/Gao-KD/smart-lock/common/slpgo/simnet"
	"github.com/Gao-KD/smart-lock/common/slpgo/slp"
	"github.com/Gao-KD/smart-lock/platform/internal/dispatch"
)

//go:embed static
var static embed.FS

// Handler serves the web console (/), the console API (/api/...) and the
// open API (/v1/...).
func (c *Console) Handler() http.Handler {
	mux := http.NewServeMux()
	sub, _ := fs.Sub(static, "static")
	mux.Handle("GET /", http.FileServer(http.FS(sub)))

	mux.HandleFunc("GET /api/state", func(w http.ResponseWriter, r *http.Request) {
		since, _ := strconv.ParseUint(r.URL.Query().Get("since"), 10, 64)
		writeJSON(w, http.StatusOK, c.State(since, r.URL.Query().Get("locks") != "0"))
	})
	mux.HandleFunc("GET /api/locks/{lock}", c.withLock(func(w http.ResponseWriter, r *http.Request, eui uint64) {
		l, _ := c.Lock(eui)
		writeJSON(w, http.StatusOK, l)
	}))
	mux.HandleFunc("GET /api/commands/{id}", func(w http.ResponseWriter, r *http.Request) {
		id, _ := strconv.ParseUint(r.PathValue("id"), 10, 32)
		rec, ok := c.Svc.Record(uint32(id))
		if !ok {
			http.Error(w, "not found", http.StatusNotFound)
			return
		}
		writeJSON(w, http.StatusOK, rec)
	})
	mux.HandleFunc("GET /api/jobs/{id}", func(w http.ResponseWriter, r *http.Request) {
		id, _ := strconv.ParseUint(r.PathValue("id"), 10, 64)
		j, ok := c.Svc.Job(id)
		if !ok {
			http.Error(w, "not found", http.StatusNotFound)
			return
		}
		writeJSON(w, http.StatusOK, j)
	})

	// ---- unicast commands ----
	mux.HandleFunc("POST /api/locks/{lock}/unlock", c.withLock(func(w http.ResponseWriter, r *http.Request, eui uint64) {
		var body struct {
			DurationS uint16 `json:"duration_s"`
		}
		_ = json.NewDecoder(r.Body).Decode(&body)
		c.submit(w, eui, dispatch.CmdUnlock, durationTLV(body.DurationS), dispatch.P1Interactive)
	}))
	mux.HandleFunc("POST /api/locks/{lock}/status", c.withLock(func(w http.ResponseWriter, r *http.Request, eui uint64) {
		c.submit(w, eui, dispatch.CmdStatus, nil, dispatch.P1Interactive)
	}))
	mux.HandleFunc("POST /api/locks/{lock}/lockdown", c.withLock(func(w http.ResponseWriter, r *http.Request, eui uint64) {
		c.submit(w, eui, dispatch.CmdLockdown, nil, dispatch.P1Interactive)
	}))
	mux.HandleFunc("POST /api/locks/{lock}/release", c.withLock(func(w http.ResponseWriter, r *http.Request, eui uint64) {
		c.submit(w, eui, dispatch.CmdRelease, nil, dispatch.P1Interactive)
	}))
	mux.HandleFunc("POST /api/locks/{lock}/cards", c.withLock(func(w http.ResponseWriter, r *http.Request, eui uint64) {
		var body credBody
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			http.Error(w, "bad body", http.StatusBadRequest)
			return
		}
		uid, err := parseUID(body.UID)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		tlv, err := credTLV(slp.CredCardUID, uid, body)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		c.submit(w, eui, dispatch.CmdCredAdd, tlv, dispatch.P2Management)
	}))
	c.registerFeatures(mux)
	mux.HandleFunc("DELETE /api/locks/{lock}/cards/{uid}", c.withLock(func(w http.ResponseWriter, r *http.Request, eui uint64) {
		uid, err := parseUID(r.PathValue("uid"))
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		c.submit(w, eui, dispatch.CmdCredDel, CardTLV(uid, 0, time.Time{}), dispatch.P2Management)
	}))

	// ---- whole building ----
	mux.HandleFunc("POST /api/buildings/{b}/{action}", func(w http.ResponseWriter, r *http.Request) {
		b := r.PathValue("b")
		switch r.PathValue("action") {
		case "emergency-open":
			c.broadcast(w, b, dispatch.CmdEmergency, dispatch.P0Emergency, "全楼应急常开")
		case "lockdown":
			c.broadcast(w, b, dispatch.CmdLockdown, dispatch.P0Emergency, "全楼封锁")
		case "release":
			c.broadcast(w, b, dispatch.CmdRelease, dispatch.P0Emergency, "全楼恢复正常")
		case "issue-cards":
			c.bulk(w, b, "全楼批量发卡 (每间一张卡)", dispatch.CmdCredAdd, dispatch.P2Management, func(f, rm int) []byte {
				return CardTLV(RoomCardUID(c.Reg.Layout.Building, f, rm), uint32(f*100+rm), time.Time{})
			})
		case "revoke-cards":
			c.bulk(w, b, "全楼批量删卡", dispatch.CmdCredDel, dispatch.P2Management, func(f, rm int) []byte {
				return CardTLV(RoomCardUID(c.Reg.Layout.Building, f, rm), 0, time.Time{})
			})
		case "unlock-all":
			c.bulk(w, b, "全楼逐间远程开门 (单播 P1)", dispatch.CmdUnlock, dispatch.P1Interactive, func(int, int) []byte { return nil })
		case "status-all":
			c.bulk(w, b, "全楼状态巡检", dispatch.CmdStatus, dispatch.P1Interactive, func(int, int) []byte { return nil })
		case "clear-creds":
			c.bulk(w, b, "全楼清空凭证 (毕业季/学期末)", dispatch.CmdCredClear, dispatch.P2Management, func(int, int) []byte { return nil })
		case "config":
			tlv, ok := c.configTLV(w, r)
			if ok {
				c.bulk(w, b, "全楼统一参数配置", dispatch.CmdConfig, dispatch.P2Management, func(int, int) []byte { return tlv })
			}
		default:
			http.Error(w, "unknown action", http.StatusNotFound)
		}
	})

	// ---- simulator controls (the "physical world") ----
	mux.HandleFunc("POST /api/sim/locks/{lock}/{action}", c.withLock(func(w http.ResponseWriter, r *http.Request, eui uint64) {
		if c.Sim == nil {
			http.Error(w, "simulator not configured", http.StatusServiceUnavailable)
			return
		}
		var body struct {
			UID    string `json:"uid"`
			MV     int    `json:"mv"`
			On     *bool  `json:"on"`
			N      int    `json:"n"`
			Digits string `json:"digits"`
			Token  string `json:"token"`
			Open   *bool  `json:"open"`
			DeltaS int    `json:"delta_s"`
		}
		_ = json.NewDecoder(r.Body).Decode(&body)
		id := fmt.Sprintf("%016x", eui)
		var cmd string
		switch r.PathValue("action") {
		case "swipe":
			uid, err := parseUID(body.UID)
			if err != nil {
				http.Error(w, err.Error(), http.StatusBadRequest)
				return
			}
			cmd = "swipe " + id + " " + hex.EncodeToString(uid)
		case "battery":
			cmd = fmt.Sprintf("battery %s %d", id, body.MV)
		case "rf":
			on := body.On == nil || *body.On
			cmd = "rf " + id + map[bool]string{true: " on", false: " off"}[on]
		case "dropack":
			cmd = fmt.Sprintf("dropack %s %d", id, max(body.N, 1))
		case "jam":
			cmd = fmt.Sprintf("jam %s %d", id, max(body.N, 1))
		case "replay", "tamper", "pry", "key", "doorbell":
			cmd = r.PathValue("action") + " " + id
		case "keypad":
			if !validDigits(body.Digits) {
				http.Error(w, "digits: 1..16 of 0-9", http.StatusBadRequest)
				return
			}
			cmd = "keypad " + id + " " + body.Digits
		case "ble":
			tok, err := hex.DecodeString(body.Token)
			if err != nil || len(tok) == 0 || len(tok) > 32 {
				http.Error(w, "token: hex", http.StatusBadRequest)
				return
			}
			cmd = "ble " + id + " " + hex.EncodeToString(tok)
		case "door":
			cmd = "door " + id + map[bool]string{true: " open", false: " close"}[body.Open != nil && *body.Open]
		case "dlock":
			cmd = "dlock " + id + map[bool]string{true: " on", false: " off"}[body.On == nil || *body.On]
		case "clock":
			cmd = fmt.Sprintf("clock %s %d", id, body.DeltaS)
		default:
			http.Error(w, "unknown action", http.StatusNotFound)
			return
		}
		out, err := c.Sim.Do(cmd)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadGateway)
			return
		}
		writeJSON(w, http.StatusOK, map[string]string{"reply": out})
	}))
	mux.HandleFunc("POST /api/sim/loss", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			Pct int `json:"pct"`
		}
		_ = json.NewDecoder(r.Body).Decode(&body)
		if c.Sim == nil {
			http.Error(w, "simulator not configured", http.StatusServiceUnavailable)
			return
		}
		out, err := c.Sim.Do(fmt.Sprintf("loss %d", body.Pct))
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadGateway)
			return
		}
		writeJSON(w, http.StatusOK, map[string]string{"reply": out})
	})

	// ---- open API (PR #1 paths, kept compatible) ----
	mux.HandleFunc("POST /v1/locks/{eui}/unlock", c.withLock(func(w http.ResponseWriter, r *http.Request, eui uint64) {
		c.submit(w, eui, dispatch.CmdUnlock, nil, dispatch.P1Interactive)
	}))
	mux.HandleFunc("POST /v1/buildings/{building}/emergency-open", func(w http.ResponseWriter, r *http.Request) {
		c.broadcast(w, r.PathValue("building"), dispatch.CmdEmergency, dispatch.P0Emergency, "全楼应急常开")
	})
	mux.HandleFunc("GET /v1/commands/{id}", func(w http.ResponseWriter, r *http.Request) {
		id, _ := strconv.ParseUint(r.PathValue("id"), 10, 32)
		st, ok := c.Svc.Status(uint32(id))
		if !ok {
			http.Error(w, "not found", http.StatusNotFound)
			return
		}
		writeJSON(w, http.StatusOK, map[string]any{"msg_id": id, "status": st})
	})
	return mux
}

// withLock resolves {lock} (room "1203" or EUI) or {eui}.
func (c *Console) withLock(h func(http.ResponseWriter, *http.Request, uint64)) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		ref := r.PathValue("lock")
		if ref == "" {
			ref = r.PathValue("eui")
		}
		eui, err := c.Reg.Resolve(ref)
		if err != nil {
			http.Error(w, "unknown lock "+ref, http.StatusNotFound)
			return
		}
		h(w, r, eui)
	}
}

func (c *Console) submit(w http.ResponseWriter, eui uint64, cmd uint8, tlv []byte, prio dispatch.Priority) {
	ids, err := c.Svc.Submit(reqCtx(), []dispatch.Request{{LockEUI: eui, Cmd: cmd, TLV: tlv, Priority: prio}})
	if errors.Is(err, dispatch.ErrUnknownLock) {
		http.Error(w, err.Error(), http.StatusNotFound)
		return
	} else if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	c.AddEvent(eui, "cmd", fmt.Sprintf("平台下发 %s (指令 %d)", dispatch.CmdNames[cmd], ids[0]))
	writeJSON(w, http.StatusAccepted, map[string]any{"msg_id": ids[0], "status": dispatch.StatusQueued})
}

func (c *Console) broadcast(w http.ResponseWriter, building string, cmd uint8, prio dispatch.Priority, name string) {
	ids, err := c.Svc.Broadcast(reqCtx(), c.Tenant, building, cmd, prio)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	for _, id := range ids {
		if rec, ok := c.Svc.Record(id); ok {
			c.Svc.SetExpected(id, c.locksOn(rec.Gateway))
		}
	}
	j := c.Svc.NewJob(name, ids)
	c.AddEvent(0, "cmd", fmt.Sprintf("平台广播 %s 到 %d 台网关", name, len(ids)))
	writeJSON(w, http.StatusAccepted, map[string]any{"job": j.ID, "msg_ids": ids, "locks": c.Reg.Layout.Floors * c.Reg.Layout.Rooms})
}

func (c *Console) bulk(w http.ResponseWriter, building, name string, cmd uint8, prio dispatch.Priority, tlv func(f, r int) []byte) {
	l := c.Reg.Layout
	if building != strconv.Itoa(l.Building) {
		http.Error(w, "unknown building", http.StatusNotFound)
		return
	}
	var reqs []dispatch.Request
	for f := 1; f <= l.Floors; f++ {
		for rm := 1; rm <= l.Rooms; rm++ {
			reqs = append(reqs, dispatch.Request{LockEUI: simnet.LockEUI(l.Building, f, rm), Cmd: cmd, TLV: tlv(f, rm), Priority: prio})
		}
	}
	ids, err := c.Svc.Submit(reqCtx(), reqs)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	j := c.Svc.NewJob(name, ids)
	c.AddEvent(0, "cmd", fmt.Sprintf("平台批量下发 %s: %d 条指令", name, len(ids)))
	writeJSON(w, http.StatusAccepted, map[string]any{"job": j.ID, "count": len(ids)})
}

func (c *Console) locksOn(gateway string) int {
	c.mu.Lock()
	defer c.mu.Unlock()
	n := 0
	for _, l := range c.locks {
		if l.Gateway == gateway {
			n++
		}
	}
	return n
}

func validDigits(s string) bool {
	if len(s) == 0 || len(s) > 16 {
		return false
	}
	for _, c := range s {
		if c < '0' || c > '9' {
			return false
		}
	}
	return true
}

func parseUID(s string) ([]byte, error) {
	s = strings.ReplaceAll(strings.ReplaceAll(s, ":", ""), " ", "")
	b, err := hex.DecodeString(s)
	if err != nil || (len(b) != 4 && len(b) != 7 && len(b) != 10) {
		return nil, fmt.Errorf("card UID must be 4, 7 or 10 bytes of hex, got %q", s)
	}
	return b, nil
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}
