package aegismeter

import (
	"bufio"
	"encoding/json"
	"errors"
	"io"
	"net"
	"sync"
	"testing"
)

type observed map[string]any
type writeFunc func([]byte) (int, error)

func (w writeFunc) Write(b []byte) (int, error) { return w(b) }

func collector(t *testing.T, reject bool) (*Flow, <-chan []observed) {
	t.Helper()
	c, s := net.Pipe()
	t.Cleanup(func() { c.Close(); s.Close() })
	done := make(chan []observed, 1)
	go func() {
		var events []observed
		defer func() { done <- events }()
		scanner := bufio.NewScanner(s)
		for scanner.Scan() {
			var m observed
			if err := json.Unmarshal(scanner.Bytes(), &m); err != nil {
				t.Errorf("invalid frame: %v", err)
				return
			}
			events = append(events, m)
			if reject {
				io.WriteString(s, "{\"ok\":false}\n")
				return
			}
			if _, err := io.WriteString(s, "{\"ok\":true}\n"); err != nil {
				return
			}
			if m["op"] == "finish" {
				return
			}
		}
	}()
	return &Flow{id: "0123456789abcdef0123456789abcdef", conn: c, reader: bufio.NewReader(c), paths: make(map[string]int64)}, done
}

func TestConfirmedPrefixes(t *testing.T) {
	for _, test := range []struct {
		name, path           string
		n, start, end, delta int
		complete             bool
	}{
		{"raw short write", "vision_raw", 3, 0, 8, 3, true},
		{"target short write", "target_tcp", 2, 0, 8, 2, true},
		{"only frame header", "vision_padded", 2, 3, 7, 0, false},
		{"partial framed payload", "vision_padded", 5, 3, 7, 2, false},
		{"payload before failed padding", "vision_padded", 7, 3, 7, 4, true},
		{"partial TLS records without padding", "vision_plain", 6, 0, 8, 6, false},
	} {
		t.Run(test.name, func(t *testing.T) {
			f, done := collector(t, false)
			w := writeFunc(func([]byte) (int, error) { return test.n, io.ErrUnexpectedEOF })
			if n, err := f.Write(w, make([]byte, 8), test.start, test.end, test.path); n != test.n || !errors.Is(err, io.ErrUnexpectedEOF) {
				t.Fatal(n, err)
			}
			if err := f.Finish(); err != nil {
				t.Fatal(err)
			}
			events := <-done
			if test.delta > 0 {
				s := events[0]
				if s["path"] != test.path || s["bytes"] != float64(test.delta) || s["pathBytes"] != float64(test.delta) {
					t.Fatal(s)
				}
			} else if len(events) != 1 {
				t.Fatal(events)
			}
			last := events[len(events)-1]
			if last["op"] != "finish" || last["complete"] != test.complete {
				t.Fatal(last)
			}
		})
	}
}

func TestConcurrentDirections(t *testing.T) {
	f, done := collector(t, false)
	var wg sync.WaitGroup
	for _, path := range []string{"target_tcp", "vision_padded", "vision_raw"} {
		wg.Go(func() {
			if _, err := f.Write(io.Discard, make([]byte, 9), 0, 9, path); err != nil {
				t.Error(err)
			}
		})
	}
	wg.Wait()
	if err := f.Finish(); err != nil {
		t.Fatal(err)
	}
	events := <-done
	last := events[len(events)-1]
	if len(events) != 4 || last["uploadedBytes"] != float64(9) || last["downloadedBytes"] != float64(18) {
		t.Fatal(events)
	}
}

func TestFailedAckStopsNewWrites(t *testing.T) {
	f, done := collector(t, true)
	w := TargetWriter{f, io.Discard}
	if n, err := w.Write([]byte{1}); n != 1 || err == nil {
		t.Fatal(n, err)
	}
	if n, err := w.Write([]byte{2}); n != 0 || err == nil {
		t.Fatal(n, err)
	}
	if err := f.Finish(); err == nil {
		t.Fatal("unexpected confirmed finish")
	}
	events := <-done
	if len(events) != 1 || events[0]["op"] != "sample" {
		t.Fatal(events)
	}
}
