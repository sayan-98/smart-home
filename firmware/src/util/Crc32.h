// ---------------------------------------------------------------------------
//  Crc32.h - table-free CRC-32 (IEEE 802.3, reflected, init/xor 0xFFFFFFFF).
//
//  Used to validate persisted NVS blobs. A half-written blob after a brownout
//  must be detected and rejected, not silently applied to the relays.
//  Deliberately not esp_rom's crc32_le: that header has moved between IDF
//  releases and this is 12 lines.
// ---------------------------------------------------------------------------
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sh {

inline uint32_t crc32(const void* data, size_t len, uint32_t seed = 0xFFFFFFFFu) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t crc = seed;
  for (size_t i = 0; i < len; ++i) {
    crc ^= p[i];
    for (uint8_t b = 0; b < 8; ++b) {
      crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

}  // namespace sh
