package config

import (
	"os"
	"testing"
)

func TestDeriveKGAudioURL(t *testing.T) {
	cases := []struct {
		chipper string
		want    string
	}{
		{"escapepod.local:443", "http://escapepod.local:8080/v1/kg-audio"},
		{"192.168.1.10:443", "http://192.168.1.10:8080/v1/kg-audio"},
		{"https://escapepod.local:443/", "http://escapepod.local:8080/v1/kg-audio"},
		{"escapepod.local", "http://escapepod.local:8080/v1/kg-audio"},
		// Anki's production hosts serve no answer audio, so the feature stays off.
		{"chipper-dev.api.anki.com:443", ""},
		{"", ""},
	}
	for _, c := range cases {
		if got := deriveKGAudioURL(c.chipper); got != c.want {
			t.Errorf("deriveKGAudioURL(%q) = %q, want %q", c.chipper, got, c.want)
		}
	}
}

func TestKGCloudAudioURLResolutionOrder(t *testing.T) {
	os.Unsetenv("VECTOR_KG_TTS_URL")
	saved := Env
	defer func() { Env = saved }()

	Env = URLs{Chipper: "escapepod.local:443"}
	if got := KGCloudAudioURL(); got != "http://escapepod.local:8080/v1/kg-audio" {
		t.Errorf("derived url = %q", got)
	}

	// An explicit config entry wins over the derived one.
	Env.KGAudio = "http://other:9000/audio"
	if got := KGCloudAudioURL(); got != "http://other:9000/audio" {
		t.Errorf("config url = %q", got)
	}

	// The environment variable wins over everything.
	os.Setenv("VECTOR_KG_TTS_URL", "http://env:1234/tts")
	defer os.Unsetenv("VECTOR_KG_TTS_URL")
	if got := KGCloudAudioURL(); got != "http://env:1234/tts" {
		t.Errorf("env url = %q", got)
	}

	// ...including an explicit opt-out.
	os.Setenv("VECTOR_KG_TTS_URL", "off")
	if got := KGCloudAudioURL(); got != "" {
		t.Errorf("expected the feature disabled, got %q", got)
	}
}
