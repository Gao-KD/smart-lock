// Package topics builds and parses the gateway <-> platform MQTT topics:
//
//	sl/v1/{tenant}/{gateway}/down/cmd     platform -> gateway, command batches
//	sl/v1/{tenant}/{gateway}/down/gw      platform -> gateway, gateway config / lock whitelist / OTA
//	sl/v1/{tenant}/{gateway}/up/ack       accepted / delivered / failed
//	sl/v1/{tenant}/{gateway}/up/result    lock results (end-to-end encrypted)
//	sl/v1/{tenant}/{gateway}/up/event     access log, alarms, battery
//	sl/v1/{tenant}/{gateway}/up/join      forwarded join requests
//	sl/v1/{tenant}/{gateway}/up/status    online state (retained, LWT)
//
// In a private (on-premise) deployment the tenant is the school.
package topics

import (
	"fmt"
	"strings"
)

const prefix = "sl/v1"

type Kind string

const (
	DownCmd  Kind = "down/cmd"
	DownGw   Kind = "down/gw"
	UpAck    Kind = "up/ack"
	UpResult Kind = "up/result"
	UpEvent  Kind = "up/event"
	UpJoin   Kind = "up/join"
	UpStatus Kind = "up/status"
)

var known = map[Kind]bool{DownCmd: true, DownGw: true, UpAck: true, UpResult: true, UpEvent: true, UpJoin: true, UpStatus: true}

// Build returns the topic for a tenant/gateway/kind.
func Build(tenant, gateway string, k Kind) string {
	return fmt.Sprintf("%s/%s/%s/%s", prefix, tenant, gateway, k)
}

// GatewaySubscriptions are the topics a gateway subscribes to.
func GatewaySubscriptions(tenant, gateway string) []string {
	return []string{Build(tenant, gateway, DownCmd), Build(tenant, gateway, DownGw)}
}

// PlatformSubscription is the shared-subscription filter the platform uses
// to receive all uplinks of one kind, load-balanced over its instances.
func PlatformSubscription(k Kind) string {
	return fmt.Sprintf("$share/platform/%s/+/+/%s", prefix, k)
}

// Parse splits a topic into tenant, gateway and kind.
func Parse(topic string) (tenant, gateway string, k Kind, err error) {
	parts := strings.Split(topic, "/")
	if len(parts) != 6 || parts[0]+"/"+parts[1] != prefix {
		return "", "", "", fmt.Errorf("topics: not a smart-lock topic: %q", topic)
	}
	k = Kind(parts[4] + "/" + parts[5])
	if !known[k] || parts[2] == "" || parts[3] == "" {
		return "", "", "", fmt.Errorf("topics: bad topic: %q", topic)
	}
	return parts[2], parts[3], k, nil
}
