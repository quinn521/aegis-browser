package embeddedcore

import (
	"io"
	"net"
	"strconv"
	"sync"
	"testing"
	"time"

	"github.com/xtls/xray-core/transport/internet"
	"google.golang.org/grpc"
	"google.golang.org/grpc/connectivity"
)

func grpcRelayConfig(port int) string {
	return `{
  "log": {"loglevel": "none"},
  "inbounds": [],
  "outbounds": [{
    "protocol": "vless",
    "tag": "relay",
    "settings": {"vnext": [{"address": "127.0.0.1", "port": ` + strconv.Itoa(port) + `,
      "users": [{"id": "00000000-0000-4000-8000-000000000001", "encryption": "none"}]}]},
    "streamSettings": {"network": "grpc", "grpcSettings": {"serviceName": "probe"}}
  }]
}`
}

func cachedGRPCClients(owner []*internet.MemoryStreamConfig) []*grpc.ClientConn {
	owned := map[*internet.MemoryStreamConfig]bool{}
	for _, settings := range owner {
		owned[settings] = true
	}
	grpcClientsMu.Lock()
	defer grpcClientsMu.Unlock()
	var clients []*grpc.ClientConn
	for key, client := range grpcClients {
		if owned[key.MemoryStreamConfig] {
			clients = append(clients, client)
		}
	}
	return clients
}

func runtimeStreamSettings(r *Runtime) []*internet.MemoryStreamConfig {
	r.mu.Lock()
	defer r.mu.Unlock()
	return append([]*internet.MemoryStreamConfig(nil), r.grpcSettings...)
}

func dialUntilGRPCClientCached(t *testing.T, r *Runtime) ([]*internet.MemoryStreamConfig, *grpc.ClientConn) {
	t.Helper()
	settings := runtimeStreamSettings(r)
	if len(settings) == 0 {
		t.Fatal("runtime does not own outbound stream settings")
	}
	if _, err := r.Dial("example.com", 443); err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		if clients := cachedGRPCClients(settings); len(clients) == 1 {
			return settings, clients[0]
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("dial did not create a cached gRPC client")
	return nil, nil
}

// Late retries of a stopped generation may briefly recreate a client; the
// reap window must release it.
func waitGRPCClientsReleased(t *testing.T, settings []*internet.MemoryStreamConfig, clients ...*grpc.ClientConn) {
	t.Helper()
	deadline := time.Now().Add(grpcRetireWindow + 2*time.Second)
	for {
		cached := cachedGRPCClients(settings)
		shutdown := true
		for _, client := range clients {
			if client.GetState() != connectivity.Shutdown {
				shutdown = false
			}
		}
		if len(cached) == 0 && shutdown {
			return
		}
		if !time.Now().Before(deadline) {
			t.Fatalf("stopped generation kept %d cached gRPC clients (released clients shut down: %v)", len(cached), shutdown)
		}
		time.Sleep(20 * time.Millisecond)
	}
}

func waitReapIdle(t *testing.T) {
	t.Helper()
	deadline := time.Now().Add(grpcRetireWindow + 2*time.Second)
	for {
		lifecycleMu.Lock()
		reaping := grpcReaping
		lifecycleMu.Unlock()
		if !reaping {
			return
		}
		if !time.Now().Before(deadline) {
			t.Fatal("gRPC reap window did not end")
		}
		time.Sleep(20 * time.Millisecond)
	}
}

func TestStopReleasesOnlyItsGenerationsCachedGRPCClients(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	var served sync.WaitGroup
	served.Add(1)
	go func() {
		defer served.Done()
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go func() { _, _ = io.Copy(io.Discard, conn); _ = conn.Close() }()
		}
	}()
	defer served.Wait()
	defer listener.Close()
	config := grpcRelayConfig(listener.Addr().(*net.TCPAddr).Port)

	first, concurrent := NewRuntime(), NewRuntime()
	if err := first.Start(config); err != nil {
		t.Fatal(err)
	}
	defer first.Stop()
	if err := concurrent.Start(config); err != nil {
		t.Fatal(err)
	}
	defer concurrent.Stop()
	firstSettings, firstClient := dialUntilGRPCClientCached(t, first)
	concurrentSettings, concurrentClient := dialUntilGRPCClientCached(t, concurrent)

	if err := first.Stop(); err != nil {
		t.Fatal(err)
	}
	waitGRPCClientsReleased(t, firstSettings, firstClient)
	waitReapIdle(t)
	if clients := cachedGRPCClients(concurrentSettings); len(clients) != 1 || clients[0] != concurrentClient {
		t.Fatal("stopping one runtime released another live runtime's gRPC client")
	}
	if concurrentClient.GetState() == connectivity.Shutdown {
		t.Fatal("stopping one runtime shut down another live runtime's gRPC client")
	}

	// Switching back to the same route starts a new generation with its own client.
	if err := first.Start(config); err != nil {
		t.Fatal(err)
	}
	switchedSettings, switchedClient := dialUntilGRPCClientCached(t, first)
	if switchedClient == firstClient {
		t.Fatal("route switch reused the retired gRPC client")
	}
	if clients := cachedGRPCClients(firstSettings); len(clients) != 0 {
		t.Fatal("route switch revived the retired generation's gRPC client")
	}
	if clients := cachedGRPCClients(concurrentSettings); len(clients) != 1 || clients[0] != concurrentClient {
		t.Fatal("the reap window released a live runtime's gRPC client")
	}

	if err := concurrent.Stop(); err != nil {
		t.Fatal(err)
	}
	if err := first.Stop(); err != nil {
		t.Fatal(err)
	}
	waitGRPCClientsReleased(t, append(concurrentSettings, switchedSettings...), concurrentClient, switchedClient)
	waitReapIdle(t)
	if clients := cachedGRPCClients(append(concurrentSettings, switchedSettings...)); len(clients) != 0 {
		t.Fatalf("stopped runtimes kept %d cached gRPC clients after the reap window", len(clients))
	}
}
