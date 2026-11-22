// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace houji_data {
constexpr uint32_t blockBytes = 4096;
constexpr uint64_t rootBytes = 16904093696ULL;
constexpr uint64_t guardBlocks = (4ULL << 20) / blockBytes;
constexpr uint64_t reserveBlocks = (1ULL << 30) / blockBytes;
constexpr uint64_t seedPages = (2ULL << 30) / blockBytes;
constexpr uint64_t bitmapBitsPerBlock = 8 * (blockBytes - sizeof(uint32_t));
constexpr uint64_t storeBlocks = 1 + (seedPages + bitmapBitsPerBlock - 1) / bitmapBitsPerBlock + seedPages;
constexpr uint32_t maxEntries = 128;
constexpr uint32_t entryBytes = 128;

inline bool separator(char c) { return c == ' ' || c == '\t'; }
// Match the whole token, not a possibly truncated XNU string result. A second occurrence of
// the same key (even a bare key) makes that option ambiguous. An unterminated line is invalid.
inline bool exactArg(const char *line, size_t size, const char *key, const char *value)
{
  if (!line) return false;
  size_t end = 0;
  while (end < size && line[end]) ++end;
  if (end == size) return false;
  const size_t keyBytes = strlen(key), valueBytes = strlen(value);
  bool seen = false, match = false;
  for (size_t i = 0; i < end;) {
    if (separator(line[i])) { ++i; continue; }
    const size_t first = i;
    while (i < end && !separator(line[i])) ++i;
    const size_t bytes = i - first;
    if (bytes < keyBytes || memcmp(line + first, key, keyBytes) ||
        (bytes != keyBytes && line[first + keyBytes] != '=')) continue;
    if (seen) return false;
    seen = true;
    match = bytes == keyBytes + 1 + valueBytes && !memcmp(line + first + keyBytes + 1, value, valueBytes);
  }
  return match;
}
inline bool optedIn(const char *line, size_t size) { return exactArg(line, size, "native-data", "userdata"); }
// "dr=1" is data-reset=1 in 5 bytes, for a test boot whose line is near the 1000-byte limit (a census round that resets its Data
// in the boot it measures).
inline bool resetRequested(const char *line, size_t size)
{
  return optedIn(line, size) && (exactArg(line, size, "data-reset", "1") || exactArg(line, size, "dr", "1"));
}

