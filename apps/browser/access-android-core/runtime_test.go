package main

import (
	"encoding/json"
	"strings"
	"sync"
	"testing"
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
		strings.Repeat(" ", maxTransfer+1),
	} {
		if _, err := proxyConfig([]byte(raw)); err != errConfig {
			t.Errorf("expected configuration rejection for input of %d bytes, got %v", len(raw), err)
		}
	}

	for _, protocol := range []string{"vless", "vmess", "trojan", "hysteria"} {
		raw := `{"outbounds":[{"protocol":"` + protocol + `","tag":"owned","settings":{}}]}`
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
