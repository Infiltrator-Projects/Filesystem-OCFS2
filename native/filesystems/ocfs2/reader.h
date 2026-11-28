/* SPDX-License-Identifier: GPL-3.0-or-later
 * Bounded local, offline OCFS2 reader. The format is decoded directly
 * through Common V2, never through a mounted kernel filesystem.
 * Initial profile: 4 KiB blocks and clusters, a single local mount
 * slot, clean journal, ordinary/inline directories and file data.
 */
#define B 4096u
#define LIMIT (UINT64_C(1)<<23)
typedef struct {
 const IfsFsHostRuntimeV2 *h;
 uint64_t blocks,root,sys,first,free;
 uint32_t gen,features;
 uint8_t *allocated;
 int verified;
} Volume;
static void clear(void *p,size_t n){uint8_t*b=p;while(n--)*b++=0;}
static int allocated(const Volume*v,uint64_t b){return b<v->blocks&&(!v->verified||(v->allocated[b/8u]&(1u<<(b%8u))));}
static int getblock(const Volume*v,uint64_t id,uint8_t*p){
 if(!id||!allocated(v,id))return IFS_FS_CORRUPT;
 return ifs_fs_host_read_bytes_v2(v->h,id*B,p,B);
}
static int integrity(Volume*v,uint8_t*p,unsigned at){return !(v->features&0x800u)||check_crc(p,B,at);}
static int inode(Volume*v,uint64_t id,uint8_t*p){
 int rc=getblock(v,id,p);if(rc)return rc;
 if(!equal(p,(const uint8_t*)"INODE01",7)||le64(p+80)!=id||le32(p+96)!=v->gen||
    !(le32(p+44)&1u)||(le32(p+44)&4u)||le64(p+72)||(!le16(p+42)&&!le16(p+16))||
    !integrity(v,p,128))return IFS_FS_CORRUPT;
 if(le16(p+118)&~1u||le64(p+120)||le64(p+136)||le64(p+144))return IFS_FS_UNSUPPORTED;
 if(le64(p+32)>v->blocks*B)return IFS_FS_CORRUPT;
 return 0;
}
/* Internal records use a 32-bit coverage, leaves use a 16-bit length.
 * Walk only validated nodes, rejecting repeated addresses and refusing
 * refcounted/unknown flags. Unwritten leaves read as logical zeros. */
static int map(Volume*v,const uint8_t*i,uint64_t k,uint64_t*out){
 uint8_t page[B];const uint8_t*p=i+192;unsigned cap=(B-208u)/16u,depth=le16(p);
 uint64_t visited[8];*out=0;
 if(depth>7u)return IFS_FS_UNSUPPORTED;
 for(unsigned level=0;;level++){
  unsigned d=le16(p),count=le16(p+2),used=le16(p+4);
  if(d!=depth-level||count>cap||used>count)return IFS_FS_CORRUPT;
  uint64_t end=0,choice=0,chosen_start=0;unsigned flag=0;
  for(unsigned j=0;j<used;j++){
   const uint8_t*r=p+16u+j*16u;uint64_t start=le32(r),len=d?le32(r+4):le16(r+4),b=le64(r+8);
   if(!len){if(j||d||b)return IFS_FS_CORRUPT;continue;}
   if(start<end||start+len>UINT64_C(0x100000000)||!b||b>=v->blocks)return IFS_FS_CORRUPT;
   end=start+len;
   if(!d){
    if(r[7]&~1u)return IFS_FS_UNSUPPORTED;
    if(len>v->blocks-b)return IFS_FS_CORRUPT;
    for(uint64_t x=0;x<len;x++)if(!allocated(v,b+x))return IFS_FS_CORRUPT;
   }
   if(k>=start&&k<end){choice=b;chosen_start=start;flag=r[7];}
  }
  if(!choice)return 0;
  if(!d){if(!(flag&1u))*out=choice+k-chosen_start;return 0;}
  for(unsigned j=0;j<level;j++)if(visited[j]==choice)return IFS_FS_CORRUPT;
  visited[level]=choice;int rc=getblock(v,choice,page);if(rc)return rc;
  if(!equal(page,(const uint8_t*)"EXBLK01",7)||le32(page+20)!=v->gen||le64(page+24)!=choice||!integrity(v,page,8))return IFS_FS_CORRUPT;
  p=page+48;cap=(B-64u)/16u;
 }
}
static int payload(Volume*v,const uint8_t*i,uint64_t off,void*out,uint32_t n,int holes){
 uint64_t size=le64(i+32);uint8_t*p=out;
 if((!out&&n)||off>size||n>size-off)return IFS_FS_INVALID;
 if(le16(i+118)&1u){
  unsigned cap=le16(i+192);if(cap>B-200u||size>cap)return IFS_FS_CORRUPT;
  for(unsigned j=0;j<n;j++)p[j]=i[200u+off+j];
  return 0;
 }
 while(n){
  unsigned skip=(unsigned)(off%B),take=B-skip;if(take>n)take=n;
  uint64_t physical;int rc=map(v,i,off/B,&physical);if(rc)return rc;
  if(!physical){if(!holes)return IFS_FS_CORRUPT;clear(p,take);}
  else {rc=ifs_fs_host_read_bytes_v2(v->h,physical*B+skip,p,take);if(rc)return rc;}
  n-=take;off+=take;p+=take;
 }
 return 0;
}
static int name_eq(const uint8_t*p,const char*q,unsigned n){for(unsigned j=0;j<n;j++)if(p[j]!=(uint8_t)q[j])return 0;return 1;}
/* Directory records are confined to their native block or inline region.
 * Validate the complete record chain, including unused records. Never
 * follow the on-disk dot entries as caller-supplied path components. */
