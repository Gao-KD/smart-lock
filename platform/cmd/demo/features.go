package main

import (
	"fmt"
	"strings"
	"time"
)

// Demos of the lock features modelled on mainstream smart-lock platforms
// (passwords, offline passwords, members, alarms, double lock, unlock
// requests, configuration) and of the connection strategy (BLE fallback,
// record recovery, uplink ACK/retry, ACK time sync, fake-wake-up guard).

var beijing = time.FixedZone("CST", 8*3600)

// simReply runs a simulator action and returns locksim's reply ("OK <result> <state>").
func simReply(room, action string, body any) (string, error) {
	var r struct {
		Reply string `json:"reply"`
	}
	err := call("POST", "/api/sim/locks/"+room+"/"+action, body, &r)
	return r.Reply, err
}

// result names follow slp_result_t
var resultNames = []string{"OK", "EXPIRED", "DUPLICATE", "DENIED", "BAD_PARAM", "FULL", "HW_FAULT", "UNKNOWN",
	"DOUBLE_LOCKED", "LOCKED_OUT", "FROZEN", "OUT_OF_SCHEDULE", "NEED_REQUEST"}

// atDoor performs a door action and returns the lock's result name.
func atDoor(room, action string, body any) (string, error) {
	rep, err := simReply(room, action, body)
	if err != nil {
		return "", err
	}
	var code int
	if _, err := fmt.Sscanf(rep, "OK %d", &code); err != nil || code < 0 || code >= len(resultNames) {
		return rep, nil
	}
	return resultNames[code], nil
}

func keypad(room, digits string) (string, error) {
	return atDoor(room, "keypad", map[string]string{"digits": digits})
}

func waitLocked(room string) {
	for i := 0; i < 40 && simState(room) != "LOCKED"; i++ {
		time.Sleep(200 * time.Millisecond)
	}
}

func cmdOK(path string, body any, what string) error {
	id, err := submit(path, body)
	if err != nil {
		return err
	}
	r, err := waitCmd(id, 15*time.Second)
	if err != nil {
		return err
	}
	return expect(r.Result == "OK", "%s（指令 %d，%d ms）", what, id, ms(r.QueuedAt, r.ExecutedAt))
}

func cmdResult(path string, body any) (record, error) {
	id, err := submit(path, body)
	if err != nil {
		return record{}, err
	}
	return waitCmd(id, 15*time.Second)
}

func demoPassword() error {
	room := "211"
	step("给 %s 下发住户密码 246810（用户 3001）", room)
	if err := cmdOK("/api/locks/"+room+"/passwords", map[string]any{"pin": "246810", "user_id": 3001}, "门锁已存入密码"); err != nil {
		return err
	}
	since := lastEventID()
	r, err := keypad(room, "246810")
	if err != nil {
		return err
	}
	if err := expect(r == "OK", "输入 246810# 开门"); err != nil {
		return err
	}
	e, found := waitEvent(since, room, 5*time.Second, "密码", "开门")
	if err := expect(found, "平台收到开门记录：%s（记录不含密码数字）", e.Text); err != nil {
		return err
	}
	waitLocked(room)
	step("虚位密码：在密码前后加任意数字，防止旁人偷看")
	if r, _ = keypad(room, "7724681093"); expect(r == "OK", "输入 77[246810]93# 也能开门") != nil {
		return fmt.Errorf("虚位密码被拒绝: %s", r)
	}
	waitLocked(room)

	step("访客一次性密码 135790（用一次自动作废）")
	if err := cmdOK("/api/locks/"+room+"/passwords", map[string]any{"pin": "135790", "user_id": 9001, "uses": 1, "valid_hours": 24}, "一次性密码已下发"); err != nil {
		return err
	}
	if r, _ = keypad(room, "135790"); r != "OK" {
		return fmt.Errorf("一次性密码第一次被拒绝: %s", r)
	}
	ok("第一次输入开门")
	waitLocked(room)
	r, _ = keypad(room, "135790")
	if err := expect(r == "DENIED", "第二次输入被拒绝（%s），门锁里已经删掉了", r); err != nil {
		return err
	}

	now := time.Now().In(beijing)
	today := int(now.Weekday()+6)%7 + 1 // 1 = Monday
	other := today%7 + 1
	step("保洁周期密码：只在每周%s 全天有效（今天是周%s）", weekday(other), weekday(today))
	if err := cmdOK("/api/locks/"+room+"/passwords", map[string]any{"pin": "808080", "user_id": 5001, "days": []int{other}, "start": "00:00", "end": "24:00"}, "周期密码已下发"); err != nil {
		return err
	}
	r, _ = keypad(room, "808080")
	if err := expect(r == "OUT_OF_SCHEDULE", "今天输入被拒绝：%s", r); err != nil {
		return err
	}
	step("改成每周%s %s-%s（包含现在）", weekday(today), now.Add(-time.Hour).Format("15:04"), "24:00")
	start := now.Add(-time.Hour)
	if start.Day() != now.Day() {
		start = time.Date(now.Year(), now.Month(), now.Day(), 0, 0, 0, 0, beijing)
	}
	if err := cmdOK("/api/locks/"+room+"/passwords", map[string]any{"pin": "808080", "user_id": 5001, "days": []int{today}, "start": start.Format("15:04"), "end": "24:00"}, "周期密码已更新（同一密码覆盖）"); err != nil {
		return err
	}
	r, _ = keypad(room, "808080")
	return expect(r == "OK", "现在可以开门")
}

