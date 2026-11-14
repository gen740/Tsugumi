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

// callScope copies share one lifetime guard, so retaining or copying a callScope
// cannot retain a host pointer. All callbacks are serialized across goroutines.
type callScope struct{ state *callState }
type callState struct {
 info [5]string
 invocation uint64
	mu         sync.Mutex
	raw        *C.tsugumi_call
	done       chan struct{}
	cancel     sync.Once
	tasks      sync.WaitGroup
	sealed     bool
	failure    chan error
}

func (scope *callScope) host(fn func(*C.tsugumi_call) error) error {
	if scope == nil || scope.state == nil {
		return ErrClosed
	}
	s := scope.state
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.raw == nil {
		return ErrClosed
	}
	return fn(s.raw)
}
func (scope *callScope) close() {
	s := scope.state
	s.mu.Lock()
	s.raw = nil
	s.sealed = true
	s.mu.Unlock()
	s.cancel.Do(func() { close(s.done) })
}
func (scope *callScope) RunID() string              { return scope.state.info[0] }
func (scope *callScope) Card() string               { return scope.state.info[1] }
func (scope *callScope) WorkingDirectory() string   { return scope.state.info[2] }
func (scope *callScope) TemporaryDirectory() string { return scope.state.info[3] }
func (scope *callScope) OutputDirectory() string    { return scope.state.info[4] }
func (scope *callScope) Invocation() uint64         { return scope.state.invocation }
func (scope *callScope) Done() <-chan struct{}      { return scope.state.done }
func (scope *callScope) Cancelled() bool {
	cancelled := true
	_ = scope.host(func(call *C.tsugumi_call) error { cancelled = C.tsugumi_go_cancelled(call) != 0; return nil })
	select {
	case <-scope.Done():
		return true
	default:
		return cancelled
	}
}

// Remaining returns -1 without a deadline, and zero after the call ends.
func (scope *callScope) Remaining() time.Duration {
	var ms int64
	_ = scope.host(func(call *C.tsugumi_call) error { ms = int64(C.tsugumi_go_remaining(call)); return nil })
	if ms < 0 {
		return -1
	}
	return time.Duration(ms) * time.Millisecond
}

