#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tsugumi_client tsugumi_client;

// Callback memory is borrowed until the callback returns. Copy before
// dispatching to an actor/event loop. Native callbacks may run on a background
// thread.
typedef void (*tsugumi_client_callback)(void* context, const uint8_t* bytes,
                                        size_t size, int error_code,
                                        const char* error_message);

// protocol: 0 = project, 1 = agent. Exceptions never cross this ABI.
tsugumi_client* tsugumi_client_create(const char* endpoint, int protocol);
void tsugumi_client_start(tsugumi_client* client,
                          tsugumi_client_callback callback, void* context);
void tsugumi_client_on_event(tsugumi_client* client,
                             tsugumi_client_callback callback, void* context);
void tsugumi_client_on_closed(tsugumi_client* client,
                              tsugumi_client_callback callback, void* context);
void tsugumi_client_call(tsugumi_client* client, const uint8_t* bytes,
                         size_t size, uint32_t timeout_ms,
                         tsugumi_client_callback callback, void* context);
void tsugumi_client_close(tsugumi_client* client);
void tsugumi_client_destroy(tsugumi_client* client);

#ifdef __cplusplus
}
#endif