func weekday(d int) string { return []string{"", "一", "二", "三", "四", "五", "六", "日"}[d] }

func demoOfflinePassword() error {
	room := "322"
	step("%s 和网关之间的射频中断（锁完全离线），宿管在平台上生成离线密码", room)
	if err := simDo(room, "rf", map[string]bool{"on": false}); err != nil {
		return err
	}
	defer simDo(room, "rf", map[string]bool{"on": true})
	var p struct {
		Password string `json:"password"`
		Desc     string `json:"desc"`
	}
	if err := call("POST", "/api/locks/"+room+"/offline-password", map[string]string{"kind": "once"}, &p); err != nil {
		return err
	}
	fmt.Printf("    一次性离线密码 %s（%s）\n", p.Password, p.Desc)
	r, _ := keypad(room, p.Password)
	if err := expect(r == "OK", "门锁本地校验通过，开门（没有任何网络通信）"); err != nil {
		return err
	}
	waitLocked(room)
	r, _ = keypad(room, p.Password)
	if err := expect(r == "EXPIRED", "同一个一次性密码再输入被拒绝（%s）", r); err != nil {
		return err
	}
	var t struct {
		Password string `json:"password"`
		Desc     string `json:"desc"`
	}
	if err := call("POST", "/api/locks/"+room+"/offline-password", map[string]any{"kind": "timed", "hours": 8}, &t); err != nil {
		return err
	}
	fmt.Printf("    限时离线密码 %s（%s，例如给维修师傅）\n", t.Password, t.Desc)
	for i := 1; i <= 2; i++ {
		if r, _ = keypad(room, t.Password); r != "OK" {
			return fmt.Errorf("限时密码第 %d 次被拒绝: %s", i, r)
		}
		ok("第 %d 次输入开门", i)
		waitLocked(room)
	}
	var c struct {
		Password string `json:"password"`
	}
	if err := call("POST", "/api/locks/"+room+"/offline-password", map[string]string{"kind": "clear"}, &c); err != nil {
		return err
	}
	step("维修结束，在门口输入清除码 %s", c.Password)
	if r, _ = keypad(room, c.Password); r != "OK" {
		return fmt.Errorf("清除码被拒绝: %s", r)
	}
	r, _ = keypad(room, t.Password)
	if err := expect(r == "EXPIRED", "之前的限时密码已作废（%s）", r); err != nil {
		return err
	}
	step("恢复射频：离线期间的开门记录自动上报平台")
	since := lastEventID()
	_ = simDo(room, "rf", map[string]bool{"on": true})
	e, found := waitEvent(since, room, 15*time.Second, "离线密码(限时)", "开门")
	return expect(found, "平台收到：%s", e.Text)
}

