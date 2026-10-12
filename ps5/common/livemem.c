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
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <float.h>
#include <stdarg.h>
#ifndef PSCLOUD_HOST_TEST
#include <sys/sysctl.h>
#include <sys/user.h>
#include <ps5/mdbg.h>
#include <ps5/kernel.h>
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
    int pid,type,busy,overflow,has;uint64_t session;char title[16],error[200],phase[16];
    uint64_t *addr;size_t count;double value;unsigned long long done,total;
} L;

struct region {uint64_t start,end;int prot,type;};
/* Search trace: before each memory region is read, one line naming it is written and forced
 * to storage, so after a console freeze the last line shows which region was being read. */
static char trace_path[1100];static int trace_fd=-1;
void pscloud_live_set_trace(const char *path) {snprintf(trace_path,sizeof trace_path,"%s",path?path:"");}
static void trace_line(const char *format,...) {
    if(trace_fd<0)return;
    char line[256];va_list ap;va_start(ap,format);int n=vsnprintf(line,sizeof line,format,ap);va_end(ap);
    if(n>0&&write(trace_fd,line,(size_t)n<sizeof line?(size_t)n:sizeof line-1)>0)(void)fsync(trace_fd);
}
static void trace_begin(const char *what,size_t regions_count,unsigned long long total) {
    if(!*trace_path)return;
    if(trace_fd>=0)close(trace_fd);
    trace_fd=open(trace_path,O_WRONLY|O_CREAT|O_TRUNC,0600);
    trace_line("%s in %s: %zu regions, %llu MiB, started %lld\n",what,L.title,regions_count,total>>20,(long long)time(NULL));
}
static void trace_region(size_t i,size_t n,const struct region *r) {
    trace_line("region %zu/%zu 0x%llx-0x%llx %llu KiB prot %d type %d\n",i+1,n,(unsigned long long)r->start,(unsigned long long)r->end,(unsigned long long)((r->end-r->start)>>10),r->prot,r->type);
}
static void trace_end(const char *result) {trace_line("finished: %s\n",result?result:"ok");if(trace_fd>=0) {close(trace_fd);trace_fd=-1;}}

/* ---------- platform ---------- */
#ifdef PSCLOUD_HOST_TEST
/* Tests: PSCLOUD_TEST_LIVE names a file that stands in for the game's memory at 0x10000000. */
#define FAKE_BASE 0x10000000ULL
static int find_game(char title[16],uint64_t *session) {
    const char *path=getenv("PSCLOUD_TEST_LIVE");struct stat st;
    if(!path||stat(path,&st))return -1;
    *session=(uint64_t)st.st_ino;const char *stamp=getenv("PSCLOUD_TEST_LIVE_SESSION");
    if(stamp) {FILE *f=fopen(stamp,"r");unsigned long long v=0;if(!f)return -1;int ok=fscanf(f,"%llu",&v)==1;fclose(f);if(!ok)return -1;*session=v;}
    strcpy(title,"PPSA02433");return 4242;
}
static int regions(int pid,struct region **out,size_t *n) {
    (void)pid;int fd=open(getenv("PSCLOUD_TEST_LIVE"),O_RDONLY);off_t size=fd<0?0:lseek(fd,0,SEEK_END);if(fd>=0)close(fd);
    if(size<=0)return -1;
    *out=malloc(sizeof **out);if(!*out)return -1;(*out)[0].start=FAKE_BASE;(*out)[0].end=FAKE_BASE+(uint64_t)size;(*out)[0].prot=3;(*out)[0].type=0;*n=1;return 0;
}
/* PSCLOUD_TEST_LIVE_UNREADABLE="start:end" (file offsets) makes reads touching that range fail. */
static int unreadable(uint64_t a,size_t len) {
    const char *r=getenv("PSCLOUD_TEST_LIVE_UNREADABLE");unsigned long long s,e;
    return r&&sscanf(r,"%llu:%llu",&s,&e)==2&&a-FAKE_BASE<e&&s<a-FAKE_BASE+len;
}
static int mem_read(int pid,uint64_t a,void *buf,size_t len) {(void)pid;if(unreadable(a,len))return -1;int fd=open(getenv("PSCLOUD_TEST_LIVE"),O_RDONLY);if(fd<0)return -1;ssize_t r=pread(fd,buf,len,(off_t)(a-FAKE_BASE));close(fd);return r==(ssize_t)len?0:-1;}
static int mem_write(int pid,uint64_t a,const void *buf,size_t len) {(void)pid;int fd=open(getenv("PSCLOUD_TEST_LIVE"),O_WRONLY);if(fd<0)return -1;ssize_t r=pwrite(fd,buf,len,(off_t)(a-FAKE_BASE));close(fd);return r==(ssize_t)len?0:-1;}
static int mem_read_back(int pid,uint64_t a,void *buf,size_t len) {return mem_read(pid,a,buf,len);}
#else
static int find_game(char title[16],uint64_t *session) {
    int mib[4]={CTL_KERN,KERN_PROC,KERN_PROC_PROC,0};size_t size=0;
    if(sysctl(mib,4,NULL,&size,NULL,0)||!size)return -1;
    size+=size/4;char *buffer=malloc(size);if(!buffer)return -1;
    if(sysctl(mib,4,buffer,&size,NULL,0)) {free(buffer);return -1;}
    int found=-1;
    for(size_t offset=0;offset+sizeof(int)<=size;) {
        struct kinfo_proc *ki=(struct kinfo_proc *)(buffer+offset);
        if(ki->ki_structsize<=0||offset+(size_t)ki->ki_structsize>size||(size_t)ki->ki_structsize<offsetof(struct kinfo_proc,ki_start)+sizeof ki->ki_start)break;
        offset+=(size_t)ki->ki_structsize;
        if(strcmp(ki->ki_comm,"eboot.bin"))continue;
        char info[256];memset(info,0,sizeof info);if(sceKernelGetAppInfo(ki->ki_pid,info))continue;
        if(memcmp(info+0x10,"PPSA",4))continue;
        memcpy(title,info+0x10,9);title[9]=0;*session=(uint64_t)ki->ki_start.tv_sec*1000000ULL+(uint64_t)ki->ki_start.tv_usec;if(!*session)continue;found=ki->ki_pid;break;
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
        list[count].start=e->kve_start;list[count].end=e->kve_end;list[count].prot=e->kve_protection;list[count].type=e->kve_type;count++;
    }
    free(buffer);if(!list)return -1;*out=list;*n=count;return 0;
}
static int mem_read(int pid,uint64_t a,void *buf,size_t len) {return mdbg_copyout(pid,(intptr_t)a,buf,len);}
/* Writes: the debug service first; if the game's memory refuses it, the SDK writes to the
 * resident page behind the address (it checks the page table, so unmapped addresses fail). */
