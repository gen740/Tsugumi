#ifndef TSUGUMI_CARD_ABI_H
#define TSUGUMI_CARD_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TSUGUMI_ABI_VERSION 0u

#define TSUGUMI_MESSAGE_MAX (1u << 24)
typedef struct tsugumi_bytes { const uint8_t* data; size_t size; } tsugumi_bytes;
typedef struct { uint8_t bytes[16]; } tsugumi_artifact_id;
#define TSUGUMI_ARTIFACT_KEEP 1u
typedef struct { uint8_t bytes[16]; } tsugumi_view_session;
typedef struct { uint8_t bytes[16]; } tsugumi_route;
typedef struct {
  uint32_t kind;                 /* 0 message, 1 close */
  tsugumi_route route;
  tsugumi_view_session session;
  tsugumi_bytes data;            /* empty on close */
} tsugumi_view_event;

enum {
  TSUGUMI_VIEW_PANEL = 0,
  TSUGUMI_VIEW_WINDOW = 1,
  TSUGUMI_VIEW_FULLSCREEN = 2,
};

typedef struct tsugumi_effects {
  /* View effects are valid from any thread for the life of the process. */
  /* Open a declared view. 0 ok, <0 error. Both dimensions may be nonzero
   * only in WINDOW mode and are in logical pixels. 0,0 lets the UI choose. */
  int (*view_open)(void* host, const char* view, uint32_t mode, uint32_t width,
                   uint32_t height, tsugumi_route* route,
                   tsugumi_view_session* session);
  /* Register a mailbox for a session, or drop a route. 0 ok, <0 error. */
  int (*route_register)(void* host, const tsugumi_view_session* session,
                        tsugumi_route* route);
  int (*route_unregister)(void* host, const tsugumi_route* route);
  /* 0 ok (including a closed window), <0 error. Blocks, cancellably, while
   * the next hop has no room for one max-size message. Oversize is an error. */
  int (*send)(void* host, const tsugumi_view_session* to, const uint8_t* bytes,
              size_t size);
  /* 0 event, 1 timeout, 2 cancelled, <0 error. A negative timeout waits
   * indefinitely. Event bytes are valid until the next recv on this thread. */
  int (*recv)(void* host, tsugumi_view_event* out, int64_t timeout_ms);
  /* The rest are valid only during the call. */
  int (*cancelled)(void* host);
  int64_t (*remaining_ms)(void* host);            /* -1 = no deadline */
  int (*artifact_create)(void* host, uint32_t flags, tsugumi_artifact_id* id, tsugumi_bytes* path);
      /* new writable directory for this call under a new random id, sealed read-only when the call returns; path is NUL-terminated, valid during the call. flags: TSUGUMI_ARTIFACT_KEEP keeps the artifact after its run; without it the artifact exists only while its run runs (later Cards of the same run can open it) and is removed, with its record, when the run ends. Unknown flag bits are an error. 0 ok, <0 error. */
  int (*artifact_open)(void* host, const tsugumi_artifact_id* id, tsugumi_bytes* path);
      /* read-only directory of a sealed artifact of this project; knowing the id is enough. 0 ok, <0 error: unknown id, not sealed yet, not kept after its run, or deleted. */
} tsugumi_effects;
typedef struct tsugumi_call { uint32_t abi_version; tsugumi_bytes input; void* host; const tsugumi_effects* effects; } tsugumi_call;
/* Input bytes are borrowed for the call. The host takes ownership of any
 * non-null result.data on return, regardless of status. It must be the base
 * pointer of a libc malloc allocation. The host frees it after its last read. */
typedef int (*tsugumi_main_fn)(const tsugumi_call* call, tsugumi_bytes* result);
int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result);

#ifdef __cplusplus
}
#endif

#endif /* TSUGUMI_CARD_ABI_H */
