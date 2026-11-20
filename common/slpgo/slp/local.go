package slp

import (
	"encoding/binary"
	"fmt"
	"time"
)

// Local credentials checked by the lock without any network: offline
// passwords and BLE tokens. Both are MACs under the lock's K_app, so only
// the platform (KMS) can issue them. Mirrors firmware/lock/credential/
// offline_pwd.c and lock_app_ble().

// DirLocal is the nonce domain bit of local-credential MACs (slp/secure.h).
const DirLocal = 0x20

// AppMAC is slp_app_mac(): AES-CCM with empty plaintext, msg as AAD.
func AppMAC(key []byte, lockEUI uint64, msg []byte) []byte {
	return CCMSeal(key, Nonce(lockEUI, 0xFFFFFFFF, SecEncMIC64|DirLocal), msg, nil, AppMICLen)
}

// Offline password kinds: 0 one-time, 1..7 time-limited classes, 9 clear.
const (
	OfflineOnce  = 0
	OfflineClear = 9
)

// OfflineClassHours is the validity of time-limited kinds 1..7.
var OfflineClassHours = [8]int{0, 1, 4, 8, 24, 72, 168, 720}

// OfflineKindFor picks the smallest time-limited class covering hours.
func OfflineKindFor(hours int) (uint8, error) {
	for k := 1; k <= 7; k++ {
		if OfflineClassHours[k] >= hours {
			return uint8(k), nil
		}
	}
	return 0, fmt.Errorf("slp: offline password longer than %d h", OfflineClassHours[7])
}

// OfflinePassword computes the 10-digit code for (kind, index, start hour).
func OfflinePassword(key []byte, lockEUI uint64, kind, index uint8, startHour uint32) string {
	msg := []byte{'O', 'F', 'P', kind, index}
	msg = binary.LittleEndian.AppendUint32(msg, startHour)
	v := binary.LittleEndian.Uint64(AppMAC(key, lockEUI, msg)) % 100000000
	return fmt.Sprintf("%d%d%08d", kind, index, v)
}

// OfflineWindow returns when a code issued at startHour stops working.
func OfflineWindow(kind uint8, startHour uint32) time.Time {
	h := 6
	switch {
	case kind == OfflineClear:
		h = 24
	case kind >= 1 && kind <= 7:
		h = OfflineClassHours[kind]
	}
	return time.Unix(int64(startHour+uint32(h))*3600, 0)
}

// BLEToken builds the 17-byte token a phone presents over BLE:
// USER_ID(4) VALID_TO(4) FLAGS(1) MAC(8).
func BLEToken(key []byte, lockEUI uint64, userID uint32, validTo time.Time, flags uint8) []byte {
	t := binary.LittleEndian.AppendUint32(nil, userID)
	t = binary.LittleEndian.AppendUint32(t, uint32(validTo.Unix()))
	t = append(t, flags)
	mac := AppMAC(key, lockEUI, append([]byte("BLE"), t...))
	return append(t, mac...)
}

// Schedule encodes TLV SCHEDULE: weekday mask (bit0 = Monday), start and end
// minutes of the local day.
func Schedule(weekMask uint8, startMin, endMin uint16) []byte {
	b := []byte{weekMask & 0x7F}
	b = binary.LittleEndian.AppendUint16(b, startMin)
	return binary.LittleEndian.AppendUint16(b, endMin)
}