static int mem_write(int pid,uint64_t a,const void *buf,size_t len) {return kernel_proc_copyin(pid,buf,(intptr_t)a,len);}
static int mem_read_back(int pid,uint64_t a,void *buf,size_t len) {return kernel_proc_copyout(pid,(intptr_t)a,buf,len);}
#endif

/* ---------- values and session controls ---------- */
#define GROUPS 32
#define TARGETS 32
#define PIN_LIMIT 128
static struct pin {unsigned id,n;int type,frozen;char name[49],error[128];uint64_t addr[TARGETS];double target[TARGETS];} pins[GROUPS];
static unsigned pin_count,next_id=1;
static pthread_cond_t wake=PTHREAD_COND_INITIALIZER;
static pthread_t freezer;
static pthread_t searcher;
static int searcher_started;
static int freezer_started,stopping,cancel_search;
static unsigned search_sequence;
unsigned pscloud_live_sequence(void) {pthread_mutex_lock(&lock);unsigned n=search_sequence;pthread_mutex_unlock(&lock);return n;}
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
        return absolute(a-x)<=tolerance||(a>=-9e15&&a<=9e15&&(double)(long long)(a+(a>=0?0.5:-0.5))==x);
    }
    return a==x;
}
static int fits(int type,double x) {
    if(!finite(x))return 0;
    if(type==T_DOUBLE)return 1;
    if(type==T_FLOAT)return x>=-FLT_MAX&&x<=FLT_MAX;
    double min=type==T_INT32?-2147483648.0:type==T_INT16?-32768:0;
    double max=type==T_INT32?2147483647.0:type==T_INT16?32767:255;
    return x>=min&&x<=max&&x==(double)(long long)x;
}
/* Called with lock held. PID plus process start time prevents PID reuse from retaining writes. */
static int session_ok(void) {
    char title[16]="";uint64_t stamp=0;int pid=find_game(title,&stamp);
    int ok=!stopping&&pid==L.pid&&stamp==L.session&&!strcmp(title,L.title);
    if(!ok) {pin_count=0;L.has=0;if(!L.busy)L.count=0;}
    return ok;
}
static int read_value(int type,uint64_t addr,double *value) {
    unsigned char v[8];if(mem_read_back(L.pid,addr,v,TYPE_SIZE[type]))return -1;
    *value=get(type,v);return finite(*value)?0:-1;
}
static int write_value(int type,uint64_t addr,double value) {
    double before;unsigned char v[8],back[8];
    if(!session_ok()||!fits(type,value)||read_value(type,addr,&before))return -1;
    put(type,v,value);
    if(mem_write(L.pid,addr,v,TYPE_SIZE[type])||mem_read_back(L.pid,addr,back,TYPE_SIZE[type])||memcmp(v,back,TYPE_SIZE[type]))return -1;
    return 0;
}
static void *freeze_main(void *arg) {
    (void)arg;pthread_mutex_lock(&lock);
    while(!stopping) {
        if(pin_count&&!L.busy&&session_ok())for(unsigned i=0;i<pin_count;i++)if(pins[i].frozen) {
            struct pin *p=&pins[i];
            for(unsigned j=0;j<p->n;j++) {
                double now;
                double held=p->type==T_FLOAT?(double)(float)p->target[j]:p->target[j];
                if(read_value(p->type,p->addr[j],&now)||(now!=held&&write_value(p->type,p->addr[j],p->target[j]))) {
                    if(i<pin_count) {p->frozen=0;snprintf(p->error,sizeof p->error,"Freeze stopped: a place could not be read or written. Check the game and search again.");pscloud_notify("Live: freezing %s stopped; a place could not be read or written",p->name);}
                    break;
                }
            }
        }
        struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_nsec+=250000000L;
        if(until.tv_nsec>=1000000000L) {until.tv_sec++;until.tv_nsec-=1000000000L;}
        (void)pthread_cond_timedwait(&wake,&lock,&until);
    }
    pthread_mutex_unlock(&lock);return NULL;
}
static int start_freezer(void) {
    if(freezer_started)return 0;
    pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,256U*1024);
    int bad=pthread_create(&freezer,&attr,freeze_main,NULL);pthread_attr_destroy(&attr);
    if(!bad)freezer_started=1;
    return bad?-1:0;
}
void pscloud_live_shutdown(void) {
    pthread_mutex_lock(&lock);stopping=1;pin_count=0;pthread_cond_broadcast(&wake);
    while(L.busy)pthread_cond_wait(&wake,&lock);
    int join=freezer_started,join_search=searcher_started;pthread_mutex_unlock(&lock);
    if(join_search)pthread_join(searcher,NULL);
    if(join)pthread_join(freezer,NULL);
    free(L.addr);L.addr=NULL;
}
/* ---------- background searches ---------- */
static void finish(const char *error) {
    pthread_mutex_lock(&lock);if(error)snprintf(L.error,sizeof L.error,"%s",error);
    L.busy=0;L.has=!error&&session_ok();pthread_cond_broadcast(&wake);pthread_mutex_unlock(&lock);
}
static int search_alive(void) {pthread_mutex_lock(&lock);int ok=!cancel_search&&session_ok();pthread_mutex_unlock(&lock);return ok;}
/* Searches stop on their own after this long, and pause between memory chunks, so a large game
 * cannot keep the console busy. Stop search (reset) ends one within a chunk. */
