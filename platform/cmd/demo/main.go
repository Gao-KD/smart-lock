// Command demo walks through every feature of the simulator, one scripted
// scenario per feature, against the running platform API (scripts/sim.sh).
//
//	go run ./cmd/demo all
//	go run ./cmd/demo card-unlock
//
// Each step prints what it does and checks the outcome on both sides: what
// the platform recorded and what the virtual lock (firmware) actually did.
package main

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"time"
)

var api string

type demo struct {
	name, title, desc string
	run               func() error
}

var demos = []demo{
	{"card-unlock", "校园卡刷卡开门", "给房间发一张校园卡（只读 UID），刷卡开门、自动落锁，陌生卡被拒绝", demoCardUnlock},
	{"remote-unlock", "远程开门", "平台 → MQTT → 网关 → 433MHz → 门锁，测量每一段耗时", demoRemoteUnlock},
	{"card-manage", "添加 / 删除卡、有效期", "加卡后能开，删卡后立即失效；带有效期的卡", demoCardManage},
	{"bulk", "整栋楼批量秒级下发", "全楼 360 把锁每间发一张卡，同时插入一条远程开门，看优先级", demoBulk},
	{"broadcast", "全楼广播：应急常开 / 封锁 / 恢复", "一条广播一个侦听周期覆盖整栋楼；封锁时刷卡无效但远程开门有效", demoBroadcast},
	{"anti-replay", "防重放 / 防篡改 / 重复指令去重", "攻击者重放或篡改截获的帧被拒绝；ACK 丢失后的重发只执行一次", demoAntiReplay},
	{"offline-retry", "离线与重试、离线补发", "射频遮挡时网关逐周期重发；管理类指令在锁重新上线后自动补发", demoOffline},
	{"status", "状态上报与查询", "心跳上报、主动查询单把锁、全楼巡检", demoStatus},
	{"low-battery", "低电量告警", "电压降到阈值以下上报告警，刷卡时有低电提示，换电池后恢复", demoLowBattery},
	{"fault", "电机故障", "电机卡死时门锁进入 FAULT 并把 HW_FAULT 返回给平台", demoFault},
}

func main() {
	flag.StringVar(&api, "api", "http://localhost:8080", "platform address")
	flag.Parse()
	if flag.NArg() == 0 {
		fmt.Print("用法: scripts/demo.sh <演示名> | all\n\n")
		for _, d := range demos {
			fmt.Printf("  %-14s %s：%s\n", d.name, d.title, d.desc)
		}
		return
	}
	if err := waitReady(); err != nil {
		fmt.Println("✗", err)
		os.Exit(1)
	}
	failed := 0
	for _, d := range demos {
		if flag.Arg(0) != "all" && flag.Arg(0) != d.name {
			continue
		}
		fmt.Printf("\n━━━━ %s ━━━━\n%s\n\n", d.title, d.desc)
		if err := d.run(); err != nil {
			failed++
			fmt.Printf("✗ 演示失败: %v\n", err)
		} else {
			fmt.Printf("✓ %s 演示通过\n", d.title)
		}
	}
	if failed > 0 {
		os.Exit(1)
	}
}

// ---- API helpers ----

func call(method, path string, body any, out any) error {
	var rd io.Reader
	if body != nil {
		b, _ := json.Marshal(body)
		rd = bytes.NewReader(b)
	}
	req, _ := http.NewRequest(method, api+path, rd)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	data, _ := io.ReadAll(resp.Body)
	if resp.StatusCode >= 300 {
		return fmt.Errorf("%s %s: %s", method, path, strings.TrimSpace(string(data)))
	}
	if out != nil {
		return json.Unmarshal(data, out)
	}
	return nil
}

type simLock struct {
	State     int  `json:"state"`
	BatteryMV int  `json:"battery_mv"`
	RF        bool `json:"rf"`
	Joined    bool `json:"joined"`
	Cards     int  `json:"cards"`
	BattLevel int  `json:"batt_level"`
	Ind       int  `json:"ind"`
	IndAgeMs  int  `json:"ind_age_ms"`
	Channel   int  `json:"channel"`
	Slot      int  `json:"slot"`
}

type lockView struct {
	Room      string    `json:"room"`
	EUI       string    `json:"eui"`
	Gateway   string    `json:"gateway"`
	Online    bool      `json:"online"`
	State     string    `json:"state"`
	BatteryMV int       `json:"battery_mv"`
	BattLevel int       `json:"batt_level"`
	Cards     int       `json:"cards"`
	LastSeen  time.Time `json:"last_seen"`
	Sim       *simLock  `json:"sim"`
}