func demoMember() error {
	room, uid, user := "416", "A0B0C0D0", 3002
	step("公租房住户（用户 %d）在 %s 有一张卡和一个密码", user, room)
	if err := cmdOK("/api/locks/"+room+"/cards", map[string]any{"uid": uid, "user_id": user}, "卡已下发"); err != nil {
		return err
	}
	if err := cmdOK("/api/locks/"+room+"/passwords", map[string]any{"pin": "416416", "user_id": user}, "密码已下发"); err != nil {
		return err
	}
	step("欠费：平台冻结该成员（一条指令，按用户冻结全部凭证）")
	if err := cmdOK(fmt.Sprintf("/api/locks/%s/users/%d/freeze", room, user), map[string]bool{"freeze": true}, "成员已冻结"); err != nil {
		return err
	}
	r1, _ := atDoor(room, "swipe", map[string]string{"uid": uid})
	r2, _ := keypad(room, "416416")
	if err := expect(r1 == "FROZEN" && r2 == "FROZEN", "刷卡和密码都被拒绝（%s / %s）", r1, r2); err != nil {
		return err
	}
	step("缴费后解冻")
	if err := cmdOK(fmt.Sprintf("/api/locks/%s/users/%d/freeze", room, user), map[string]bool{"freeze": false}, "成员已解冻"); err != nil {
		return err
	}
	if r, _ := atDoor(room, "swipe", map[string]string{"uid": uid}); r != "OK" {
		return fmt.Errorf("解冻后刷卡被拒绝: %s", r)
	}
	ok("刷卡开门")
	waitLocked(room)
	step("退租/退宿：删除该成员")
	before := lock(room).Sim.Cards
	var del struct {
		MsgID uint32 `json:"msg_id"`
	}
	if err := call("DELETE", fmt.Sprintf("/api/locks/%s/users/%d", room, user), nil, &del); err != nil {
		return err
	}
	if _, err := waitCmd(del.MsgID, 15*time.Second); err != nil {
		return err
	}
	after := lock(room).Sim.Cards
	if err := expect(before-after == 2, "门锁里该成员的 2 个凭证全部删除（%d → %d）", before, after); err != nil {
		return err
	}
	r, _ := atDoor(room, "swipe", map[string]string{"uid": uid})
	return expect(r == "DENIED", "原来的卡已打不开（%s）", r)
}

func demoAlarms() error {
	room := "527"
	uid := roomCard(room)
	if err := addCard(room, uid, 0); err != nil {
		return err
	}
	step("有人在 %s 门口连续刷 5 张陌生卡", room)
	since := lastEventID()
	for i := 0; i < 5; i++ {
		_, _ = atDoor(room, "swipe", map[string]string{"uid": fmt.Sprintf("BAD0000%d", i)})
	}
	e, found := waitEvent(since, room, 5*time.Second, "多次试错")
	if err := expect(found, "平台收到：%s", e.Text); err != nil {
		return err
	}
	r, _ := atDoor(room, "swipe", map[string]string{"uid": uid})
	if err := expect(r == "LOCKED_OUT", "冻结期间（默认 3 分钟）合法卡也刷不开（%s）", r); err != nil {
		return err
	}
	if err := cmdOK("/api/locks/"+room+"/unlock", map[string]any{"duration_s": 2}, "宿管远程开门不受影响"); err != nil {
		return err
	}
	waitLocked(room)

	room = "528"
	step("胁迫密码：住户被人逼迫开门时输入 %s 的“胁迫密码”", room)
	if err := cmdOK("/api/locks/"+room+"/passwords", map[string]any{"pin": "99887766", "user_id": 3003, "duress": true}, "胁迫密码已下发"); err != nil {
		return err
	}
	since = lastEventID()
	r, _ = keypad(room, "99887766")
	if err := expect(r == "OK", "门照常打开，门口看不出任何异常"); err != nil {
		return err
	}
	e, found = waitEvent(since, room, 5*time.Second, "胁迫")
	if err := expect(found, "平台收到静默告警：%s", e.Text); err != nil {
		return err
	}
	step("撬面板、用机械钥匙开门")
	since = lastEventID()
	_, _ = simReply(room, "pry", nil)
	e, found = waitEvent(since, room, 5*time.Second, "告警: 防撬")
	if err := expect(found, "平台收到：%s", e.Text); err != nil {
		return err
	}
	since = lastEventID()
	_, _ = simReply(room, "key", nil)
	e, found = waitEvent(since, room, 5*time.Second, "告警: 机械钥匙")
	if err := expect(found, "平台收到：%s", e.Text); err != nil {
		return err
	}
	step("门未关好：把门未关告警时间设为 3 秒，推开门不关")
	if err := cmdOK("/api/locks/"+room+"/config", map[string]any{"door_alarm_s": 3}, "参数已下发"); err != nil {
		return err
	}
	since = lastEventID()
	_, _ = simReply(room, "door", map[string]bool{"open": true})
	e, found = waitEvent(since, room, 8*time.Second, "门未关好")
	if err := expect(found, "约 3 秒后平台收到：%s", e.Text); err != nil {
		return err
	}
	_, _ = simReply(room, "door", map[string]bool{"open": false})
	return cmdOK("/api/locks/"+room+"/config", map[string]any{"door_alarm_s": 30}, "恢复门未关告警时间")
}

