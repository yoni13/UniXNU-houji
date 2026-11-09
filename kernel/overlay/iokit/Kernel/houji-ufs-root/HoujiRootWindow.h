// SPDX-License-Identifier: BSD-2-Clause-Patent
// Plan at most one DMA-sized span; partial writes require read/modify/write.
struct HoujiRootSpan {uint32_t lba,blocks,skip,bytes;bool partial;};
static bool houjiRootSpan(uint64_t offset,uint64_t bytes,HoujiRootSpan &out) {
  if(!bytes || offset>kHoujiRootBytes || bytes>kHoujiRootBytes-offset || ((offset|bytes)&511))return false;
  out.lba=kHoujiRootFirst+uint32_t(offset/4096);out.skip=unsigned(offset&4095);
  out.partial=out.skip || bytes<4096;
  if(out.partial){out.blocks=1;out.bytes=unsigned(bytes<4096-out.skip?bytes:4096-out.skip);}
  else{uint64_t n=bytes<131072?bytes:131072;out.blocks=unsigned(n/4096);out.bytes=out.blocks*4096;}
  return out.blocks && out.bytes && out.lba>=kHoujiRootFirst &&
    out.lba-kHoujiRootFirst<kHoujiRootBlocks && out.blocks<=kHoujiRootBlocks-(out.lba-kHoujiRootFirst);
}
static bool houjiHfsRootHeader(const uint8_t *page,unsigned n) {
  if(!page || n!=4096)return false;
  const uint8_t *h=page+1024;
  static const uint8_t id[8]={0x1e,0x38,0x30,0x08,0x64,0xba,0x2b,0x7b};
  uint32_t total=(uint32_t(h[44])<<24)|(uint32_t(h[45])<<16)|(uint32_t(h[46])<<8)|h[47];
  if(h[0]!=0x48 || h[1]!=0x58 || h[2] || h[3]!=5 ||
     h[40] || h[41] || h[42]!=0x10 || h[43] || total!=kHoujiRootBlocks)return false;
  for(unsigned i=0;i<8;i++)if(h[104+i]!=id[i])return false;
  return true;
}
// Returns 0, invalid-span=1, transport-error=2, or refused=3 (nothing written; *refusedAt gets the byte offset). The caller
// serializes I/O. Refused writes: a 512-byte request that is all zero (HFS+ makes 512-byte requests only for the volume header,
// its alternate and the journal header, never zero-filled; journal data requests are larger and may hold zero sectors), and a
// volume block 0 that would not hold a valid HFSX header. Both were seen once: after a reboot the volume and journal headers were
// zero and the volume did not mount.
template<class Ops>
static int houjiRootTransfer(Ops &ops,uint64_t offset,void *data,uint64_t bytes,bool write,uint8_t *scratch,
                             uint64_t *refusedAt=nullptr) {
  if((!data && bytes) || !scratch || offset>kHoujiRootBytes || bytes>kHoujiRootBytes-offset || ((offset|bytes)&511))return 1;
  uint8_t *cursor=static_cast<uint8_t *>(data);
  if(write && bytes==512){
    bool zero=true;for(unsigned i=0;i<512 && zero;i++)zero=!cursor[i];
    if(zero){if(refusedAt)*refusedAt=offset;return 3;}
  }
  while(bytes){
    HoujiRootSpan span;if(!houjiRootSpan(offset,bytes,span))return 1;
    if((!write || span.partial) && !ops.read(span.lba,span.blocks,scratch))return 2;
    if(write){
      __builtin_memcpy(scratch+span.skip,cursor,span.bytes);
      if(span.lba==kHoujiRootFirst && !houjiHfsRootHeader(scratch,4096)){if(refusedAt)*refusedAt=offset;return 3;}
      if(!ops.write(span.lba,span.blocks,scratch))return 2;
    }else __builtin_memcpy(cursor,scratch+span.skip,span.bytes);
    cursor+=span.bytes;offset+=span.bytes;bytes-=span.bytes;
  }
  return 0;
}