static int scan(Volume*v,const uint8_t*i,const char*name,unsigned len,uint32_t index,uint64_t*out,char*found,unsigned*flen){
 uint64_t size=le64(i+32),self=le64(i+80);int inl=(le16(i+118)&1u)!=0;
 uint8_t page[B];unsigned matches=0,dots=0,parents=0;uint32_t ordinal=0;
 if((le16(i+40)&0170000u)!=0040000u||!size||size>UINT64_C(64)*1024u*1024u)return IFS_FS_UNSUPPORTED;
 if(!inl&&size%B)return IFS_FS_CORRUPT;
 for(uint64_t off=0;off<size;){
  unsigned bytes=inl?(unsigned)size:B,limit=bytes;
  if(bytes>B)return IFS_FS_CORRUPT;
  int rc=payload(v,i,off,page,bytes,0);if(rc)return rc;
  if(!inl&&(v->features&0x800u)){
   uint64_t physical;rc=map(v,i,off/B,&physical);if(rc)return rc;
   uint8_t*t=page+B-64u;
   if(!equal(t+16,(const uint8_t*)"DIRTRL1",7)||le64(t+40)!=physical||le64(t+48)!=self||
      !integrity(v,page,B-8u))return IFS_FS_CORRUPT;
   limit-=64u;
  }
  for(unsigned at=0;at<limit;){
   if(limit-at<12u)return IFS_FS_CORRUPT;
   const uint8_t*d=page+at;uint64_t id=le64(d);unsigned rec=le16(d+8),n=d[10],type=d[11];
   if(rec<12u||rec%4u||rec>limit-at||n>rec-12u)return IFS_FS_CORRUPT;
   if(id){
    if(!n||!allocated(v,id)||type>7u)return IFS_FS_CORRUPT;
    for(unsigned j=0;j<n;j++)if(!d[12+j]||d[12+j]=='/')return IFS_FS_CORRUPT;
    if(n==1u&&d[12]=='.'){if(id!=self||++dots!=1u)return IFS_FS_CORRUPT;}
    else if(n==2u&&d[12]=='.'&&d[13]=='.'){if(++parents!=1u)return IFS_FS_CORRUPT;}
    else {
     if((name&&n==len&&name_eq(d+12,name,n))||(!name&&ordinal==index)){
      if(++matches!=1u)return IFS_FS_CORRUPT;
      *out=id;*flen=n;for(unsigned j=0;j<n;j++)found[j]=(char)d[12+j];found[n]=0;
     }
     ordinal++;
    }
   }
   at+=rec;
  }
  off+=bytes;
 }
 if(dots!=1u||parents!=1u)return IFS_FS_CORRUPT;
 return matches?0:name?IFS_FS_NOT_FOUND:IFS_FS_END;
}
static int lookup(Volume*v,const uint8_t*i,const char*name,unsigned n,uint64_t*out){
 char found[256];unsigned len;return scan(v,i,name,n,0,out,found,&len);
}
/* Rebuild the cluster allocation bitmap from its native chain list.
 * A separate coverage map detects duplicate/cyclic groups even when
 * their allocation bits are clear. The first descriptor is special:
 * its represented range starts at cluster zero. */