type record struct {
	MsgID       uint32    `json:"msg_id"`
	Status      string    `json:"status"`
	Result      string    `json:"result"`
	Reason      string    `json:"reason"`
	Attempts    int       `json:"attempts"`
	Resends     int       `json:"resends"`
	QueuedAt    time.Time `json:"queued_at"`
	AcceptedAt  time.Time `json:"accepted_at"`
	DeliveredAt time.Time `json:"delivered_at"`
	ExecutedAt  time.Time `json:"executed_at"`
}

type event struct {
	ID   uint64 `json:"id"`
	Room string `json:"room"`
	Kind string `json:"kind"`
	Text string `json:"text"`
}

var states = []string{"LOCKED", "UNLOCKED", "LOCKDOWN", "EMERGENCY", "FAULT"}

func lock(room string) lockView {
	time.Sleep(350 * time.Millisecond) // the console polls locksim every 250 ms
	var l lockView
	_ = call("GET", "/api/locks/"+room, nil, &l)
	return l
}

func simState(room string) string {
	l := lock(room)
	if l.Sim == nil {
		return "?"
	}
	return states[l.Sim.State]
}

func lastEventID() uint64 {
	var s struct{ Events []event }
	_ = call("GET", "/api/state?locks=0", nil, &s)
	if len(s.Events) == 0 {
		return 0
	}
	return s.Events[len(s.Events)-1].ID
}

// waitEvent waits for an event of a room containing all substrings.
func waitEvent(since uint64, room string, timeout time.Duration, subs ...string) (event, bool) {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		var s struct{ Events []event }
		_ = call("GET", fmt.Sprintf("/api/state?locks=0&since=%d", since), nil, &s)
		for _, e := range s.Events {
			if room != "" && e.Room != room {
				continue
			}
			ok := true
			for _, sub := range subs {
				if !strings.Contains(e.Text, sub) {
					ok = false
				}
			}
			if ok {
				return e, true
			}
		}
		time.Sleep(150 * time.Millisecond)
	}
	return event{}, false
}

func submit(path string, body any) (uint32, error) {
	var r struct {
		MsgID uint32 `json:"msg_id"`
	}
	err := call("POST", path, body, &r)
	return r.MsgID, err
}

func waitCmd(id uint32, timeout time.Duration, final ...string) (record, error) {
	if len(final) == 0 {
		final = []string{"executed", "failed", "expired"}
	}
	deadline := time.Now().Add(timeout)
	var r record
	for time.Now().Before(deadline) {
		_ = call("GET", fmt.Sprintf("/api/commands/%d", id), nil, &r)
		for _, f := range final {
			if r.Status == f {
				return r, nil
			}
		}
		time.Sleep(50 * time.Millisecond)
	}
	return r, fmt.Errorf("指令 %d 超时，当前状态 %s", id, r.Status)
}

func simDo(room, action string, body any) error {
	return call("POST", "/api/sim/locks/"+room+"/"+action, body, nil)
}

func step(format string, a ...any) { fmt.Printf("• "+format+"\n", a...) }
func ok(format string, a ...any)   { fmt.Printf("  ✓ "+format+"\n", a...) }

func expect(cond bool, format string, a ...any) error {
	if !cond {
		return fmt.Errorf(format, a...)
	}
	ok(format, a...)
	return nil
}

func ms(a, b time.Time) int64 {
	if a.IsZero() || b.IsZero() {
		return -1
	}
	return b.Sub(a).Milliseconds()
}

func roomCard(room string) string {
	var n int
	fmt.Sscanf(room, "%d", &n)
	return fmt.Sprintf("C100%02X%02X", n/100, n%100) // same rule as console.RoomCardUID (building 1)
}

func waitReady() error {
	fmt.Print("等待模拟平台就绪")
	for i := 0; i < 60; i++ {
		var s struct {
			SimOK bool       `json:"sim_ok"`
			Locks []lockView `json:"locks"`
		}
		if call("GET", "/api/state", nil, &s) == nil && s.SimOK {
			joined := 0
			for _, l := range s.Locks {
				if l.Sim != nil && l.Sim.Joined {
					joined++
				}
			}
			if joined == len(s.Locks) && joined > 0 {
				fmt.Printf("：%d 把锁全部入网\n", joined)
				return nil
			}
		}
		fmt.Print(".")
		time.Sleep(time.Second)
	}
	return fmt.Errorf("平台或模拟器没有就绪，请先运行 scripts/sim.sh")
}

