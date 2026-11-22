#ifndef TSUGUMI_CARD_ABI_H
#define TSUGUMI_CARD_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TSUGUMI_ABI_VERSION 9u

typedef struct tsugumi_bytes {
  const uint8_t* data;
  size_t size;
} tsugumi_bytes;

typedef struct tsugumi_effects {
  /* Process-lifetime callback, callable from Card threads between calls.
   * The host consumes or copies the opaque bytes before returning.
   * Returns zero on success. The host context remains valid for send.
   */
  int (*send)(void* host, const char* view, const uint8_t* bytes, size_t size);
  /* These callbacks and their context are valid only during the call.
   * They must not reenter the SDK's cancellation or deadline queries.
   */
  int (*cancelled)(void* host);
  /* Remaining milliseconds, or -1 when there is no deadline. */
  int64_t (*remaining_ms)(void* host);
} tsugumi_effects;

typedef struct tsugumi_call {
  uint32_t abi_version; /* Checked before reading the rest of the layout. */
  tsugumi_bytes input;
  void* host;
  const tsugumi_effects* effects;
} tsugumi_call;

/* The host calls serially. A zero return writes a borrowed CardResult,
 * including for encoded Card failures. The bytes last until the next
 * call. A nonzero return makes no result available.
 */
typedef int (*tsugumi_main_fn)(const tsugumi_call* call, tsugumi_bytes* result);
int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result);

#ifdef __cplusplus
}
#endif

#endif /* TSUGUMI_CARD_ABI_H */
