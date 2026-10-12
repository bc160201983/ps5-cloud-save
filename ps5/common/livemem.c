/* Live mode. The game's memory is read and written through the system debug service
 * (mdbg, as the payload SDK provides it); its regions come from sysctl KERN_PROC_VMMAP.
 * Both were checked on 11.40 with ps5/vmmap-probe.c. Only writable regions are searched,
 * and only addresses from the current search results can be changed. */
#ifndef __FreeBSD__
#define _POSIX_C_SOURCE 200809L
#endif
#include "livemem.h"
#include "log.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#ifndef PSCLOUD_HOST_TEST
#include <sys/sysctl.h>
#include <sys/user.h>
#include <ps5/mdbg.h>
int sceKernelGetAppInfo(pid_t pid,void *info);
#ifndef KERN_PROC_VMMAP
#define KERN_PROC_VMMAP 32
#endif
#endif

#define CAP 2000000
#define CHUNK (1024*1024)
enum {T_INT32,T_INT16,T_UINT8,T_FLOAT,T_DOUBLE};
static const char *const TYPE_NAMES[]={"int32","int16","uint8","float","double"};
static const unsigned TYPE_SIZE[]={4,2,1,4,8};

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static struct {
    int pid,type,busy,overflow,has;char title[16],error[200],phase[16];
    uint64_t *addr;size_t count;double value;unsigned long long done,total;
} L;

struct region {uint64_t start,end;};

/* ---------- platform ---------- */
#ifdef PSCLOUD_HOST_TEST
/* Tests: PSCLOUD_TEST_LIVE names a file that stands in for the game's memory at 0x10000000. */
#define FAKE_BASE 0x10000000ULL
static int find_game(char title[16]) {if(!getenv("PSCLOUD_TEST_LIVE"))return -1;strcpy(title,"PPSA02433");return 4242;}
static int regions(int pid,struct region **out,size_t *n) {
    (void)pid;int fd=open(getenv("PSCLOUD_TEST_LIVE"),O_RDONLY);off_t size=fd<0?0:lseek(fd,0,SEEK_END);if(fd>=0)close(fd);
    if(size<=0)return -1;
    *out=malloc(sizeof **out);if(!*out)return -1;(*out)[0].start=FAKE_BASE;(*out)[0].end=FAKE_BASE+(uint64_t)size;*n=1;return 0;
}
static int mem_read(int pid,uint64_t a,void *buf,size_t len) {(void)pid;int fd=open(getenv("PSCLOUD_TEST_LIVE"),O_RDONLY);if(fd<0)return -1;ssize_t r=pread(fd,buf,len,(off_t)(a-FAKE_BASE));close(fd);return r==(ssize_t)len?0:-1;}
static int mem_write(int pid,uint64_t a,const void *buf,size_t len) {(void)pid;int fd=open(getenv("PSCLOUD_TEST_LIVE"),O_WRONLY);if(fd<0)return -1;ssize_t r=pwrite(fd,buf,len,(off_t)(a-FAKE_BASE));close(fd);return r==(ssize_t)len?0:-1;}
#else
static int find_game(char title[16]) {
    int mib[4]={CTL_KERN,KERN_PROC,KERN_PROC_PROC,0};size_t size=0;
    if(sysctl(mib,4,NULL,&size,NULL,0)||!size)return -1;
    size+=size/4;char *buffer=malloc(size);if(!buffer)return -1;
    if(sysctl(mib,4,buffer,&size,NULL,0)) {free(buffer);return -1;}
    int found=-1;
    for(size_t offset=0;offset+sizeof(int)<=size;) {
        struct kinfo_proc *ki=(struct kinfo_proc *)(buffer+offset);
        if(ki->ki_structsize<=0||offset+(size_t)ki->ki_structsize>size)break;
        offset+=(size_t)ki->ki_structsize;
        if(strcmp(ki->ki_comm,"eboot.bin"))continue;
        char info[256];memset(info,0,sizeof info);if(sceKernelGetAppInfo(ki->ki_pid,info))continue;
        if(memcmp(info+0x10,"PPSA",4))continue;
        memcpy(title,info+0x10,9);title[9]=0;found=ki->ki_pid;break;
    }
    free(buffer);return found;
}
static int regions(int pid,struct region **out,size_t *n) {
    int mib[4]={CTL_KERN,KERN_PROC,KERN_PROC_VMMAP,pid};size_t size=0;
    if(sysctl(mib,4,NULL,&size,NULL,0)||!size)return -1;
    size=size*4/3;char *buffer=malloc(size);if(!buffer)return -1;
    if(sysctl(mib,4,buffer,&size,NULL,0)) {free(buffer);return -1;}
    size_t cap=1024,count=0;struct region *list=malloc(cap*sizeof *list);
    for(size_t offset=0;list&&offset+sizeof(int)<=size;) {
        struct kinfo_vmentry *e=(struct kinfo_vmentry *)(buffer+offset);
        if(e->kve_structsize<=0||offset+(size_t)e->kve_structsize>size)break;
        offset+=(size_t)e->kve_structsize;
        if((e->kve_protection&(KVME_PROT_READ|KVME_PROT_WRITE))!=(KVME_PROT_READ|KVME_PROT_WRITE))continue;
        if(count==cap) {struct region *bigger=realloc(list,cap*2*sizeof *list);if(!bigger)break;list=bigger;cap*=2;}
        list[count].start=e->kve_start;list[count].end=e->kve_end;count++;
    }
    free(buffer);if(!list)return -1;*out=list;*n=count;return 0;
}
static int mem_read(int pid,uint64_t a,void *buf,size_t len) {return mdbg_copyout(pid,(intptr_t)a,buf,len);}
static int mem_write(int pid,uint64_t a,const void *buf,size_t len) {return mdbg_copyin(pid,buf,(intptr_t)a,len);}
#endif

