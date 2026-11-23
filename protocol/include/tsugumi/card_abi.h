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
typedef struct tsugumi_effects {
  /* send/recv: valid for the whole process lifetime, from any thread, also after the call returns. */
  int (*send)(void* host, const char* view, const uint8_t* bytes, size_t size);
      /* 0 ok; <0 error (size > TSUGUMI_MESSAGE_MAX errors immediately). Blocks, cancellably, while the worker has no room for one max-size message. */
  int (*recv)(void* host, const char* view, tsugumi_bytes* out, int64_t timeout_ms);
      /* 0 message (out valid until the next recv on this thread), 1 timeout, 2 cancelled (outside a call: the process is being stopped), <0 error. timeout_ms < 0 = no timeout. */
  /* The rest are valid only during the call. */
  int (*cancelled)(void* host);
  int64_t (*remaining_ms)(void* host);            /* -1 = no deadline */
  int (*artifact_create)(void* host, tsugumi_artifact_id* id, tsugumi_bytes* path);
      /* new writable directory for this call, sealed read-only when the call returns; path is NUL-terminated, valid during the call. 0 ok, <0 error. */
  int (*artifact_open)(void* host, const tsugumi_artifact_id* id, tsugumi_bytes* path);
      /* read-only directory path, only for ids granted to this call (in its input CardPorts); opened lazily. 0 ok, <0 error (not granted / missing). */
} tsugumi_effects;
typedef struct tsugumi_call { uint32_t abi_version; tsugumi_bytes input; void* host; const tsugumi_effects* effects; } tsugumi_call;
typedef int (*tsugumi_main_fn)(const tsugumi_call* call, tsugumi_bytes* result);
int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result);

#ifdef __cplusplus
}
#endif

#endif /* TSUGUMI_CARD_ABI_H */
