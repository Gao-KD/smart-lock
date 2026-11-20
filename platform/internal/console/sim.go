package console

import (
	"context"
	"errors"
	"fmt"
	"strconv"
	"strings"
	"time"

	"github.com/Gao-KD/smart-lock/common/slpgo/simnet"
	"github.com/Gao-KD/smart-lock/common/slpgo/slp"
	"github.com/Gao-KD/smart-lock/platform/internal/dispatch"
)

// Registry resolves locks from the simulated building layout (production:
// PostgreSQL).
type Registry struct {
	Tenant string
	Layout simnet.Layout
}

func (r Registry) Lock(_ context.Context, eui uint64) (dispatch.LockInfo, error) {
	b, f, room, ok := simnet.SplitLockEUI(eui)
	if !ok || b != r.Layout.Building || f < 1 || f > r.Layout.Floors || room < 1 || room > r.Layout.Rooms {
		return dispatch.LockInfo{}, errors.New("not found")
	}
	return dispatch.LockInfo{EUI: eui, Tenant: r.Tenant, Gateway: simnet.GatewayID(b, r.Layout.GatewayOfFloor(f))}, nil
}

func (r Registry) GatewaysOfBuilding(_ context.Context, _, building string) ([]string, error) {
	if building != strconv.Itoa(r.Layout.Building) {
		return nil, fmt.Errorf("unknown building %q", building)
	}
	var out []string
	for g := 1; g <= r.Layout.Gateways(); g++ {
		out = append(out, simnet.GatewayID(r.Layout.Building, g))
	}
	return out, nil
}

// Resolve accepts a room name ("1203") or a 16-digit EUI.
func (r Registry) Resolve(s string) (uint64, error) {
	if len(s) == 16 {
		v, err := strconv.ParseUint(s, 16, 64)
		if err == nil {
			if _, err := r.Lock(context.Background(), v); err == nil {
				return v, nil
			}
		}
		return 0, dispatch.ErrUnknownLock
	}
	n, err := strconv.Atoi(s)
	if err != nil {
		return 0, dispatch.ErrUnknownLock
	}
	eui := simnet.LockEUI(r.Layout.Building, n/100, n%100)
	if _, err := r.Lock(context.Background(), eui); err != nil {
		return 0, dispatch.ErrUnknownLock
	}
	return eui, nil
}

func gatewayEUI(id string) (uint64, error) {
	var b, g int
	if _, err := fmt.Sscanf(strings.TrimPrefix(id, "gw-"), "%d-%d", &b, &g); err != nil {
		return 0, err
	}
	return simnet.GatewayEUI(b, g), nil
}

// Sealer is the simulator's stand-in for the KMS-backed sealer: real
// AES-128-CCM* end-to-end encryption, keys derived from the simulation
// master key.
type Sealer struct{}

func (Sealer) Seal(_ context.Context, eui uint64, cmd uint8, msgID uint32, exp time.Time, tlv []byte) ([]byte, error) {
	return slp.SealDownRaw(simnet.Key(simnet.TagApp, eui), eui, cmd, msgID, uint32(exp.Unix()), tlv), nil
}

func (Sealer) SealGroup(_ context.Context, _, gateway string, _ uint16, cmd uint8, msgID uint32, exp time.Time, tlv []byte) ([]byte, error) {
	ge, err := gatewayEUI(gateway)
	if err != nil {
		return nil, err
	}
	return slp.SealDownRaw(simnet.Key(simnet.TagGrpApp, ge), ge, cmd, msgID, uint32(exp.Unix()), tlv), nil
}
