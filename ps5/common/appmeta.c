#define _POSIX_C_SOURCE 200809L
#include "appmeta.h"
#include "restore.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
int pscloud_title_valid(const char *s) {
    if(strlen(s)!=9 || strncmp(s,"PPSA",4))return 0;
    for(unsigned i=4;i<9;i++)if(s[i]<'0' || s[i]>'9')return 0;
    return 1;
}
static int string_value(const char *p,char *out,size_t max) {
    if(*p!='"')return -1;
    p++;size_t n=0;
    while(*p && *p!='"') {
        unsigned char ch=(unsigned char)*p++;
        if(ch=='\\') {
            ch=(unsigned char)*p++;if(!ch)return -1;
            if(ch=='u') {
                unsigned value=0;
                for(unsigned i=0;i<4;i++) {char digit=*p++;if(!digit)return -1;const char *at=strchr("0123456789abcdef",digit);if(!at)return -1;value=value*16+(unsigned)(at-"0123456789abcdef");}
                if(value>=0xd800 && value<=0xdfff)return -1;
                unsigned char bytes[3];size_t count=0;
                if(value<128)bytes[count++]=(unsigned char)value;
                else if(value<2048) {bytes[count++]=(unsigned char)(0xc0|(value>>6));bytes[count++]=(unsigned char)(0x80|(value&63));}
                else {bytes[count++]=(unsigned char)(0xe0|(value>>12));bytes[count++]=(unsigned char)(0x80|((value>>6)&63));bytes[count++]=(unsigned char)(0x80|(value&63));}
                if(!value || n+count>=max)return -1;
                memcpy(out+n,bytes,count);n+=count;continue;
            }
            if(ch=='n' || ch=='r' || ch=='t' || ch=='b' || ch=='f')ch=' ';
            else if(ch!='"' && ch!='\\' && ch!='/')return -1;
        }
        if(ch<32 || n+1>=max)return -1;
        out[n++]=(char)ch;
    }
    if(*p!='"' || !n)return -1;
    out[n]=0;return 0;
}
static char *read_json(const char *path) {
    int fd=open(path,O_RDONLY | O_NOFOLLOW | O_NONBLOCK);if(fd<0)return NULL;
    struct stat st;if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<=0 || st.st_size>65536) {close(fd);return NULL;}
    char *json=malloc((size_t)st.st_size+1);if(!json) {close(fd);return NULL;}
    size_t have=0;
    while(have<(size_t)st.st_size) {ssize_t n=read(fd,json+have,(size_t)st.st_size-have);if(n<=0)break;have+=(size_t)n;}
    close(fd);json[have]=0;return json;
}
/* First string value of "key" anywhere in the document. */
static int json_string(const char *json,const char *key,char *out,size_t max) {
    char quoted[64];snprintf(quoted,sizeof quoted,"\"%s\"",key);
    const char *at=strstr(json,quoted);if(!at)return -1;
    const char *p=at+strlen(quoted);while(*p==' '||*p=='\n'||*p=='\r'||*p=='\t')p++;
    if(*p!=':')return -1;
    p++;while(*p==' '||*p=='\n'||*p=='\r'||*p=='\t')p++;
    return string_value(p,out,max);
}
/* Finds the metadata folder for a title: <root>/<title>, else a folder (in <root> or
 * <root>/external, possibly named by content ID) whose param.json has this titleId, else an
 * installed game that keeps its saves under this ID (savedata.titleIdForSharing; e.g. Ghost
 * of Tsushima PPSA03210 saves as PPSA02225). */
