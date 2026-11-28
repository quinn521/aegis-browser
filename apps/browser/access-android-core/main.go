package main

/*
#include <stdint.h>

enum AegisAndroidCoreStatus {
  AEGIS_ANDROID_CORE_OK = 0,
  AEGIS_ANDROID_CORE_INVALID_ARGUMENT = -1,
  AEGIS_ANDROID_CORE_UNAVAILABLE = -2,
  AEGIS_ANDROID_CORE_ERROR = -3,
};
*/
import "C"

import (
	"bytes"
	"unicode/utf8"
	"unsafe"
)

const (
	statusOK          = int(C.AEGIS_ANDROID_CORE_OK)
	statusInvalid     = int(C.AEGIS_ANDROID_CORE_INVALID_ARGUMENT)
	statusUnavailable = int(C.AEGIS_ANDROID_CORE_UNAVAILABLE)
	statusCoreError   = int(C.AEGIS_ANDROID_CORE_ERROR)
)

var cores coreRegistry

// AegisAndroidCoreCreate returns a new isolated, stopped core. Zero indicates
// failure.
//
//export AegisAndroidCoreCreate
func AegisAndroidCoreCreate() C.uint64_t {
	return C.uint64_t(cores.create())
}

// AegisAndroidCoreStart starts the core from a trusted, UTF-8 configuration.
//
//export AegisAndroidCoreStart
func AegisAndroidCoreStart(id C.uint64_t, config *C.char, size C.int) C.int {
	if config == nil || size <= 0 || size > maxTransfer {
		return C.AEGIS_ANDROID_CORE_INVALID_ARGUMENT
	}
	instance := cores.lookup(uint64(id))
	if instance == nil {
		return C.AEGIS_ANDROID_CORE_UNAVAILABLE
	}
	raw := C.GoBytes(unsafe.Pointer(config), size)
	if !utf8.Valid(raw) {
		return C.AEGIS_ANDROID_CORE_INVALID_ARGUMENT
	}
	return C.int(instance.start(raw))
}

// AegisAndroidCoreIsRunning returns 1 for a running core, 0 for a stopped
// core, or a negative status for an unavailable handle.
//
//export AegisAndroidCoreIsRunning
func AegisAndroidCoreIsRunning(id C.uint64_t) C.int {
	instance := cores.lookup(uint64(id))
	if instance == nil {
		return C.AEGIS_ANDROID_CORE_UNAVAILABLE
	}
	if instance.runtime.IsRunning() {
		return 1
	}
	return 0
}

// AegisAndroidCoreDial returns a positive stream ID or a negative status.
// Host names must come from the trusted request owner.
//
//export AegisAndroidCoreDial
func AegisAndroidCoreDial(id C.uint64_t, host *C.char, length C.int, port C.uint16_t) C.int64_t {
	if host == nil || length <= 0 || length > 253 || port == 0 {
		return C.AEGIS_ANDROID_CORE_INVALID_ARGUMENT
	}
	destination := C.GoBytes(unsafe.Pointer(host), length)
	if !utf8.Valid(destination) || bytes.IndexByte(destination, 0) >= 0 {
		return C.AEGIS_ANDROID_CORE_INVALID_ARGUMENT
	}
	instance := cores.lookup(uint64(id))
	if instance == nil {
		return C.AEGIS_ANDROID_CORE_UNAVAILABLE
	}
	stream, err := instance.runtime.Dial(string(destination), int(port))
	if err != nil {
		return C.AEGIS_ANDROID_CORE_ERROR
	}
	return C.int64_t(stream)
}

// AegisAndroidCoreRead returns the byte count, zero at EOF, or a negative
// status. The caller provides a writable buffer for the duration of the call.
//
//export AegisAndroidCoreRead
func AegisAndroidCoreRead(id C.uint64_t, stream C.int64_t, output *C.char, capacity C.int) C.int {
	if stream <= 0 || output == nil || capacity <= 0 || capacity > maxTransfer {
		return C.AEGIS_ANDROID_CORE_INVALID_ARGUMENT
	}
	instance := cores.lookup(uint64(id))
	if instance == nil {
		return C.AEGIS_ANDROID_CORE_UNAVAILABLE
	}
	data, err := instance.runtime.Read(int64(stream), int(capacity))
	if err != nil {
		return C.AEGIS_ANDROID_CORE_ERROR
	}
	copy(unsafe.Slice((*byte)(unsafe.Pointer(output)), int(capacity)), data)
	return C.int(len(data))
}

// AegisAndroidCoreWrite sends a chunk through a stream owned by the supplied core.
//
//export AegisAndroidCoreWrite
func AegisAndroidCoreWrite(id C.uint64_t, stream C.int64_t, input *C.char, size C.int) C.int {
	if stream <= 0 || input == nil || size <= 0 || size > maxTransfer {
		return C.AEGIS_ANDROID_CORE_INVALID_ARGUMENT
	}
	instance := cores.lookup(uint64(id))
	if instance == nil {
		return C.AEGIS_ANDROID_CORE_UNAVAILABLE
	}
	if err := instance.runtime.Write(int64(stream), C.GoBytes(unsafe.Pointer(input), size)); err != nil {
		return C.AEGIS_ANDROID_CORE_ERROR
	}
	return C.AEGIS_ANDROID_CORE_OK
}

// AegisAndroidCoreCloseStream retires a stream. Closing an already closed
// stream in a live core succeeds.
//
//export AegisAndroidCoreCloseStream
func AegisAndroidCoreCloseStream(id C.uint64_t, stream C.int64_t) C.int {
	if stream <= 0 {
		return C.AEGIS_ANDROID_CORE_INVALID_ARGUMENT
	}
	instance := cores.lookup(uint64(id))
	if instance == nil {
		return C.AEGIS_ANDROID_CORE_UNAVAILABLE
	}
	instance.runtime.CloseStream(int64(stream))
	return C.AEGIS_ANDROID_CORE_OK
}

// AegisAndroidCoreDestroy withdraws the handle and stops the runtime and streams.
//
//export AegisAndroidCoreDestroy
func AegisAndroidCoreDestroy(id C.uint64_t) C.int {
	return C.int(cores.close(uint64(id)))
}

func main() {}
