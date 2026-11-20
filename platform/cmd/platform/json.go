package main

import (
	"encoding/json"
	"strconv"

	"github.com/Gao-KD/smart-lock/platform/internal/dispatch"
)

// wireItem matches gateway/internal/runtime.BatchItem (lock EUI as hex).
type wireItem struct {
	dispatch.Item
	LockEUI string `json:"lock_eui,omitempty"`
}

func jsonMarshal(b dispatch.Batch) ([]byte, error) {
	items := make([]wireItem, len(b.Items))
	for i, it := range b.Items {
		items[i] = wireItem{Item: it}
		if it.LockEUI != 0 {
			items[i].LockEUI = leftPad(strconv.FormatUint(it.LockEUI, 16))
		}
	}
	return json.Marshal(struct {
		dispatch.Batch
		Items []wireItem `json:"items"`
	}{b, items})
}

func leftPad(s string) string {
	for len(s) < 16 {
		s = "0" + s
	}
	return s
}
