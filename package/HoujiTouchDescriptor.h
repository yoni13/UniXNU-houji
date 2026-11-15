// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bounds-checked TouchCom descriptor interpreter. Protocol tokens cross-checked
// against native iOS data-a-live@66b556ca (tc_touch_report), not its board data.
// Real IC fields are repacked for the already-tested contact state decoder.
#ifndef HOUJI_TOUCH_DESCRIPTOR_H
#define HOUJI_TOUCH_DESCRIPTOR_H
#include "HoujiTouchReport.h"
struct hd_layout {unsigned begin,end,finish,active;};
static inline int hd_layout_parse(const uint8_t *cfg,unsigned bytes,struct hd_layout *out) {
  if(!cfg || !out || !bytes || bytes>512)return 1;
  struct hd_layout layout={0};unsigned in=0,loops=0,fields=0,progress=0;
  for(unsigned i=0;i<bytes;) {
    unsigned token=cfg[i++];
    if(!token) {
      if(in || loops!=1 || (fields&14)!=14 || !progress)return 2;
      layout.finish=i;*out=layout;return 0;
    }
    if(token==1 || token==2) {
      if(in || loops++)return 3;
      in=1;layout.active=token==1;layout.begin=i;continue;
    }
    if(token==3) {if(!in)return 4;in=0;layout.end=i-1;continue;}
    if(token==4)continue;
    if(i>=bytes)return 5;
    unsigned bits=cfg[i++];if(!bits)return 6;
    if(token>=6 && token<=9) {
      if(!in || bits>(token>=8?16:8))return 7;
      fields|=1u<<(token-6);
    }
    if(token==0x18 && (in || bits>8))return 8;
    if(in)progress+=bits;
  }
  return 9;
}
static inline int hd_bits(const uint8_t *p,unsigned bytes,unsigned *offset,unsigned bits,unsigned *value) {
  if(!p || !offset || !value || !bits || bits>32 || *offset>bytes*8 || bits>bytes*8-*offset)return 1;
  unsigned v=0;
  for(unsigned i=0;i<bits;i++)v|=((unsigned)(p[(*offset+i)/8]>>((*offset+i)%8))&1u)<<i;
  *offset+=bits;*value=v;return 0;
}
static inline int hd_field(const uint8_t *cfg,unsigned limit,unsigned *index,
  const uint8_t *data,unsigned bytes,unsigned *offset,unsigned *value) {
  if(*index>=limit)return 1;
  unsigned bits=cfg[(*index)++];
  if(!bits || *offset>bytes*8 || bits>bytes*8-*offset)return 2;
  if(bits<=32)return hd_bits(data,bytes,offset,bits,value);
  *offset+=bits;*value=0;return 0;
}
static inline int hd_object(const uint8_t *cfg,const struct hd_layout *layout,
  const uint8_t *data,unsigned bytes,unsigned *offset,unsigned ordinal,
  unsigned *seen,uint8_t *packed,unsigned *contacts,unsigned mx,unsigned my) {
  unsigned id=ordinal,kind=0,x=0,y=0,flags=0;
  for(unsigned i=layout->begin;i<layout->end;) {
    unsigned token=cfg[i++],value=0;
    if(token==4) {*offset=(*offset+7)&~7u;if(*offset>bytes*8)return 1;continue;}
    if(token<5 || hd_field(cfg,layout->end,&i,data,bytes,offset,&value))return 2;
    if(token==6)id=value;
    if(token==7) {kind=value;flags|=1;}
    if(token==8) {x=value;flags|=2;}
    if(token==9) {y=value;flags|=4;}
  }
  if(id>=GT_CONTACTS || (*seen&(1u<<id)) || kind>9 || kind==7 || !(flags&1))return 3;
  *seen|=1u<<id;
  if(kind!=1 && kind!=2)return 0;
  if(flags!=7 || x>mx || y>my || *contacts>=GT_CONTACTS)return 4;
  uint8_t *v=packed+1+6*(*contacts);v[0]=id;v[1]=kind;v[2]=x;v[3]=x>>8;v[4]=y;v[5]=y>>8;
  (*contacts)++;return 0;
}
static inline int hd_decode(const uint8_t *cfg,unsigned config_bytes,const uint8_t *data,
  unsigned bytes,unsigned mx,unsigned my,struct gt_state *state,struct gt_frame *out) {
  if(!data || !state || !out || !mx || !my || bytes>512)return 1;
  struct hd_layout layout;if(hd_layout_parse(cfg,config_bytes,&layout))return 2;
  unsigned offset=0,active=0,have_count=0,seen=0,contacts=0;
  uint8_t packed[1+6*GT_CONTACTS]={0};
  for(unsigned i=0;i<layout.finish;) {
    unsigned token=cfg[i++],value=0;
    if(!token)break;
    if(token==1 || token==2) {
      unsigned count=layout.active?(have_count?active:GT_CONTACTS):GT_CONTACTS;
      if(count>GT_CONTACTS)return 3;
      for(unsigned object=0;object<count;object++) {
        if(layout.active && !have_count && offset==bytes*8)break;
        unsigned before=offset;
        if(hd_object(cfg,&layout,data,bytes,&offset,object,&seen,packed,&contacts,mx,my))return 4;
        if(offset<=before)return 5;
      }
      i=layout.end+1;continue;
    }
    if(token==3)return 6;
    if(token==4) {offset=(offset+7)&~7u;if(offset>bytes*8)return 7;continue;}
    if(hd_field(cfg,layout.finish,&i,data,bytes,&offset,&value))return 8;
    if(token==0x18) {if(value>GT_CONTACTS)return 9;active=value;have_count=1;}
  }
  packed[0]=contacts;
  return gt_decode(0x11,packed,1+6*contacts,mx,my,1,state,out);
}
#endif
