// Package embeddedcore is the in-process Access core bridge.
//
// It exposes StartInstance / Dial / stream I/O for gomobile. The product
// config must contain only the admitted reality_vision outbound — no
// DIRECT/freedom fallback and no second HTTP inbound.
package embeddedcore

import (
	"context"
	"errors"
	"io"
	"strings"
	"sync"
	"sync/atomic"

	xraylog "github.com/xtls/xray-core/app/log"
	clog "github.com/xtls/xray-core/common/log"
	"github.com/xtls/xray-core/common/net"
	"github.com/xtls/xray-core/core"
	_ "github.com/xtls/xray-core/main/distro/all"
)

// Runtime owns an isolated Xray instance and its streams.
// Legacy package entry points below retain their original single-runtime API.
type Runtime struct {
	mu              sync.Mutex
	instance        *core.Instance
	streams         map[int64]*streamHandle
	nextID          atomic.Int64
	instanceContext context.Context
	instanceCancel  context.CancelFunc
}

func NewRuntime() *Runtime { return &Runtime{streams: map[int64]*streamHandle{}} }

var legacy = NewRuntime()

// Xray initialization and the selected logger are process-wide even though
// instance routing and stream tables are independent.
var lifecycleMu sync.Mutex
var liveInstances []*core.Instance

type discardedLog struct{}

func (discardedLog) Handle(clog.Message) {}

func selectLiveLogger() {
	for i := len(liveInstances) - 1; i >= 0; i-- {
		if handler, ok := liveInstances[i].GetFeature((*xraylog.Instance)(nil).Type()).(clog.Handler); ok {
			clog.RegisterHandler(handler)
			return
		}
	}
	clog.RegisterHandler(discardedLog{})
}

var (
	errNotRunning     = errors.New("embedded core is not running")
	errAlreadyRunning = errors.New("embedded core is already running")
	errBadStream      = errors.New("unknown or closed stream")
	errEmptyDial      = errors.New("dial host is empty")
)

type streamHandle struct {
	conn   net.Conn
	cancel context.CancelFunc
}

// Start boots one Xray instance from JSON config bytes (UTF-8).
// Calling Start after Stop creates a fresh instance rather than reusing it.
func (r *Runtime) Start(configJSON string) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.instance != nil {
		return errAlreadyRunning
	}
	lifecycleMu.Lock()
	defer lifecycleMu.Unlock()
	inst, err := core.StartInstance("json", []byte(configJSON))
	if err != nil {
		selectLiveLogger()
		return err
	}
	r.instance = inst
	liveInstances = append(liveInstances, inst)
	r.instanceContext, r.instanceCancel = context.WithCancel(context.Background())
	return nil
}

// Stop closes every open dial stream and the running instance.
func (r *Runtime) Stop() error {
	r.mu.Lock()
	inst, cancel, closing := r.instance, r.instanceCancel, r.streams
	r.instance, r.instanceContext, r.instanceCancel = nil, nil, nil
	r.streams = map[int64]*streamHandle{}
	r.mu.Unlock()
	if cancel != nil {
		cancel()
	}
	for _, handle := range closing {
		handle.cancel()
		_ = handle.conn.Close()
	}
	if inst != nil {
		lifecycleMu.Lock()
		defer lifecycleMu.Unlock()
		for i, live := range liveInstances {
			if live == inst {
				copy(liveInstances[i:], liveInstances[i+1:])
				liveInstances[len(liveInstances)-1] = nil
				liveInstances = liveInstances[:len(liveInstances)-1]
				break
			}
		}
		if len(liveInstances) > 0 {
			selectLiveLogger()
		}
		err := inst.Close()
		if len(liveInstances) == 0 {
			selectLiveLogger()
		}
		return err
	}
	return nil
}

// IsRunning reports whether the embedded instance is live.
func (r *Runtime) IsRunning() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.instance != nil && r.instance.IsRunning()
}

// Dial opens one TCP path through the running instance to host:port.
// Returns a stream id used by Read / Write / CloseStream.
func (r *Runtime) Dial(host string, port int) (int64, error) {
	host = strings.TrimSpace(host)
	if host == "" || port <= 0 || port > 65535 {
		return 0, errEmptyDial
	}
	r.mu.Lock()
	inst, parent := r.instance, r.instanceContext
	r.mu.Unlock()
	if inst == nil || !inst.IsRunning() {
		return 0, errNotRunning
	}
	ctx, cancel := context.WithCancel(parent)
	dest := net.TCPDestination(net.ParseAddress(host), net.Port(port))
	conn, err := core.Dial(ctx, inst, dest)
	if err != nil {
		cancel()
		return 0, err
	}
	id := r.nextID.Add(1)
	r.mu.Lock()
	if r.instance != inst {
		r.mu.Unlock()
		cancel()
		_ = conn.Close()
		return 0, errNotRunning
	}
	r.streams[id] = &streamHandle{conn: conn, cancel: cancel}
	r.mu.Unlock()
	return id, nil
}

