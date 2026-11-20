// Command dispatch is the command-dispatch service of the on-premise
// platform. It exposes the open HTTP API used by the school's systems and
// publishes command batches to the MQTT broker.
//
//	go run ./cmd/dispatch -addr :8080
//	curl -X POST localhost:8080/v1/locks/0000000000000001/unlock
//	curl localhost:8080/v1/commands/1
//
// TODO: PostgreSQL registry, KMS-backed sealer (AES-CCM with K_app),
// MQTT publisher + up/ack|result consumers, authentication, audit log.
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"log"
	"net/http"
	"strconv"
	"time"

	"github.com/Gao-KD/smart-lock/platform/internal/dispatch"
)

// demoRegistry: 3 gateways x 10 locks, until the database is wired.
type demoRegistry struct{}

func (demoRegistry) Lock(_ context.Context, eui uint64) (dispatch.LockInfo, error) {
	if eui == 0 || eui > 30 {
		return dispatch.LockInfo{}, errors.New("not found")
	}
	gw := "demo-gw" + strconv.FormatUint((eui-1)/10+1, 10)
	return dispatch.LockInfo{EUI: eui, Tenant: "school-demo", Gateway: gw}, nil
}

func (demoRegistry) GatewaysOfBuilding(_ context.Context, _, _ string) ([]string, error) {
	return []string{"demo-gw1", "demo-gw2", "demo-gw3"}, nil
}

// plainSealer is NOT secure: it only frames the app message. Replace with
// the KMS-backed implementation before any real deployment.
type plainSealer struct{}

func (plainSealer) Seal(_ context.Context, _ uint64, cmd uint8, msgID uint32, exp time.Time, tlv []byte) ([]byte, error) {
	return frame(cmd, msgID, exp, tlv), nil
}

func (plainSealer) SealGroup(_ context.Context, _, _ string, _ uint16, cmd uint8, msgID uint32, exp time.Time, tlv []byte) ([]byte, error) {
	return frame(cmd, msgID, exp, tlv), nil
}

func frame(cmd uint8, msgID uint32, exp time.Time, tlv []byte) []byte {
	e := uint32(exp.Unix())
	b := []byte{cmd, byte(msgID), byte(msgID >> 8), byte(msgID >> 16), byte(msgID >> 24),
		byte(e), byte(e >> 8), byte(e >> 16), byte(e >> 24)}
	return append(b, tlv...)
}

type logPublisher struct{}

func (logPublisher) Publish(_ context.Context, b dispatch.Batch) error {
	log.Printf("publish sl/v1/%s/%s/down/cmd: batch %d, %d items", b.Tenant, b.Gateway, b.BatchID, len(b.Items))
	return nil
}

func main() {
	addr := flag.String("addr", ":8080", "listen address")
	flag.Parse()

	svc := dispatch.New(dispatch.Config{}, demoRegistry{}, plainSealer{}, logPublisher{})
	mux := http.NewServeMux()

	mux.HandleFunc("POST /v1/locks/{eui}/unlock", func(w http.ResponseWriter, r *http.Request) {
		eui, err := strconv.ParseUint(r.PathValue("eui"), 16, 64)
		if err != nil {
			http.Error(w, "bad eui", http.StatusBadRequest)
			return
		}
		id, err := svc.Unlock(r.Context(), eui, 0)
		if errors.Is(err, dispatch.ErrUnknownLock) {
			http.Error(w, err.Error(), http.StatusNotFound)
			return
		} else if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		writeJSON(w, http.StatusAccepted, map[string]any{"msg_id": id, "status": dispatch.StatusQueued})
	})

	mux.HandleFunc("POST /v1/buildings/{building}/emergency-open", func(w http.ResponseWriter, r *http.Request) {
		ids, err := svc.EmergencyOpen(r.Context(), "school-demo", r.PathValue("building"))
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		writeJSON(w, http.StatusAccepted, map[string]any{"msg_ids": ids})
	})

	mux.HandleFunc("GET /v1/commands/{id}", func(w http.ResponseWriter, r *http.Request) {
		id, err := strconv.ParseUint(r.PathValue("id"), 10, 32)
		if err != nil {
			http.Error(w, "bad id", http.StatusBadRequest)
			return
		}
		st, ok := svc.Status(uint32(id))
		if !ok {
			http.Error(w, "not found", http.StatusNotFound)
			return
		}
		writeJSON(w, http.StatusOK, map[string]any{"msg_id": id, "status": st})
	})

	log.Printf("dispatch listening on %s", *addr)
	srv := &http.Server{Addr: *addr, Handler: mux, ReadHeaderTimeout: 5 * time.Second}
	log.Fatal(srv.ListenAndServe())
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}