#define SEARCH_SECONDS 180
static void search_pause(void) {struct timespec pause={0,3000000};nanosleep(&pause,NULL);}
static const char *stop_reason(int timed_out) {
    pthread_mutex_lock(&lock);int cancelled=cancel_search;pthread_mutex_unlock(&lock);
    return cancelled?"Search stopped":timed_out?"Search stopped after 3 minutes to keep the console responsive. Narrow it down or try again":"The game session changed; search again";
}
static void *scan_main(void *arg) {
    (void)arg;struct region *list=NULL;size_t n=0;
    if(regions(L.pid,&list,&n)) {finish("The game's memory map could not be read");return NULL;}
    unsigned long long total=0;for(size_t i=0;i<n;i++)total+=list[i].end-list[i].start;
    pthread_mutex_lock(&lock);L.total=total;pthread_mutex_unlock(&lock);
    pscloud_log("INFO","Live search reads %zu memory regions, %llu MiB",n,total>>20);trace_begin("search",n,total);
    unsigned char *buf=malloc(CHUNK+8);const unsigned size=TYPE_SIZE[L.type];size_t count=0;int overflow=0,alive=1,timed_out=0;
    const time_t deadline=time(NULL)+SEARCH_SECONDS;
    for(size_t i=0;buf&&alive&&i<n;i++)for(uint64_t a=list[i].start;a<list[i].end;a+=CHUNK) {
        if(!(alive=search_alive()))break;
        if(time(NULL)>deadline) {alive=0;timed_out=1;break;}
        if(a==list[i].start)trace_region(i,n,&list[i]);
        size_t len=list[i].end-a<CHUNK?(size_t)(list[i].end-a):CHUNK;
        if(!overflow&&!mem_read(L.pid,a,buf,len))for(size_t o=0;o+size<=len;o+=size) {
            if(!same(L.type,get(L.type,buf+o),L.value))continue;
            if(count==CAP) {overflow=1;break;}
            L.addr[count++]=a+o;
        }
        pthread_mutex_lock(&lock);L.done+=len;L.count=count;L.overflow=overflow;pthread_mutex_unlock(&lock);
        search_pause();
    }
    int ok=buf!=NULL;free(buf);free(list);
    if(ok&&alive)pscloud_notify("Live search in %s: %zu places%s",L.title,count,overflow?" (too many; narrow it down)":"");
    trace_end(!ok?"out of memory":!alive?stop_reason(timed_out):NULL);
    if(ok&&!alive)pscloud_notify("Live search in %s: %s",L.title,stop_reason(timed_out));
    finish(!ok?"Out of memory":!alive?stop_reason(timed_out):NULL);return NULL;
}
static void *next_main(void *arg) {
    (void)arg;unsigned char page[4096];uint64_t loaded=UINT64_MAX;int ok=0,alive=1,timed_out=0;size_t kept=0;const unsigned size=TYPE_SIZE[L.type];size_t count=L.count;
    const time_t deadline=time(NULL)+SEARCH_SECONDS;
    for(size_t i=0;i<count;i++) {
        if((i&4095)==0) {
            if(!(alive=search_alive()))break;
            if(time(NULL)>deadline) {alive=0;timed_out=1;break;}
            if(i)search_pause();
            pthread_mutex_lock(&lock);L.done=i;pthread_mutex_unlock(&lock);
        }
        uint64_t a=L.addr[i],base=a&~(uint64_t)4095;
        if(base!=loaded) {loaded=base;ok=!mem_read(L.pid,base,page,sizeof page);}
        unsigned char value[8];
        if(a-base+size<=sizeof page) {if(!ok)continue;memcpy(value,page+(a-base),size);}
        else if(mem_read(L.pid,a,value,size))continue;
        if(same(L.type,get(L.type,value),L.value))L.addr[kept++]=a;
    }
    pthread_mutex_lock(&lock);L.count=kept;pthread_mutex_unlock(&lock);
    if(alive)pscloud_notify("Live next search in %s: %zu places left",L.title,kept);
    if(!alive)pscloud_notify("Live next search in %s: %s",L.title,stop_reason(timed_out));
    finish(alive?NULL:stop_reason(timed_out));return NULL;
}
static int start(void *(*fn)(void *),const char *phase,char *message,size_t max) {
    if(searcher_started) {pthread_join(searcher,NULL);searcher_started=0;}
    pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,256U*1024);
    cancel_search=0;search_sequence++;snprintf(L.phase,sizeof L.phase,"%s",phase);L.busy=1;L.done=0;L.error[0]=0;
    int bad=pthread_create(&searcher,&attr,fn,NULL);pthread_attr_destroy(&attr);
    if(bad) {L.busy=0;snprintf(message,max,"Could not start the search");return -1;}
    searcher_started=1;snprintf(message,max,"Searching the running game…");return 0;
}
int pscloud_live_scan(const char *type_name,double value,char *message,size_t max) {
    int type=type_of(type_name);if(type<0||!fits(type,value)) {snprintf(message,max,"That number does not fit the chosen kind of value");return -1;}
    pthread_mutex_lock(&lock);
    if(L.busy||stopping) {pthread_mutex_unlock(&lock);snprintf(message,max,"A search is still running or PSCloud is stopping");return -1;}
    (void)session_ok();char title[16]="";uint64_t stamp=0;int pid=find_game(title,&stamp);
    if(pid<0) {pthread_mutex_unlock(&lock);snprintf(message,max,"Start the game first; no game is running");return -1;}
    if(!L.addr)L.addr=malloc(CAP*sizeof *L.addr);
    if(!L.addr) {pthread_mutex_unlock(&lock);snprintf(message,max,"Out of memory");return -1;}
    L.pid=pid;L.session=stamp;snprintf(L.title,sizeof L.title,"%s",title);L.type=type;L.value=value;L.count=0;L.overflow=0;L.has=0;L.total=0;
    pscloud_log("EVENT","Live search in %s for %g (%s)",title,value,TYPE_NAMES[type]);
    int r=start(scan_main,"scan",message,max);pthread_mutex_unlock(&lock);return r;
}
int pscloud_live_next(double value,char *message,size_t max) {
    pthread_mutex_lock(&lock);
    if(L.busy||!session_ok()||!L.has||!fits(L.type,value)) {pthread_mutex_unlock(&lock);snprintf(message,max,"Wait for the search, check the value, or search again for this game session");return -1;}
    pscloud_log("EVENT","Live next search in %s for %g",L.title,value);
    L.value=value;L.total=L.count;int r=start(next_main,"next",message,max);pthread_mutex_unlock(&lock);return r;
}
void pscloud_live_reset(void) {pthread_mutex_lock(&lock);pscloud_log("EVENT","Live search cleared%s",L.busy?" (stopping the running search)":"");if(L.busy)cancel_search=1;if(!L.busy) {L.count=0;L.has=0;L.overflow=0;L.error[0]=0;}pthread_mutex_unlock(&lock);}
void pscloud_live_state_json(char *out,size_t max) {
    pthread_mutex_lock(&lock);char title[16]="";uint64_t stamp=0;int pid=find_game(title,&stamp);int same_game=session_ok();
    snprintf(out,max,"{\"running\":%s,\"title\":\"%s\",\"busy\":%s,\"phase\":\"%s\",\"done\":%llu,\"total\":%llu,\"count\":%zu,\"has\":%s,\"overflow\":%s,\"type\":\"%s\",\"value\":%g,\"error\":\"%s\",\"same_game\":%s,\"pinned\":%u}",
        pid>=0?"true":"false",title,L.busy?"true":"false",L.phase,L.done,L.total,L.count,L.has?"true":"false",L.overflow?"true":"false",TYPE_NAMES[L.type],L.value,L.error,same_game?"true":"false",pin_count);
    pthread_mutex_unlock(&lock);
}
void pscloud_live_list_json(char *out,size_t max,int limit) {
    pthread_mutex_lock(&lock);(void)session_ok();size_t pos=(size_t)snprintf(out,max,"{\"places\":[");
    for(size_t i=0;L.has&&!L.busy&&i<L.count&&(int)i<limit&&pos+128<max;i++) {
        double now;int bad=read_value(L.type,L.addr[i],&now);char number[48]="null";if(!bad)snprintf(number,sizeof number,"%.17g",now);
        pos+=(size_t)snprintf(out+pos,max-pos,"%s{\"address\":\"0x%llx\",\"value\":%s}",i?",":"",(unsigned long long)L.addr[i],number);
    }
    snprintf(out+pos,max-pos,"],\"count\":%zu,\"type\":\"%s\",\"search_sequence\":%u}",L.count,TYPE_NAMES[L.type],search_sequence);pthread_mutex_unlock(&lock);
}
static int selected(const char *text,uint64_t addresses[TARGETS],unsigned *n) {
    *n=0;if(!text||!*text||L.busy||!L.has)return -1;
    const char *p=text;
    while(*p) {
        if(*n==TARGETS||strncmp(p,"0x",2))return -1;
        char *end=NULL;errno=0;unsigned long long a=strtoull(p+2,&end,16);
        if(errno||end==p+2||(*end&&*end!=','))return -1;
        for(unsigned j=0;j<*n;j++)if(addresses[j]==a)return -1;
        size_t lo=0,hi=L.count;while(lo<hi) {size_t mid=(lo+hi)/2;if(L.addr[mid]<a)lo=mid+1;else hi=mid;}
        if(lo==L.count||L.addr[lo]!=a)return -1;
        addresses[(*n)++]=a;if(!*end)break;p=end+1;if(!*p)return -1;
    }
    return 0;
}
int pscloud_live_pin(const char *addresses,const char *name,char *message,size_t max) {
    pthread_mutex_lock(&lock);int result=-1;struct pin p={0};unsigned total=0;
    (void)session_ok();
    for(unsigned i=0;i<pin_count;i++)total+=pins[i].n;
    if(!session_ok()||selected(addresses,p.addr,&p.n))snprintf(message,max,"Select up to 32 places from the current search");
    else if(!name||!*name||strlen(name)>48)snprintf(message,max,"Name this value (up to 48 characters)");
    else if(pin_count==GROUPS||total+p.n>PIN_LIMIT)snprintf(message,max,"Saved values are full; remove a value first");
    else {
        int bad=0;for(const unsigned char *c=(const unsigned char *)name;*c;c++)if(*c<32||*c==127)bad=1;
        for(unsigned i=0;i<pin_count;i++)for(unsigned j=0;j<pins[i].n;j++)for(unsigned k=0;k<p.n;k++) {
            uint64_t a=p.addr[k],b=pins[i].addr[j];
            if(a<b+TYPE_SIZE[pins[i].type]&&b<a+TYPE_SIZE[L.type])bad=1;
        }
        p.type=L.type;for(unsigned j=0;j<p.n;j++)if(read_value(p.type,p.addr[j],&p.target[j]))bad=1;
        if(bad||start_freezer())snprintf(message,max,"These places are unreadable, overlap a saved value, or could not be saved. Use the existing saved value or search again.");
        else {p.id=next_id++;snprintf(p.name,sizeof p.name,"%s",name);pins[pin_count++]=p;result=0;snprintf(message,max,"Saved %s (%u places) for this game session",name,p.n);pscloud_notify("Live: saved %s (%u places) in %s",name,p.n,L.title);}
    }
    pthread_mutex_unlock(&lock);return result;
}
/* All addresses and computed values are checked before the first write. A game can still
 * reject a later write; report the exact partial count rather than claiming atomicity. */
