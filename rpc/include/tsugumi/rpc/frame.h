#ifndef TSUGUMI_RPC_FRAME_H
#define TSUGUMI_RPC_FRAME_H
/* The frame of tsugumi/rpc/frame.hh for C and Swift callers on a blocking
 * file descriptor: a 4-byte little-endian length, then that many bytes. On any
 * status other than OK and END the caller closes the connection. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum tsugumi_frame_status {
  TSUGUMI_FRAME_OK = 0,
  /* The stream ended before a frame began. */
  TSUGUMI_FRAME_END = 1,
  /* The length is zero or over max_bytes. Nothing was allocated. */
  TSUGUMI_FRAME_SIZE = 2,
  /* The stream ended inside a frame. */
  TSUGUMI_FRAME_TRUNCATED = 3,
  /* read/write failed or memory ran out; errno says why. */
  TSUGUMI_FRAME_IO = 4,
} tsugumi_frame_status;

/* On OK, *bytes holds *size bytes; release them with tsugumi_frame_free. */
tsugumi_frame_status tsugumi_frame_read(int fd, uint32_t max_bytes,
                                        uint8_t** bytes, uint32_t* size);
void tsugumi_frame_free(uint8_t* bytes);
tsugumi_frame_status tsugumi_frame_write(int fd, const uint8_t* bytes,
                                         uint32_t size, uint32_t max_bytes);

#ifdef __cplusplus
}
#endif
#endif