// Read returns up to maxBytes from the stream. An empty slice with a nil
// error means EOF. A closed stream returns an error.
func (r *Runtime) Read(streamID int64, maxBytes int) ([]byte, error) {
	if maxBytes <= 0 {
		maxBytes = 16 * 1024
	}
	handle, err := r.lookup(streamID)
	if err != nil {
		return nil, err
	}
	buf := make([]byte, maxBytes)
	n, readErr := handle.conn.Read(buf)
	if n > 0 {
		return buf[:n], nil
	}
	if readErr == io.EOF {
		return []byte{}, nil
	}
	if readErr != nil {
		return nil, readErr
	}
	return []byte{}, nil
}

// Write sends the whole chunk on the stream.
func (r *Runtime) Write(streamID int64, data []byte) error {
	handle, err := r.lookup(streamID)
	if err != nil {
		return err
	}
	_, err = handle.conn.Write(data)
	return err
}

// CloseStream closes one dial stream. Idempotent for unknown ids.
func (r *Runtime) CloseStream(streamID int64) {
	r.mu.Lock()
	handle, ok := r.streams[streamID]
	if ok {
		delete(r.streams, streamID)
	}
	r.mu.Unlock()
	if !ok {
		return
	}
	handle.cancel()
	_ = handle.conn.Close()
}

func (r *Runtime) lookup(streamID int64) (*streamHandle, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	handle, ok := r.streams[streamID]
	if !ok {
		return nil, errBadStream
	}
	return handle, nil
}

// Version returns the linked Xray-core version string.
func Version() string {
	return core.Version()
}

// Completion is retained by the native operation until exactly one result is
// delivered. Async entry points return immediately; socket waits park goroutines,
// never Swift cooperative workers or libdispatch worker threads.
type Completion interface {
	Complete(value int64, data []byte, failure string)
}

func complete(c Completion, value int64, data []byte, err error) {
	failure := ""
	if err != nil {
		failure = err.Error()
	}
	c.Complete(value, data, failure)
}

func (r *Runtime) StartAsync(configJSON string, c Completion) {
	go func() { complete(c, 0, nil, r.Start(configJSON)) }()
}
func (r *Runtime) DialAsync(host string, port int, c Completion) {
	go func() { id, err := r.Dial(host, port); complete(c, id, nil, err) }()
}
func (r *Runtime) ReadAsync(streamID int64, maxBytes int, c Completion) {
	go func() { data, err := r.Read(streamID, maxBytes); complete(c, 0, data, err) }()
}

// Gomobile lends argument bytes only for this call; own them before returning.
func (r *Runtime) WriteAsync(streamID int64, data []byte, c Completion) {
	data = append([]byte(nil), data...)
	go func() { complete(c, 0, nil, r.Write(streamID, data)) }()
}
func (r *Runtime) StopAsync(c Completion) {
	go func() { complete(c, 0, nil, r.Stop()) }()
}
func (r *Runtime) CloseStreamAsync(streamID int64) {
	go r.CloseStream(streamID)
}

func Start(configJSON string) error                  { return legacy.Start(configJSON) }
func Stop() error                                    { return legacy.Stop() }
func IsRunning() bool                                { return legacy.IsRunning() }
func Dial(host string, port int) (int64, error)      { return legacy.Dial(host, port) }
func Read(id int64, maxBytes int) ([]byte, error)    { return legacy.Read(id, maxBytes) }
func Write(id int64, data []byte) error              { return legacy.Write(id, data) }
func CloseStream(id int64)                           { legacy.CloseStream(id) }
func StartAsync(configJSON string, c Completion)     { legacy.StartAsync(configJSON, c) }
func StopAsync(c Completion)                         { legacy.StopAsync(c) }
func DialAsync(host string, port int, c Completion)  { legacy.DialAsync(host, port, c) }
func ReadAsync(id int64, maxBytes int, c Completion) { legacy.ReadAsync(id, maxBytes, c) }
func WriteAsync(id int64, data []byte, c Completion) { legacy.WriteAsync(id, data, c) }
func CloseStreamAsync(id int64)                      { legacy.CloseStreamAsync(id) }