int pscloud_live_edit(const char *addresses,unsigned id,const char *action,double value,char *message,size_t max) {
    pthread_mutex_lock(&lock);int result=-1;struct pin *p=NULL;uint64_t addr[TARGETS];double target[TARGETS];unsigned n=0;int type=L.type;
    if(!session_ok()) {snprintf(message,max,"The game session changed; search again");goto done;}
    if(id) {for(unsigned i=0;i<pin_count;i++)if(pins[i].id==id)p=&pins[i];if(!p) {snprintf(message,max,"This saved value expired; search again");goto done;}n=p->n;type=p->type;memcpy(addr,p->addr,n*sizeof *addr);}
    else if(selected(addresses,addr,&n)) {snprintf(message,max,"Select up to 32 places from the current search");goto done;}
    if(!strcmp(action,"remove")&&p) {pscloud_log("EVENT","Live: removed saved value %s",p->name);unsigned i=(unsigned)(p-pins);memmove(p,p+1,(pin_count-i-1)*sizeof *p);pin_count--;result=0;snprintf(message,max,"Saved value removed");goto done;}
    if(!strcmp(action,"unfreeze")&&p) {p->frozen=0;p->error[0]=0;result=0;snprintf(message,max,"Value unfrozen");pscloud_notify("Live: %s unfrozen",p->name);goto done;}
    int add=!strcmp(action,"add"),freeze=!strcmp(action,"freeze");
    if((strcmp(action,"set")&&!add&&!freeze)||(freeze&&!p)||!finite(value)) {snprintf(message,max,"Choose Set, Increase/decrease, Freeze or Unfreeze");goto done;}
    if(!p)for(unsigned k=0;k<pin_count;k++)for(unsigned j=0;j<pins[k].n;j++)for(unsigned i=0;i<n;i++) {
        uint64_t a=addr[i],b=pins[k].addr[j];
        if(a<b+TYPE_SIZE[pins[k].type]&&b<a+TYPE_SIZE[type]&&(a!=b||type!=pins[k].type)) {
            snprintf(message,max,"These places overlap a saved value with a different type. Use or remove the saved value first; nothing changed.");goto done;
        }
    }
    for(unsigned i=0;i<n;i++) {
        double current;if(read_value(type,addr[i],&current)) {if(p)p->frozen=0;snprintf(message,max,"A place is unreadable; no values were changed");goto done;}
        target[i]=add?(p&&p->frozen?p->target[i]:current)+value:value;
        if(!fits(type,target[i])) {snprintf(message,max,"A resulting number does not fit; no values were changed");goto done;}
    }
    unsigned changed=0;
    for(;changed<n;changed++)if(write_value(type,addr[changed],target[changed]))break;
    if(changed!=n) {
        /* session_ok may have expired the group during a write. */
        if(p&&pin_count&&(unsigned)(p-pins)<pin_count)p->frozen=0;
        if(!p)for(unsigned k=0;k<pin_count;k++)for(unsigned j=0;j<pins[k].n;j++)for(unsigned i=0;i<n;i++)if(pins[k].addr[j]==addr[i])pins[k].frozen=0;
        snprintf(message,max,"Confirmed %u of %u places. A later write failed or changed back; freeze stopped. Check the game.",changed,n);goto done;
    }
    if(p) {memcpy(p->target,target,n*sizeof *target);if(freeze)p->frozen=1;p->error[0]=0;pthread_cond_signal(&wake);}
    else for(unsigned k=0;k<pin_count;k++)for(unsigned j=0;j<pins[k].n;j++)for(unsigned i=0;i<n;i++)if(pins[k].addr[j]==addr[i]&&pins[k].type==type)pins[k].target[j]=target[i];
    result=0;snprintf(message,max,freeze?"Frozen %u places at %g":add?"Adjusted %u places by %g":"Set %u places to %g",n,value);
    if(p)pscloud_notify(freeze?"Live: %s frozen at %g":add?"Live: %s changed by %g":"Live: %s set to %g",p->name,value);
    else pscloud_notify(add?"Live: %u places in %s changed by %g":"Live: %u places in %s set to %g",n,L.title,value);
done:
    pthread_mutex_unlock(&lock);return result;
}
int pscloud_live_set(unsigned long long address,double value,char *message,size_t max) {
    char addresses[32];snprintf(addresses,sizeof addresses,"0x%llx",address);return pscloud_live_edit(addresses,0,"set",value,message,max);
}
static void escape_name(char *out,const char *name) {
    while(*name) {if(*name=='"'||*name=='\\')*out++='\\';*out++=*name++;}*out=0;
}
void pscloud_live_pins_json(char *out,size_t max) {
    pthread_mutex_lock(&lock);(void)session_ok();size_t at=(size_t)snprintf(out,max,"{\"groups\":[");
    for(unsigned i=0;i<pin_count&&at+512<max;i++) {
        struct pin *p=&pins[i];char name[100],number[48]="null";double current;escape_name(name,p->name);
        if(!read_value(p->type,p->addr[0],&current))snprintf(number,sizeof number,"%.17g",current);
        at+=(size_t)snprintf(out+at,max-at,"%s{\"id\":%u,\"name\":\"%s\",\"count\":%u,\"type\":\"%s\",\"value\":%s,\"target\":%.17g,\"frozen\":%s,\"error\":\"%s\"}",i?",":"",p->id,name,p->n,TYPE_NAMES[p->type],number,p->target[0],p->frozen?"true":"false",p->error);
    }
    snprintf(out+at,max-at,"]}");pthread_mutex_unlock(&lock);
}