static int load_bitmap(Volume*v,uint64_t id,const uint8_t*i){
 if((le32(i+44)&0x491u)!=0x491u||le32(i+188)!=v->blocks||le32(i+184)>v->blocks)return IFS_FS_CORRUPT;
 const uint8_t*l=i+192;unsigned cpg=le16(l),bpc=le16(l+2),cap=le16(l+4),used=le16(l+6);
 if(!cpg||bpc!=1u||cap>(B-208u)/16u||used>cap||!used)return IFS_FS_CORRUPT;
 size_t bytes=(size_t)((v->blocks+7u)/8u);
 v->allocated=v->h->allocate(v->h->context,bytes);if(!v->allocated)return IFS_FS_NO_MEMORY;
 uint8_t*covered=v->h->allocate(v->h->context,bytes);if(!covered)return IFS_FS_NO_MEMORY;
 clear(v->allocated,bytes);clear(covered,bytes);
 uint64_t total=0,free=0;int rc=0;uint8_t p[B];
 for(unsigned chain=0;chain<used&&!rc;chain++){
  const uint8_t*c=l+16u+16u*chain;uint64_t group=le64(c+8),ct=0,cf=0;
  while(group){
   rc=getblock(v,group,p);if(rc)break;
   unsigned bits=le16(p+10),sz=le16(p+8),unused=le16(p+12);
   uint64_t start=group==v->first?0:group;
   if(!equal(p,(const uint8_t*)"GROUP01",7)||le16(p+14)!=chain||le32(p+16)!=v->gen||
      le64(p+32)!=id||le64(p+40)!=group||!integrity(v,p,48)||!bits||bits>cpg||
      sz>B-64u||bits>sz*8u||unused>bits||start>=v->blocks||bits>v->blocks-start){
     rc=IFS_FS_CORRUPT;break;
   }
   unsigned local=0;
   for(unsigned j=0;j<bits;j++){
    uint64_t k=start+j;unsigned mask=1u<<(k%8u);
    if(covered[k/8u]&mask){rc=IFS_FS_CORRUPT;break;}
    covered[k/8u]|=(uint8_t)mask;
    if(p[64+j/8u]&(1u<<(j%8u)))v->allocated[k/8u]|=(uint8_t)mask;else local++;
   }
   if(rc)break;
   if(local!=unused){rc=IFS_FS_CORRUPT;break;}
   ct+=bits;cf+=local;group=le64(p+24);
  }
  if(!rc&&(ct!=le32(c+4)||cf!=le32(c)))rc=IFS_FS_CORRUPT;
  total+=ct;free+=cf;
 }
 v->h->release(v->h->context,covered);
 if(!rc&&(total!=v->blocks||free!=total-le32(i+184)))rc=IFS_FS_CORRUPT;
 if(!rc){v->free=free;v->verified=1;}
 return rc;
}
static uint32_t be32(const uint8_t*p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static void destroy(Volume*v){if(v->allocated)v->h->release(v->h->context,v->allocated);v->h->release(v->h->context,v);}
static int volume_info(void*vol,IfsFsVolumeInfoV2*i){
 Volume*v=vol;if(!v||!i||i->structure_bytes<sizeof(*i))return IFS_FS_INVALID;
 i->variant=12;i->block_bytes=B;i->max_name_bytes=255;i->total_bytes=v->blocks*B;i->free_bytes=v->free*B;return 0;
}
static int mount_volume(const IfsFsHostRuntimeV2*h,void**out,IfsFsVolumeInfoV2*info){
 if(!out||!info||info->structure_bytes<sizeof(*info)||!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
 *out=NULL;if((h->capabilities&(IFS_FS_HOST_READ_ONLY|IFS_FS_HOST_EXCLUSIVE))!=(IFS_FS_HOST_READ_ONLY|IFS_FS_HOST_EXCLUSIVE))return IFS_FS_ACCESS;
 uint32_t variant;int rc=identify(h,&variant);if(rc)return rc;
 uint8_t sb[B],sys[B],i[B],log[B];if(variant!=12u)return IFS_FS_UNSUPPORTED;
 rc=read_bytes(h,2u*B,sb,B);if(rc)return rc;
 const uint8_t*s=sb+192;uint32_t features=le32(s+32);
 /* Reject clustered, multi-slot, indexed, reflink and xattr layouts.
  * Sparse, inline, metadata ECC, backup superblock and JBD2 are admitted. */
 if(le32(s+60)!=12u||le16(s+64)!=1u||!(features&8u)||(features&~0x858u)||
    (le32(s+28)&~3u)||(le32(s+36)&~1u)||le32(sb+20)>LIMIT)return IFS_FS_UNSUPPORTED;
 Volume*v=h->allocate(h->context,sizeof(*v));if(!v)return IFS_FS_NO_MEMORY;
 clear(v,sizeof(*v));v->h=h;v->blocks=le32(sb+20);v->root=le64(s+40);v->sys=le64(s+48);v->first=le64(s+72);v->gen=le32(sb+96);v->features=features;
 uint64_t id;
 rc=inode(v,v->sys,sys);
 if(!rc)rc=lookup(v,sys,"global_bitmap",13,&id);
 if(!rc)rc=inode(v,id,i);
 if(!rc)rc=load_bitmap(v,id,i);
 if(!rc)rc=inode(v,v->sys,sys);
 if(!rc)rc=lookup(v,sys,"journal:0000",12,&id);
 if(!rc)rc=inode(v,id,i);
 if(!rc&&((le32(i+44)&0x110u)!=0x110u||(le32(i+184)&1u)))rc=IFS_FS_UNSUPPORTED;
 if(!rc)rc=payload(v,i,0,log,B,0);
 if(!rc&&(be32(log)!=0xc03b3998u||(be32(log+4)!=3u&&be32(log+4)!=4u)||be32(log+12)!=B||be32(log+28)))rc=IFS_FS_UNSUPPORTED;
 if(!rc)rc=inode(v,v->root,i);
 if(!rc){char name[256];unsigned n;rc=scan(v,i,NULL,0,UINT32_MAX,&id,name,&n);if(rc==IFS_FS_END)rc=0;}
 if(rc){destroy(v);return rc;}*out=v;return volume_info(v,info);
}
static int unmount_volume(void*vol){if(!vol)return IFS_FS_INVALID;destroy(vol);return 0;}
static int resolve(Volume*v,const char*path,uint8_t*i,const char**name,unsigned*n){
 if(!v||!path||path[0]!='/')return IFS_FS_INVALID;
 int rc=inode(v,v->root,i);if(rc)return rc;
 *name=path;*n=1;const char*p=path+1;
 for(unsigned depth=0;*p;depth++){
  if(depth>=64u)return IFS_FS_UNSUPPORTED;
  unsigned len=0;while(p[len]&&p[len]!='/'){if(len==255u)return IFS_FS_INVALID;len++;}
  if(!len||(len==1u&&p[0]=='.')||(len==2u&&p[0]=='.'&&p[1]=='.'))return IFS_FS_INVALID;
  uint64_t id;rc=lookup(v,i,p,len,&id);if(rc)return rc;
  rc=inode(v,id,i);if(rc)return rc;
  *name=p;*n=len;p+=len;if(*p=='/'){p++;if(!*p)return IFS_FS_INVALID;}
 }
 return 0;
}
static int fill(const uint8_t*i,const char*name,unsigned n,IfsFsEntryV2*e){
 if(!e||e->structure_bytes<sizeof(*e)||!e->name||n>=e->name_capacity)return IFS_FS_INVALID;
 switch(le16(i+40)&0170000u){case 0040000u:e->type=IFS_FS_ENTRY_DIRECTORY;break;case 0100000u:e->type=IFS_FS_ENTRY_FILE;break;case 0120000u:e->type=IFS_FS_ENTRY_LINK;break;default:return IFS_FS_UNSUPPORTED;}
 for(unsigned j=0;j<n;j++)e->name[j]=name[j];
 e->name[n]=0;e->name_bytes=n;e->size_bytes=le64(i+32);e->attributes=0;return 0;
}
static int stat_path(void*vol,const char*path,IfsFsEntryV2*e){uint8_t i[B];const char*name;unsigned n;int rc=resolve(vol,path,i,&name,&n);return rc?rc:fill(i,name,n,e);}
static int list_path(void*vol,const char*path,uint32_t index,IfsFsEntryV2*e){
 uint8_t i[B];const char*name;unsigned n;uint64_t id;char found[256];int rc=resolve(vol,path,i,&name,&n);
 if(!rc)rc=scan(vol,i,NULL,0,index,&id,found,&n);
 if(!rc)rc=inode(vol,id,i);
 return rc?rc:fill(i,found,n,e);
}
static int read_path(void*vol,const char*path,uint64_t off,void*out,uint32_t n){
 uint8_t i[B];const char*name;unsigned len;int rc=resolve(vol,path,i,&name,&len);if(rc)return rc;
 if((le16(i+40)&0170000u)!=0100000u)return IFS_FS_INVALID;
 return payload(vol,i,off,out,n,1);
}
static const IfsFsVolumeOpsV2 volume_ops={.header={2,sizeof(IfsFsVolumeOpsV2),IFS_FS_GROUP_VOLUME,0,IFS_FS_VOLUME_QUERY},.query=volume_info};
