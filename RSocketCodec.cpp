#include "RSocketCodec.h"

#include <string.h>

static void writeU16(uint8_t *buf, uint16_t v) {
  buf[0] = (v >> 8) & 0xFF;
  buf[1] = v & 0xFF;
}
static uint16_t readU16(const uint8_t *buf) {
  return ((uint16_t)buf[0] << 8) | buf[1];
}
static void writeU24(uint8_t *buf, uint32_t v) {
  buf[0] = (v >> 16) & 0xFF;
  buf[1] = (v >> 8) & 0xFF;
  buf[2] = v & 0xFF;
}
static uint32_t readU24(const uint8_t *buf) {
  return ((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | buf[2];
}
static void writeU32(uint8_t *buf, uint32_t v) {
  buf[0] = (v >> 24) & 0xFF;
  buf[1] = (v >> 16) & 0xFF;
  buf[2] = (v >> 8) & 0xFF;
  buf[3] = v & 0xFF;
}
static uint32_t readU32(const uint8_t *buf) {
  return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | buf[3];
}
static void writeU64(uint8_t *buf, uint64_t v) {
  for (int i = 0; i < 8; i++) buf[i] = (uint8_t)((v >> (56 - 8 * i)) & 0xFF);
}
static uint64_t readU64(const uint8_t *buf) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | buf[i];
  return v;
}

// 4-byte stream id + 2-byte (6-bit type << 10 | 10-bit flags). Always 6 bytes.
static size_t writeHeader(uint8_t *out, uint32_t streamId, uint8_t frameType, uint16_t flags) {
  writeU32(out, streamId);
  uint16_t typeAndFlags = (uint16_t)(((frameType & 0x3F) << 10) | (flags & 0x3FF));
  writeU16(out + 4, typeAndFlags);
  return 6;
}

size_t encodeSetup(uint8_t *out, size_t outCap, uint32_t keepaliveMs, uint32_t lifetimeMs) {
  static const char *metaMime = "message/x.rsocket.routing.v0";
  static const char *dataMime = "application/json";
  size_t metaMimeLen = strlen(metaMime);
  size_t dataMimeLen = strlen(dataMime);

  size_t needed = 6 + 2 + 2 + 4 + 4 + 1 + metaMimeLen + 1 + dataMimeLen;
  if (needed > outCap) return 0;

  size_t pos = writeHeader(out, 0, RS_SETUP, 0);
  writeU16(out + pos, 1); pos += 2;  // major version
  writeU16(out + pos, 0); pos += 2;  // minor version
  writeU32(out + pos, keepaliveMs); pos += 4;
  writeU32(out + pos, lifetimeMs); pos += 4;
  out[pos++] = (uint8_t)metaMimeLen;
  memcpy(out + pos, metaMime, metaMimeLen); pos += metaMimeLen;
  out[pos++] = (uint8_t)dataMimeLen;
  memcpy(out + pos, dataMime, dataMimeLen); pos += dataMimeLen;
  return pos;
}

size_t encodeRequestStream(uint8_t *out, size_t outCap, uint32_t streamId, const char *route) {
  size_t routeLen = strlen(route);
  size_t metaLen = 1 + routeLen;  // routing metadata: 1-byte length + route bytes
  size_t needed = 6 + 4 + 3 + metaLen;
  if (needed > outCap) return 0;

  size_t pos = writeHeader(out, streamId, RS_REQUEST_STREAM, RS_FLAG_METADATA);
  writeU32(out + pos, 0x7FFFFFFF); pos += 4;  // initial request N = 2^31 - 1 ("give me everything")
  writeU24(out + pos, (uint32_t)metaLen); pos += 3;
  out[pos++] = (uint8_t)routeLen;
  memcpy(out + pos, route, routeLen); pos += routeLen;
  return pos;
}

size_t encodeKeepalive(uint8_t *out, size_t outCap, uint64_t position) {
  size_t needed = 6 + 8;
  if (needed > outCap) return 0;
  size_t pos = writeHeader(out, 0, RS_KEEPALIVE, 0);  // respond=false
  writeU64(out + pos, position); pos += 8;
  return pos;
}

size_t encodePayload(uint8_t *out, size_t outCap, uint32_t streamId,
                      const uint8_t *metadata, size_t metadataLen,
                      const uint8_t *data, size_t dataLen) {
  uint16_t flags = RS_FLAG_COMPLETE | RS_FLAG_NEXT;
  size_t metaBlock = 0;
  if (metadata != nullptr) {
    flags |= RS_FLAG_METADATA;
    metaBlock = 3 + metadataLen;
  }
  size_t needed = 6 + metaBlock + dataLen;
  if (needed > outCap) return 0;

  size_t pos = writeHeader(out, streamId, RS_PAYLOAD, flags);
  if (metadata != nullptr) {
    writeU24(out + pos, (uint32_t)metadataLen); pos += 3;
    memcpy(out + pos, metadata, metadataLen); pos += metadataLen;
  }
  if (data != nullptr && dataLen > 0) {
    memcpy(out + pos, data, dataLen); pos += dataLen;
  }
  return pos;
}

bool decodeFrame(const uint8_t *raw, size_t len, RSocketFrame &out) {
  if (len < 6) return false;

  out.streamId = readU32(raw);
  uint16_t typeAndFlags = readU16(raw + 4);
  out.frameType = (uint8_t)((typeAndFlags >> 10) & 0x3F);
  out.flags = typeAndFlags & 0x3FF;

  const uint8_t *body = raw + 6;
  size_t bodyLen = len - 6;

  out.metadata = nullptr;
  out.metadataLen = 0;
  out.data = nullptr;
  out.dataLen = 0;
  out.keepaliveposition = 0;

  if (out.frameType == RS_KEEPALIVE) {
    if (bodyLen >= 8) {
      out.keepaliveposition = readU64(body);
      out.data = body + 8;
      out.dataLen = bodyLen - 8;
    }
    return true;
  }

  bool hasMetadata = (out.flags & RS_FLAG_METADATA) != 0;

  if (out.frameType == RS_PAYLOAD || out.frameType == RS_REQUEST_RESPONSE) {
    if (hasMetadata) {
      if (bodyLen < 3) return false;
      uint32_t metaLen = readU24(body);
      if (3 + metaLen > bodyLen) return false;
      out.metadata = body + 3;
      out.metadataLen = metaLen;
      out.data = body + 3 + metaLen;
      out.dataLen = bodyLen - 3 - metaLen;
    } else {
      out.data = body;
      out.dataLen = bodyLen;
    }
    return true;
  }

  if (out.frameType == RS_ERROR) {
    if (bodyLen >= 4) {
      out.data = body + 4;  // error message text follows a 4-byte error code
      out.dataLen = bodyLen - 4;
    }
    return true;
  }

  // Other frame types: expose the raw body as data, unparsed.
  out.data = body;
  out.dataLen = bodyLen;
  return true;
}