/* Portable Live locators use masked context, never a saved absolute address. New rules keep
 * 256 bytes around the value (value at offset 128); 64-byte rules (offset 24) from 0.34.0
 * still load. A value in mostly empty memory needs the wider window to stay distinctive. */
#define WINDOW 256
#define WINDOW_OFFSET 128
#define OLD_WINDOW 64
#define OLD_OFFSET 24
#define PROFILE_RULES 12 /* 256-byte rules for 12 places fit the 8 KiB request limit */
#define PAGE 4096
struct locator_rule {unsigned char bytes[WINDOW],mask[WINDOW];unsigned len,offset,matches,first;uint64_t address;};
static struct locator_rule locator_rules[TARGETS];static unsigned locator_count;
static int signature_strength(const unsigned char *b,const unsigned char *mask,unsigned len,unsigned *fixed,unsigned *nonzero) {
    *fixed=0;*nonzero=0;for(unsigned i=0;i<len;i++)if(mask[i]) {(*fixed)++;if(b[i])(*nonzero)++;}
    return *fixed>=16&&*nonzero>=8;
}
/* Writes the profile JSON to out, or on failure the reason (plain text) and returns -1. */
int pscloud_live_locator(unsigned id,const char *version,char *out,size_t max) {
    pthread_mutex_lock(&lock);char why[200]="";struct pin *p=NULL;
    if(!session_ok())snprintf(why,sizeof why,"The game restarted, so this saved value expired. Search for it again");
    for(unsigned i=0;!*why&&i<pin_count;i++)if(pins[i].id==id)p=&pins[i];
    if(!*why&&!p)snprintf(why,sizeof why,"This saved value expired. Search for it again");
    if(!*why&&(strlen(version)>31||!*version||strspn(version,"0123456789.")!=strlen(version)))snprintf(why,sizeof why,"The game version could not be identified");
    if(!*why&&p->n>PROFILE_RULES)snprintf(why,sizeof why,"Retain at most %d places in one profile",PROFILE_RULES);
    size_t pos=0;char name[100];
    if(!*why) {escape_name(name,p->name);pos=(size_t)snprintf(out,max,"{\"format\":\"PSCLOUD_EDIT_PROFILE_V1\",\"kind\":\"live\",\"name\":\"%s\",\"title\":\"%s\",\"version\":\"%s\",\"rules\":[",name,L.title,version);}
    const unsigned size=*why?0:TYPE_SIZE[p->type];
    for(unsigned j=0;!*why&&j<p->n;j++) {
        unsigned char b[WINDOW],mask[WINDOW];memset(mask,1,sizeof mask);
        if(p->addr[j]<WINDOW_OFFSET) {snprintf(why,sizeof why,"This place is too close to the start of memory to retain");break;}
        const uint64_t start=p->addr[j]-WINDOW_OFFSET;
        /* Read page by page; context that cannot be read becomes a wildcard. */
        for(unsigned i=0;i<WINDOW;) {
            unsigned len=PAGE-(unsigned)((start+i)%PAGE);if(len>WINDOW-i)len=WINDOW-i;
            if(mem_read_back(L.pid,start+i,b+i,len)) {memset(b+i,0,len);memset(mask+i,0,len);}
            i+=len;
        }
        for(unsigned i=0;i<size;i++)if(!mask[WINDOW_OFFSET+i]) {snprintf(why,sizeof why,"The value itself could not be read. Check the game and search again");break;}
        if(*why)break;
        memset(mask+WINDOW_OFFSET,0,size);
        /* The other places of this value change with it. */
        for(unsigned k=0;k<p->n;k++)if(k!=j&&p->addr[k]+size>start&&p->addr[k]<start+WINDOW)
            for(unsigned i=0;i<size;i++)if(p->addr[k]+i>=start&&p->addr[k]+i<start+WINDOW)mask[p->addr[k]+i-start]=0;
        /* Mask likely pointers: their absolute values change with allocation and ASLR. */
        for(unsigned i=0;i+8<=WINDOW;i++)if(!((start+i)&7U)) {
            uint64_t v=0;memcpy(&v,b+i,8);
            if(v>=0x100000ULL&&v<0x800000000000ULL)memset(mask+i,0,8);
        }
        unsigned fixed,nonzero;
        if(!signature_strength(b,mask,WINDOW,&fixed,&nonzero)) {snprintf(why,sizeof why,"Too little stable memory around this value (%u fixed bytes, %u nonzero; 16 and 8 are needed). Retain another place of the same value",fixed,nonzero);break;}
        if(pos+2*WINDOW+128>=max) {snprintf(why,sizeof why,"The profile is too large");break;}
        char pattern[2*WINDOW+1];for(unsigned i=0;i<WINDOW;i++) {if(mask[i])snprintf(pattern+2*i,3,"%02x",b[i]);else memcpy(pattern+2*i,"??",2);}pattern[2*WINDOW]=0;
        pos+=(size_t)snprintf(out+pos,max-pos,"%s{\"type\":\"%s\",\"offset\":%d,\"pattern\":\"%s\"}",j?",":"",TYPE_NAMES[p->type],WINDOW_OFFSET,pattern);
    }
    if(!*why&&!session_ok())snprintf(why,sizeof why,"The game restarted, so this saved value expired. Search for it again");
    if(!*why) {snprintf(out+pos,max-pos,"]}");pscloud_log("EVENT","Live: profile %s built for %s %s; the dashboard page stores it",p->name,L.title,version);}
    else {snprintf(out,max,"%s",why);pscloud_log("WARN","Live profile not retained: %s",why);}
    pthread_mutex_unlock(&lock);return *why?-1:0;
}
int pscloud_live_game(char title[16]) {
    pthread_mutex_lock(&lock);uint64_t stamp=0;int pid=find_game(title,&stamp);pthread_mutex_unlock(&lock);return pid<0?-1:0;
}
static int hex_digit(char c) {return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;}
static int locator_parse(const char *patterns,int type) {
    locator_count=0;
    for(const char *p=patterns;*p;) {
        const char *end=strchr(p,',');size_t chars=end?(size_t)(end-p):strlen(p);
        if(locator_count==TARGETS||(chars!=2*OLD_WINDOW&&chars!=2*WINDOW))return -1;
        struct locator_rule *r=&locator_rules[locator_count];memset(r,0,sizeof *r);
        r->len=(unsigned)chars/2;r->offset=r->len==WINDOW?WINDOW_OFFSET:OLD_OFFSET;
        for(unsigned i=0;i<r->len;i++) {
            if((p[2*i]=='x'&&p[2*i+1]=='x')||(p[2*i]=='?'&&p[2*i+1]=='?'))continue;
            int a=hex_digit(p[2*i]),b=hex_digit(p[2*i+1]);if(a<0||b<0)return -1;
            r->mask[i]=1;r->bytes[i]=(unsigned char)(a*16+b);
        }
        for(unsigned i=r->offset;i<r->offset+TYPE_SIZE[type];i++)if(r->mask[i])return -1;
        unsigned fixed,nonzero;if(!signature_strength(r->bytes,r->mask,r->len,&fixed,&nonzero))return -1;
        for(unsigned i=0;i<r->len;i++)if(r->mask[i]) {r->first=i;break;}
        locator_count++;if(!end)break;p=end+1;if(!*p)return -1;
    }
    return locator_count?0:-1;
}
static int address_order(const void *a,const void *b) {uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return x<y?-1:x>y;}
static void *resolve_main(void *arg) {
    (void)arg;struct region *list=NULL;size_t count=0;const char *error=NULL;
    if(regions(L.pid,&list,&count)) {finish("The memory map could not be read");return NULL;}
    unsigned long long total=0;for(size_t i=0;i<count;i++) {if(list[i].end<list[i].start) {error="Invalid memory map";break;}total+=list[i].end-list[i].start;}
    if(total>16ULL*1024*1024*1024)error="Memory scan exceeds 16 GiB; this profile needs a more specific locator";
    pthread_mutex_lock(&lock);L.total=total;pthread_mutex_unlock(&lock);
    unsigned char *buf=malloc(CHUNK+WINDOW);if(!buf)error="Not enough memory";time_t deadline=time(NULL)+180;
    /* Like the search, unreadable memory is skipped: a failed chunk is retried page by page and
     * no signature window touching an unread page can match. Matches stay unique among the
     * memory that was read. */
    unsigned char bad[(CHUNK+WINDOW)/PAGE+2];unsigned long long skipped=0;trace_begin("relocation",count,total);
    for(size_t i=0;!error&&i<count;i++)for(uint64_t base=list[i].start;base<list[i].end;base+=CHUNK) {
        if(!search_alive()) {error=stop_reason(0);break;}
        if(base==list[i].start)trace_region(i,count,&list[i]);
        if(time(NULL)>deadline) {error="Relocation exceeded three minutes; nothing was bound";break;}
        size_t remain=(size_t)(list[i].end-base),n=remain<CHUNK+WINDOW-1?remain:CHUNK+WINDOW-1;
        memset(bad,0,sizeof bad);
        if(mem_read(L.pid,base,buf,n))for(size_t k=0;k*PAGE<n;k++) {
            size_t len=n-k*PAGE<PAGE?n-k*PAGE:PAGE;
            if(mem_read(L.pid,base+k*PAGE,buf+k*PAGE,len)) {bad[k]=1;if(k*PAGE<CHUNK)skipped+=len;}
        }
        for(unsigned k=0;k<locator_count;k++) {
            struct locator_rule *r=&locator_rules[k];if(r->matches>1)continue;
            size_t starts=n>=r->len?n-r->len+1:0;if(starts>CHUNK)starts=CHUNK;
            for(size_t o=0;o<starts;o++) {
                if((base+o+r->offset)%TYPE_SIZE[L.type]||buf[o+r->first]!=r->bytes[r->first]||bad[o/PAGE]||bad[(o+r->len-1)/PAGE])continue;
                unsigned j=0;for(;j<r->len;j++)if(r->mask[j]&&buf[o+j]!=r->bytes[j])break;
                if(j==r->len) {r->address=base+o+r->offset;if(++r->matches>1)break;}
            }
        }
        pthread_mutex_lock(&lock);L.done+=remain<CHUNK?remain:CHUNK;pthread_mutex_unlock(&lock);
        struct timespec pause={0,3000000};nanosleep(&pause,NULL);
    }
    trace_end(error);
    if(skipped)pscloud_log("INFO","Relocation skipped %llu KiB of unreadable game memory",skipped/1024);
    unsigned found_rules=0;
    for(unsigned k=0;!error&&k<locator_count;k++) {
        if(locator_rules[k].matches!=1)error=locator_rules[k].matches?"Signature is ambiguous; profile needs refinement":"Signature not found; profile needs refinement";
        else for(unsigned j=0;j<k;j++)if(locator_rules[j].address==locator_rules[k].address)error="Profile targets overlap; nothing was bound";
    }
    pthread_mutex_lock(&lock);
    if(!error)found_rules=locator_count;
    if(!error)pscloud_notify("Live profile found %u places in %s. Check the values before changing them",found_rules,L.title);
    else pscloud_notify("Live profile could not be relocated in %s: %s",L.title,error);
    if(!error) {for(unsigned k=0;k<locator_count;k++)L.addr[k]=locator_rules[k].address;L.count=locator_count;qsort(L.addr,L.count,sizeof *L.addr,address_order);}
    else L.count=0;
    pthread_mutex_unlock(&lock);free(buf);free(list);finish(error);return NULL;
}
int pscloud_live_resolve(const char *title,const char *type,const char *patterns,char *message,size_t max) {
    pthread_mutex_lock(&lock);int result=-1;char running[16]="";uint64_t stamp=0;int pid=find_game(running,&stamp),t=type_of(type);
    if(L.busy||stopping)snprintf(message,max,"Wait for the current search or stop it first");
    else if(pid<0||strcmp(title,running))snprintf(message,max,"Start the exact game this profile belongs to");
    else if(t<0)snprintf(message,max,"Unsupported numeric type");
    else {
        (void)session_ok();
        if(locator_parse(patterns,t))snprintf(message,max,"Invalid or weak memory signature");
        else {
            if(!L.addr)L.addr=malloc(CAP*sizeof *L.addr);
            if(!L.addr)snprintf(message,max,"Not enough memory");
            else {L.type=t;L.pid=pid;L.session=stamp;strcpy(L.title,running);L.has=0;L.count=0;L.value=0;
                pscloud_log("EVENT","Relocating a Live profile in %s (%u places)",running,locator_count);result=start(resolve_main,"resolve",message,max);}
        }
    }
    pthread_mutex_unlock(&lock);return result;
}
