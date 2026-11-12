// Package tsugumi gives a Card owned values and guarded, call-scoped host access.
package tsugumi

/*
#include "host.h"
*/
import "C"

import (
	"errors"
	"fmt"
	"strings"
	"sync"
	"time"
	"unsafe"
)

// Failure records a structured failure understood by the worker.
type Failure struct{ Kind, Message string }

func (e *Failure) Error() string      { return e.Message }
func Fail(kind, message string) error { return &Failure{kind, message} }

var ErrClosed = errors.New("the Card invocation has ended")

// Context copies share one lifetime guard, so retaining or copying a Context
// cannot retain a host pointer. All callbacks are serialized across goroutines.
type Context struct{ state *callState }
type callState struct {
	mu         sync.Mutex
	raw        *C.tsugumi_call
	info       [5]string
	invocation uint64
	done       chan struct{}
	cancel     sync.Once
	tasks      sync.WaitGroup
	sealed     bool
	failure    chan error
}

func (ctx *Context) host(fn func(*C.tsugumi_call) error) error {
	if ctx == nil || ctx.state == nil {
		return ErrClosed
	}
	s := ctx.state
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.raw == nil {
		return ErrClosed
	}
	return fn(s.raw)
}
func (ctx *Context) close() {
	s := ctx.state
	s.mu.Lock()
	s.raw = nil
	s.sealed = true
	s.mu.Unlock()
	s.cancel.Do(func() { close(s.done) })
}
func (ctx *Context) RunID() string              { return ctx.state.info[0] }
func (ctx *Context) Card() string               { return ctx.state.info[1] }
func (ctx *Context) WorkingDirectory() string   { return ctx.state.info[2] }
func (ctx *Context) TemporaryDirectory() string { return ctx.state.info[3] }
func (ctx *Context) OutputDirectory() string    { return ctx.state.info[4] }
func (ctx *Context) Invocation() uint64         { return ctx.state.invocation }
func (ctx *Context) Done() <-chan struct{}      { return ctx.state.done }
func (ctx *Context) Cancelled() bool {
	cancelled := true
	_ = ctx.host(func(call *C.tsugumi_call) error { cancelled = C.tsugumi_go_cancelled(call) != 0; return nil })
	select {
	case <-ctx.Done():
		return true
	default:
		return cancelled
	}
}

// Remaining returns -1 without a deadline, and zero after the call ends.
func (ctx *Context) Remaining() time.Duration {
	var ms int64
	_ = ctx.host(func(call *C.tsugumi_call) error { ms = int64(C.tsugumi_go_remaining(call)); return nil })
	if ms < 0 {
		return -1
	}
	return time.Duration(ms) * time.Millisecond
}