func demoDoubleLock() error {
	room := "624"
	uid := roomCard(room)
	if err := addCard(room, uid, 0); err != nil {
		return err
	}
	step("住户在室内拧下反锁旋钮")
	since := lastEventID()
	_, _ = simReply(room, "dlock", map[string]bool{"on": true})
	e, found := waitEvent(since, room, 5*time.Second, "室内已反锁")
	if err := expect(found && lock(room).DoubleLock, "平台收到：%s", e.Text); err != nil {
		return err
	}
	r, _ := atDoor(room, "swipe", map[string]string{"uid": uid})
	if err := expect(r == "DOUBLE_LOCKED", "门外刷卡打不开（%s），也不计入试错次数", r); err != nil {
		return err
	}
	rec, err := cmdResult("/api/locks/"+room+"/unlock", map[string]any{"duration_s": 3})
	if err != nil {
		return err
	}
	if err := expect(rec.Result == "DOUBLE_LOCKED", "远程开门也返回 DOUBLE_LOCKED（平台可提示“室内已反锁”）"); err != nil {
		return err
	}
	_, _ = simReply(room, "dlock", map[string]bool{"on": false})
	step("解除反锁后刷卡进门，进门后随手关门")
	if r, _ = atDoor(room, "swipe", map[string]string{"uid": uid}); r != "OK" {
		return fmt.Errorf("刷卡失败: %s", r)
	}
	_, _ = simReply(room, "door", map[string]bool{"open": true})
	time.Sleep(time.Second)
	t0 := time.Now()
	_, _ = simReply(room, "door", map[string]bool{"open": false})
	return expect(simState(room) == "LOCKED", "门磁检测到关门立即落锁（%d ms，不用等 5 秒）", time.Since(t0).Milliseconds())
}

func demoUnlockRequest() error {
	room := "725"
	step("%s 的住户关闭“允许远程开门”（例如公租房住户不希望管理方随时能开门）", room)
	if err := cmdOK("/api/locks/"+room+"/config", map[string]any{"remote_allowed": false}, "参数已下发"); err != nil {
		return err
	}
	rec, err := cmdResult("/api/locks/"+room+"/unlock", map[string]any{"duration_s": 3})
	if err != nil {
		return err
	}
	if err := expect(rec.Result == "NEED_REQUEST", "平台直接远程开门被门锁拒绝（%s）", rec.Result); err != nil {
		return err
	}
	step("访客在门口按呼叫键")
	since := lastEventID()
	_, _ = simReply(room, "doorbell", nil)
	e, found := waitEvent(since, room, 5*time.Second, "请求开门")
	if err := expect(found && lock(room).Request != nil, "平台/住户 App 收到：%s", e.Text); err != nil {
		return err
	}
	step("住户在 App 上点“开门”（批准这一次请求）")
	if err := cmdOK("/api/locks/"+room+"/requests/approve", nil, "门锁核对请求号后开门"); err != nil {
		return err
	}
	if err := expect(simState(room) == "UNLOCKED", "门已打开"); err != nil {
		return err
	}
	return cmdOK("/api/locks/"+room+"/config", map[string]any{"remote_allowed": true}, "恢复允许远程开门")
}

func countAutolock(v int) int {
	time.Sleep(350 * time.Millisecond)
	var s struct {
		Locks []lockView `json:"locks"`
	}
	_ = call("GET", "/api/state", nil, &s)
	n := 0
	for _, l := range s.Locks {
		if l.Sim != nil && l.Sim.AutolockS == v {
			n++
		}
	}
	return n
}

