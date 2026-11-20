package topics

import "testing"

func TestBuildParseRoundTrip(t *testing.T) {
	topic := Build("school-a", "gw-b12-01", UpResult)
	if topic != "sl/v1/school-a/gw-b12-01/up/result" {
		t.Fatalf("topic = %s", topic)
	}
	tenant, gw, k, err := Parse(topic)
	if err != nil || tenant != "school-a" || gw != "gw-b12-01" || k != UpResult {
		t.Fatalf("parse = %s %s %s %v", tenant, gw, k, err)
	}
}

func TestParseRejectsForeignTopics(t *testing.T) {
	for _, bad := range []string{
		"sl/v1/school-a/gw/up/unknown",
		"sl/v2/school-a/gw/up/ack",
		"sl/v1//gw/up/ack",
		"other/topic",
	} {
		if _, _, _, err := Parse(bad); err == nil {
			t.Errorf("expected error for %q", bad)
		}
	}
}

func TestSubscriptions(t *testing.T) {
	subs := GatewaySubscriptions("s", "g")
	if len(subs) != 2 || subs[0] != "sl/v1/s/g/down/cmd" {
		t.Fatalf("subs = %v", subs)
	}
	if PlatformSubscription(UpEvent) != "$share/platform/sl/v1/+/+/up/event" {
		t.Fatal(PlatformSubscription(UpEvent))
	}
}
