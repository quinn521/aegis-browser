package aegismeter

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"
)

type scriptedQuota struct {
	reply func(map[string]any) map[string]any
}

func (s scriptedQuota) accept(t *testing.T, c net.Conn) {
	defer c.Close()
	scan := bufio.NewScanner(c)
	for scan.Scan() {
		var msg map[string]any
		if err := json.Unmarshal(scan.Bytes(), &msg); err != nil {
			t.Error(err)
			return
		}
		if err := json.NewEncoder(c).Encode(s.reply(msg)); err != nil {
			return
		}
	}
}

func TestQuotaTailFrameAndTicketCommit(t *testing.T) {
	socket, server := net.Pipe()
	device, remote := net.Pipe()
	defer remote.Close()
	sink, receiver := net.Pipe()
	defer receiver.Close()
	_, cancel := context.WithCancel(context.Background())
	defer cancel()
	f := &Flow{id: "0123456789abcdef0123456789abcdef", conn: socket, reader: bufio.NewReader(socket), quota: &quotaGuard{inbound: device, target: sink, expires: time.Now().Add(time.Minute), cancel: cancel, closed: make(chan struct{})}}
	observed := make(chan map[string]any, 4)
	go scriptedQuota{reply: func(m map[string]any) map[string]any {
		observed <- m
		return map[string]any{"ok": true, "granted": 3, "expiresAt": time.Now().Add(time.Minute).UnixMilli()}
	}}.accept(t, server)
	frame := []byte{2, 0, 5, 0, 2, 11, 12, 13, 14, 15, 0, 0}
	out := make(chan []byte, 1)
	go func() { b, _ := io.ReadAll(receiver); out <- b }()
	n, err := f.Write(sink, frame, 5, 10, "vision_padded")
	if !errors.Is(err, errCutoff) || n != 10 {
		t.Fatal(n, err)
	}
	// A valid final frame carries the original command + three payload bytes + two padding bytes.
	if b := <-out; string(b) != string([]byte{2, 0, 3, 0, 2, 11, 12, 13, 0, 0}) {
		t.Fatal(b)
	}
	reserve := <-observed
	commit := <-observed
	if reserve["op"] != "reserve" || reserve["bytes"] != float64(5) || reserve["ticket"] != float64(1) || reserve["direction"] != "down" ||
		commit["op"] != "commit" || commit["bytes"] != float64(3) || commit["ticket"] != reserve["ticket"] || commit["complete"] != true {
		t.Fatal(reserve, commit)
	}
	if err := f.Finish(); err != nil {
		t.Fatal(err)
	}
	last := <-observed
	if last["downloadedBytes"] != float64(3) || last["complete"] != true {
		t.Fatal(last)
	}
}

func TestIdleExpiryClosesBothSockets(t *testing.T) {
	// Use a short pathname even on hosts with deeply nested TMPDIR.
	dir, err := os.MkdirTemp("", "q")
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(dir)
	path := filepath.Join(dir, "q")
	listener, err := net.Listen("unix", path)
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	expiry := time.Now().Add(150 * time.Millisecond)
	go func() {
		c, err := listener.Accept()
		if err == nil {
			scriptedQuota{reply: func(msg map[string]any) map[string]any {
				if msg["op"] != "check" {
					t.Error(msg)
				}
				return map[string]any{"ok": true, "expiresAt": expiry.UnixMilli()}
			}}.accept(t, c)
		}
	}()
	client, peer := net.Pipe()
	target, origin := net.Pipe()
	defer peer.Close()
	defer origin.Close()
	q, err := startGuard(context.Background(), path, "flow", client, expiry.UnixMilli())
	if err != nil {
		t.Fatal(err)
	}
	f := Flow{quota: q}
	if err := f.BindTarget(target); err != nil {
		t.Fatal(err)
	}
	for _, conn := range []net.Conn{peer, origin} {
		conn.SetReadDeadline(expiry.Add(200 * time.Millisecond))
		if _, err := conn.Read(make([]byte, 1)); err != io.EOF {
			t.Fatal(err)
		}
	}
	if time.Now().Before(time.UnixMilli(expiry.UnixMilli())) {
		t.Fatal("cut off before expiry")
	}
}

func TestShortenedExpiryHasIndependentDeadline(t *testing.T) {
	client, peer := net.Pipe()
	target, origin := net.Pipe()
	defer peer.Close()
	defer origin.Close()
	ctx, cancel := context.WithCancel(context.Background())
	q := &quotaGuard{inbound: client, target: target, expires: time.Now().Add(time.Minute), closed: make(chan struct{}), expiryChanged: make(chan struct{}, 1), cancel: cancel}
	defer q.stop()
	go q.expire(ctx)
	expiry := time.Now().Add(30 * time.Millisecond)
	q.shorten(expiry.UnixMilli())
	for _, conn := range []net.Conn{peer, origin} {
		conn.SetReadDeadline(expiry.Add(100 * time.Millisecond))
		if _, err := conn.Read(make([]byte, 1)); err != io.EOF {
			t.Fatal("shortened expiry did not close socket", err)
		}
	}
	if time.Now().Before(time.UnixMilli(expiry.UnixMilli())) {
		t.Fatal("socket closed before shortened expiry")
	}
}

func TestQuotaInvalidSpanNeverWrites(t *testing.T) {
	for _, test := range []struct {
		start, end int
		path       string
	}{
		{-1, 1, "target_tcp"},
		{0, 5, "target_tcp"},
		{2, 1, "vision_plain"},
		{1, 3, "vision_raw"},
		{0, 3, "vision_padded"},
	} {
		client, peer := net.Pipe()
		_, cancel := context.WithCancel(context.Background())
		q := &quotaGuard{inbound: client, expires: time.Now().Add(time.Minute), closed: make(chan struct{}), cancel: cancel}
		f := &Flow{quota: q}
		w := writeFunc(func([]byte) (int, error) {
			t.Fatal("invalid target span reached writer")
			return 0, nil
		})
		if n, err := f.Write(w, make([]byte, 4), test.start, test.end, test.path); n != 0 || !errors.Is(err, errCutoff) {
			t.Fatal(n, err)
		}
		peer.Close()
	}
}