func demoConfig() error {
	step("整栋楼统一把自动落锁时间改为 8 秒、试错 5 次冻结 180 秒（一次批量下发）")
	var r struct {
		Job uint64 `json:"job"`
	}
	if err := call("POST", "/api/buildings/1/config", map[string]any{"autolock_s": 8, "wrong_limit": 5, "lockout_s": 180}, &r); err != nil {
		return err
	}
	j, err := waitJob(r.Job, 30*time.Second)
	if err != nil {
		return err
	}
	if err := expect(j.Results["OK"] == j.LocksTotal && countAutolock(8) == j.LocksTotal, "%d 把锁全部生效，总耗时 %.2f 秒", j.LocksTotal, float64(j.ElapsedMs)/1000); err != nil {
		return err
	}
	step("恢复 5 秒")
	if err := call("POST", "/api/buildings/1/config", map[string]any{"autolock_s": 5}, &r); err != nil {
		return err
	}
	if _, err := waitJob(r.Job, 30*time.Second); err != nil {
		return err
	}
	return expect(countAutolock(5) > 0, "已恢复")
}

func waitJob(id uint64, timeout time.Duration) (jobView, error) {
	deadline := time.Now().Add(timeout)
	var j jobView
	for time.Now().Before(deadline) {
		_ = call("GET", fmt.Sprintf("/api/jobs/%d", id), nil, &j)
		if j.Done {
			return j, nil
		}
		time.Sleep(250 * time.Millisecond)
	}
	return j, fmt.Errorf("%s 超时：%d/%d", j.Name, j.LocksDone, j.LocksTotal)
}

func demoBLE() error {
	room := "826"
	step("宿管先把 %s 封锁（在线时），然后这把锁和网关之间断网", room)
	if err := cmdOK("/api/locks/"+room+"/lockdown", nil, "已封锁"); err != nil {
		return err
	}
	_ = simDo(room, "rf", map[string]bool{"on": false})
	defer simDo(room, "rf", map[string]bool{"on": true})
	var stu, adm struct {
		Token string `json:"token"`
	}
	if err := call("POST", "/api/locks/"+room+"/ble-key", map[string]any{"user_id": 2024835, "hours": 24}, &stu); err != nil {
		return err
	}
	if err := call("POST", "/api/locks/"+room+"/ble-key", map[string]any{"user_id": 1, "hours": 2, "admin": true}, &adm); err != nil {
		return err
	}
	fmt.Printf("    平台事先签发到手机的蓝牙钥匙（17 字节，用门锁密钥签名）：学生 %s…，宿管 %s…\n", stu.Token[:12], adm.Token[:12])
	r, _ := atDoor(room, "ble", map[string]string{"token": stu.Token})
	if err := expect(r == "DENIED", "封锁期间学生手机开不了（%s）", r); err != nil {
		return err
	}
	r, _ = atDoor(room, "ble", map[string]string{"token": adm.Token})
	if err := expect(r == "OK", "宿管手机蓝牙开门成功（不经过网关和平台）"); err != nil {
		return err
	}
	forged := []byte(adm.Token)
	forged[1] ^= 0x01 // flips one bit of the user id: '0'<->'1', 'A'<->'@' would not be hex, so pick a digit
	if forged[1] < '0' || forged[1] > '9' {
		forged[1] = '0' + (forged[1] & 0x07)
	}
	r, _ = atDoor(room, "ble", map[string]string{"token": string(forged)})
	if err := expect(r == "DENIED", "篡改过的钥匙被拒绝（%s）", r); err != nil {
		return err
	}
	time.Sleep(6 * time.Second) // stay offline long enough to count as a lost connection
	step("网络恢复")
	since := lastEventID()
	_ = simDo(room, "rf", map[string]bool{"on": true})
	e, found := waitEvent(since, room, 15*time.Second, "手机蓝牙", "开门")
	if err := expect(found, "离线期间的蓝牙开门记录补传到平台：%s", e.Text); err != nil {
		return err
	}
	return cmdOK("/api/locks/"+room+"/release", nil, "解除封锁")
}