/* ---------- values ---------- */
static int type_of(const char *name) {for(int i=0;i<5;i++)if(!strcmp(name,TYPE_NAMES[i]))return i;return -1;}
static double get(int type,const unsigned char *p) {
    switch(type) {
        case T_INT32: {int32_t v;memcpy(&v,p,4);return v;}
        case T_INT16: {int16_t v;memcpy(&v,p,2);return v;}
        case T_UINT8: return p[0];
        case T_FLOAT: {float v;memcpy(&v,p,4);return v;}
        default: {double v;memcpy(&v,p,8);return v;}
    }
}
static void put(int type,unsigned char *p,double x) {
    switch(type) {
        case T_INT32: {int32_t v=(int32_t)x;memcpy(p,&v,4);break;}
        case T_INT16: {int16_t v=(int16_t)x;memcpy(p,&v,2);break;}
        case T_UINT8: p[0]=(unsigned char)x;break;
        case T_FLOAT: {float v=(float)x;memcpy(p,&v,4);break;}
        default: memcpy(p,&x,8);
    }
}
static double absolute(double x) {return x<0?-x:x;}
static int finite(double x) {return x==x&&x-x==0;}
static int same(int type,double a,double x) {
    if(type==T_FLOAT||type==T_DOUBLE) {
        if(!finite(a))return 0;
        double tolerance=absolute(x)*1e-6;if(tolerance<0.01)tolerance=0.01;
        double rounded=(double)(long long)(a+(a>=0?0.5:-0.5));
        return absolute(a-x)<=tolerance||rounded==x;
    }
    return a==x;
}
static int fits(int type,double x) {
    if(type==T_FLOAT||type==T_DOUBLE)return finite(x);
    if(!finite(x)||x!=(double)(long long)x)return 0;
    return type==T_INT32?x>=-2147483648.0&&x<=2147483647.0:type==T_INT16?x>=-32768&&x<=32767:x>=0&&x<=255;
}