// ---- demos ----

func addCard(room, uid string, days int) error {
	id, err := submit("/api/locks/"+room+"/cards", map[string]any{"uid": uid, "user_id": 2024001, "valid_days": days})
	if err != nil {
		return err
	}
	r, err := waitCmd(id, 10*time.Second)
	if err != nil {
		return err
	}
	return expect(r.Status == "executed" && r.Result == "OK", "门锁 %s 已存入卡 %s（指令 %d，%d ms）", room, uid, id, ms(r.QueuedAt, r.ExecutedAt))
}

func swipe(room, uid string) (string, error) {
	since := lastEventID()
	if err := simDo(room, "swipe", map[string]string{"uid": uid}); err != nil {
		return "", err
	}
	e, found := waitEvent(since, room, 5*time.Second, "刷卡 "+strings.ToUpper(uid))
	if !found {
		return "", fmt.Errorf("平台没有收到 %s 的开门记录上报", room)
	}
	return e.Text, nil
}

func demoCardUnlock() error {
	room, uid := "101", roomCard("101")
	step("平台给 %s 下发校园卡 %s（P2 管理类指令，端到端 AES-CCM 加密）", room, uid)
	if err := addCard(room, uid, 0); err != nil {
		return err
	}
	step("学生在门口刷卡")
	txt, err := swipe(room, uid)
	if err != nil {
		return err
	}
	if err := expect(strings.Contains(txt, "开门"), "平台收到开门记录：%s", txt); err != nil {
		return err
	}
	if err := expect(simState(room) == "UNLOCKED", "门锁离合啮合，状态 UNLOCKED"); err != nil {
		return err
	}
	step("等待默认 5 秒开门时间结束")
	time.Sleep(5500 * time.Millisecond)
	if err := expect(simState(room) == "LOCKED", "自动落锁，状态 LOCKED"); err != nil {
		return err
	}
	step("刷一张未授权的卡 DEADBEEF")
	txt, err = swipe(room, "DEADBEEF")
	if err != nil {
		return err
	}
	return expect(strings.Contains(txt, "拒绝") && simState(room) == "LOCKED", "被拒绝且门保持上锁：%s", txt)
}

func demoRemoteUnlock() error {
	for _, room := range []string{"203", "718", "1130"} {
		step("远程开门 %s（P1 交互类，有效期 30 秒）", room)
		id, err := submit("/api/locks/"+room+"/unlock", map[string]any{"duration_s": 3})
		if err != nil {
			return err
		}
		r, err := waitCmd(id, 10*time.Second)
		if err != nil {
			return err
		}
		l := lock(room)
		fmt.Printf("    平台→网关 %d ms，空口送达(ACK) %d ms，门锁执行结果回到平台 %d ms（锁在信道 %d 时隙 %d 侦听）\n",
			ms(r.QueuedAt, r.AcceptedAt), ms(r.QueuedAt, r.DeliveredAt), ms(r.QueuedAt, r.ExecutedAt), l.Sim.Channel, l.Sim.Slot)
		if err := expect(r.Result == "OK" && states[l.Sim.State] == "UNLOCKED", "%s 已开门", room); err != nil {
			return err
		}
	}
	fmt.Println("    说明：锁每 1 秒只醒来一次（在自己的时隙），所以单条指令耗时 ≈ 等到该锁的时隙，最长约 1 个周期。")
	return nil
}

func demoCardManage() error {
	room, uid := "305", "A1B2C3D4"
	step("给 %s 加一张 7 天有效期的临时卡 %s", room, uid)
	if err := addCard(room, uid, 7); err != nil {
		return err
	}
	txt, err := swipe(room, uid)
	if err != nil {
		return err
	}
	if err := expect(strings.Contains(txt, "开门"), "刷卡开门：%s", txt); err != nil {
		return err
	}
	step("平台删除这张卡")
	var r struct {
		MsgID uint32 `json:"msg_id"`
	}
	if err := call("DELETE", "/api/locks/"+room+"/cards/"+uid, nil, &r); err != nil {
		return err
	}
	rec, err := waitCmd(r.MsgID, 10*time.Second)
	if err != nil {
		return err
	}
	if err := expect(rec.Result == "OK", "删卡指令已执行（%d ms）", ms(rec.QueuedAt, rec.ExecutedAt)); err != nil {
		return err
	}
	time.Sleep(5500 * time.Millisecond) // let the door relock
	txt, err = swipe(room, uid)
	if err != nil {
		return err
	}
	return expect(strings.Contains(txt, "拒绝"), "同一张卡再刷被拒绝：%s", txt)
}

