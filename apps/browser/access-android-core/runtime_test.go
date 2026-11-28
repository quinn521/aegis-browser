package main

import (
	"bytes"
	"encoding/json"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestProxyConfigRequiresOneProxyOutbound(t *testing.T) {
	for _, raw := range []string{
		"",
		"null",
		`{"outbounds":[]}`,
		`{"outbounds":[null]}`,
		`{"outbounds":[{"protocol":"freedom"}]}`,
		`{"outbounds":[{"protocol":"vless"},{"protocol":"freedom"}]}`,
		`{"outbounds":[{"protocol":"vless"}],"routing":{}}`,
		`{"outbounds":[{"protocol":"vless"}],"inbounds":[]}`,
		`{"outbounds":[{"protocol":"vless"}],"log":{"access":"/tmp/configured-path"}}`,
		`{"outbounds":[{"protocol":"vless","ProxySettings":{"tag":"backup"}}]}`,
		`{"outbounds":[{"protocol":"trojan","settings":{"servers":[null]}}]}`,
		`{"outbounds":[{"protocol":"vmess","settings":{"vnext":[null]}}]}`,
		`{"outbounds":[{"protocol":"vless","settings":{"vnext":[null]}}]}`,
		`{"outbounds":[{"protocol":"vless","settings":{"vnext":[{"address":"127.0.0.1","port":9,"users":[null]}]}}]}`,
		`{"outbounds":[{"protocol":"vless","settings":{"vnext":[{"address":"127.0.0.1","port":9,"users":[
			{"id":"fixture","encryption":"none","testpre":1}]}]}}]}`,
		`{"outbounds":[{"protocol":"hysteria","settings":{"address":null,"port":9,"version":2}}]}`,
		strings.Repeat(" ", maxTransfer+1),
	} {
		if _, err := proxyConfig([]byte(raw)); err != errConfig {
			t.Errorf("expected configuration rejection for input of %d bytes, got %v", len(raw), err)
		}
	}

	for protocol, settings := range map[string]string{
		"vless":    `{"vnext":[{"address":"127.0.0.1","port":9,"users":[{"id":"fixture","encryption":"none"}]}]}`,
		"vmess":    `{"address":"127.0.0.1","port":9,"id":"fixture","security":"auto"}`,
		"trojan":   `{"address":"127.0.0.1","port":9,"password":"fixture"}`,
		"hysteria": `{"address":"127.0.0.1","port":9,"version":2}`,
	} {
		raw := `{"outbounds":[{"protocol":"` + protocol + `","tag":"owned","settings":` + settings + `}]}`
		config, err := proxyConfig([]byte(raw))
		if err != nil {
			t.Fatalf("prepare %s: %v", protocol, err)
		}
		var result struct {
			Outbounds []struct {
				Protocol string
			}
			Log      map[string]string
			Inbounds []json.RawMessage
		}
		if err := json.Unmarshal(config, &result); err != nil {
			t.Fatal(err)
		}
		if len(result.Outbounds) != 1 || result.Outbounds[0].Protocol != protocol ||
			result.Log["loglevel"] != "none" || result.Log["access"] != "none" || len(result.Inbounds) != 0 {
			t.Fatalf("unexpected generated configuration for %s", protocol)
		}
	}
}

var testConfig = []byte(`{"outbounds":[{"tag":"fixture","protocol":"vless","settings":{"vnext":[{"address":"127.0.0.1","port":9,"users":[{"id":"12345678-1234-1234-1234-123456789abc","encryption":"none"}]}]}}]}`)

func withStreamSettings(config []byte, stream string) []byte {
	base := strings.TrimSuffix(string(config), "}]}")
	return []byte(base + `,"streamSettings":` + stream + `}]}`)
}

func TestStartRequiresAccessTransportFields(t *testing.T) {
	withTLS := func(settings string) []byte {
		return withStreamSettings(testConfig, `{"network":"ws","security":"tls","tlsSettings":`+
			settings+`,"wsSettings":{"host":"node.example","path":"/test?key=value"}}`)
	}
	var registry coreRegistry
	id := registry.create()
	instance := registry.lookup(id)
	t.Cleanup(func() { registry.close(id) })
	for _, settings := range []string{
		`{"certificates":[null]}`,
		`{"Certificates":[]}`,
		`{"allowInsecure":true}`,
		`{"allowInsecure":null}`,
		`{"allowInsecure":"false"}`,
		`null`,
	} {
		if status := instance.start(withTLS(settings)); status != statusInvalid {
			t.Errorf("TLS settings %s returned status %d", settings, status)
		}
	}
	if instance.runtime.IsRunning() {
		t.Fatal("rejected TLS settings left the core running")
	}
	visionConfig := bytes.Replace(testConfig, []byte(`"encryption":"none"`),
		[]byte(`"encryption":"none","flow":"xtls-rprx-vision"`), 1)
	for name, config := range map[string][]byte{
		"iOS WS/TLS": withTLS(`{"serverName":"node.example","useSystemCA":true,"alpn":["http/1.1"]}`),
		"browser WS/TLS": withTLS(`{"serverName":"node.example","useSystemCA":true,
			"allowInsecure":false,"alpn":["http/1.1"]}`),
		"browser TCP/TLS Vision": withStreamSettings(visionConfig, `{"network":"tcp","security":"tls",
			"tlsSettings":{"serverName":"node.example","useSystemCA":true,"allowInsecure":false}}`),
	} {
		t.Run(name, func(t *testing.T) {
			id := registry.create()
			t.Cleanup(func() { registry.close(id) })
			if status := registry.lookup(id).start(config); status != statusOK {
				t.Fatalf("starting verified TLS returned %d", status)
			}
		})
	}
}

func TestStartRejectsMalformedRealityAndDuplicateFields(t *testing.T) {
	realityStream := func(fingerprint, path string) string {
		settings := map[string]string{
			"serverName":  "node.example",
			"publicKey":   "CQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
			"shortId":     "ab",
			"fingerprint": fingerprint,
			"spiderX":     path,
		}
		fields, err := json.Marshal(settings)
		if err != nil {
			t.Fatal(err)
		}
		return `{"network":"tcp","security":"reality","realitySettings":` + string(fields) + `}`
	}
	var registry coreRegistry
	id := registry.create()
	instance := registry.lookup(id)
	t.Cleanup(func() { registry.close(id) })
	for name, stream := range map[string]string{
		"invalid REALITY path":        realityStream("chrome", "/bad%escape"),
		"invalid REALITY fingerprint": realityStream("hellogolang", "/"),
		"REALITY spider tuning":       realityStream("chrome", "/?p=1-2"),
		"duplicate TLS settings": `{"network":"ws","security":"tls",
			"tlsSettings":{"certificates":[null]},"tlsSettings":{"useSystemCA":true}}`,
		"duplicate nested TLS field": `{"network":"ws","security":"tls",
			"tlsSettings":{"useSystemCA":false,"useSystemCA":true}}`,
	} {
		t.Run(name, func(t *testing.T) {
			if status := instance.start(withStreamSettings(testConfig, stream)); status != statusInvalid {
				t.Fatalf("start returned %d", status)
			}
			if instance.runtime.IsRunning() {
				t.Fatal("rejected configuration left the core running")
			}
		})
	}
	validReality := withStreamSettings(testConfig, realityStream("chrome", "/path?fixture=two%20words"))
	if status := instance.start(validReality); status != statusOK {
		t.Fatalf("starting REALITY with an ordinary path and query returned %d", status)
	}
}

func TestRegistryRetiresOnlyTheSelectedRuntime(t *testing.T) {
	var registry coreRegistry
	firstID, secondID := registry.create(), registry.create()
	first, second := registry.lookup(firstID), registry.lookup(secondID)
	if firstID == 0 || firstID == secondID || first == nil || second == nil {
		t.Fatal("registry did not create distinct cores")
	}

	t.Cleanup(func() {
		registry.close(firstID)
		registry.close(secondID)
	})
	if first.start(testConfig) != statusOK || second.start(testConfig) != statusOK {
		t.Fatal("could not start isolated cores")
	}
	if status := registry.close(firstID); status != statusOK {
		t.Fatalf("close status = %d", status)
	}
	if registry.lookup(firstID) != nil || first.runtime.IsRunning() || !second.runtime.IsRunning() {
		t.Fatal("closing one handle did not retire exactly one core")
	}
	if status := first.start(testConfig); status != statusUnavailable {
		t.Fatalf("late start on a retired core returned %d", status)
	}
	if status := registry.close(firstID); status != statusUnavailable {
		t.Fatalf("second close returned %d", status)
	}
	if nextID := registry.create(); nextID <= secondID {
		t.Fatal("retired handle was recycled")
	}
}

func TestConcurrentStartAndDestroyCannotLeaveCoreRunning(t *testing.T) {
	var registry coreRegistry
	id := registry.create()
	instance := registry.lookup(id)
	var calls sync.WaitGroup
	calls.Add(2)
	go func() {
		defer calls.Done()
		status := instance.start(testConfig)
		if status != statusOK && status != statusUnavailable {
			t.Errorf("start status = %d", status)
		}
	}()
	go func() {
		defer calls.Done()
		if status := registry.close(id); status != statusOK {
			t.Errorf("close status = %d", status)
		}
	}()
	calls.Wait()
	if instance.runtime.IsRunning() || registry.lookup(id) != nil {
		t.Fatal("destroy returned with an active core")
	}
}

type blockingReadRuntime struct {
	streamRuntime
	entered chan struct{}
	closed  chan struct{}
	stop    sync.Once
}

func (r *blockingReadRuntime) Dial(string, int) (int64, error) { return 1, nil }

func (r *blockingReadRuntime) Read(int64, int) ([]byte, error) {
	r.entered <- struct{}{}
	<-r.closed
	return nil, errStream
}

func (r *blockingReadRuntime) CloseStream(int64) {
	r.stop.Do(func() { close(r.closed) })
}

func TestReadsOnOneStreamAreSerializedAndCloseInterrupts(t *testing.T) {
	runtime := &blockingReadRuntime{entered: make(chan struct{}, 2), closed: make(chan struct{})}
	instance := &coreInstance{runtime: runtime}
	stream, err := instance.dial("127.0.0.1", 9)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { runtime.CloseStream(stream) })
	readFinished := make(chan error, 2)
	read := func() {
		_, err := instance.read(stream, 1)
		readFinished <- err
	}
	go read()
	select {
	case <-runtime.entered:
	case <-time.After(time.Second):
		t.Fatal("first read never reached the core")
	}
	secondStarted := make(chan struct{})
	go func() {
		close(secondStarted)
		read()
	}()
	<-secondStarted
	select {
	case <-runtime.entered:
		t.Fatal("second read entered while the first read was blocked")
	case <-time.After(20 * time.Millisecond):
	}
	go instance.closeStream(stream)
	for range 2 {
		select {
		case err := <-readFinished:
			if err != errStream {
				t.Errorf("read after close returned %v", err)
			}
		case <-time.After(time.Second):
			t.Fatal("close did not interrupt pending reads")
		}
	}
}