int pscloud_app_meta_dir(const char *root,const char *title,char *out,size_t max) {
    if(!max||!pscloud_title_valid(title))return -1;
    char path[1400];struct stat st;
    if(snprintf(path,sizeof path,"%s/%s",root,title)<(int)sizeof path && !lstat(path,&st) && S_ISDIR(st.st_mode)) {
        return snprintf(out,max,"%s/%s",root,title)<(int)max?0:-1;
    }
    char sharing[1400]={0};
    for(int b=0;b<2;b++) {
        char base[1200];if(snprintf(base,sizeof base,b?"%s/external":"%s",root)>=(int)sizeof base)continue;
        DIR *d=opendir(base);if(!d)continue;
        struct dirent *e;
        while((e=readdir(d))) {
            if(e->d_name[0]=='.'||strlen(e->d_name)>96||strchr(e->d_name,'/'))continue;
            if(snprintf(path,sizeof path,"%s/%s/param.json",base,e->d_name)>=(int)sizeof path)continue;
            char *json=read_json(path);if(!json)continue;
            char id[32]={0},shared[32]={0};
            int exact=!json_string(json,"titleId",id,sizeof id)&&!strcmp(id,title);
            int shares=!exact&&!*sharing&&!json_string(json,"titleIdForSharing",shared,sizeof shared)&&!strcmp(shared,title);
            free(json);
            if(exact) {closedir(d);return snprintf(out,max,"%s/%s",base,e->d_name)<(int)max?0:-1;}
            if(shares)snprintf(sharing,sizeof sharing,"%s/%s",base,e->d_name);
        }
        closedir(d);
    }
    if(!*sharing)return -1;
    return snprintf(out,max,"%s",sharing)<(int)max?0:-1;
}
int pscloud_app_name(const char *root,const char *title,char *name,size_t max) {
    if(!max || !pscloud_title_valid(title))return -1;
    snprintf(name,max,"%s",!strcmp(title,"PPSA02433")?"Crash Bandicoot 4":title);
    char path[1400];if(pscloud_app_meta_dir(root,title,path,sizeof path))return 0;
    char file[1500];snprintf(file,sizeof file,"%s/param.json",path);
    char *json=read_json(file);if(!json)return 0;
    const char *language=json;
    while((language=strstr(language,"\"en-US\""))) {const char *after=language+7;while(*after==' ' || *after=='\n' || *after=='\r' || *after=='\t')after++;if(*after==':')break;language+=7;}
    const char *key=strstr(language?language:json,"\"titleName\"");
    if(key) {const char *p=strchr(key,':');if(p) {p++;while(*p==' ' || *p=='\n' || *p=='\r' || *p=='\t')p++;char decoded[256]={0};if(!string_value(p,decoded,sizeof decoded))snprintf(name,max,"%s",decoded);}}
    free(json);return 0;
}
int pscloud_app_version(const char *root,const char *title,char *version,size_t max) {
    if(!max||!pscloud_title_valid(title))return -1;
    char path[1400];if(pscloud_app_meta_dir(root,title,path,sizeof path))return -1;
    int dir=pscloud_open_directory(path);if(dir<0)return -1;
    unsigned char *data=NULL;size_t size=0;
    int bad=pscloud_read_archive(dir,"param.json",&data,&size);close(dir);
    if(bad||size>65536) {free(data);return -1;}
    char *json=malloc(size+1);if(!json) {free(data);return -1;}
    memcpy(json,data,size);json[size]=0;free(data);bad=1;
    const char *key=strstr(json,"\"contentVersion\"");
    if(key&&!strstr(key+16,"\"contentVersion\"")) {
        const char *p=key+16;while(*p==' '||*p=='\n'||*p=='\r'||*p=='\t')p++;
        if(*p==':') {p++;while(*p==' '||*p=='\n'||*p=='\r'||*p=='\t')p++;
            if(!string_value(p,version,max)) {bad=0;for(const char *q=version;*q;q++)if((*q<'0'||*q>'9')&&*q!='.')bad=1;}}
    }
    free(json);return bad?-1:0;
}