// Go starts a managed task. Its errors and panics fail the invocation; successful
// calls wait for every task. Schedule tasks before run returns, and select on
// Done for cancellation. An uncooperative task is stopped with its worker.
func (ctx *Context) Go(fn func() error) error {
	s := ctx.state
	s.mu.Lock()
	if s.raw == nil || s.sealed {
		s.mu.Unlock()
		return ErrClosed
	}
	s.tasks.Add(1)
	s.mu.Unlock()
	go func() {
		defer s.tasks.Done()
		if err := recoverError(fn); err != nil {
			select {
			case s.failure <- err:
			default:
			}
			s.cancel.Do(func() { close(s.done) })
		}
	}()
	return nil
}
func (ctx *Context) wait() error {
	s := ctx.state
	s.mu.Lock()
	s.sealed = true
	s.mu.Unlock()
	joined := make(chan struct{})
	go func() { s.tasks.Wait(); close(joined) }()
	select {
	case <-joined:
		select {
		case err := <-s.failure:
			return err
		default:
			return nil
		}
	case err := <-s.failure:
		return err
	case <-s.done:
		select {
		case err := <-s.failure:
			return err
		default:
			return Fail("cancelled", "Card cancelled")
		}
	}
}
func recoverError(fn func() error) (err error) {
	defer func() {
		if value := recover(); value != nil {
			err = Fail("worker_failed", fmt.Sprintf("Go panic: %v", value))
		}
	}()
	return fn()
}
func cstring(value string) (*C.char, error) {
	if strings.IndexByte(value, 0) >= 0 {
		return nil, Fail("output_invalid", "text contains a NUL byte")
	}
	return C.CString(value), nil
}
func accepted(value C.int) error {
	if value != 0 {
		return Fail("output_invalid", "the worker rejected the value")
	}
	return nil
}
func copied(ptr *C.uchar, n C.size_t) ([]byte, error) {
	if uint64(n) > uint64(1<<31-1) {
		return nil, Fail("type_mismatch", "input exceeds Go's copy limit")
	}
	if ptr == nil {
		return nil, nil
	}
	return C.GoBytes(unsafe.Pointer(ptr), C.int(n)), nil
}
func (ctx *Context) inputBytes(port string) ([]byte, error) {
	name, err := cstring(port)
	if err != nil {
		return nil, err
	}
	defer C.free(unsafe.Pointer(name))
	var bytes []byte
	err = ctx.host(func(call *C.tsugumi_call) error {
		var n C.size_t
		p := C.tsugumi_go_input(call, name, &n)
		bytes, err = copied(p, n)
		return err
	})
	return bytes, err
}
func (ctx *Context) paramsBytes() ([]byte, error) {
	var bytes []byte
	var err error
	err = ctx.host(func(call *C.tsugumi_call) error {
		var n C.size_t
		p := C.tsugumi_go_params(call, &n)
		bytes, err = copied(p, n)
		return err
	})
	return bytes, err
}
func bytesPointer(bytes []byte) *C.uchar {
	if len(bytes) == 0 {
		return nil
	}
	return (*C.uchar)(unsafe.Pointer(&bytes[0]))
}
func (ctx *Context) emit(port string, bytes []byte) error {
	name, err := cstring(port)
	if err != nil {
		return err
	}
	defer C.free(unsafe.Pointer(name))
	// The host copies bytes synchronously; no Go pointer is retained by C.
	return ctx.host(func(call *C.tsugumi_call) error {
		return accepted(C.tsugumi_go_emit(call, name, bytesPointer(bytes), C.size_t(len(bytes))))
	})
}
func (ctx *Context) Log(text string) error {
	value, err := cstring(text)
	if err != nil {
		return err
	}
	defer C.free(unsafe.Pointer(value))
	return ctx.host(func(call *C.tsugumi_call) error { C.tsugumi_go_log(call, value); return nil })
}
func (ctx *Context) text(text string, markdown bool) error {
	value, err := cstring(text)
	if err != nil {
		return err
	}
	defer C.free(unsafe.Pointer(value))
	flag := C.int(0)
	if markdown {
		flag = 1
	}
	return ctx.host(func(call *C.tsugumi_call) error { return accepted(C.tsugumi_go_text(call, value, flag)) })
}
func (ctx *Context) DisplayText(text string) error     { return ctx.text(text, false) }
func (ctx *Context) DisplayMarkdown(text string) error { return ctx.text(text, true) }
func (ctx *Context) DisplayScalar(value float64, label string) error {
	name, err := cstring(label)
	if err != nil {
		return err
	}
	defer C.free(unsafe.Pointer(name))
	return ctx.host(func(call *C.tsugumi_call) error { return accepted(C.tsugumi_go_scalar(call, C.double(value), name)) })
}
func (ctx *Context) DisplayModule(name, json string) error {
	a, err := cstring(name)
	if err != nil {
		return err
	}
	defer C.free(unsafe.Pointer(a))
	b, err := cstring(json)
	if err != nil {
		return err
	}
	defer C.free(unsafe.Pointer(b))
	return ctx.host(func(call *C.tsugumi_call) error { return accepted(C.tsugumi_go_module(call, a, b)) })
}
func (ctx *Context) DisplayImage(media string, bytes []byte, alt string) error {
	a, err := cstring(media)
	if err != nil {
		return err
	}
	defer C.free(unsafe.Pointer(a))
	b, err := cstring(alt)
	if err != nil {
		return err
	}
	defer C.free(unsafe.Pointer(b))
	return ctx.host(func(call *C.tsugumi_call) error {
		return accepted(C.tsugumi_go_image(call, a, bytesPointer(bytes), C.size_t(len(bytes)), b))
	})
}
func (ctx *Context) DisplayLineChart(id, label string, x, y []float64) error {
	if len(x) != len(y) {
		return Fail("output_invalid", "line coordinates must have equal lengths")
	}
	a, err := cstring(id)
	if err != nil {
		return err
	}
	defer C.free(unsafe.Pointer(a))
	b, err := cstring(label)
	if err != nil {
		return err
	}
	defer C.free(unsafe.Pointer(b))
	var px, py *C.double
	if len(x) > 0 {
		px = (*C.double)(unsafe.Pointer(&x[0]))
		py = (*C.double)(unsafe.Pointer(&y[0]))
	}
	return ctx.host(func(call *C.tsugumi_call) error {
		return accepted(C.tsugumi_go_line(call, a, b, px, py, C.size_t(len(x))))
	})
}
func (ctx *Context) DisplayPoints(x, y, z []float32, colors []uint32) error {
	if len(x) != len(y) || len(x) != len(z) || (colors != nil && len(x) != len(colors)) {
		return Fail("output_invalid", "point coordinates and colors must have equal lengths")
	}
	var px, py, pz *C.float
	var pc *C.uint32_t
	if len(x) > 0 {
		px = (*C.float)(unsafe.Pointer(&x[0]))
		py = (*C.float)(unsafe.Pointer(&y[0]))
		pz = (*C.float)(unsafe.Pointer(&z[0]))
	}
	if len(colors) > 0 {
		pc = (*C.uint32_t)(unsafe.Pointer(&colors[0]))
	}
	return ctx.host(func(call *C.tsugumi_call) error {
		return accepted(C.tsugumi_go_points(call, px, py, pz, pc, C.size_t(len(x))))
	})
}
func cstrings(values []string) (**C.char, func(), error) {
	if len(values) == 0 {
		return nil, func() {}, nil
	}
	memory := C.malloc(C.size_t(len(values)) * C.size_t(unsafe.Sizeof(uintptr(0))))
	pointers := unsafe.Slice((**C.char)(memory), len(values))
	count := 0
	cleanup := func() {
		for _, p := range pointers[:count] {
			C.free(unsafe.Pointer(p))
		}
		C.free(memory)
	}
	for i, value := range values {
		p, err := cstring(value)
		if err != nil {
			cleanup()
			return nil, func() {}, err
		}
		pointers[i] = p
		count++
	}
	return (**C.char)(memory), cleanup, nil
}
func (ctx *Context) DisplayTable(columns []string, rows [][]string) error {
	cells := []string{}
	for _, row := range rows {
		if len(row) != len(columns) {
			return Fail("output_invalid", "table rows must match columns")
		}
		cells = append(cells, row...)
	}
	a, freeA, err := cstrings(columns)
	if err != nil {
		return err
	}
	defer freeA()
	b, freeB, err := cstrings(cells)
	if err != nil {
		return err
	}
	defer freeB()
	return ctx.host(func(call *C.tsugumi_call) error {
		return accepted(C.tsugumi_go_table(call, a, C.size_t(len(columns)), b, C.size_t(len(rows))))
	})
}

