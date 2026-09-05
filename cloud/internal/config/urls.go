package config

import (
	"encoding/json"
	"io/ioutil"
	"os"
	"strings"
)

// URLs represents a set of URLs where Anki's cloud services can be reached
type URLs struct {
	JDocs          string  `json:"jdocs"`
	Token          string  `json:"tms"`
	Chipper        string  `json:"chipper"`
	Check          string  `json:"check"`
	LogFiles       string  `json:"logfiles"`
	AppKey         string  `json:"appkey"`
	OffboardVision *string `json:"offboard_vision,omitempty"`
	// KGAudio is the base URL serving Knowledge Graph answer audio. Optional;
	// when unset it is derived from Chipper. See KGCloudAudioURL.
	KGAudio string `json:"kg_audio,omitempty"`
}

// DefaultURLs provides a default, hard-coded configuration that can be used
// if an expected configuration is not found on disk
var DefaultURLs = URLs{
	JDocs:    "jdocs-dev.api.anki.com:443",
	Token:    "token-dev.api.anki.com:443",
	Chipper:  "chipper-dev.api.anki.com:443",
	Check:    "conncheck.global.anki-dev-services.com/ok",
	LogFiles: "s3://anki-device-logs-dev/victor",
	AppKey:   "",
}

// Env represents the URLs associated with the most recent successful call to
// SetGlobal. Before this, it has the same values as DefaultURLs.
var Env = DefaultURLs

// SetGlobal sets the public Env variable to the URLs in the given filename. If the given
// filename is blank, a known hardcoded location for server_config.json on the robot is used.
func SetGlobal(filename string) error {
	urls, err := LoadURLs(filename)
	if err != nil {
		return err
	}
	Env = *urls
	return nil
}

// KGCloudAudioURL returns the base endpoint used to obtain Knowledge Graph
// answer audio, or an empty string if the feature is disabled. When empty,
// vic-cloud reports cloud_audio_available=false and the robot uses local TTS as
// it always has.
//
// The endpoint serves raw 16 kHz mono signed 16-bit little-endian PCM, streamed
// as it is produced. When the Knowledge service returned an audio id, that id is
// appended to this base URL and fetched with GET; otherwise the answer text is
// POSTed to this URL for synthesis.
//
// Resolution order:
//  1. VECTOR_KG_TTS_URL, for a fully custom endpoint.
//  2. kg_audio in server_config.json.
//  3. Derived from the configured chipper host, which is where an escape-pod
//     style server serving answer audio already lives.
//
// Set VECTOR_KG_TTS_URL to "off" to disable the feature outright.
func KGCloudAudioURL() string {
	if env := os.Getenv("VECTOR_KG_TTS_URL"); env != "" {
		if env == "off" {
			return ""
		}
		return env
	}
	if Env.KGAudio != "" {
		return Env.KGAudio
	}
	return deriveKGAudioURL(Env.Chipper)
}

// kgAudioDefaultPort is the port an escape-pod style server exposes its web
// interface - and therefore the answer audio route - on.
const kgAudioDefaultPort = "8080"

// kgAudioDefaultPath is the answer-audio route; an audio id is appended to it.
const kgAudioDefaultPath = "/v1/kg-audio"

// deriveKGAudioURL builds the answer-audio base URL from the chipper host.
// Anki's production chipper hosts have no such route, so only local/self-hosted
// servers are derived from; anything else disables the feature.
func deriveKGAudioURL(chipper string) string {
	host := chipper
	if idx := strings.Index(host, "://"); idx >= 0 {
		host = host[idx+3:]
	}
	if idx := strings.Index(host, "/"); idx >= 0 {
		host = host[:idx]
	}
	if idx := strings.LastIndex(host, ":"); idx >= 0 {
		host = host[:idx]
	}
	if host == "" || strings.HasSuffix(host, ".anki.com") {
		return ""
	}
	return "http://" + host + ":" + kgAudioDefaultPort + kgAudioDefaultPath
}

var defaultFilename = "/anki/data/assets/cozmo_resources/config/server_config.json"
var wirepodFilename = "/data/data/server_config.json"

// LoadURLs attempts to load a URL config from the given filename. If the given filename
// is blank, a known hardcoded location for server_config.json on the robot is used.
func LoadURLs(filename string) (*URLs, error) {
	if filename == "" {
		if _, err := os.Open(wirepodFilename); err != nil {
			filename = defaultFilename
		} else {
			filename = wirepodFilename
		}
	}
	buf, err := ioutil.ReadFile(filename)
	if err != nil {
		return nil, err
	}
	var urls URLs
	if err := json.Unmarshal(buf, &urls); err != nil {
		return nil, err
	}
	return &urls, nil
}