type jobView struct {
	Name       string         `json:"name"`
	Done       bool           `json:"done"`
	LocksDone  int            `json:"locks_done"`
	LocksTotal int            `json:"locks_total"`
	ElapsedMs  int64          `json:"elapsed_ms"`
	P50Ms      int64          `json:"p50_ms"`
	MaxMs      int64          `json:"max_ms"`
	Results    map[string]int `json:"results"`
}

func runJob(action string, timeout time.Duration, during func()) (jobView, error) {
	var r struct {
		Job uint64 `json:"job"`
	}
	if err := call("POST", "/api/buildings/1/"+action, nil, &r); err != nil {
		return jobView{}, err
	}
	if during != nil {
		during()
	}
	deadline := time.Now().Add(timeout)
	var j jobView
	last := -1
	for time.Now().Before(deadline) {
		_ = call("GET", fmt.Sprintf("/api/jobs/%d", r.Job), nil, &j)
		if j.LocksDone != last {
			fmt.Printf("    %.1fs: %d/%d 把锁已执行\n", float64(j.ElapsedMs)/1000, j.LocksDone, j.LocksTotal)
			last = j.LocksDone
		}
		if j.Done {
			return j, nil
		}
		time.Sleep(250 * time.Millisecond)
	}
	return j, fmt.Errorf("%s 超时：%d/%d", j.Name, j.LocksDone, j.LocksTotal)
}

func demoBulk() error {
	step("整栋楼批量发卡：每间房一条加卡指令（P2，网关按 信道×时隙 并行下发）")
	var urgent uint32
	j, err := runJob("issue-cards", 30*time.Second, func() {
		time.Sleep(100 * time.Millisecond)
		urgent, _ = submit("/api/locks/1015/unlock", map[string]any{"duration_s": 3})
		fmt.Println("    （批量下发过程中，插入一条 1015 的远程开门）")
	})
	if err != nil {
		return err
	}
	if err := expect(j.Results["OK"] == j.LocksTotal, "%d 把锁全部加卡成功，总耗时 %.2f 秒（单锁 P50 %d ms，最慢 %d ms）",
		j.LocksTotal, float64(j.ElapsedMs)/1000, j.P50Ms, j.MaxMs); err != nil {
		return err
	}
	r, err := waitCmd(urgent, 5*time.Second)
	if err != nil {
		return err
	}
	if err := expect(r.Result == "OK", "插队的远程开门 %d ms 完成：P1 优先于 P2，管理类最多占用每个信道 70%% 的时隙", ms(r.QueuedAt, r.ExecutedAt)); err != nil {
		return err
	}
	step("整栋楼批量删卡")
	j, err = runJob("revoke-cards", 30*time.Second, nil)
	if err != nil {
		return err
	}
	return expect(j.Done, "删卡完成，总耗时 %.2f 秒", float64(j.ElapsedMs)/1000)
}

func countStates() map[string]int {
	time.Sleep(350 * time.Millisecond)
	var s struct {
		Locks []lockView `json:"locks"`
	}
	_ = call("GET", "/api/state", nil, &s)
	m := map[string]int{}
	for _, l := range s.Locks {
		if l.Sim != nil {
			m[states[l.Sim.State]]++
		}
	}
	return m
}

func demoBroadcast() error {
	step("消防联动：全楼应急常开（P0 广播，每台网关只发一帧，在所有 信道×时隙 上各发一次）")
	j, err := runJob("emergency-open", 10*time.Second, nil)
	if err != nil {
		return err
	}
	c := countStates()
	if err := expect(c["EMERGENCY"] == j.LocksTotal, "%d 把锁在 %.2f 秒内全部进入应急常开", c["EMERGENCY"], float64(j.ElapsedMs)/1000); err != nil {
		return err
	}
	step("恢复正常")
	if _, err := runJob("release", 10*time.Second, nil); err != nil {
		return err
	}
	if err := expect(countStates()["LOCKED"] == j.LocksTotal, "全部回到 LOCKED"); err != nil {
		return err
	}
	step("全楼封锁（例如考试周 / 突发事件）")
	if _, err := runJob("lockdown", 10*time.Second, nil); err != nil {
		return err
	}
	room, uid := "612", roomCard("612")
	if err := addCard(room, uid, 0); err != nil {
		return err
	}
	txt, err := swipe(room, uid)
	if err != nil {
		return err
	}
	if err := expect(strings.Contains(txt, "拒绝"), "封锁期间合法卡也被拒绝：%s", txt); err != nil {
		return err
	}
	id, _ := submit("/api/locks/"+room+"/unlock", map[string]any{"duration_s": 3})
	r, err := waitCmd(id, 10*time.Second)
	if err != nil {
		return err
	}
	if err := expect(r.Result == "OK", "宿管远程开门仍然有效（开一次后继续封锁）"); err != nil {
		return err
	}
	step("解除封锁")
	_, err = runJob("release", 10*time.Second, nil)
	return err
}

