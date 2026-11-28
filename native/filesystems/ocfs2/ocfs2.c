/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native bounded recognition using the documented on-disk structures and
 * the pinned Linux v6.12 format reference. No kernel, libc or tool backend.
 * Recognition is not evidence of a safe mounted namespace or mutation.
 */
#include <infiltratr/filesystem_module_v2.h>
static inline uint32_t le16(const uint8_t *p){return p[0]|((uint32_t)p[1]<<8);}
static inline uint32_t le32(const uint8_t *p){return le16(p)|(le16(p+2)<<16);}
static inline uint64_t le64(const uint8_t *p){return le32(p)|((uint64_t)le32(p+4)<<32);}
static inline int equal(const uint8_t *a,const uint8_t *b,uint32_t n){
 for(uint32_t i=0;i<n;i++)if(a[i]!=b[i])return 0;
 return 1;
}
static inline uint32_t crc32(uint32_t c,const uint8_t *p,uint32_t n){
 while(n--){c^=*p++;for(unsigned i=0;i<8u;i++)c=(c>>1)^((0u-(c&1u))&0xedb88320u);}return c;
}
static int read_bytes(const IfsFsHostRuntimeV2 *h,uint64_t off,void *p,uint32_t n){
 if(off>h->volume_bytes||n>h->volume_bytes-off)return IFS_FS_NOT_FOUND;
 return ifs_fs_host_read_bytes_v2(h,off,p,n);
}


static int check_crc(uint8_t*p,unsigned bytes,unsigned at)
{
 uint32_t want=le32(p+at);for(unsigned k=0;k<8;k++)p[at+k]=0;
 return crc32(UINT32_MAX,p,bytes)==want;
}
static int nonzero(const uint8_t*p,unsigned n){while(n--)if(*p++)return 1;return 0;}
/* The superblock is itself a dinode. Check the geometry before following
 * its physical inode addresses; the root and system directory must both
 * corroborate the filesystem generation and their own block identities. */
static int directory_inode(const IfsFsHostRuntimeV2*h,uint64_t id,unsigned block,uint32_t gen,int ecc)
{
 uint8_t p[4096];int rc=read_bytes(h,id*block,p,block);if(rc)return rc;
 if(!equal(p,(const uint8_t*)"INODE01",7)||le64(p+80)!=id||
    le32(p+96)!=gen||!(le32(p+44)&1u)||le64(p+72)||
    (le16(p+40)&0170000u)!=0040000u||(!le16(p+42)&&!le16(p+16)))
  return IFS_FS_CORRUPT;
 if(ecc&&!check_crc(p,block,128))return IFS_FS_CORRUPT;
 return IFS_FS_OK;
}
static int identify(const IfsFsHostRuntimeV2*h,uint32_t*variant)
{
 uint8_t p[4096];int seen=0,result=IFS_FS_NOT_FOUND;
 for(unsigned shift=9;shift<=12;shift++){
  unsigned b=1u<<shift;int rc=read_bytes(h,2u*b,p,b);
  if(rc==IFS_FS_NOT_FOUND)continue;
  if(rc)return rc;
  if(!equal(p,(const uint8_t*)"OCFSV2",6))continue;
  if(seen++)return IFS_FS_CORRUPT;
  const uint8_t*s=p+192;unsigned cl=le32(s+60);
  uint32_t features=le32(s+32),gen=le32(p+96);
  if(le16(s)!=0u||le16(s+2)!=90u||features&~0xffffu||features&0x26u){result=IFS_FS_UNSUPPORTED;continue;}
  if(le32(s+56)!=shift||cl<12u||cl>20u||cl<shift||!gen||
     le64(p+80)!=2u||(le32(p+44)&0x21u)!=0x21u||
     !le16(s+64)||le16(s+64)>255u||!nonzero(s+144,16))
   {result=IFS_FS_CORRUPT;continue;}
  uint64_t clusters=le32(p+20),blocks=clusters<<(cl-shift),root=le64(s+40),sys=le64(s+48),group=le64(s+72);
  if(!clusters||clusters>h->volume_bytes/(UINT64_C(1)<<cl)||
     root<3u||sys<3u||group<3u||root>=blocks||sys>=blocks||group>=blocks||root==sys)
   {result=IFS_FS_CORRUPT;continue;}
  int ecc=(features&0x800u)!=0;
  if(ecc&&!check_crc(p,b,128)){result=IFS_FS_CORRUPT;continue;}
  rc=directory_inode(h,root,b,gen,ecc);
  if(!rc)rc=directory_inode(h,sys,b,gen,ecc);
  if(rc){result=rc;continue;}
  /* Recognition does not admit a cluster mount or replay a dirty journal. */
  *variant=shift;result=IFS_FS_OK;
 }
 return result;
}
static int probe(const IfsFsHostRuntimeV2 *h,uint32_t *variant){
 if(!variant||!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
 *variant=0;
 if(h->volume_bytes%h->logical_sector_bytes)return IFS_FS_INVALID;
 return identify(h,variant);
}
static const IfsFsCoreOpsV2 core={
 .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),IFS_FS_GROUP_CORE,0,IFS_FS_CORE_PROBE},.probe=probe
};
static const IfsFsGroupHeaderV2 *query(uint32_t id,uint32_t min,uint32_t max){
 return id==IFS_FS_GROUP_CORE&&min<=2u&&max>=2u?&core.header:NULL;
}
static const IfsFsModuleApiV2 api={
 .abi_version=2,.structure_bytes=sizeof(IfsFsModuleApiV2),.format_id="ocfs2",
#ifdef IFS_FREESTANDING
 .runtime_profiles=IFS_FS_PROFILE_FREESTANDING,
#else
 .runtime_profiles=IFS_FS_PROFILE_HOSTED,
#endif
 .group_mask=IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE),.query_group=query
};
__attribute__((visibility("default"))) const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void){return &api;}