func demoOfflineRecords() error {
	room := "927"
	uid := roomCard(room)
	if err := addCard(room, uid, 0); err != nil {
		return err
	}
	before := lock(room)
	step("%s 离线，期间有人刷卡 25 次（锁内上行队列只有 16 条）", room)
	_ = simDo(room, "rf", map[string]bool{"on": false})
	for i := 0; i < 25; i++ {
		if _, err := atDoor(room, "swipe", map[string]string{"uid": uid}); err != nil {
			return err
		}
	}
	time.Sleep(5500 * time.Millisecond)
	fmt.Printf("    门锁记录序号 %d，平台目前收到 %d 条\n", lock(room).Sim.LogSeq, lock(room).Records)
	step("恢复在线：锁先上报重连心跳和队列里的 16 条；平台发现序号缺口，下发补传指令")
	_ = simDo(room, "rf", map[string]bool{"on": true})
	var l lockView
	for i := 0; i < 60; i++ {
		l = lock(room)
		if l.Records-before.Records >= 25 && l.Missing == 0 {
			break
		}
		time.Sleep(500 * time.Millisecond)
	}
	if err := expect(l.Records-before.Records >= 25 && l.Missing == 0, "25 条开门记录全部到达平台（补传 %d 次）", l.ReadLogs-before.ReadLogs); err != nil {
		return err
	}

	step("空口丢包率调到 30%%，再刷卡 10 次")
	before = lock(room)
	_ = call("POST", "/api/sim/loss", map[string]int{"pct": 30}, nil)
	for i := 0; i < 10; i++ {
		_, _ = atDoor(room, "swipe", map[string]string{"uid": uid})
		time.Sleep(200 * time.Millisecond)
	}
	for i := 0; i < 60; i++ {
		l = lock(room)
		if l.Records-before.Records >= 10 && l.Sim.UpPending == 0 {
			break
		}
		time.Sleep(500 * time.Millisecond)
	}
	_ = call("POST", "/api/sim/loss", map[string]int{"pct": 0}, nil)
	return expect(l.Records-before.Records >= 10, "10 条记录全部到达：上行没收到网关 ACK 就按 300ms/600ms/1.2s… 重发，重复的由平台去重")
}

func demoTimeSync() error {
	room := "1029"
	step("模拟 %s 的时钟慢了 5 分钟（晶振误差、换电池后）", room)
	since := lastEventID()
	if err := simDo(room, "clock", map[string]int{"delta_s": -300}); err != nil {
		return err
	}
	step("网关不发信标；门锁下一次上行（心跳、开门记录或这里的状态查询回复）收到的 ACK 里带标准时间")
	if _, err := submit("/api/locks/"+room+"/status", nil); err != nil {
		return err
	}
	e, found := waitEvent(since, room, 8*time.Second, "ACK 校时")
	l := lock(room)
	return expect(found && l.Sim.ClockOffs > -3 && l.Sim.ClockOffs < 3, "%s（ACK 用链路密钥认证，伪造不了时间）", strings.TrimSpace(e.Text))
}

func demoWakeJam() error {
	room := "1130"
	step("攻击者对 %s 连续发送 25 个伪造前导码（想让它一直开着接收机，把电池耗光）", room)
	since := lastEventID()
	if err := simDo(room, "wakejam", map[string]int{"n": 25}); err != nil {
		return err
	}
	e, found := waitEvent(since, room, 5*time.Second, "耗电攻击")
	l := lock(room)
	if err := expect(found && l.Sim.SniffDiv >= 4, "门锁把侦听降为每 %d 个周期一次，平台收到告警：%s", l.Sim.SniffDiv, strings.TrimSpace(e.Text)); err != nil {
		return err
	}
	step("攻击期间平台下发远程开门：网关只在这把锁侦听的周期发送")
	id, err := submit("/api/locks/"+room+"/unlock", map[string]any{"duration_s": 3})
	if err != nil {
		return err
	}
	r, err := waitCmd(id, 15*time.Second)
	if err != nil {
		return err
	}
	return expect(r.Result == "OK", "照样送达并开门（耗时 %d ms，最多多等 %d 个周期）；攻击停止 5 分钟后侦听频率逐级恢复", ms(r.QueuedAt, r.ExecutedAt), l.Sim.SniffDiv-1)
}