/* ---------- background work ---------- */
static void finish(const char *error) {
    pthread_mutex_lock(&lock);if(error)snprintf(L.error,sizeof L.error,"%s",error);L.busy=0;L.has=!error;pthread_mutex_unlock(&lock);
}
static void *scan_main(void *arg) {
    (void)arg;struct region *list=NULL;size_t n=0;
    if(regions(L.pid,&list,&n)) {finish("The game's memory map could not be read");return NULL;}
    unsigned long long total=0;for(size_t i=0;i<n;i++)total+=list[i].end-list[i].start;
    pthread_mutex_lock(&lock);L.total=total;pthread_mutex_unlock(&lock);
    unsigned char *buf=malloc(CHUNK+8);const unsigned size=TYPE_SIZE[L.type],step=size;
    for(size_t i=0;buf&&i<n;i++)for(uint64_t a=list[i].start;a<list[i].end;a+=CHUNK) {
        size_t len=list[i].end-a<CHUNK?(size_t)(list[i].end-a):CHUNK;
        if(!mem_read(L.pid,a,buf,len))for(size_t o=0;o+size<=len;o+=step) {
            if(!same(L.type,get(L.type,buf+o),L.value))continue;
            if(L.count==CAP) {L.overflow=1;break;}
            if(L.overflow)break;
            L.addr[L.count++]=a+o;
        }
        pthread_mutex_lock(&lock);L.done+=len;pthread_mutex_unlock(&lock);
    }
    int ok=buf!=NULL;free(buf);free(list);
    pscloud_log("INFO","Live search in %s: %zu places hold %g (%s)%s",L.title,L.count,L.value,TYPE_NAMES[L.type],L.overflow?", list full":"");
    finish(ok?NULL:"Out of memory");return NULL;
}
static void *next_main(void *arg) {
    (void)arg;unsigned char page[4096];uint64_t loaded=UINT64_MAX;int ok=0;size_t kept=0;const unsigned size=TYPE_SIZE[L.type];
    for(size_t i=0;i<L.count;i++) {
        uint64_t a=L.addr[i],base=a&~(uint64_t)4095;
        if(base!=loaded) {loaded=base;ok=!mem_read(L.pid,base,page,sizeof page);}
        unsigned char value[8];
        if(a-base+size<=sizeof page) {if(!ok)continue;memcpy(value,page+(a-base),size);}
        else if(mem_read(L.pid,a,value,size))continue;
        if(same(L.type,get(L.type,value),L.value))L.addr[kept++]=a;
        if((i&4095)==0) {pthread_mutex_lock(&lock);L.done=i;pthread_mutex_unlock(&lock);}
    }
    L.count=kept;finish(NULL);return NULL;
}
static int start(void *(*fn)(void *),const char *phase,char *message,size_t max) {
    pthread_t thread;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    snprintf(L.phase,sizeof L.phase,"%s",phase);L.busy=1;L.done=0;L.error[0]=0;
    int bad=pthread_create(&thread,&attr,fn,NULL);pthread_attr_destroy(&attr);
    if(bad) {L.busy=0;snprintf(message,max,"Could not start the search");return -1;}
    snprintf(message,max,"Searching the running game…");return 0;
}