/* Text fields of a save's param.sfo, by key. Missing fields come back empty. */
static int sfo_fields(const char *path,const char *const wanted[],char *const outs[],const size_t maxes[],unsigned count_keys) {
    for(unsigned k=0;k<count_keys;k++)outs[k][0]=0;
    int fd=open(path,O_RDONLY | O_NOFOLLOW | O_NONBLOCK);if(fd<0)return -1;
    struct stat st;if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<20 || st.st_size>65536) {close(fd);return -1;}
    size_t size=(size_t)st.st_size;unsigned char *d=malloc(size);if(!d) {close(fd);return -1;}
    size_t have=0;while(have<size) {ssize_t n=read(fd,d+have,size-have);if(n<=0)break;have+=(size_t)n;}close(fd);
    #define R16(p) ((unsigned)(p)[0]|((unsigned)(p)[1]<<8))
    #define R32(p) (R16(p)|((size_t)R16((p)+2)<<16))
    int bad=have!=size||memcmp(d,"\0PSF",4);
    size_t keys=bad?0:R32(d+8),values=bad?0:R32(d+12),count=bad?0:R32(d+16);
    if(count>128||keys<20+16*count||keys>values||values>size)bad=1;
    for(size_t i=0;!bad&&i<count;i++) {
        const unsigned char *e=d+20+16*i;size_t ko=R16(e),length=R32(e+4),offset=R32(e+12);
        if(ko>=values-keys||offset>size-values||length>size-values-offset)break;
        const char *key=(const char *)d+keys+ko;if(!memchr(key,0,values-keys-ko))break;
        unsigned which=count_keys;for(unsigned k=0;k<count_keys;k++)if(!strcmp(key,wanted[k]))which=k;
        if(which==count_keys||R16(e+2)!=0x0204||!length)continue;
        const unsigned char *v=d+values+offset,*end=memchr(v,0,length);size_t n=end?(size_t)(end-v):length;
        if(n&&n<maxes[which]) {memcpy(outs[which],v,n);outs[which][n]=0;}
    }
    #undef R16
    #undef R32
    free(d);return bad?-1:0;
}
static int generic_label(const char *label) {
    static const char *const generic[]={"save data","saved data","savedata","game data","gamedata","data","save","saves","settings",
        "options","profile","profile data","system","system data","autosave","auto save","user data","preferences","config",NULL};
    char lower[64];size_t n=strlen(label);if(n>=sizeof lower)return 0;
    for(size_t i=0;i<=n;i++)lower[i]=(char)((label[i]>='A'&&label[i]<='Z')?label[i]+32:label[i]);
    for(unsigned i=0;generic[i];i++)if(!strcmp(lower,generic[i]))return 1;
    return 0;
}
static int sfo_texts(const char *path,char *title,size_t title_max,char *slot,size_t slot_max) {
    static const char *const keys[]={"MAINTITLE","SAVEDATA_DIRECTORY"};char *const outs[]={title,slot};const size_t maxes[]={title_max,slot_max};
    return sfo_fields(path,keys,outs,maxes,2)||!title[0]?-1:0;
}
/* What the PS5 shows for one save slot: its title, subtitle and detail lines. */
int pscloud_save_slot_info(const char *home,const char *user,const char *title,const char *slot,char *main,size_t main_max,char *sub,size_t sub_max,char *detail,size_t detail_max) {
    main[0]=sub[0]=detail[0]=0;
    if(!pscloud_title_valid(title)||strchr(user,'/')||strlen(user)>16||strchr(slot,'/')||strlen(slot)>63)return -1;
    char path[1600];if(snprintf(path,sizeof path,"%s/%s/savedata_prospero_meta/user/%s/sce_bu_%s.sfo",home,user,title,slot)>=(int)sizeof path)return -1;
    static const char *const keys[]={"MAINTITLE","SUBTITLE","DETAIL"};char *const outs[]={main,sub,detail};const size_t maxes[]={main_max,sub_max,detail_max};
    return sfo_fields(path,keys,outs,maxes,3);
}
int pscloud_save_title_name(const char *home,const char *user,const char *title,char *name,size_t max) {
    if(!max||!pscloud_title_valid(title)||strchr(user,'/')||strlen(user)>16)return -1;
    char dir[1400];if(snprintf(dir,sizeof dir,"%s/%s/savedata_prospero_meta/user/%s",home,user,title)>=(int)sizeof dir)return -1;
    DIR *d=opendir(dir);if(!d)return -1;
    char agreed[256]={0};int conflict=0;unsigned files=0;struct dirent *e;
    while((e=readdir(d))&&files<64) {
        size_t n=strlen(e->d_name);if(n<5||strcmp(e->d_name+n-4,".sfo")||strchr(e->d_name,'/'))continue;
        char path[1700],label[256],slot[96];if(snprintf(path,sizeof path,"%s/%s",dir,e->d_name)>=(int)sizeof path)continue;
        files++;if(sfo_texts(path,label,sizeof label,slot,sizeof slot))continue;
        if(!strcmp(label,slot))continue; /* "ProfileData" for ProfileData says nothing about the game */
        if(generic_label(label)) {conflict=1;continue;} /* "Settings", "Save Data": saves are named by slot, not game */
        if(!agreed[0])snprintf(agreed,sizeof agreed,"%s",label);else if(strcmp(agreed,label))conflict=1;
    }
    closedir(d);
    if(!agreed[0]||conflict)return -1;
    snprintf(name,max,"%s",agreed);return 0;
}

