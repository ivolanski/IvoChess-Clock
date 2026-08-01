#ifndef RSOCKET_CODEC_H
#define RSOCKET_CODEC_H

#include <Arduino.h>

// RSocket protocol - spec: https://rsocket.io/about/protocol
// Ported byte-for-byte from a Python prototype validated against the
// real chess.com server (SETUP/REQUEST_STREAM/PAYLOAD/KEEPALIVE, mime
// types "message/x.rsocket.routing.v0" / "application/json").

enum RSocketFrameType {
  RS_SETUP = 0x01,
  RS_KEEPALIVE = 0x03,
  RS_REQUEST_RESPONSE = 0x04,
  RS_REQUEST_FNF = 0x05,
  RS_REQUEST_STREAM = 0x06,
  RS_REQUEST_CHANNEL = 0x07,
  RS_REQUEST_N = 0x08,
  RS_CANCEL = 0x09,
  RS_PAYLOAD = 0x0A,
  RS_ERROR = 0x0B,
  RS_METADATA_PUSH = 0x0C,
};

// Flags, already shifted to their final position within the 10-bit flags field.
#define RS_FLAG_METADATA 0x100
#define RS_FLAG_COMPLETE 0x040
#define RS_FLAG_NEXT     0x020
#define RS_FLAG_RESPOND  0x080

// Builds a SETUP frame (stream id 0). Returns the number of bytes
// written to 'out', or 0 if it didn't fit in 'outCap'.
size_t encodeSetup(uint8_t *out, size_t outCap, uint32_t keepaliveMs, uint32_t lifetimeMs);

// Builds a REQUEST_STREAM frame subscribing to 'route' via routing metadata.
size_t encodeRequestStream(uint8_t *out, size_t outCap, uint32_t streamId, const char *route);

// Builds a KEEPALIVE frame (respond=false, echoing 'position' back).
size_t encodeKeepalive(uint8_t *out, size_t outCap, uint64_t position);

// Builds a PAYLOAD frame (complete+next) - used to echo the server's
// application-level REQUEST_RESPONSE ping back.
size_t encodePayload(uint8_t *out, size_t outCap, uint32_t streamId,
                      const uint8_t *metadata, size_t metadataLen,
                      const uint8_t *data, size_t dataLen);

// Decoded frame - metadata/data point INTO the 'raw' buffer passed to
// decodeFrame(), so that buffer must stay valid as long as this is used.
struct RSocketFrame {
  uint32_t streamId;
  uint8_t frameType;
  uint16_t flags;
  const uint8_t *metadata;
  size_t metadataLen;
  const uint8_t *data;
  size_t dataLen;
  uint64_t keepaliveposition;
};

bool decodeFrame(const uint8_t *raw, size_t len, RSocketFrame &out);

#endif  // RSOCKET_CODEC_H