inline uint32_t le32(const uint8_t *p)
{
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint64_t le64(const uint8_t *p) { return le32(p) | uint64_t(le32(p + 4)) << 32; }
inline uint32_t crc32(const uint8_t *p, size_t bytes, bool header = false)
{
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < bytes; ++i) {
    crc ^= header && i >= 16 && i < 20 ? 0 : p[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
  }
  return ~crc;
}

struct Gpt {
  uint64_t firstUsable, lastUsable, entriesLba;
  uint32_t headerCrc, entriesCrc, count, entryBlocks;
};
struct Extent { uint64_t first, last; }; // inclusive
struct Window { uint64_t first, blocks; };

// Primary GPT only. READ CAPACITY(10)'s sentinel is not an actual last LBA.
inline bool gptHeader(const uint8_t block[blockBytes], uint64_t capacityLast, Gpt &gpt)
{
  const uint32_t bytes = le32(block + 12), count = le32(block + 80);
  if (capacityLast >= UINT32_MAX || memcmp(block, "EFI PART", 8) || le32(block + 8) != 0x10000 ||
      bytes < 92 || bytes > blockBytes || le32(block + 20) || le64(block + 24) != 1 ||
      le64(block + 32) != capacityLast || le32(block + 84) != entryBytes || !count || count > maxEntries ||
      crc32(block, bytes, true) != le32(block + 16)) return false;
  const uint64_t first = le64(block + 40), last = le64(block + 48), table = le64(block + 72);
  const uint32_t blocks = (count * entryBytes + blockBytes - 1) / blockBytes;
  if (table < 2 || table > 64 || first < table + blocks || last < first || last >= capacityLast ||
      capacityLast - last <= blocks) return false;
  gpt = {first, last, table, le32(block + 16), le32(block + 88), count, blocks};
  return true;
}

inline bool entryUsed(const uint8_t *entry)
{
  for (unsigned i = 0; i < 16; ++i) if (entry[i]) return true;
  return false;
}
inline bool userdataName(const uint8_t *name)
{
  const char wanted[] = "userdata";
  for (unsigned i = 0; i < sizeof(wanted); ++i)
    if (name[2 * i] != uint8_t(wanted[i]) || name[2 * i + 1]) return false;
  return true;
}
inline bool gptUserdata(const Gpt &gpt, const uint8_t *entries, size_t bytes, Extent &out)
{
  if (!entries || !gpt.count || gpt.count > maxEntries || bytes < gpt.count * entryBytes ||
      crc32(entries, gpt.count * entryBytes) != gpt.entriesCrc) return false;
  bool found = false;
  Extent result = {};
  for (uint32_t i = 0; i < gpt.count; ++i) {
    const uint8_t *entry = entries + i * entryBytes;
    if (!entryUsed(entry)) continue;
    const uint64_t first = le64(entry + 32), last = le64(entry + 40);
    if (first < gpt.firstUsable || first > last || last > gpt.lastUsable || last > UINT32_MAX) return false;
    for (uint32_t j = 0; j < i; ++j) {
      const uint8_t *other = entries + j * entryBytes;
      if (entryUsed(other) && first <= le64(other + 40) && le64(other + 32) <= last) return false;
    }
    if (userdataName(entry + 56)) {
      if (found) return false;
      result = {first, last};
      found = true;
    }
  }
  if (found) out = result;
  return found;
}

inline bool tailWindow(Extent part, uint64_t manifestBytes, uint64_t blocks, Window &out)
{
  if (!part.first || part.first > part.last || part.last > UINT32_MAX || !blocks || !manifestBytes) return false;
  const uint64_t partitionBlocks = part.last - part.first + 1;
  const uint64_t protectedBytes = manifestBytes < rootBytes ? rootBytes : manifestBytes;
  const uint64_t imageBlocks = protectedBytes / blockBytes + (protectedBytes % blockBytes != 0);
  if (reserveBlocks >= partitionBlocks || blocks > partitionBlocks - reserveBlocks) return false;
  const uint64_t prefix = partitionBlocks - reserveBlocks - blocks;
  if (prefix < guardBlocks || imageBlocks > prefix - guardBlocks) return false;
  out = {part.first + prefix, blocks};
  return true;
}
inline bool contains(Window window, uint64_t lba, uint64_t blocks)
{
  return window.blocks && window.first <= UINT32_MAX && window.blocks - 1 <= UINT32_MAX - window.first &&
         blocks && lba >= window.first && lba - window.first < window.blocks &&
         blocks <= window.blocks - (lba - window.first);
}

// This is the final doorbell policy, independent of how a caller built the UPIU.
inline bool commandAllowed(bool armed, Window window, const uint8_t *upiu, uint32_t direction, uint32_t bytes)
{
  if (upiu[0] == 0) return !direction && !bytes; // NOP OUT
  if (upiu[0] == 0x16) return !direction && !bytes && upiu[5] == 1 && (upiu[12] == 3 || upiu[12] == 5);
  if (upiu[0] != 1) return false;
  const auto *cdb = upiu + 16;
  switch (cdb[0]) {
    case 0x00: case 0x1b: return !direction && !bytes; // TUR / START STOP UNIT
    case 0x25: case 0x28: return direction == 2 && bytes;
    case 0x35: return armed && window.blocks && !direction && !bytes && upiu[2] == 0;
    case 0x2a: {
      const uint32_t lba = uint32_t(cdb[2]) << 24 | uint32_t(cdb[3]) << 16 | uint32_t(cdb[4]) << 8 | cdb[5];
      const uint32_t blocks = uint32_t(cdb[7]) << 8 | cdb[8];
      return armed && direction == 1 && upiu[1] == 0x20 && upiu[2] == 0 &&
             bytes == blocks * blockBytes && contains(window, lba, blocks);
    }
    default: return false;
  }
}
} // namespace houji_data