// User-defined Error/Unwrap methods may themselves panic. Error reporting must
// still return a failure through C instead of starting a second unwind.
func describe(err error) (kind, message string) {
	kind, message = "worker_failed", "Go error reporting panicked"
	defer func() { _ = recover() }()
	var failure *Failure
	if errors.As(err, &failure) {
		kind = failure.Kind
	}
	message = err.Error()
	return
}

// Invoke is called only by the generated C entry points. The worker guarantees
// raw remains valid until this function returns. The call pointer never survives
// close, even if user code keeps a Context or starts its own goroutine.
func Invoke(raw unsafe.Pointer, body func(*Context) error) (result int) {
	call := (*C.tsugumi_call)(raw)
	if C.tsugumi_go_check(call) == 0 {
		return 1
	}
	state := &callState{raw: call, done: make(chan struct{}), failure: make(chan error, 1)}
	for i := range state.info {
		state.info[i] = C.GoString(C.tsugumi_go_info(call, C.uint(i)))
	}
	state.invocation = uint64(C.tsugumi_go_invocation(call))
	ctx := &Context{state: state}
	monitorDone := make(chan struct{})
	go func() {
		defer close(monitorDone)
		ticker := time.NewTicker(10 * time.Millisecond)
		defer ticker.Stop()
		for {
			select {
			case <-state.done:
				return
			case <-ticker.C:
				if ctx.Cancelled() {
					state.cancel.Do(func() { close(state.done) })
					return
				}
			}
		}
	}()
	defer func() { ctx.close(); <-monitorDone }()
	err := recoverError(func() error {
		if err := body(ctx); err != nil {
			return err
		}
		return ctx.wait()
	})
	if err == nil {
		return 0
	}
	kind, message := describe(err)
	message = strings.ReplaceAll(message, "\x00", "\\0")
	k := C.CString(strings.ReplaceAll(kind, "\x00", "\\0"))
	defer C.free(unsafe.Pointer(k))
	m := C.CString(message)
	defer C.free(unsafe.Pointer(m))
	_ = ctx.host(func(call *C.tsugumi_call) error { C.tsugumi_go_log(call, m); C.tsugumi_go_fail(call, k, m); return nil })
	return 1
}