func demoAntiReplay() error {
	room := "410"
	step("远程开门 %s，攻击者在空中截获了这一帧", room)
	id, _ := submit("/api/locks/"+room+"/unlock", map[string]any{"duration_s": 2})
	if _, err := waitCmd(id, 10*time.Second); err != nil {
		return err
	}
	time.Sleep(2500 * time.Millisecond)
	if err := expect(simState(room) == "LOCKED", "门已自动落锁"); err != nil {
		return err
	}
	step("攻击者原样重放截获的帧")
	since := lastEventID()
	if err := simDo(room, "replay", nil); err != nil {
		return err
	}
	e, found := waitEvent(since, room, 3*time.Second, "防重放")
	if err := expect(found && simState(room) == "LOCKED", "门锁拒绝：%s", e.Text); err != nil {
		return err
	}
	step("攻击者篡改帧内容后重发（并修正 CRC）")
	since = lastEventID()
	if err := simDo(room, "tamper", nil); err != nil {
		return err
	}
	e, found = waitEvent(since, room, 3*time.Second, "MIC")
	if err := expect(found && simState(room) == "LOCKED", "门锁拒绝：%s", e.Text); err != nil {
		return err
	}
	step("模拟门锁的 ACK 在空中丢失：网关会重发同一条指令")
	if err := simDo(room, "dropack", map[string]int{"n": 1}); err != nil {
		return err
	}
	since = lastEventID()
	id, _ = submit("/api/locks/"+room+"/unlock", map[string]any{"duration_s": 2})
	r, err := waitCmd(id, 10*time.Second)
	if err != nil {
		return err
	}
	_, dup := waitEvent(since, room, 4*time.Second, "DUPLICATE")
	for i := 0; i < 20 && r.Attempts < 2; i++ { // the delivered ACK can arrive after the result
		time.Sleep(150 * time.Millisecond)
		r, _ = waitCmd(id, time.Second)
	}
	return expect(r.Attempts >= 2 && dup, "第 %d 次发送才收到 ACK，门锁按指令号去重，重发的那次回复 DUPLICATE，只开了一次门", r.Attempts)
}

func demoOffline() error {
	room := "520"
	step("%s 射频被遮挡（离线），平台下发远程开门", room)
	if err := simDo(room, "rf", map[string]bool{"on": false}); err != nil {
		return err
	}
	id, _ := submit("/api/locks/"+room+"/unlock", map[string]any{"duration_s": 3})
	time.Sleep(2500 * time.Millisecond)
	r, _ := waitCmd(id, 100*time.Millisecond)
	fmt.Printf("    2.5 秒后：状态 %s，已发送 %d 次（%s）\n", r.Status, r.Attempts, r.Reason)
	step("恢复射频")
	if err := simDo(room, "rf", map[string]bool{"on": true}); err != nil {
		return err
	}
	r, err := waitCmd(id, 10*time.Second)
	if err != nil {
		return err
	}
	if err := expect(r.Result == "OK", "网关下一个周期重发成功：共发送 %d 次，总耗时 %d ms", r.Attempts, ms(r.QueuedAt, r.ExecutedAt)); err != nil {
		return err
	}
	time.Sleep(3500 * time.Millisecond)

	step("再次离线，同时下发远程开门(P1)和加卡(P2)，一直不恢复")
	_ = simDo(room, "rf", map[string]bool{"on": false})
	u, _ := submit("/api/locks/"+room+"/unlock", nil)
	c, _ := submit("/api/locks/"+room+"/cards", map[string]any{"uid": "0BADCAFE", "user_id": 7})
	ru, _ := waitCmd(u, 12*time.Second, "failed", "expired")
	rc, _ := waitCmd(c, 12*time.Second, "failed", "expired")
	if err := expect(ru.Status == "failed" && rc.Status == "failed", "网关重发 %d 次后放弃，两条都报告失败", ru.Attempts); err != nil {
		return err
	}
	fmt.Printf("    加卡指令：%s\n", rc.Reason)
	step("锁恢复在线（重新上报心跳）")
	_ = simDo(room, "rf", map[string]bool{"on": true})
	rc, err = waitCmd(c, 10*time.Second, "executed")
	if err != nil {
		return err
	}
	if err := expect(rc.Result == "OK" && rc.Resends == 1, "加卡指令自动补发并执行成功（同一指令号，锁端去重保证不重复执行）"); err != nil {
		return err
	}
	ru, _ = waitCmd(u, 100*time.Millisecond)
	return expect(ru.Status == "failed", "远程开门不补发（人早已离开，过时的开门指令不应再执行）")
}