// Go starts a managed task. Its errors and panics fail the invocation; successful
// calls wait for every task. Schedule tasks before run returns, and select on
// Done for cancellation. An uncooperative task is stopped with its worker.
func (scope *callScope) Go(fn func() error) error {
	s := scope.state
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
func (scope *callScope) wait() error {
	s := scope.state
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
func (scope *callScope) inputBytes(port string) ([]byte, error) {
	name, err := cstring(port)
	if err != nil {
		return nil, err
	}
	defer C.free(unsafe.Pointer(name))
	var bytes []byte
	err = scope.host(func(call *C.tsugumi_call) error {
		var n C.size_t
		p := C.tsugumi_go_input(call, name, &n)
		bytes, err = copied(p, n)
		return err
	})
	return bytes, err
}
func (scope *callScope) paramsBytes() ([]byte, error) {
	var bytes []byte
	var err error
	err = scope.host(func(call *C.tsugumi_call) error {
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
func (scope *callScope) emit(port string, bytes []byte) error {
	name, err := cstring(port)
	if err != nil {
		return err
	}
	defer C.free(unsafe.Pointer(name))
	// The host copies bytes synchronously; no Go pointer is retained by C.
	return scope.host(func(call *C.tsugumi_call) error {
		return accepted(C.tsugumi_go_emit(call, name, bytesPointer(bytes), C.size_t(len(bytes))))
	})
}
func (scope *callScope) Log(text string) error {
	value, err := cstring(text)
	if err != nil {
		return err
	}
	defer C.free(unsafe.Pointer(value))
	return scope.host(func(call *C.tsugumi_call) error { C.tsugumi_go_log(call, value); return nil })
}
func (scope *callScope) sendText(text string, markdown bool) error {
	value, err := cstring(text)
	if err != nil {
		return err
	}
	defer C.free(unsafe.Pointer(value))
	flag := C.int(0)
	if markdown {
		flag = 1
	}
	return scope.host(func(call *C.tsugumi_call) error { return accepted(C.tsugumi_go_text(call, value, flag)) })
}
func (scope *callScope) Text(text string) error     { return scope.sendText(text, false) }
func (scope *callScope) Markdown(text string) error { return scope.sendText(text, true) }
func (scope *callScope) Scalar(value float64, label string) error {
	name, err := cstring(label)
	if err != nil {
		return err
	}
	defer C.free(unsafe.Pointer(name))
	return scope.host(func(call *C.tsugumi_call) error { return accepted(C.tsugumi_go_scalar(call, C.double(value), name)) })
}
func (scope *callScope) Module(name, json string) error {
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
	return scope.host(func(call *C.tsugumi_call) error { return accepted(C.tsugumi_go_module(call, a, b)) })
}
func (scope *callScope) Image(media string, bytes []byte, alt string) error {
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
	return scope.host(func(call *C.tsugumi_call) error {
		return accepted(C.tsugumi_go_image(call, a, bytesPointer(bytes), C.size_t(len(bytes)), b))
	})
}
func (scope *callScope) LineChart(id, label string, x, y []float64) error {
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
	return scope.host(func(call *C.tsugumi_call) error {
		return accepted(C.tsugumi_go_line(call, a, b, px, py, C.size_t(len(x))))
	})
}
func (scope *callScope) Points(x, y, z []float32, colors []uint32) error {
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
	return scope.host(func(call *C.tsugumi_call) error {
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
func (scope *callScope) Table(columns []string, rows [][]string) error {
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
	return scope.host(func(call *C.tsugumi_call) error {
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
// close, even if user code keeps a callScope or starts its own goroutine.
func Invoke(raw unsafe.Pointer, body func(*callScope) error) (result int) {
	call := (*C.tsugumi_call)(raw)
	if C.tsugumi_go_check(call) == 0 {
		return 1
	}
	state := &callState{raw: call, done: make(chan struct{}), failure: make(chan error, 1)}
	scope := &callScope{state: state}
    for i := range state.info { state.info[i] = C.GoString(C.tsugumi_go_info(call, C.uint(i))) }
    state.invocation = uint64(C.tsugumi_go_invocation(call))
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
				if scope.Cancelled() {
					state.cancel.Do(func() { close(state.done) })
					return
				}
			}
		}
	}()
	defer func() { scope.close(); <-monitorDone }()
	err := recoverError(func() error {
		if err := body(scope); err != nil {
			return err
		}
		return scope.wait()
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
	_ = scope.host(func(call *C.tsugumi_call) error { C.tsugumi_go_log(call, m); C.tsugumi_go_fail(call, k, m); return nil })
	return 1
}

func (scope *callScope) finishOutputs() error {
  return scope.host(func(call *C.tsugumi_call) error {
    if C.tsugumi_go_finish_outputs(call) != 0 { return Fail("output_invalid", "required outputs were not emitted") }
    return nil
  })
}

// Report publishes presentation and log side effects during the call.
type Report struct{ scope *callScope }

// Run exposes invocation facts and cooperative work through the same guard.
type Run struct { scope *callScope }
func (run *Run) RunID() string { return run.scope.RunID() }
func (run *Run) Card() string { return run.scope.Card() }
func (run *Run) Invocation() uint64 { return run.scope.Invocation() }
func (run *Run) WorkingDirectory() string { return run.scope.WorkingDirectory() }
func (run *Run) TemporaryDirectory() string { return run.scope.TemporaryDirectory() }
func (run *Run) OutputDirectory() string { return run.scope.OutputDirectory() }
func (run *Run) Done() <-chan struct{} { return run.scope.Done() }
func (run *Run) Cancelled() bool { return run.scope.Cancelled() }
func (run *Run) Remaining() time.Duration { return run.scope.Remaining() }
func (run *Run) Go(fn func() error) error { return run.scope.Go(fn) }

func (report *Report) Log(text string) error      { return report.scope.Log(text) }
func (report *Report) Text(text string) error     { return report.scope.Text(text) }
func (report *Report) Markdown(text string) error { return report.scope.Markdown(text) }
func (report *Report) Scalar(value float64, label string) error {
	return report.scope.Scalar(value, label)
}
func (report *Report) Module(name, json string) error { return report.scope.Module(name, json) }
func (report *Report) Image(media string, bytes []byte, alt string) error {
	return report.scope.Image(media, bytes, alt)
}
func (report *Report) LineChart(id, label string, x, y []float64) error {
	return report.scope.LineChart(id, label, x, y)
}
func (report *Report) Points(x, y, z []float32, colors []uint32) error {
	return report.scope.Points(x, y, z, colors)
}
func (report *Report) Table(columns []string, rows [][]string) error {
	return report.scope.Table(columns, rows)
}
