// Package main exposes the existing Access Go runtime to Android native code.
package main

import (
	"encoding/json"
	"errors"
	"sync"

	embeddedcore "aegis.browser/ios/embeddedcore"
)

const maxTransfer = 64 * 1024

var errConfig = errors.New("invalid Access outbound configuration")

// The browser's trusted configuration generator supplies this document. The
// bridge admits one proxy outbound and owns diagnostic logging configuration.
func proxyConfig(raw []byte) ([]byte, error) {
	if len(raw) == 0 || len(raw) > maxTransfer {
		return nil, errConfig
	}

	var document map[string]json.RawMessage
	if err := json.Unmarshal(raw, &document); err != nil || len(document) != 1 {
		return nil, errConfig
	}
	var outbounds []map[string]json.RawMessage
	if err := json.Unmarshal(document["outbounds"], &outbounds); err != nil || len(outbounds) != 1 {
		return nil, errConfig
	}

	outbound := outbounds[0]
	for key := range outbound {
		switch key {
		case "protocol", "tag", "settings", "streamSettings":
		default:
			return nil, errConfig
		}
	}
	var protocol string
	if err := json.Unmarshal(outbound["protocol"], &protocol); err != nil {
		return nil, errConfig
	}
	switch protocol {
	case "vless", "vmess", "trojan", "hysteria":
	default:
		return nil, errConfig
	}

	canonicalOutbounds, err := json.Marshal(outbounds)
	if err != nil {
		return nil, errConfig
	}
	document["outbounds"] = canonicalOutbounds
	document["log"] = json.RawMessage(`{"loglevel":"none","access":"none","error":"none"}`)
	return json.Marshal(document)
}

type coreInstance struct {
	lifecycle sync.Mutex
	retired   bool
	runtime   *embeddedcore.Runtime
}

func (c *coreInstance) start(raw []byte) int {
	config, err := proxyConfig(raw)
	if err != nil {
		return statusInvalid
	}

	c.lifecycle.Lock()
	defer c.lifecycle.Unlock()
	if c.retired {
		return statusUnavailable
	}
	if err := c.runtime.Start(string(config)); err != nil {
		return statusCoreError
	}
	return statusOK
}

func (c *coreInstance) close() int {
	c.lifecycle.Lock()
	defer c.lifecycle.Unlock()
	c.retired = true
	if err := c.runtime.Stop(); err != nil {
		return statusCoreError
	}
	return statusOK
}

// Handles are process-local and never reused. Each owns a separate runtime
// and its stream registry.
type coreRegistry struct {
	mu        sync.Mutex
	next      uint64
	instances map[uint64]*coreInstance
}

func (r *coreRegistry) create() uint64 {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.next == ^uint64(0) {
		return 0
	}
	if r.instances == nil {
		r.instances = make(map[uint64]*coreInstance)
	}
	r.next++
	r.instances[r.next] = &coreInstance{runtime: embeddedcore.NewRuntime()}
	return r.next
}

func (r *coreRegistry) lookup(id uint64) *coreInstance {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.instances[id]
}

func (r *coreRegistry) close(id uint64) int {
	r.mu.Lock()
	instance := r.instances[id]
	delete(r.instances, id)
	r.mu.Unlock()
	if instance == nil {
		return statusUnavailable
	}
	return instance.close()
}