/* ---------- The PS5's save database, read-only ----------
 * /system_data/savedata_prospero/<profile>/db/user/savedata.db is SQLite (rollback journal).
 * The table "savedata" has one row per save slot; its "mtime" column ("2026-10-11T14:13:29.00Z")
 * is when the game last saved that slot. This is a minimal reader for that one table: it walks
 * the table b-tree and decodes three text columns. It never writes. */
static unsigned long long db_varint(const unsigned char *p,const unsigned char *end,int *len) {
    unsigned long long v=0;int i;
    for(i=0;i<8&&p+i<end;i++) {v=(v<<7)|(p[i]&0x7f);if(!(p[i]&0x80)) {*len=i+1;return v;}}
    if(p+8<end) {v=(v<<8)|p[8];*len=9;return v;}
    *len=0;return 0;
}
static unsigned db_u16(const unsigned char *p) {return ((unsigned)p[0]<<8)|p[1];}
static unsigned db_u32(const unsigned char *p) {return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3];}
struct sqlite_file {unsigned char *data;size_t size;unsigned page,usable;};
/* Full payload of a table-leaf cell (follows overflow pages). Caller frees. */
static unsigned char *db_payload(const struct sqlite_file *f,const unsigned char *cell,const unsigned char *page_end,size_t *out) {
    int a=0,b=0;unsigned long long size=db_varint(cell,page_end,&a);if(!a)return NULL;
    db_varint(cell+a,page_end,&b);if(!b||size>8*1024*1024)return NULL;
    const unsigned char *local=cell+a+b;unsigned U=f->usable,X=U-35;size_t here=size;
    if(size>X) {unsigned M=((U-12)*32/255)-23,K=M+(unsigned)((size-M)%(U-4));here=K<=X?K:M;}
    if(local+here>page_end)return NULL;
    unsigned char *buf=malloc(size+1);if(!buf)return NULL;memcpy(buf,local,here);size_t have=here;
    unsigned next=size>here?db_u32(local+here):0;int guard=0;
    while(have<size&&next&&guard++<100000) {
        size_t off=(size_t)(next-1)*f->page;if(off+f->page>f->size) {free(buf);return NULL;}
        const unsigned char *pg=f->data+off;size_t take=size-have<U-4?size-have:U-4;memcpy(buf+have,pg+4,take);have+=take;next=db_u32(pg);
    }
    if(have<size) {free(buf);return NULL;}
    *out=size;return buf;
}
/* Column n of a record as text (or NULL). */
static int db_column(const unsigned char *rec,size_t size,int n,char *out,size_t max,long long *number) {
    int l=0;unsigned long long header=db_varint(rec,rec+size,&l);if(!l||header>size)return -1;
    size_t at=(size_t)l,body=(size_t)header;out[0]=0;if(number)*number=0;
    for(int col=0;at<header;col++) {
        int k=0;unsigned long long type=db_varint(rec+at,rec+header,&k);if(!k)return -1;at+=(size_t)k;
        size_t len=type>=12?(size_t)((type-12)/2):type<=4?(size_t)type:type==5?6:type==6||type==7?8:0;
        if(col==n) {
            if(body+len>size)return -1;
            if(type>=13&&type%2) {size_t c=len<max-1?len:max-1;memcpy(out,rec+body,c);out[c]=0;return 0;}
            if(type>=1&&type<=6&&number) {long long v=(rec[body]&0x80)?-1:0;for(size_t i=0;i<len;i++)v=(v<<8)|rec[body+i];*number=v;return 0;}
            return type==0?1:0;
        }
        body+=len;
    }
    return -1;
}
typedef void (*db_row_fn)(const unsigned char *rec,size_t size,void *ctx);
static void db_walk(const struct sqlite_file *f,unsigned pageno,int depth,db_row_fn fn,void *ctx) {
    if(!pageno||depth>20||(size_t)pageno*f->page>f->size)return;
    const unsigned char *pg=f->data+(size_t)(pageno-1)*f->page,*hdr=pg+(pageno==1?100:0),*end=pg+f->page;
    unsigned type=hdr[0],cells=db_u16(hdr+3),hlen=type==5?12:8;
    for(unsigned i=0;i<cells;i++) {
        unsigned off=db_u16(hdr+hlen+2*i);if(off>=f->page)return;
        if(type==5)db_walk(f,db_u32(pg+off),depth+1,fn,ctx);
        else if(type==13) {size_t n=0;unsigned char *rec=db_payload(f,pg+off,end,&n);if(rec) {fn(rec,n,ctx);free(rec);}}
    }
    if(type==5)db_walk(f,db_u32(hdr+8),depth+1,fn,ctx);
}
struct master_ctx {unsigned root;char sql[4096];};
static void master_row(const unsigned char *rec,size_t size,void *ctx) {
    struct master_ctx *m=ctx;char kind[16],name[64];long long root=0;
    if(db_column(rec,size,0,kind,sizeof kind,NULL)||db_column(rec,size,1,name,sizeof name,NULL))return;
    if(strcmp(kind,"table")||strcmp(name,"savedata"))return;
    char ignored[8];if(db_column(rec,size,3,ignored,sizeof ignored,&root))return;
    m->root=(unsigned)root;db_column(rec,size,4,m->sql,sizeof m->sql,NULL);
}
/* Position of a column in CREATE TABLE savedata(...). */
static int db_column_index(const char *sql,const char *name) {
    const char *p=strchr(sql,'(');if(!p)return -1;p++;int index=0,depth=0;
    while(*p) {
        while(*p==' '||*p=='\n'||*p=='\t'||*p=='"'||*p=='`'||*p=='[')p++;
        size_t n=strlen(name);if(!strncmp(p,name,n)&&(p[n]==' '||p[n]=='"'||p[n]==','||p[n]=='`'||p[n]==']'||p[n]==')'))return index;
        for(;*p;p++) {if(*p=='(')depth++;else if(*p==')') {if(!depth)return -1;depth--;}else if(*p==','&&!depth) {p++;break;}}
        index++;
    }
    return -1;
}
static long long iso_time(const char *t) {
    int y,mo,d,h,mi,s;if(sscanf(t,"%d-%d-%dT%d:%d:%d",&y,&mo,&d,&h,&mi,&s)!=6)return 0;
    long long yy=y-(mo<=2),era=yy/400,yoe=yy-era*400,mp=(mo+9)%12,doy=(153*mp+2)/5+d-1,doe=yoe*365+yoe/4-yoe/100+doy;
    return (era*146097+doe-719468)*86400LL+h*3600+mi*60+s;
}
struct slots_ctx {const char *title;int c_title,c_dir,c_mtime;struct pscloud_db_slot *out;int n,max;};
static void slot_row(const unsigned char *rec,size_t size,void *ctx) {
    struct slots_ctx *c=ctx;if(c->n>=c->max)return;char title[16],dir[80],when[40];
    if(db_column(rec,size,c->c_title,title,sizeof title,NULL)||strcmp(title,c->title))return;
    if(db_column(rec,size,c->c_dir,dir,sizeof dir,NULL)||db_column(rec,size,c->c_mtime,when,sizeof when,NULL))return;
    long long t=iso_time(when);if(!t||!*dir)return;
    snprintf(c->out[c->n].slot,sizeof c->out[c->n].slot,"%s",dir);c->out[c->n].saved=t;c->n++;
}
int pscloud_save_db_times(const char *path,const char *title,struct pscloud_db_slot *out,int max) {
    if(!pscloud_title_valid(title))return -1;
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return -1;
    struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<512||st.st_size>64*1024*1024) {close(fd);return -1;}
    struct sqlite_file f={malloc((size_t)st.st_size),(size_t)st.st_size,0,0};size_t have=0;
    while(f.data&&have<f.size) {ssize_t n=pread(fd,f.data+have,f.size-have,(off_t)have);if(n<=0)break;have+=(size_t)n;}
    close(fd);
    if(!f.data||have!=f.size||memcmp(f.data,"SQLite format 3",16)) {free(f.data);return -1;}
    f.page=db_u16(f.data+16);if(f.page==1)f.page=65536;f.usable=f.page-f.data[20];
    if(f.page<512||f.size%f.page) {free(f.data);return -1;}
    struct master_ctx m={0,{0}};db_walk(&f,1,0,master_row,&m);
    struct slots_ctx c={title,db_column_index(m.sql,"title_id"),db_column_index(m.sql,"dir_name"),db_column_index(m.sql,"mtime"),out,0,max};
    if(!m.root||c.c_title<0||c.c_dir<0||c.c_mtime<0) {free(f.data);return -1;}
    db_walk(&f,m.root,0,slot_row,&c);free(f.data);return c.n;
}
