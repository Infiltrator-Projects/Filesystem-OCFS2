/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <infiltratr/filesystem_module_v2.h>
#include "../Libraries/tests/filesystem_reader_lifetime.h"
#include <assert.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define SIZE (134217728u)
static uint8_t *image;
struct context{int fd,fail;uint32_t sector,allocs;uint64_t bytes;};
static int rd(void *p,uint64_t off,void *out,uint32_t n){
 struct context *c=p;assert(n&&n<=4096u&&!(off%c->sector)&&!(n%c->sector)&&off<=c->bytes&&n<=c->bytes-off);
 if(c->fail)return -1;
 if(c->fd>=0)return pread(c->fd,out,n,(off_t)off)==(ssize_t)n?0:-1;
 memcpy(out,image+off,n);return 0;
}
static void *allocate(void *p,size_t n){struct context*c=p;void *m=malloc(n);if(m)c->allocs++;return m;}
static void release(void *p,void *m){struct context*c=p;assert(m&&c->allocs);c->allocs--;free(m);}
static inline void put16(uint8_t*p,uint32_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static inline void put32(uint8_t*p,uint32_t n){put16(p,n);put16(p+2,n>>16);}
static inline void put64(uint8_t*p,uint64_t n){put32(p,(uint32_t)n);put32(p+4,(uint32_t)(n>>32));}
static inline uint32_t crc32(uint32_t c,const uint8_t*p,uint32_t n){
 while(n--){c^=*p++;for(unsigned i=0;i<8u;i++)c=(c>>1)^((0u-(c&1u))&0xedb88320u);}return c;
}


static void inode(unsigned b,unsigned id){
 uint8_t*p=image+b*id;memcpy(p,"INODE01",7);put16(p+40,0040755);put16(p+42,2);
 put32(p+44,1);put64(p+80,id);put32(p+96,42);
 put32(p+128,crc32(UINT32_MAX,p,b));
}
static void fixture(void){
 unsigned b=4096;uint8_t*p=image+2*b;memcpy(p,"OCFSV2",6);put32(p+20,SIZE/b);
 put32(p+44,0x21);put64(p+80,2);put32(p+96,42);uint8_t*s=p+192;
 put16(s+2,90);put32(s+32,0x808);put64(s+40,10);put64(s+48,11);
 put32(s+56,12);put32(s+60,12);put16(s+64,1);put64(s+72,12);s[144]=9;
 put32(p+128,crc32(UINT32_MAX,p,b));inode(b,10);inode(b,11);
}
static void corrupt(void){image[10*4096+32]^=1;}
static void absent(void){image[8192]=0;}
static void expect(const IfsFsCoreOpsV2 *core,IfsFsHostRuntimeV2*h,struct context*c,int want){
 uint32_t v=99;int rc=core->probe(h,&v);
 if(rc!=want)fprintf(stderr,"expected %d got %d at sector %u\n",want,rc,c->sector);
 assert(rc==want&&(!rc?v!=0:v==0)&&!c->allocs);
}

/* This fixture encodes actual OCFS2 structures independently of the
 * production decoder, including two allocation groups, a JBD2 journal,
 * inline directories, a sparse extent tree and inline payload bytes. */
#define B 4096u
#define GROUP0 3u
#define GROUP1 32256u
#define JOURNAL 32u
#define JOURNAL_N 1024u
#define FILE_A 1100u
#define FILE_B 1104u
static uint8_t *blk(unsigned n){return image+(size_t)n*B;}
static void putbe32(uint8_t*p,uint32_t n){p[0]=(uint8_t)(n>>24);p[1]=(uint8_t)(n>>16);p[2]=(uint8_t)(n>>8);p[3]=(uint8_t)n;}
static void make_inode(unsigned n,unsigned mode,unsigned flags,uint64_t size,unsigned clusters){
 inode(B,n);uint8_t*p=blk(n);put16(p+40,mode);put16(p+42,1);
 put32(p+44,flags|1u);put64(p+32,size);put32(p+20,clusters);
}
static void extent(unsigned n,unsigned slot,unsigned logical,unsigned count,unsigned physical){
 uint8_t*p=blk(n)+192;put16(p+2,243);put16(p+4,slot+1u);
 p+=16+slot*16;put32(p,logical);put16(p+4,count);put64(p+8,physical);
}
struct ent {unsigned id,type;const char*name;};
static void directory(unsigned n,const struct ent*e,unsigned count){
 make_inode(n,0040755,0,3896,0);uint8_t*p=blk(n);
 put16(p+118,1);put16(p+192,3896);p+=200;
 unsigned off=0;
 for(unsigned j=0;j<count;j++){
  unsigned len=(unsigned)strlen(e[j].name),rec=j+1==count?3896-off:(12+len+3)&~3u;
  assert(rec>=12+len&&off+rec<=3896);
  put64(p+off,e[j].id);put16(p+off+8,rec);p[off+10]=(uint8_t)len;p[off+11]=(uint8_t)e[j].type;
  memcpy(p+off+12,e[j].name,len);off+=rec;
 }
 assert(off==3896);
}
static void group(unsigned id,unsigned chain,unsigned total){
 uint8_t*p=blk(id);memcpy(p,"GROUP01",7);put16(p+8,4032);put16(p+10,total);put16(p+14,chain);
 put32(p+16,42);put64(p+32,13);put64(p+40,id);
}
static void mark(unsigned n){
 uint8_t*p=blk(n<GROUP1?GROUP0:GROUP1)+64;unsigned k=n<GROUP1?n:n-GROUP1;
 assert(!(p[k/8]&(1u<<(k%8))));p[k/8]|=(uint8_t)(1u<<(k%8));
}
static void populated(void){
 memset(image,0,SIZE);fixture();uint8_t*s=blk(2)+192;
 put32(s+32,0x58);put64(s+72,GROUP0);
 static const struct ent root[]={{10,2,"."},{10,2,".."},{20,1,"sparse.bin"},{21,1,"inline.txt"},{22,2,"sub"}};
 static const struct ent sys[]={{11,2,"."},{11,2,".."},{13,1,"global_bitmap"},{14,1,"journal:0000"}};
 static const struct ent sub[]={{22,2,"."},{10,2,".."},{23,1,"child"}};
 directory(10,root,5);directory(11,sys,4);directory(22,sub,3);
 make_inode(20,0100644,0,3u*B+5u,3);
 extent(20,0,0,1,FILE_A);extent(20,1,2,2,FILE_B);
 memset(blk(FILE_A),0x41,B);memset(blk(FILE_B),0x42,B);memset(blk(FILE_B+1),0x43,B);
 make_inode(21,0100644,0,5,0);put16(blk(21)+118,1);put16(blk(21)+192,3896);memcpy(blk(21)+200,"hello",5);
 make_inode(23,0100644,0,3,0);put16(blk(23)+118,1);put16(blk(23)+192,3896);memcpy(blk(23)+200,"yes",3);
 make_inode(14,0100644,0x110,JOURNAL_N*(uint64_t)B,JOURNAL_N);extent(14,0,0,JOURNAL_N,JOURNAL);
 uint8_t*p=blk(JOURNAL);putbe32(p,0xc03b3998);putbe32(p+4,4);putbe32(p+12,B);putbe32(p+16,JOURNAL_N);putbe32(p+20,1);putbe32(p+24,1);
 group(GROUP0,0,GROUP1);group(GROUP1,1,SIZE/B-GROUP1);
 for(unsigned j=0;j<24;j++)mark(j);
 for(unsigned j=JOURNAL;j<JOURNAL+JOURNAL_N;j++)mark(j);
 mark(FILE_A);mark(FILE_B);mark(FILE_B+1);mark(GROUP1);
 unsigned used=24+JOURNAL_N+3+1;
 put16(blk(GROUP0)+12,GROUP1-(used-1));put16(blk(GROUP1)+12,SIZE/B-GROUP1-1);
 make_inode(13,0100644,0x490,SIZE,SIZE/B);
 p=blk(13);put32(p+184,used);put32(p+188,SIZE/B);p+=192;
 put16(p,32256);put16(p+2,1);put16(p+4,243);put16(p+6,2);
 put32(p+16,GROUP1-(used-1));put32(p+20,GROUP1);put64(p+24,GROUP0);
 put32(p+32,SIZE/B-GROUP1-1);put32(p+36,SIZE/B-GROUP1);put64(p+40,GROUP1);
}
static void mount_expect(const IfsFsCoreOpsV2*core,IfsFsHostRuntimeV2*h,struct context*c,int want){
 void*v=NULL;IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
 int rc=core->mount(h,&v,&info);if(rc!=want)fprintf(stderr,"mount expected %d got %d\n",want,rc);
 assert(rc==want);if(!rc)assert(v&&core->unmount(v)==0);else assert(!v);assert(!c->allocs);
}
static void read_tests(const IfsFsCoreOpsV2*core,IfsFsHostRuntimeV2*h,struct context*c){
 populated();mount_expect(core,h,c,IFS_FS_ACCESS);h->capabilities|=IFS_FS_HOST_EXCLUSIVE;
 void*v=NULL;IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
 check_reader_lifetime(core, h);
 assert(core->mount(h,&v,&info)==0&&v&&info.free_bytes==(uint64_t)(SIZE/B-24-JOURNAL_N-4)*B);
 char name[256];IfsFsEntryV2 e={.structure_bytes=sizeof(e),.name=name,.name_capacity=sizeof(name)};
 assert(core->directory_entry(v,"/",0,&e)==0&&!strcmp(name,"sparse.bin"));
 assert(core->directory_entry(v,"/",2,&e)==0&&!strcmp(name,"sub"));
 assert(core->directory_entry(v,"/",3,&e)==IFS_FS_END);
 assert(core->stat(v,"/sparse.bin",&e)==0&&e.size_bytes==3u*B+5u);
 assert(core->stat(v,"/sub/child",&e)==0&&e.size_bytes==3);
 uint8_t out[2*B+6];assert(core->read(v,"/inline.txt",0,out,5)==0&&!memcmp(out,"hello",5));
 assert(core->read(v,"/sub/child",0,out,3)==0&&!memcmp(out,"yes",3));
 assert(core->read(v,"/sparse.bin",B-1,out,sizeof(out))==0&&out[0]==0x41);
 for(unsigned j=1;j<=B;j++)assert(out[j]==0);
 for(unsigned j=B+1;j<=2*B;j++)assert(out[j]==0x42);
 for(unsigned j=2*B+1;j<sizeof(out);j++)assert(out[j]==0x43);
 assert(core->read(v,"/sparse.bin",3u*B+5u,out,1)==IFS_FS_INVALID);
 put64(blk(20)+192+16+8,FILE_A+1);
 assert(core->read(v,"/sparse.bin",0,out,1)==IFS_FS_CORRUPT);
 assert(core->unmount(v)==0&&!c->allocs);
 populated();put32(blk(14)+184,1);mount_expect(core,h,c,IFS_FS_UNSUPPORTED);
 populated();putbe32(blk(JOURNAL)+28,1);blk(JOURNAL+1)[0]=1;mount_expect(core,h,c,IFS_FS_UNSUPPORTED);
 populated();put16(blk(GROUP0)+12,1);mount_expect(core,h,c,IFS_FS_CORRUPT);
 populated();put64(blk(GROUP0)+24,GROUP0);mount_expect(core,h,c,IFS_FS_CORRUPT);
 h->capabilities&=~IFS_FS_HOST_EXCLUSIVE;
}

int main(int argc,char **argv){
 assert(argc==2||argc==3||argc==4);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(lib);
 IfsFsModuleApiEntryV2 get=(IfsFsModuleApiEntryV2)dlsym(lib,IFS_FS_COMMON_EXPORT_V2);assert(get);
 const IfsFsModuleApiV2*a=get();assert(ifs_fs_api_v2_valid(a,"ocfs2"));
 const IfsFsCoreOpsV2*core=(const void*)ifs_fs_group_v2(a,IFS_FS_GROUP_CORE);
 assert(core&&core->mount&&core->read&&!core->write&&!ifs_fs_group_v2(a,IFS_FS_GROUP_DEFRAG));
 struct context c={.fd=-1,.bytes=SIZE};
 if(argc>=3){struct stat st;c.fd=open(argv[2],O_RDONLY);assert(c.fd>=0&&!fstat(c.fd,&st)&&st.st_size>0);c.bytes=(uint64_t)st.st_size;}
 else {image=calloc(1,SIZE);assert(image);}
 IfsFsHostRuntimeV2 h={.abi_version=2,.structure_bytes=sizeof(h),.capabilities=IFS_FS_HOST_READ_ONLY,.volume_bytes=c.bytes,.maximum_transfer_bytes=4096,.context=&c,.read_at=rd,.allocate=allocate,.release=release};
 for(c.sector=512;c.sector<=4096;c.sector*=8){
  h.logical_sector_bytes=h.physical_sector_bytes=c.sector;
  if(argc>=3){expect(core,&h,&c,IFS_FS_OK);if(argc==4){
   void*v=NULL;IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
   assert(core->mount(&h,&v,&info)==IFS_FS_ACCESS&&!v);
   h.capabilities|=IFS_FS_HOST_EXCLUSIVE;
   int rc=core->mount(&h,&v,&info);if(rc)fprintf(stderr,"local OCFS2 mount failed: %d\n",rc);
   assert(rc==0&&v&&info.block_bytes==4096&&info.free_bytes>0);
   char name[256];IfsFsEntryV2 e={.structure_bytes=sizeof(e),.name=name,.name_capacity=sizeof(name)};
   assert(core->stat(v,"/",&e)==0&&e.type==IFS_FS_ENTRY_DIRECTORY);
   rc=core->directory_entry(v,"/",0,&e);assert(rc==IFS_FS_END||(rc==0&&!strcmp(name,"lost+found")));
   assert(core->stat(v,"/absent",&e)==IFS_FS_NOT_FOUND);
   assert(core->unmount(v)==0&&!c.allocs);h.capabilities&=~IFS_FS_HOST_EXCLUSIVE;
  }continue;}
  memset(image,0,SIZE);fixture();expect(core,&h,&c,IFS_FS_OK);
  c.fail=1;expect(core,&h,&c,IFS_FS_IO_ERROR);c.fail=0;
  corrupt();expect(core,&h,&c,IFS_FS_CORRUPT);
  memset(image,0,SIZE);fixture();absent();expect(core,&h,&c,IFS_FS_NOT_FOUND);
  read_tests(core,&h,&c);
 }
 if(c.fd>=0)assert(!close(c.fd));
 free(image);assert(!dlclose(lib));
 puts("OCFS2 native recognition, offline namespace, sparse payload and corruption refusal PASS");return 0;
}
