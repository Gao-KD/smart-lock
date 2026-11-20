// Package slp is the Go side of the Smart Lock Protocol (common/slp in C):
// MAC frame codec, AES-128-CCM*, end-to-end app messages (TLV) and the
// two-layer seal/open helpers. Byte-for-byte compatible with the C library;
// slp_test.go checks the shared test vectors.
//
// The gateway uses the frame and link-layer parts, the platform uses the
// app-layer parts. Neither needs cgo.
package slp