func demoStatus() error {
	room := "615"
	step("门锁每 30 秒发一次心跳（状态、电量、卡数），平台据此判断在线")
	l := lock(room)
	if err := expect(l.Online, "%s 在线，最近上报 %s 前", room, time.Since(l.LastSeen).Round(time.Second)); err != nil {
		return err
	}
	step("主动查询 %s 的状态", room)
	id, _ := submit("/api/locks/"+room+"/status", nil)
	r, err := waitCmd(id, 10*time.Second)
	if err != nil {
		return err
	}
	l = lock(room)
	if err := expect(r.Result == "OK" && l.BatteryMV > 0, "回复：状态 %s，电池 %d mV，卡 %d 张（%d ms）", l.State, l.BatteryMV, l.Cards, ms(r.QueuedAt, r.ExecutedAt)); err != nil {
		return err
	}
	step("全楼巡检：逐把查询状态")
	j, err := runJob("status-all", 30*time.Second, nil)
	if err != nil {
		return err
	}
	return expect(j.Done, "%d 把锁巡检完成，耗时 %.2f 秒", j.LocksTotal, float64(j.ElapsedMs)/1000)
}

func demoLowBattery() error {
	room := "708"
	uid := roomCard(room)
	_ = simDo(room, "battery", map[string]int{"mv": 5800}) // start from fresh batteries
	if err := addCard(room, uid, 0); err != nil {
		return err
	}
	step("电池电压降到 4300 mV（4 节 AA，低电阈值 4400 mV）")
	since := lastEventID()
	if err := simDo(room, "battery", map[string]int{"mv": 4300}); err != nil {
		return err
	}
	e, found := waitEvent(since, room, 5*time.Second, "电池告警")
	if err := expect(found, "平台收到：%s", e.Text); err != nil {
		return err
	}
	if err := expect(lock(room).BattLevel == 1, "平台把 %s 标记为低电量", room); err != nil {
		return err
	}
	step("学生刷卡：照常开门，但指示灯/蜂鸣器给出低电提示")
	if _, err := swipe(room, uid); err != nil {
		return err
	}
	l := lock(room)
	if err := expect(l.Sim.Ind == 2, "门锁给出低电指示（HAL_IND_LOW_BATT）"); err != nil {
		return err
	}
	step("继续降到 3900 mV")
	since = lastEventID()
	_ = simDo(room, "battery", map[string]int{"mv": 3900})
	e, found = waitEvent(since, room, 5*time.Second, "电池告警", "严重低电")
	if err := expect(found, "平台收到：%s", e.Text); err != nil {
		return err
	}
	step("更换新电池 6000 mV")
	since = lastEventID()
	_ = simDo(room, "battery", map[string]int{"mv": 6000})
	e, found = waitEvent(since, room, 5*time.Second, "恢复正常")
	return expect(found, "平台收到：%s", e.Text)
}

func demoFault() error {
	room := "903"
	step("模拟 %s 电机卡死（连续两次驱动失败），平台远程开门", room)
	_ = simDo(room, "jam", map[string]int{"n": 2})
	id, _ := submit("/api/locks/"+room+"/unlock", nil)
	r, err := waitCmd(id, 10*time.Second)
	if err != nil {
		return err
	}
	if err := expect(r.Result == "HW_FAULT" && simState(room) == "FAULT", "门锁回复 HW_FAULT，进入 FAULT 状态"); err != nil {
		return err
	}
	step("故障排除后再次远程开门（下一条指令会重新驱动电机）")
	id, _ = submit("/api/locks/"+room+"/unlock", map[string]any{"duration_s": 2})
	r, err = waitCmd(id, 10*time.Second)
	if err != nil {
		return err
	}
	return expect(r.Result == "OK", "恢复正常开门")
}
