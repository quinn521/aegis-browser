// Package main exposes the existing Access Go runtime to Android native code.
package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"slices"
	"strings"
	"sync"
	"sync/atomic"

	embeddedcore "aegis.browser/ios/embeddedcore"
)

const maxTransfer = 64 * 1024

var errConfig = errors.New("invalid Access outbound configuration")
var errStream = errors.New("unavailable Access stream")

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
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.UseNumber()
	if !uniqueJSONFields(decoder) {
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
	if !validSettings(protocol, outbound["settings"]) {
		return nil, errConfig
	}
	if stream, present := outbound["streamSettings"]; present && !validStreamSettings(stream) {
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

// Xray decodes nested objects into structs, where duplicate object fields
// can merge. Reject them before validating and forwarding the raw values.
func uniqueJSONFields(decoder *json.Decoder) bool {
	token, err := decoder.Token()
	if err != nil {
		return false
	}
	switch token {
	case json.Delim('{'):
		fields := make(map[string]struct{})
		for decoder.More() {
			key, err := decoder.Token()
			name, isString := key.(string)
			if err != nil || !isString {
				return false
			}
			if _, duplicate := fields[name]; duplicate {
				return false
			}
			fields[name] = struct{}{}
			if !uniqueJSONFields(decoder) {
				return false
			}
		}
	case json.Delim('['):
		for decoder.More() {
			if !uniqueJSONFields(decoder) {
				return false
			}
		}
	default:
		return true
	}
	_, err = decoder.Token()
	return err == nil
}

// Only pass transport fields emitted by the Access configuration generators.
// Xray's general TLS configuration accepts certificate files and assumes
// certificate array entries are non-nil.
func validStreamSettings(raw json.RawMessage) bool {
	var stream map[string]json.RawMessage
	if err := json.Unmarshal(raw, &stream); err != nil || stream == nil {
		return false
	}
	for key, value := range stream {
		switch key {
		case "network":
			if !oneOf(value, "tcp", "ws", "grpc", "httpupgrade", "hysteria") {
				return false
			}
		case "security":
			if !oneOf(value, "tls", "reality") {
				return false
			}
		case "tlsSettings":
			if !validTLSSettings(value) {
				return false
			}
		case "realitySettings":
			if !validRealitySettings(value) {
				return false
			}
		case "wsSettings", "httpupgradeSettings":
			if !hasOnlyFields(value, "host", "path") {
				return false
			}
		case "grpcSettings":
			if !hasOnlyFields(value, "serviceName") {
				return false
			}
		case "hysteriaSettings":
			if !hasOnlyFields(value, "version", "auth") {
				return false
			}
		case "finalmask":
			var mask map[string]json.RawMessage
			if json.Unmarshal(value, &mask) != nil || len(mask) != 1 ||
				!hasOnlyFields(mask["quicParams"], "congestion", "brutalUp", "brutalDown") {
				return false
			}
		default:
			return false
		}
	}
	return true
}

func validTLSSettings(raw json.RawMessage) bool {
	if !hasOnlyFields(raw, "serverName", "useSystemCA", "fingerprint", "alpn", "allowInsecure") {
		return false
	}
	// Browser-native TLS outbounds explicitly disable insecure verification.
	var settings struct {
		AllowInsecure json.RawMessage `json:"allowInsecure"`
	}
	return json.Unmarshal(raw, &settings) == nil &&
		(settings.AllowInsecure == nil || string(settings.AllowInsecure) == "false")
}

func validRealitySettings(raw json.RawMessage) bool {
	if !hasOnlyFields(raw, "serverName", "fingerprint", "publicKey", "shortId", "spiderX") {
		return false
	}
	var client struct {
		Fingerprint string `json:"fingerprint"`
		SpiderX     string `json:"spiderX"`
	}
	return json.Unmarshal(raw, &client) == nil &&
		embeddedcore.ValidateRealityClientFields(client.Fingerprint, client.SpiderX) == ""
}

func hasOnlyFields(raw json.RawMessage, allowed ...string) bool {
	var object map[string]json.RawMessage
	if err := json.Unmarshal(raw, &object); err != nil || object == nil {
		return false
	}
	for key := range object {
		if !slices.Contains(allowed, key) {
			return false
		}
	}
	return true
}

func oneOf(raw json.RawMessage, allowed ...string) bool {
	var value string
	return json.Unmarshal(raw, &value) == nil && slices.Contains(allowed, value)
}

// Accept the endpoint shapes produced by the shared Access configuration
// generators. In particular, a nil entry must never reach Xray's builders.
func validSettings(protocol string, raw json.RawMessage) bool {
	var settings map[string]json.RawMessage
	if err := json.Unmarshal(raw, &settings); err != nil {
		return false
	}
	switch protocol {
	case "vless":
		var endpoints []map[string]json.RawMessage
		if len(settings) != 1 || json.Unmarshal(settings["vnext"], &endpoints) != nil || len(endpoints) != 1 {
			return false
		}
		endpoint := endpoints[0]
		var users []map[string]json.RawMessage
		return len(endpoint) == 3 && validEndpoint(endpoint) &&
			json.Unmarshal(endpoint["users"], &users) == nil && len(users) == 1 && users[0] != nil
	case "vmess":
		return len(settings) == 4 && validEndpoint(settings) &&
			nonemptyString(settings["id"]) && nonemptyString(settings["security"])
	case "trojan":
		return len(settings) == 3 && validEndpoint(settings) && nonemptyString(settings["password"])
	case "hysteria":
		var version int
		return len(settings) == 3 && validEndpoint(settings) &&
			json.Unmarshal(settings["version"], &version) == nil && version == 2
	}
	return false
}

func validEndpoint(fields map[string]json.RawMessage) bool {
	var port uint16
	return nonemptyString(fields["address"]) && json.Unmarshal(fields["port"], &port) == nil && port != 0
}

func nonemptyString(raw json.RawMessage) bool {
	var value string
	return json.Unmarshal(raw, &value) == nil && strings.TrimSpace(value) != ""
}

type streamRuntime interface {
	Start(string) error
	Stop() error
	IsRunning() bool
	Dial(string, int) (int64, error)
	Read(int64, int) ([]byte, error)
	Write(int64, []byte) error
	CloseStream(int64)
}

type coreStream struct {
	readMu  sync.Mutex
	writeMu sync.Mutex
}

type coreInstance struct {
	lifecycle sync.Mutex
	retired   atomic.Bool
	runtime   streamRuntime
	streamsMu sync.Mutex
	streams   map[int64]*coreStream
}

func (c *coreInstance) start(raw []byte) int {
	config, err := proxyConfig(raw)
	if err != nil {
		return statusInvalid
	}

	c.lifecycle.Lock()
	defer c.lifecycle.Unlock()
	if c.retired.Load() {
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
	c.retired.Store(true)
	c.streamsMu.Lock()
	c.streams = nil
	c.streamsMu.Unlock()
	if err := c.runtime.Stop(); err != nil {
		return statusCoreError
	}
	return statusOK
}

func (c *coreInstance) dial(host string, port int) (int64, error) {
	id, err := c.runtime.Dial(host, port)
	if err != nil {
		return 0, err
	}
	c.streamsMu.Lock()
	if c.retired.Load() {
		c.streamsMu.Unlock()
		c.runtime.CloseStream(id)
		return 0, errStream
	}
	if c.streams == nil {
		c.streams = make(map[int64]*coreStream)
	}
	c.streams[id] = &coreStream{}
	c.streamsMu.Unlock()
	return id, nil
}

func (c *coreInstance) stream(id int64) *coreStream {
	c.streamsMu.Lock()
	defer c.streamsMu.Unlock()
	return c.streams[id]
}

func (c *coreInstance) read(id int64, capacity int) ([]byte, error) {
	stream := c.stream(id)
	if stream == nil {
		return nil, errStream
	}
	stream.readMu.Lock()
	defer stream.readMu.Unlock()
	return c.runtime.Read(id, capacity)
}

func (c *coreInstance) write(id int64, data []byte) error {
	stream := c.stream(id)
	if stream == nil {
		return errStream
	}
	stream.writeMu.Lock()
	defer stream.writeMu.Unlock()
	return c.runtime.Write(id, data)
}

func (c *coreInstance) closeStream(id int64) {
	c.streamsMu.Lock()
	delete(c.streams, id)
	c.streamsMu.Unlock()
	// Close can interrupt a read or write currently holding a direction lock.
	c.runtime.CloseStream(id)
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
