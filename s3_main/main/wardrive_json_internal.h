#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* A bounded token tree avoids a heap allocation for every server field. Only
 * strict ASCII names/receipts are interpreted; escaped/non-ASCII strings are
 * still syntax-checked and cannot alias a decision key. */
typedef struct { uint16_t start, end, next; char type; } token_t;
typedef struct { const char *s; size_t n, pos; unsigned count; token_t t[160]; } json_t;
static inline void space(json_t *j) { while (j->pos < j->n && strchr(" \r\n\t", j->s[j->pos])) ++j->pos; }
static inline bool ch(json_t *j, char c) { space(j); if (j->pos < j->n && j->s[j->pos] == c) { ++j->pos; return true; } return false; }
static inline bool hex4(json_t *j, unsigned *v)
{
    *v=0;
    for (unsigned i=0;i<4;++i) {
        if (j->pos == j->n) return false;
        unsigned char c=j->s[j->pos++]; unsigned x;
        if(c>='0'&&c<='9')x=c-'0'; else if(c>='a'&&c<='f')x=c-'a'+10;
        else if(c>='A'&&c<='F')x=c-'A'+10; else return false;
        *v=*v*16+x;
    } return true;
}
static inline bool string(json_t *j)
{
    if (!ch(j,'"')) return false;
    while(j->pos<j->n) {
        unsigned c=(unsigned char)j->s[j->pos++];
        if(c=='"')return true;
        if(c<32)return false;
        if(c=='\\') {
            if(j->pos==j->n)return false;
            c=(unsigned char)j->s[j->pos++];
            if(c=='u') {
                if(!hex4(j,&c))return false;
                if(c>=0xd800&&c<=0xdbff) {
                    unsigned low;
                    if(j->n-j->pos<2||j->s[j->pos++]!='\\'||j->s[j->pos++]!='u'||
                        !hex4(j,&low)||low<0xdc00||low>0xdfff)return false;
                } else if(c>=0xdc00&&c<=0xdfff)return false;
            } else if(!strchr("\"\\/bfnrt",(int)c))return false;
        } else if(c>=128) {
            unsigned more,min;
            if(c>=0xc2&&c<=0xdf){more=1;min=0x80;c&=31;}
            else if(c>=0xe0&&c<=0xef){more=2;min=0x800;c&=15;}
            else if(c>=0xf0&&c<=0xf4){more=3;min=0x10000;c&=7;} else return false;
            while(more--){if(j->pos==j->n||((unsigned char)j->s[j->pos]&0xc0)!=0x80)return false;
                c=c*64+((unsigned char)j->s[j->pos++]&63);}
            if(c<min||c>0x10ffff||(c>=0xd800&&c<=0xdfff))return false;
        }
    } return false;
}
static inline bool value(json_t *j,unsigned depth)
{
    space(j); if(depth>10||j->pos==j->n||j->count==160)return false;
    unsigned index=j->count++; token_t *t=&j->t[index];t->start=(uint16_t)j->pos;
    char c=j->s[j->pos]; t->type=c;
    if(c=='{'||c=='[') {
        ++j->pos; char end=c=='{'?'}':']';
        if(!ch(j,end))for(;;) {
            if(c=='{') {
                space(j); unsigned key=j->count;
                if(j->pos==j->n||j->s[j->pos]!='"'||!value(j,depth+1)||!ch(j,':'))return false;
                if(memchr(j->s+j->t[key].start,'\\',j->t[key].end-j->t[key].start))return false;
                /* Reject duplicate keys, including unknown ones. Escaped names
                 * cannot be used as protocol keys and so fail closed. */
                for(unsigned k=index+1;k<key;k=j->t[j->t[k].next].next)
                    if(j->t[k].end-j->t[k].start==j->t[key].end-j->t[key].start&&
                        !memcmp(j->s+j->t[k].start,j->s+j->t[key].start,j->t[k].end-j->t[k].start))return false;
            }
            if(!value(j,depth+1))return false;
            if(ch(j,end))break;
            if(!ch(j,','))return false;
        }
    } else if(c=='"') { if(!string(j))return false; }
    else if(c=='t'||c=='f'||c=='n') {
        const char *v=c=='t'?"true":c=='f'?"false":"null";size_t n=strlen(v);
        if(j->n-j->pos<n||memcmp(j->s+j->pos,v,n))return false;
        j->pos+=n;
    } else {
        if(c=='-')++j->pos;
        if(j->pos==j->n)return false;
        c=j->s[j->pos];
        if(c=='0')++j->pos;
        else { if(c<'1'||c>'9')return false;do{++j->pos;}while(j->pos<j->n&&j->s[j->pos]>='0'&&j->s[j->pos]<='9'); }
        if(j->pos<j->n&&j->s[j->pos]=='.') {
            size_t start=++j->pos;while(j->pos<j->n&&j->s[j->pos]>='0'&&j->s[j->pos]<='9')++j->pos;
            if(j->pos==start)return false;
        }
        if(j->pos<j->n&&(j->s[j->pos]=='e'||j->s[j->pos]=='E')) {
            ++j->pos;if(j->pos<j->n&&(j->s[j->pos]=='+'||j->s[j->pos]=='-'))++j->pos;
            size_t start=j->pos;while(j->pos<j->n&&j->s[j->pos]>='0'&&j->s[j->pos]<='9')++j->pos;
            if(j->pos==start)return false;
        } t->type='0';
    }
    t->end=(uint16_t)j->pos;t->next=(uint16_t)j->count;return true;
}
static inline int field(const json_t *j,int parent,const char *key)
{
    if(parent<0||j->t[parent].type!='{')return -1;
    size_t n=strlen(key);
    for(unsigned k=(unsigned)parent+1;k<j->t[parent].next;) {
        const token_t *t=&j->t[k];unsigned v=t->next;
        if(t->end-t->start==n+2&&!memcmp(j->s+t->start+1,key,n))return (int)v;
        k=j->t[v].next;
    } return -1;
}
static inline bool literal(const json_t *j,int i,const char *v)
{return i>=0&&j->t[i].end-j->t[i].start==strlen(v)&&!memcmp(j->s+j->t[i].start,v,strlen(v));}
