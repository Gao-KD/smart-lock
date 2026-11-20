package slp

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/subtle"
	"errors"
)

// NonceLen is the CCM* nonce length (L = 2).
const NonceLen = 13

var ErrAuth = errors.New("slp: message authentication failed")

// Security levels used in the nonce (CCM* M field), see slp/security.h.
const (
	SecEncMIC32 = 0x05
	SecEncMIC64 = 0x06
	DirDown     = 0x00
	DirUp       = 0x40
)

// Nonce = src EUI-64 (BE) | frame counter (BE) | level.
func Nonce(srcEUI uint64, counter uint32, level byte) []byte {
	n := make([]byte, NonceLen)
	for i := 0; i < 8; i++ {
		n[i] = byte(srcEUI >> (8 * (7 - i)))
	}
	for i := 0; i < 4; i++ {
		n[8+i] = byte(counter >> (8 * (3 - i)))
	}
	n[12] = level
	return n
}

func ccmMAC(b cipher.Block, nonce, aad, msg []byte, micLen int) []byte {
	tag := make([]byte, 16)
	blk := make([]byte, 16)
	flags := byte(((micLen - 2) / 2) << 3)
	flags |= 1
	if len(aad) > 0 {
		flags |= 0x40
	}
	blk[0] = flags
	copy(blk[1:], nonce)
	blk[14], blk[15] = byte(len(msg)>>8), byte(len(msg))
	b.Encrypt(tag, blk)

	mix := func(data []byte) {
		for off := 0; off < len(data); off += 16 {
			for j := 0; j < 16 && off+j < len(data); j++ {
				tag[j] ^= data[off+j]
			}
			b.Encrypt(tag, tag)
		}
	}
	if len(aad) > 0 {
		a := append([]byte{byte(len(aad) >> 8), byte(len(aad))}, aad...)
		mix(a)
	}
	mix(msg)
	return tag[:micLen]
}

func ccmCTR(b cipher.Block, nonce []byte, i uint16) []byte {
	a := make([]byte, 16)
	a[0] = 1
	copy(a[1:], nonce)
	a[14], a[15] = byte(i>>8), byte(i)
	out := make([]byte, 16)
	b.Encrypt(out, a)
	return out
}

func ccmCrypt(b cipher.Block, nonce, buf []byte) {
	ctr := uint16(1)
	for off := 0; off < len(buf); off += 16 {
		s := ccmCTR(b, nonce, ctr)
		for j := 0; j < 16 && off+j < len(buf); j++ {
			buf[off+j] ^= s[j]
		}
		ctr++
	}
}

// CCMSeal returns ciphertext || MIC for plaintext (RFC 3610, L = 2).
func CCMSeal(key, nonce, aad, plaintext []byte, micLen int) []byte {
	b, err := aes.NewCipher(key)
	if err != nil {
		panic(err)
	}
	tag := ccmMAC(b, nonce, aad, plaintext, micLen)
	s0 := ccmCTR(b, nonce, 0)
	out := append([]byte(nil), plaintext...)
	ccmCrypt(b, nonce, out)
	for j := 0; j < micLen; j++ {
		out = append(out, tag[j]^s0[j])
	}
	return out
}

// CCMOpen verifies and decrypts ciphertext || MIC.
func CCMOpen(key, nonce, aad, sealed []byte, micLen int) ([]byte, error) {
	if len(sealed) < micLen {
		return nil, ErrAuth
	}
	b, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}
	n := len(sealed) - micLen
	pt := append([]byte(nil), sealed[:n]...)
	ccmCrypt(b, nonce, pt)
	tag := ccmMAC(b, nonce, aad, pt, micLen)
	s0 := ccmCTR(b, nonce, 0)
	want := make([]byte, micLen)
	for j := range want {
		want[j] = tag[j] ^ s0[j]
	}
	if subtle.ConstantTimeCompare(want, sealed[n:]) != 1 {
		return nil, ErrAuth
	}
	return pt, nil
}
