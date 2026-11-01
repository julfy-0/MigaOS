#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <kernel/elf.h>
#include <kernel/initrd.h>
struct aspace { int n; uint64_t va[4200]; unsigned fl[4200]; };
static _Alignas(4096) uint8_t pg[4096]; uint64_t pmm_alloc_page(void){ return (uint64_t)(uintptr_t)pg; }
void *phys_to_virt(uint64_t pa){ return (void*)(uintptr_t)pa; }
int vmm_map_user(struct aspace *as, uint64_t va, uint64_t pa, unsigned fl){ (void)pa; as->va[as->n]=va; as->fl[as->n++]=fl; return 0; }
int main(void){
  FILE*f=fopen("build/initrd.img","rb"); static uint8_t buf[1<<20]; size_t n=fread(buf,1,sizeof buf,f); fclose(f);
  int r=initrd_init(buf,n); printf("initrd_init=%d\n",r);
  const void*d; uint64_t sz;
  printf("find hello=%d init=%d conf=%d nope=%d\n",initrd_find("hello",&d,&sz),initrd_find("init",&d,&sz),initrd_find("init.conf",&d,&sz),initrd_find("nope",&d,&sz));
  initrd_find("hello",&d,&sz);
  static struct aspace as; uint64_t ent=0;
  r=elf_load(d,sz,&as,&ent); printf("elf_load=%d entry=%#lx pages=%d\n",r,ent,as.n);
  for(int i=0;i<as.n;i++) printf("  va=%#lx W=%d X=%d\n",as.va[i],!!(as.fl[i]&1),!!(as.fl[i]&2));
  /* фаззинг: портим байты, не должно быть падений/ASan */
  uint8_t *cp=malloc(sz); int ok=0,bad=0; srand(1);
  for(int it=0;it<200000;it++){ memcpy(cp,d,sz); int k=1+rand()%4; while(k--) cp[rand()%(sz<400?sz:400)]=rand();
    static struct aspace a2; a2.n=0; uint64_t e2; if(elf_load(cp,sz,&a2,&e2)==0) ok++; else bad++; }
  printf("fuzz ok=%d rejected=%d\n",ok,bad);
  for(int len=0;len<(int)sz && len<200;len++){ static struct aspace a2; a2.n=0; uint64_t e2; elf_load(d,len,&a2,&e2);} 
  /* initrd фаззинг */
  static uint8_t b2[1<<20]; int iok=0;
  for(int it=0;it<100000;it++){ memcpy(b2,buf,n); int k=1+rand()%3; while(k--) b2[rand()%300]=rand(); if(initrd_init(b2,n)==0) iok++; }
  printf("initrd fuzz accepted=%d\n",iok);
  return 0; }