/* ---------- API ---------- */
int pscloud_live_scan(const char *type_name,double value,char *message,size_t max) {
    int type=type_of(type_name);if(type<0||!fits(type,value)) {snprintf(message,max,"That number does not fit the chosen kind of value");return -1;}
    pthread_mutex_lock(&lock);
    if(L.busy) {pthread_mutex_unlock(&lock);snprintf(message,max,"A search is still running");return -1;}
    char title[16]="";int pid=find_game(title);
    if(pid<0) {pthread_mutex_unlock(&lock);snprintf(message,max,"Start the game first; no game is running");return -1;}
    if(!L.addr)L.addr=malloc(CAP*sizeof *L.addr);
    if(!L.addr) {pthread_mutex_unlock(&lock);snprintf(message,max,"Out of memory");return -1;}
    L.pid=pid;snprintf(L.title,sizeof L.title,"%s",title);L.type=type;L.value=value;L.count=0;L.overflow=0;L.has=0;L.total=0;
    int r=start(scan_main,"scan",message,max);pthread_mutex_unlock(&lock);return r;
}
int pscloud_live_next(double value,char *message,size_t max) {
    pthread_mutex_lock(&lock);
    if(L.busy) {pthread_mutex_unlock(&lock);snprintf(message,max,"A search is still running");return -1;}
    if(!L.has) {pthread_mutex_unlock(&lock);snprintf(message,max,"Search first");return -1;}
    char title[16]="";int pid=find_game(title);
    if(pid!=L.pid) {L.has=0;pthread_mutex_unlock(&lock);snprintf(message,max,"The game was restarted or closed; search again");return -1;}
    if(!fits(L.type,value)) {pthread_mutex_unlock(&lock);snprintf(message,max,"That number does not fit the chosen kind of value");return -1;}
    L.value=value;L.total=L.count;
    int r=start(next_main,"next",message,max);pthread_mutex_unlock(&lock);return r;
}
void pscloud_live_reset(void) {pthread_mutex_lock(&lock);if(!L.busy) {L.count=0;L.has=0;L.overflow=0;L.error[0]=0;}pthread_mutex_unlock(&lock);}
void pscloud_live_state_json(char *out,size_t max) {
    char title[16]="";int pid=find_game(title);
    pthread_mutex_lock(&lock);
    snprintf(out,max,"{\"running\":%s,\"title\":\"%s\",\"busy\":%s,\"phase\":\"%s\",\"done\":%llu,\"total\":%llu,\"count\":%zu,\"has\":%s,\"overflow\":%s,\"type\":\"%s\",\"value\":%g,\"error\":\"%s\",\"same_game\":%s}",
        pid>=0?"true":"false",title,L.busy?"true":"false",L.phase,L.done,L.total,L.count,L.has?"true":"false",L.overflow?"true":"false",TYPE_NAMES[L.type],L.value,L.error,pid>=0&&pid==L.pid?"true":"false");
    pthread_mutex_unlock(&lock);
}
void pscloud_live_list_json(char *out,size_t max,int limit) {
    pthread_mutex_lock(&lock);size_t pos=(size_t)snprintf(out,max,"{\"places\":[");
    for(size_t i=0;L.has&&!L.busy&&i<L.count&&(int)i<limit&&pos+96<max;i++) {
        unsigned char v[8];double now=0;if(!mem_read(L.pid,L.addr[i],v,TYPE_SIZE[L.type]))now=get(L.type,v);
        pos+=(size_t)snprintf(out+pos,max-pos,"%s{\"address\":\"0x%llx\",\"value\":%.10g}",i?",":"",(unsigned long long)L.addr[i],finite(now)?now:0);
    }
    snprintf(out+pos,max-pos,"],\"count\":%zu,\"type\":\"%s\"}",L.count,TYPE_NAMES[L.type]);
    pthread_mutex_unlock(&lock);
}
int pscloud_live_set(unsigned long long address,double value,char *message,size_t max) {
    pthread_mutex_lock(&lock);int r=-1;
    size_t lo=0,hi=L.count;while(lo<hi) {size_t mid=(lo+hi)/2;if(L.addr[mid]<address)lo=mid+1;else hi=mid;}
    char title[16]="";int pid=find_game(title);
    if(L.busy)snprintf(message,max,"A search is still running");
    else if(!L.has||lo>=L.count||L.addr[lo]!=address)snprintf(message,max,"Only places from your search results can be changed");
    else if(pid!=L.pid)snprintf(message,max,"The game was restarted or closed; search again");
    else if(!fits(L.type,value))snprintf(message,max,"That number does not fit this place");
    else {
        unsigned char v[8],back[8];put(L.type,v,value);
        if(mem_write(L.pid,address,v,TYPE_SIZE[L.type])||mem_read(L.pid,address,back,TYPE_SIZE[L.type])||memcmp(v,back,TYPE_SIZE[L.type]))snprintf(message,max,"The game did not accept the change");
        else {r=0;snprintf(message,max,"Changed to %g. Check it in the game; it is kept when the game saves",value);pscloud_log("EVENT","Live change in %s at 0x%llx to %g",L.title,address,value);}
    }
    pthread_mutex_unlock(&lock);return r;
}
