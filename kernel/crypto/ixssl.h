#ifndef IXSSL_H
#define IXSSL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ixssl_sha256(const uint8_t* data, uint32_t len, uint8_t hash[32]);
void ixssl_hmac_sha256(const uint8_t* key, uint32_t key_len,
                       const uint8_t* data, uint32_t len, uint8_t mac[32]);
void ixssl_hkdf_extract(const uint8_t* salt, uint32_t salt_len,
                        const uint8_t* ikm, uint32_t ikm_len, uint8_t prk[32]);
void ixssl_hkdf_expand(const uint8_t* prk, uint32_t prk_len,
                       const uint8_t* info, uint32_t info_len,
                       uint8_t* okm, uint32_t okm_len);
void ixssl_aes_encrypt(const uint8_t in[16], uint8_t out[16], const uint8_t* key, int key_bits);
void ixssl_aes_gcm_encrypt(const uint8_t* key, uint32_t key_len,
                           const uint8_t* iv, uint32_t iv_len,
                           const uint8_t* aad, uint32_t aad_len,
                           const uint8_t* pt, uint32_t pt_len,
                           uint8_t* ct, uint8_t tag[16]);
int  ixssl_aes_gcm_decrypt(const uint8_t* key, uint32_t key_len,
                           const uint8_t* iv, uint32_t iv_len,
                           const uint8_t* aad, uint32_t aad_len,
                           const uint8_t* ct, uint32_t ct_len,
                           uint8_t* pt, const uint8_t tag[16]);
void ixssl_chacha20(const uint8_t key[32], const uint8_t nonce[12],
                    uint32_t counter, const uint8_t* in, uint8_t* out, uint32_t len);
void ixssl_poly1305(const uint8_t key[32], const uint8_t* msg, uint32_t len, uint8_t tag[16]);
void ixssl_chacha20_poly1305_encrypt(const uint8_t key[32], const uint8_t nonce[12],
    const uint8_t* aad, uint32_t aad_len,
    const uint8_t* pt, uint32_t pt_len, uint8_t* ct, uint8_t tag[16]);
int  ixssl_chacha20_poly1305_decrypt(const uint8_t key[32], const uint8_t nonce[12],
    const uint8_t* aad, uint32_t aad_len,
    const uint8_t* ct, uint32_t ct_len, uint8_t* pt, const uint8_t tag[16]);
void ixssl_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void ixssl_x25519_base(uint8_t pub[32], const uint8_t priv[32]);

#ifdef IXSSL_IMPLEMENTATION

#ifndef IXSSL_MEMCPY
static void ixssl__memcpy(void*d,const void*s,uint32_t n){uint8_t*dd=(uint8_t*)d;const uint8_t*ss=(const uint8_t*)s;while(n--)(*dd++=*ss++);}
static void ixssl__memset(void*d,int v,uint32_t n){uint8_t*dd=(uint8_t*)d;while(n--)(*dd++=(uint8_t)v);}
static int ixssl__memcmp(const void*a,const void*b,uint32_t n){const uint8_t*aa=(const uint8_t*)a;const uint8_t*bb=(const uint8_t*)b;for(uint32_t i=0;i<n;i++){if(aa[i]!=bb[i])return aa[i]-bb[i];}return 0;}
#define IXSSL_MEMCPY ixssl__memcpy
#define IXSSL_MEMSET ixssl__memset
#define IXSSL_MEMCMP ixssl__memcmp
#endif

#if 1
typedef struct { uint64_t lo, hi; } ixssl_u128;
static ixssl_u128 ixssl__u128_mul(uint64_t a, uint64_t b){
    uint64_t al=a&0xFFFFFFFFu,ah=a>>32,bl=b&0xFFFFFFFFu,bh=b>>32;
    uint64_t t0=al*bl,t1=al*bh,t2=ah*bl,t3=ah*bh;
    uint64_t lo=t0+(t1<<32)+(t2<<32);
    uint64_t hi=t3+(t1>>32)+(t2>>32)+(lo<t0?1:0);
    ixssl_u128 r;r.lo=lo;r.hi=hi;return r;
}
static ixssl_u128 ixssl__u128_add(ixssl_u128 a, ixssl_u128 b){
    ixssl_u128 r;r.lo=a.lo+b.lo;r.hi=a.hi+b.hi+(r.lo<a.lo?1:0);return r;
}
static uint64_t ixssl__u128_shr(ixssl_u128 v, int n){
    return(n<64)?((v.lo>>n)|(v.hi<<(64-n))):(v.hi>>(n-64));
}
static ixssl_u128 ixssl__u128_from64(uint64_t v){ixssl_u128 r;r.lo=v;r.hi=0;return r;}
#define IXSSL_U128_MUL(a,b) ixssl__u128_mul((uint64_t)(a),(uint64_t)(b))
#define IXSSL_U128_ADD(a,b) ixssl__u128_add(a,b)
#define IXSSL_U128_SHR(v,n) ixssl__u128_shr(v,n)
#define IXSSL_U128_LO(v) ((v).lo)
#define IXSSL_U128_FROM64(v) ixssl__u128_from64(v)
#define IXSSL_MSVC_U128 1
#endif

typedef struct {
  uint32_t state[8];
  uint64_t count;
  uint8_t buf[64];
} ixssl_sha256_ctx;

typedef struct {
  uint32_t r0, r1, r2, r3, r4;
  uint32_t s1v, s2v, s3v, s4v;
  uint32_t h0, h1, h2, h3, h4;
  uint8_t sv[16];
  uint8_t buf[16];
  uint32_t buf_used;
} ixssl_poly1305_ctx;

static const uint32_t ixssl__sha256_k[64]={
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

static uint32_t ixssl__rotr(uint32_t x,uint32_t n){return(x>>n)|(x<<(32-n));}

static void ixssl__sha256_transform(uint32_t state[8],const uint8_t blk[64]){
  uint32_t m[64],a,b,c,d,e,f,g,h;
  for(uint32_t i=0;i<16;i++){uint32_t o=i*4;m[i]=((uint32_t)blk[o]<<24)|((uint32_t)blk[o+1]<<16)|((uint32_t)blk[o+2]<<8)|blk[o+3];}
  for(uint32_t i=16;i<64;i++){uint32_t s0=ixssl__rotr(m[i-15],7)^ixssl__rotr(m[i-15],18)^(m[i-15]>>3);uint32_t s1=ixssl__rotr(m[i-2],17)^ixssl__rotr(m[i-2],19)^(m[i-2]>>10);m[i]=m[i-16]+s0+m[i-7]+s1;}
  a=state[0];b=state[1];c=state[2];d=state[3];e=state[4];f=state[5];g=state[6];h=state[7];
  for(uint32_t i=0;i<64;i++){uint32_t S1=ixssl__rotr(e,6)^ixssl__rotr(e,11)^ixssl__rotr(e,25);uint32_t ch=(e&f)^((~e)&g);uint32_t t1=h+S1+ch+ixssl__sha256_k[i]+m[i];uint32_t S0=ixssl__rotr(a,2)^ixssl__rotr(a,13)^ixssl__rotr(a,22);uint32_t maj=(a&b)^(a&c)^(b&c);uint32_t t2=S0+maj;h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
  state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
}

static void ixssl_sha256_init(ixssl_sha256_ctx* ctx) {
  ctx->state[0]=0x6a09e667;ctx->state[1]=0xbb67ae85;ctx->state[2]=0x3c6ef372;ctx->state[3]=0xa54ff53a;
  ctx->state[4]=0x510e527f;ctx->state[5]=0x9b05688c;ctx->state[6]=0x1f83d9ab;ctx->state[7]=0x5be0cd19;
  ctx->count=0;
}

static void ixssl_sha256_update(ixssl_sha256_ctx* ctx, const uint8_t* data, uint32_t len) {
  uint32_t active=(uint32_t)(ctx->count&63);
  ctx->count+=len;
  uint32_t available=64-active;
  uint32_t offset=0;
  if(len>=available){
    IXSSL_MEMCPY(ctx->buf+active,data,available);
    ixssl__sha256_transform(ctx->state,ctx->buf);
    offset=available;
    while(len-offset>=64){
      ixssl__sha256_transform(ctx->state,data+offset);
      offset+=64;
    }
    active=0;
  }
  if(offset<len){
    IXSSL_MEMCPY(ctx->buf+active,data+offset,len-offset);
  }
}

static void ixssl_sha256_final(ixssl_sha256_ctx* ctx, uint8_t hash[32]) {
  uint32_t active=(uint32_t)(ctx->count&63);
  uint64_t bits=ctx->count*8;
  ctx->buf[active++]=0x80;
  if(active>56){
    while(active<64)ctx->buf[active++]=0;
    ixssl__sha256_transform(ctx->state,ctx->buf);
    active=0;
  }
  while(active<56)ctx->buf[active++]=0;
  for(int i=7;i>=0;i--)ctx->buf[56+(7-i)]=(uint8_t)(bits>>(i*8));
  ixssl__sha256_transform(ctx->state,ctx->buf);
  for(int i=0;i<8;i++){
    hash[i*4]=(uint8_t)(ctx->state[i]>>24);
    hash[i*4+1]=(uint8_t)(ctx->state[i]>>16);
    hash[i*4+2]=(uint8_t)(ctx->state[i]>>8);
    hash[i*4+3]=(uint8_t)ctx->state[i];
  }
}

void ixssl_sha256(const uint8_t* data, uint32_t len, uint8_t hash[32]){
  ixssl_sha256_ctx ctx;
  ixssl_sha256_init(&ctx);
  if(data&&len)ixssl_sha256_update(&ctx,data,len);
  ixssl_sha256_final(&ctx,hash);
}

void ixssl_hmac_sha256(const uint8_t* key,uint32_t key_len,const uint8_t* data,uint32_t len,uint8_t mac[32]){
  uint8_t kb[64],ip[64],op[64],ih[32];
  IXSSL_MEMSET(kb,0,64);
  if(key_len>64){ixssl_sha256(key,key_len,kb);}else if(key&&key_len){IXSSL_MEMCPY(kb,key,key_len);}
  for(int i=0;i<64;i++){ip[i]=kb[i]^0x36;op[i]=kb[i]^0x5c;}
  ixssl_sha256_ctx ctx;
  ixssl_sha256_init(&ctx);
  ixssl_sha256_update(&ctx,ip,64);
  if(data&&len)ixssl_sha256_update(&ctx,data,len);
  ixssl_sha256_final(&ctx,ih);
  ixssl_sha256_init(&ctx);
  ixssl_sha256_update(&ctx,op,64);
  ixssl_sha256_update(&ctx,ih,32);
  ixssl_sha256_final(&ctx,mac);
}

void ixssl_hkdf_extract(const uint8_t* salt,uint32_t sl,const uint8_t* ikm,uint32_t il,uint8_t prk[32]){
  if(!salt||sl==0){uint8_t z[32];IXSSL_MEMSET(z,0,32);ixssl_hmac_sha256(z,32,ikm,il,prk);}
  else ixssl_hmac_sha256(salt,sl,ikm,il,prk);
}

void ixssl_hkdf_expand(const uint8_t* prk,uint32_t pl,const uint8_t* info,uint32_t il,uint8_t* okm,uint32_t ol){
  uint8_t T[32];uint32_t Tl=0,done=0;uint8_t ctr=1;
  while(done<ol){
    ixssl_sha256_ctx ctx;
    uint8_t kb[64],ip[64],op[64];
    IXSSL_MEMSET(kb,0,64);
    if(pl>64){ixssl_sha256(prk,pl,kb);}else if(prk&&pl){IXSSL_MEMCPY(kb,prk,pl);}
    for(int i=0;i<64;i++){ip[i]=kb[i]^0x36;op[i]=kb[i]^0x5c;}
    ixssl_sha256_init(&ctx);
    ixssl_sha256_update(&ctx,ip,64);
    if(Tl>0)ixssl_sha256_update(&ctx,T,Tl);
    if(info&&il>0)ixssl_sha256_update(&ctx,info,il);
    ixssl_sha256_update(&ctx,&ctr,1);
    uint8_t ih[32];
    ixssl_sha256_final(&ctx,ih);
    ixssl_sha256_init(&ctx);
    ixssl_sha256_update(&ctx,op,64);
    ixssl_sha256_update(&ctx,ih,32);
    ixssl_sha256_final(&ctx,T);
    Tl=32;
    uint32_t cp=ol-done;if(cp>32)cp=32;
    IXSSL_MEMCPY(okm+done,T,cp);done+=cp;ctr++;
  }
}

static uint32_t ixssl__rotl(uint32_t x,int n){return(x<<n)|(x>>(32-n));}
static void ixssl__qr(uint32_t*a,uint32_t*b,uint32_t*c,uint32_t*d){*a+=*b;*d^=*a;*d=ixssl__rotl(*d,16);*c+=*d;*b^=*c;*b=ixssl__rotl(*b,12);*a+=*b;*d^=*a;*d=ixssl__rotl(*d,8);*c+=*d;*b^=*c;*b=ixssl__rotl(*b,7);}
static uint32_t ixssl__le32(const uint8_t*p){return(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void ixssl__le32w(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}

void ixssl_chacha20(const uint8_t key[32],const uint8_t nonce[12],uint32_t counter,const uint8_t* in,uint8_t* out,uint32_t len){
  uint32_t s[16]={0x61707865,0x3320646e,0x79622d32,0x6b206574};
  for(int i=0;i<8;i++)s[4+i]=ixssl__le32(key+i*4);
  s[12]=counter;s[13]=ixssl__le32(nonce);s[14]=ixssl__le32(nonce+4);s[15]=ixssl__le32(nonce+8);
  uint32_t off=0;
  while(off<len){
    uint32_t x[16];IXSSL_MEMCPY(x,s,64);
    for(int i=0;i<10;i++){ixssl__qr(&x[0],&x[4],&x[8],&x[12]);ixssl__qr(&x[1],&x[5],&x[9],&x[13]);ixssl__qr(&x[2],&x[6],&x[10],&x[14]);ixssl__qr(&x[3],&x[7],&x[11],&x[15]);ixssl__qr(&x[0],&x[5],&x[10],&x[15]);ixssl__qr(&x[1],&x[6],&x[11],&x[12]);ixssl__qr(&x[2],&x[7],&x[8],&x[13]);ixssl__qr(&x[3],&x[4],&x[9],&x[14]);}
    uint8_t ks[64];for(int i=0;i<16;i++)ixssl__le32w(ks+i*4,x[i]+s[i]);
    uint32_t ch=len-off;if(ch>64)ch=64;
    for(uint32_t i=0;i<ch;i++)out[off+i]=in[off+i]^ks[i];
    off+=ch;s[12]++;
  }
}

static void ixssl__poly1305_process_block(ixssl_poly1305_ctx* ctx, const uint8_t* block, uint32_t block_len) {
  uint8_t blk[17];
  IXSSL_MEMSET(blk,0,17);
  IXSSL_MEMCPY(blk,block,block_len);
  blk[block_len]=1;
  uint32_t t0=ixssl__le32(blk),t1=ixssl__le32(blk+4),t2=ixssl__le32(blk+8),t3=ixssl__le32(blk+12);
  ctx->h0+=(t0)&0x3FFFFFFu;
  ctx->h1+=((t0>>26)|(t1<<6))&0x3FFFFFFu;
  ctx->h2+=((t1>>20)|(t2<<12))&0x3FFFFFFu;
  ctx->h3+=((t2>>14)|(t3<<18))&0x3FFFFFFu;
  ctx->h4+=((t3>>8))|((uint32_t)blk[16]<<24);
  uint64_t d0=(uint64_t)ctx->h0*ctx->r0+(uint64_t)ctx->h1*ctx->s4v+(uint64_t)ctx->h2*ctx->s3v+(uint64_t)ctx->h3*ctx->s2v+(uint64_t)ctx->h4*ctx->s1v;
  uint64_t d1=(uint64_t)ctx->h0*ctx->r1+(uint64_t)ctx->h1*ctx->r0+(uint64_t)ctx->h2*ctx->s4v+(uint64_t)ctx->h3*ctx->s3v+(uint64_t)ctx->h4*ctx->s2v;
  uint64_t d2=(uint64_t)ctx->h0*ctx->r2+(uint64_t)ctx->h1*ctx->r1+(uint64_t)ctx->h2*ctx->r0+(uint64_t)ctx->h3*ctx->s4v+(uint64_t)ctx->h4*ctx->s3v;
  uint64_t d3=(uint64_t)ctx->h0*ctx->r3+(uint64_t)ctx->h1*ctx->r2+(uint64_t)ctx->h2*ctx->r1+(uint64_t)ctx->h3*ctx->r0+(uint64_t)ctx->h4*ctx->s4v;
  uint64_t d4=(uint64_t)ctx->h0*ctx->r4+(uint64_t)ctx->h1*ctx->r3+(uint64_t)ctx->h2*ctx->r2+(uint64_t)ctx->h3*ctx->r1+(uint64_t)ctx->h4*ctx->r0;
  uint32_t c;
  c=(uint32_t)(d0>>26);ctx->h0=(uint32_t)d0&0x3FFFFFFu;d1+=c;
  c=(uint32_t)(d1>>26);ctx->h1=(uint32_t)d1&0x3FFFFFFu;d2+=c;
  c=(uint32_t)(d2>>26);ctx->h2=(uint32_t)d2&0x3FFFFFFu;d3+=c;
  c=(uint32_t)(d3>>26);ctx->h3=(uint32_t)d3&0x3FFFFFFu;d4+=c;
  c=(uint32_t)(d4>>26);ctx->h4=(uint32_t)d4&0x3FFFFFFu;ctx->h0+=c*5;
  c=ctx->h0>>26;ctx->h0&=0x3FFFFFFu;ctx->h1+=c;
}

static void ixssl_poly1305_init(ixssl_poly1305_ctx* ctx, const uint8_t key[32]) {
  uint8_t r[16];
  IXSSL_MEMCPY(r,key,16);
  IXSSL_MEMCPY(ctx->sv,key+16,16);
  r[3]&=15;r[7]&=15;r[11]&=15;r[15]&=15;r[4]&=252;r[8]&=252;r[12]&=252;
  ctx->r0=ixssl__le32(r)&0x3FFFFFFu;
  ctx->r1=((ixssl__le32(r)>>26)|(ixssl__le32(r+3)>>2))&0x3FFFFFFu;
  ctx->r2=(ixssl__le32(r+6)>>4)&0x3FFFFFFu;
  ctx->r3=(ixssl__le32(r+9)>>6)&0x3FFFFFFu;
  ctx->r4=(ixssl__le32(r+12)>>8)&0x3FFFFFFu;
  ctx->s1v=ctx->r1*5;ctx->s2v=ctx->r2*5;ctx->s3v=ctx->r3*5;ctx->s4v=ctx->r4*5;
  ctx->h0=ctx->h1=ctx->h2=ctx->h3=ctx->h4=0;
  ctx->buf_used=0;
}

static void ixssl_poly1305_update(ixssl_poly1305_ctx* ctx, const uint8_t* msg, uint32_t len) {
  uint32_t off=0;
  if(ctx->buf_used>0){
    uint32_t fill=16-ctx->buf_used;
    if(len<fill){
      IXSSL_MEMCPY(ctx->buf+ctx->buf_used,msg,len);
      ctx->buf_used+=len;
      return;
    }
    IXSSL_MEMCPY(ctx->buf+ctx->buf_used,msg,fill);
    ixssl__poly1305_process_block(ctx,ctx->buf,16);
    ctx->buf_used=0;
    off+=fill;
  }
  while(len-off>=16){
    ixssl__poly1305_process_block(ctx,msg+off,16);
    off+=16;
  }
  if(off<len){
    IXSSL_MEMCPY(ctx->buf,msg+off,len-off);
    ctx->buf_used=len-off;
  }
}

static void ixssl_poly1305_final(ixssl_poly1305_ctx* ctx, uint8_t tag[16]) {
  if(ctx->buf_used>0){
    ixssl__poly1305_process_block(ctx,ctx->buf,ctx->buf_used);
    ctx->buf_used=0;
  }
  uint32_t c;
  c=ctx->h1>>26;ctx->h1&=0x3FFFFFFu;ctx->h2+=c;c=ctx->h2>>26;ctx->h2&=0x3FFFFFFu;ctx->h3+=c;
  c=ctx->h3>>26;ctx->h3&=0x3FFFFFFu;ctx->h4+=c;c=ctx->h4>>26;ctx->h4&=0x3FFFFFFu;ctx->h0+=c*5;
  c=ctx->h0>>26;ctx->h0&=0x3FFFFFFu;ctx->h1+=c;
  uint32_t g0=ctx->h0+5;c=g0>>26;g0&=0x3FFFFFFu;
  uint32_t g1=ctx->h1+c;c=g1>>26;g1&=0x3FFFFFFu;
  uint32_t g2=ctx->h2+c;c=g2>>26;g2&=0x3FFFFFFu;
  uint32_t g3=ctx->h3+c;c=g3>>26;g3&=0x3FFFFFFu;
  uint32_t g4=ctx->h4+c-(1u<<26);
  uint32_t mask=~((g4>>31)-1);uint32_t nmask=~mask;
  ctx->h0=(ctx->h0&nmask)|(g0&mask);ctx->h1=(ctx->h1&nmask)|(g1&mask);
  ctx->h2=(ctx->h2&nmask)|(g2&mask);ctx->h3=(ctx->h3&nmask)|(g3&mask);
  ctx->h4=(ctx->h4&nmask)|(g4&mask);
  uint64_t f0=(uint64_t)ctx->h0|((uint64_t)ctx->h1<<26);uint64_t f1=(uint64_t)(ctx->h1>>6)|((uint64_t)ctx->h2<<20);
  uint64_t f2=(uint64_t)(ctx->h2>>12)|((uint64_t)ctx->h3<<14);uint64_t f3=(uint64_t)(ctx->h3>>18)|((uint64_t)ctx->h4<<8);
  uint64_t sv0=(uint64_t)ixssl__le32(ctx->sv)|((uint64_t)ixssl__le32(ctx->sv+4)<<32);
  uint64_t sv1=(uint64_t)ixssl__le32(ctx->sv+8)|((uint64_t)ixssl__le32(ctx->sv+12)<<32);
  f0+=sv0&0xFFFFFFFFu;uint64_t cc=f0>>32;
  f1+=(sv0>>32)+cc;cc=f1>>32;
  f2+=(sv1&0xFFFFFFFFu)+cc;cc=f2>>32;
  f3+=(sv1>>32)+cc;
  ixssl__le32w(tag,(uint32_t)f0);ixssl__le32w(tag+4,(uint32_t)f1);
  ixssl__le32w(tag+8,(uint32_t)f2);ixssl__le32w(tag+12,(uint32_t)f3);
}

void ixssl_poly1305(const uint8_t key[32],const uint8_t* msg,uint32_t len,uint8_t tag[16]){
  ixssl_poly1305_ctx ctx;
  ixssl_poly1305_init(&ctx,key);
  if(msg&&len)ixssl_poly1305_update(&ctx,msg,len);
  ixssl_poly1305_final(&ctx,tag);
}

void ixssl_chacha20_poly1305_encrypt(const uint8_t key[32],const uint8_t nonce[12],const uint8_t* aad,uint32_t al,const uint8_t* pt,uint32_t pl,uint8_t* ct,uint8_t tag[16]){
  uint8_t pk[64],z[64];IXSSL_MEMSET(z,0,64);ixssl_chacha20(key,nonce,0,z,pk,64);
  ixssl_chacha20(key,nonce,1,pt,ct,pl);
  ixssl_poly1305_ctx poly;
  ixssl_poly1305_init(&poly,pk);
  if(aad&&al>0){
    ixssl_poly1305_update(&poly,aad,al);
    uint32_t pad=(16-(al%16))%16;
    if(pad>0){uint8_t zero[16];IXSSL_MEMSET(zero,0,16);ixssl_poly1305_update(&poly,zero,pad);}
  }
  if(ct&&pl>0){
    ixssl_poly1305_update(&poly,ct,pl);
    uint32_t pad=(16-(pl%16))%16;
    if(pad>0){uint8_t zero[16];IXSSL_MEMSET(zero,0,16);ixssl_poly1305_update(&poly,zero,pad);}
  }
  uint8_t len_blk[16];
  ixssl__le32w(len_blk,al);ixssl__le32w(len_blk+4,0);
  ixssl__le32w(len_blk+8,pl);ixssl__le32w(len_blk+12,0);
  ixssl_poly1305_update(&poly,len_blk,16);
  ixssl_poly1305_final(&poly,tag);
}

int ixssl_chacha20_poly1305_decrypt(const uint8_t key[32],const uint8_t nonce[12],const uint8_t* aad,uint32_t al,const uint8_t* ct,uint32_t cl,uint8_t* pt,const uint8_t tag[16]){
  uint8_t pk[64],z[64],exp[16];IXSSL_MEMSET(z,0,64);ixssl_chacha20(key,nonce,0,z,pk,64);
  ixssl_poly1305_ctx poly;
  ixssl_poly1305_init(&poly,pk);
  if(aad&&al>0){
    ixssl_poly1305_update(&poly,aad,al);
    uint32_t pad=(16-(al%16))%16;
    if(pad>0){uint8_t zero[16];IXSSL_MEMSET(zero,0,16);ixssl_poly1305_update(&poly,zero,pad);}
  }
  if(ct&&cl>0){
    ixssl_poly1305_update(&poly,ct,cl);
    uint32_t pad=(16-(cl%16))%16;
    if(pad>0){uint8_t zero[16];IXSSL_MEMSET(zero,0,16);ixssl_poly1305_update(&poly,zero,pad);}
  }
  uint8_t len_blk[16];
  ixssl__le32w(len_blk,al);ixssl__le32w(len_blk+4,0);
  ixssl__le32w(len_blk+8,cl);ixssl__le32w(len_blk+12,0);
  ixssl_poly1305_update(&poly,len_blk,16);
  ixssl_poly1305_final(&poly,exp);
  if(IXSSL_MEMCMP(exp,tag,16)!=0)return 0;
  ixssl_chacha20(key,nonce,1,ct,pt,cl);return 1;
}

typedef uint64_t ixssl_fe[5];
static void ixssl__fe_zero(ixssl_fe o){o[0]=o[1]=o[2]=o[3]=o[4]=0;}
static void ixssl__fe_one(ixssl_fe o){o[0]=1;o[1]=o[2]=o[3]=o[4]=0;}
static void ixssl__fe_copy(ixssl_fe o,const ixssl_fe a){for(int i=0;i<5;i++)o[i]=a[i];}
static void ixssl__fe_add(ixssl_fe o,const ixssl_fe a,const ixssl_fe b){for(int i=0;i<5;i++)o[i]=a[i]+b[i];}
static void ixssl__fe_sub(ixssl_fe o,const ixssl_fe a,const ixssl_fe b){o[0]=a[0]+0xFFFFFFFFFFFDAull-b[0];o[1]=a[1]+0xFFFFFFFFFFFFEull-b[1];o[2]=a[2]+0xFFFFFFFFFFFFEull-b[2];o[3]=a[3]+0xFFFFFFFFFFFFEull-b[3];o[4]=a[4]+0xFFFFFFFFFFFFEull-b[4];}
static void ixssl__fe_reduce(ixssl_fe o){for(int i=0;i<4;i++){o[i+1]+=o[i]>>51;o[i]&=0x7FFFFFFFFFFFFull;}o[0]+=19*(o[4]>>51);o[4]&=0x7FFFFFFFFFFFFull;o[1]+=o[0]>>51;o[0]&=0x7FFFFFFFFFFFFull;}
static void ixssl__fe_mul(ixssl_fe o,const ixssl_fe a,const ixssl_fe b){
  ixssl_u128 b19_1=IXSSL_U128_MUL(b[1],19),b19_2=IXSSL_U128_MUL(b[2],19),b19_3=IXSSL_U128_MUL(b[3],19),b19_4=IXSSL_U128_MUL(b[4],19);
  uint64_t b19v[4]={IXSSL_U128_LO(b19_1),IXSSL_U128_LO(b19_2),IXSSL_U128_LO(b19_3),IXSSL_U128_LO(b19_4)};
  ixssl_u128 t[5];
  t[0]=IXSSL_U128_ADD(IXSSL_U128_ADD(IXSSL_U128_ADD(IXSSL_U128_ADD(IXSSL_U128_MUL(a[0],b[0]),IXSSL_U128_MUL(a[1],b19v[3])),IXSSL_U128_MUL(a[2],b19v[2])),IXSSL_U128_MUL(a[3],b19v[1])),IXSSL_U128_MUL(a[4],b19v[0]));
  t[1]=IXSSL_U128_ADD(IXSSL_U128_ADD(IXSSL_U128_ADD(IXSSL_U128_ADD(IXSSL_U128_MUL(a[0],b[1]),IXSSL_U128_MUL(a[1],b[0])),IXSSL_U128_MUL(a[2],b19v[3])),IXSSL_U128_MUL(a[3],b19v[2])),IXSSL_U128_MUL(a[4],b19v[1]));
  t[2]=IXSSL_U128_ADD(IXSSL_U128_ADD(IXSSL_U128_ADD(IXSSL_U128_ADD(IXSSL_U128_MUL(a[0],b[2]),IXSSL_U128_MUL(a[1],b[1])),IXSSL_U128_MUL(a[2],b[0])),IXSSL_U128_MUL(a[3],b19v[3])),IXSSL_U128_MUL(a[4],b19v[2]));
  t[3]=IXSSL_U128_ADD(IXSSL_U128_ADD(IXSSL_U128_ADD(IXSSL_U128_ADD(IXSSL_U128_MUL(a[0],b[3]),IXSSL_U128_MUL(a[1],b[2])),IXSSL_U128_MUL(a[2],b[1])),IXSSL_U128_MUL(a[3],b[0])),IXSSL_U128_MUL(a[4],b19v[3]));
  t[4]=IXSSL_U128_ADD(IXSSL_U128_ADD(IXSSL_U128_ADD(IXSSL_U128_ADD(IXSSL_U128_MUL(a[0],b[4]),IXSSL_U128_MUL(a[1],b[3])),IXSSL_U128_MUL(a[2],b[2])),IXSSL_U128_MUL(a[3],b[1])),IXSSL_U128_MUL(a[4],b[0]));
  for(int i=0;i<4;i++){t[i+1]=IXSSL_U128_ADD(t[i+1],IXSSL_U128_FROM64(IXSSL_U128_SHR(t[i],51)));o[i]=IXSSL_U128_LO(t[i])&0x7FFFFFFFFFFFFull;}
  o[4]=IXSSL_U128_LO(t[4])&0x7FFFFFFFFFFFFull;o[0]+=19*IXSSL_U128_SHR(t[4],51);o[1]+=o[0]>>51;o[0]&=0x7FFFFFFFFFFFFull;
}
static void ixssl__fe_sq(ixssl_fe o,const ixssl_fe a){ixssl__fe_mul(o,a,a);}
static void ixssl__fe_invert(ixssl_fe o,const ixssl_fe z){
  ixssl_fe t0,t1,t2,t3;
  ixssl__fe_sq(t0,z);ixssl__fe_sq(t1,t0);ixssl__fe_sq(t1,t1);ixssl__fe_mul(t1,z,t1);ixssl__fe_mul(t0,t0,t1);ixssl__fe_sq(t2,t0);ixssl__fe_mul(t1,t1,t2);
  ixssl__fe_sq(t2,t1);for(int i=0;i<4;i++)ixssl__fe_sq(t2,t2);ixssl__fe_mul(t1,t2,t1);
  ixssl__fe_sq(t2,t1);for(int i=0;i<9;i++)ixssl__fe_sq(t2,t2);ixssl__fe_mul(t2,t2,t1);
  ixssl__fe_sq(t3,t2);for(int i=0;i<19;i++)ixssl__fe_sq(t3,t3);ixssl__fe_mul(t2,t3,t2);
  ixssl__fe_sq(t2,t2);for(int i=0;i<9;i++)ixssl__fe_sq(t2,t2);ixssl__fe_mul(t1,t2,t1);
  ixssl__fe_sq(t2,t1);for(int i=0;i<49;i++)ixssl__fe_sq(t2,t2);ixssl__fe_mul(t2,t2,t1);
  ixssl__fe_sq(t3,t2);for(int i=0;i<99;i++)ixssl__fe_sq(t3,t3);ixssl__fe_mul(t2,t3,t2);
  ixssl__fe_sq(t2,t2);for(int i=0;i<49;i++)ixssl__fe_sq(t2,t2);ixssl__fe_mul(t1,t2,t1);
  ixssl__fe_sq(t1,t1);for(int i=0;i<4;i++)ixssl__fe_sq(t1,t1);ixssl__fe_mul(o,t1,t0);
}
static void ixssl__fe_from_bytes(ixssl_fe o,const uint8_t s[32]){
  o[0]=((uint64_t)s[0])|((uint64_t)s[1]<<8)|((uint64_t)s[2]<<16)|((uint64_t)s[3]<<24)|((uint64_t)s[4]<<32)|((uint64_t)s[5]<<40)|((uint64_t)(s[6]&7)<<48);
  o[1]=((uint64_t)(s[6]>>3))|((uint64_t)s[7]<<5)|((uint64_t)s[8]<<13)|((uint64_t)s[9]<<21)|((uint64_t)s[10]<<29)|((uint64_t)s[11]<<37)|((uint64_t)(s[12]&0x3F)<<45);
  o[2]=((uint64_t)(s[12]>>6))|((uint64_t)s[13]<<2)|((uint64_t)s[14]<<10)|((uint64_t)s[15]<<18)|((uint64_t)s[16]<<26)|((uint64_t)s[17]<<34)|((uint64_t)s[18]<<42)|((uint64_t)(s[19]&1)<<50);
  o[3]=((uint64_t)(s[19]>>1))|((uint64_t)s[20]<<7)|((uint64_t)s[21]<<15)|((uint64_t)s[22]<<23)|((uint64_t)s[23]<<31)|((uint64_t)s[24]<<39)|((uint64_t)(s[25]&0xF)<<47);
  o[4]=((uint64_t)(s[25]>>4))|((uint64_t)s[26]<<4)|((uint64_t)s[27]<<12)|((uint64_t)s[28]<<20)|((uint64_t)s[29]<<28)|((uint64_t)s[30]<<36)|((uint64_t)(s[31]&0x7F)<<44);
  ixssl__fe_reduce(o);
}
static void ixssl__fe_to_bytes(uint8_t s[32],const ixssl_fe h){
  ixssl_fe t;ixssl__fe_copy(t,h);ixssl__fe_reduce(t);ixssl__fe_reduce(t);
  s[0]=(uint8_t)t[0];s[1]=(uint8_t)(t[0]>>8);s[2]=(uint8_t)(t[0]>>16);s[3]=(uint8_t)(t[0]>>24);s[4]=(uint8_t)(t[0]>>32);s[5]=(uint8_t)(t[0]>>40);
  s[6]=(uint8_t)((t[0]>>48)|(t[1]<<3));s[7]=(uint8_t)(t[1]>>5);s[8]=(uint8_t)(t[1]>>13);s[9]=(uint8_t)(t[1]>>21);s[10]=(uint8_t)(t[1]>>29);s[11]=(uint8_t)(t[1]>>37);
  s[12]=(uint8_t)((t[1]>>45)|(t[2]<<6));s[13]=(uint8_t)(t[2]>>2);s[14]=(uint8_t)(t[2]>>10);s[15]=(uint8_t)(t[2]>>18);s[16]=(uint8_t)(t[2]>>26);s[17]=(uint8_t)(t[2]>>34);s[18]=(uint8_t)(t[2]>>42);
  s[19]=(uint8_t)((t[2]>>50)|(t[3]<<1));s[20]=(uint8_t)(t[3]>>7);s[21]=(uint8_t)(t[3]>>15);s[22]=(uint8_t)(t[3]>>23);s[23]=(uint8_t)(t[3]>>31);s[24]=(uint8_t)(t[3]>>39);
  s[25]=(uint8_t)((t[3]>>47)|(t[4]<<4));s[26]=(uint8_t)(t[4]>>4);s[27]=(uint8_t)(t[4]>>12);s[28]=(uint8_t)(t[4]>>20);s[29]=(uint8_t)(t[4]>>28);s[30]=(uint8_t)(t[4]>>36);s[31]=(uint8_t)(t[4]>>44);
}

void ixssl_x25519(uint8_t out[32],const uint8_t scalar[32],const uint8_t point[32]){
  uint8_t e[32];IXSSL_MEMCPY(e,scalar,32);e[0]&=248;e[31]&=127;e[31]|=64;
  ixssl_fe u,x2,z2,x3,z3,A,B,AA,BB,C,D,DA,CB,E,t0,t1;
  ixssl__fe_from_bytes(u,point);ixssl__fe_one(x2);ixssl__fe_zero(z2);ixssl__fe_copy(x3,u);ixssl__fe_one(z3);
  int swap=0;
  for(int pos=254;pos>=0;pos--){
    int bit=(e[pos/8]>>(pos%8))&1;swap^=bit;
    if(swap){ixssl_fe tmp;ixssl__fe_copy(tmp,x2);ixssl__fe_copy(x2,x3);ixssl__fe_copy(x3,tmp);ixssl__fe_copy(tmp,z2);ixssl__fe_copy(z2,z3);ixssl__fe_copy(z3,tmp);}
    swap=bit;
    ixssl__fe_add(A,x2,z2);ixssl__fe_reduce(A);
    ixssl__fe_sub(B,x2,z2);ixssl__fe_reduce(B);
    ixssl__fe_sq(AA,A);
    ixssl__fe_sq(BB,B);
    ixssl__fe_add(C,x3,z3);ixssl__fe_reduce(C);
    ixssl__fe_sub(D,x3,z3);ixssl__fe_reduce(D);
    ixssl__fe_mul(DA,D,A);
    ixssl__fe_mul(CB,C,B);
    ixssl__fe_add(t0,DA,CB);ixssl__fe_reduce(t0);ixssl__fe_sq(x3,t0);
    ixssl__fe_sub(t0,DA,CB);ixssl__fe_reduce(t0);ixssl__fe_sq(t1,t0);ixssl__fe_mul(z3,t1,u);
    ixssl__fe_mul(x2,AA,BB);
    ixssl__fe_sub(E,AA,BB);ixssl__fe_reduce(E);
    ixssl_fe a24;ixssl__fe_zero(a24);a24[0]=121666;
    ixssl__fe_mul(t0,a24,E);ixssl__fe_add(t0,t0,BB);ixssl__fe_reduce(t0);
    ixssl__fe_mul(z2,E,t0);
  }
  if(swap){ixssl_fe tmp;ixssl__fe_copy(tmp,x2);ixssl__fe_copy(x2,x3);ixssl__fe_copy(x3,tmp);ixssl__fe_copy(tmp,z2);ixssl__fe_copy(z2,z3);ixssl__fe_copy(z3,tmp);}
  ixssl__fe_invert(z2,z2);ixssl__fe_mul(x2,x2,z2);ixssl__fe_to_bytes(out,x2);
}

void ixssl_x25519_base(uint8_t pub[32],const uint8_t priv[32]){
  uint8_t base[32];IXSSL_MEMSET(base,0,32);base[0]=9;ixssl_x25519(pub,priv,base);
}

void ixssl_aes_encrypt(const uint8_t in[16],uint8_t out[16],const uint8_t* key,int key_bits){
  static const uint8_t sb[256]={0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};
  uint8_t rk[240],s[16];
  int rounds=(key_bits==256)?14:10;
  int key_len=key_bits/8;
  IXSSL_MEMCPY(rk,key,key_len);
  int bytes_gen=key_len;
  int rcon_ptr=0;
  static const uint8_t rcon[]={0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};
  while(bytes_gen<(rounds+1)*16){
    uint8_t temp[4];
    IXSSL_MEMCPY(temp,rk+bytes_gen-4,4);
    if(bytes_gen%key_len==0){
      uint8_t t=temp[0];temp[0]=sb[temp[1]];temp[1]=sb[temp[2]];temp[2]=sb[temp[3]];temp[3]=sb[t];
      temp[0]^=rcon[rcon_ptr++];
    }else if(key_bits==256&&(bytes_gen%key_len)==16){
      temp[0]=sb[temp[0]];temp[1]=sb[temp[1]];temp[2]=sb[temp[2]];temp[3]=sb[temp[3]];
    }
    for(int i=0;i<4;i++){rk[bytes_gen]=rk[bytes_gen-key_len]^temp[i];bytes_gen++;}
  }
  IXSSL_MEMCPY(s,in,16);
  for(int i=0;i<16;i++)s[i]^=rk[i];
  for(int r=1;r<rounds;r++){
    for(int i=0;i<16;i++)s[i]=sb[s[i]];
    uint8_t t=s[1];s[1]=s[5];s[5]=s[9];s[9]=s[13];s[13]=t;
    t=s[2];s[2]=s[10];s[10]=t;
    t=s[6];s[6]=s[14];s[14]=t;
    t=s[15];s[15]=s[11];s[11]=s[7];s[7]=s[3];s[3]=t;
    for(int c=0;c<4;c++){
      int ii=c*4;uint8_t a0=s[ii],a1=s[ii+1],a2=s[ii+2],a3=s[ii+3];uint8_t tt=a0^a1^a2^a3;
      s[ii]^=((uint8_t)((a0^a1)<<1)^((a0^a1)&0x80?0x1b:0))^tt;
      s[ii+1]^=((uint8_t)((a1^a2)<<1)^((a1^a2)&0x80?0x1b:0))^tt;
      s[ii+2]^=((uint8_t)((a2^a3)<<1)^((a2^a3)&0x80?0x1b:0))^tt;
      s[ii+3]^=((uint8_t)((a3^a0)<<1)^((a3^a0)&0x80?0x1b:0))^tt;
    }
    for(int i=0;i<16;i++)s[i]^=rk[r*16+i];
  }
  for(int i=0;i<16;i++)s[i]=sb[s[i]];
  uint8_t t2=s[1];s[1]=s[5];s[5]=s[9];s[9]=s[13];s[13]=t2;
  t2=s[2];s[2]=s[10];s[10]=t2;
  t2=s[6];s[6]=s[14];s[14]=t2;
  t2=s[15];s[15]=s[11];s[11]=s[7];s[7]=s[3];s[3]=t2;
  for(int i=0;i<16;i++)s[i]^=rk[rounds*16+i];
  IXSSL_MEMCPY(out,s,16);
}

static void ixssl__gcm_gf_mul(uint8_t X[16],const uint8_t H[16]){
  uint8_t V[16],Z[16];IXSSL_MEMCPY(V,H,16);IXSSL_MEMSET(Z,0,16);
  for(int i=0;i<128;i++){
    if(X[i/8]&(0x80u>>(i%8))){for(int j=0;j<16;j++)Z[j]^=V[j];}
    uint8_t lsb=V[15]&1;
    for(int j=15;j>0;j--)V[j]=(V[j]>>1)|(V[j-1]<<7);
    V[0]>>=1;if(lsb)V[0]^=0xe1u;
  }
  IXSSL_MEMCPY(X,Z,16);
}

static void ixssl__gcm_inc(uint8_t c[16]){
  uint32_t v=((uint32_t)c[12]<<24)|((uint32_t)c[13]<<16)|((uint32_t)c[14]<<8)|c[15];
  v++;c[12]=(uint8_t)(v>>24);c[13]=(uint8_t)(v>>16);c[14]=(uint8_t)(v>>8);c[15]=(uint8_t)v;
}

static void ixssl__ghash_update(uint8_t ghash[16],const uint8_t H[16],const uint8_t* data,uint32_t len){
  uint32_t off=0;
  while(off<len){
    uint32_t ch=len-off;if(ch>16)ch=16;
    uint8_t blk[16];IXSSL_MEMSET(blk,0,16);IXSSL_MEMCPY(blk,data+off,ch);
    for(int i=0;i<16;i++)ghash[i]^=blk[i];
    ixssl__gcm_gf_mul(ghash,H);
    off+=ch;
  }
}

static void ixssl__aes_gcm_core(const uint8_t* key, uint32_t kl,
                                const uint8_t* iv, uint32_t il,
                                const uint8_t* aad, uint32_t al,
                                const uint8_t* in, uint32_t len,
                                uint8_t* out, uint8_t tag[16], int encrypt) {
  uint8_t H[16], z[16], J0[16], E0[16], ctr[16];
  IXSSL_MEMSET(z,0,16);ixssl_aes_encrypt(z,H,key,(int)kl*8);
  if(il==12){
    IXSSL_MEMSET(J0,0,16);IXSSL_MEMCPY(J0,iv,12);J0[15]=1;
  }else{
    IXSSL_MEMSET(J0,0,16);ixssl__ghash_update(J0,H,iv,il);
    uint8_t len_blk[16];IXSSL_MEMSET(len_blk,0,16);
    uint64_t bit_len=(uint64_t)il*8;
    for(int i=0;i<8;i++)len_blk[15-i]=(uint8_t)(bit_len>>(i*8));
    for(int i=0;i<16;i++)J0[i]^=len_blk[i];
    ixssl__gcm_gf_mul(J0,H);
  }
  ixssl_aes_encrypt(J0,E0,key,(int)kl*8);IXSSL_MEMCPY(ctr,J0,16);
  uint32_t off=0;
  while(off<len){
    ixssl__gcm_inc(ctr);uint8_t eb[16];ixssl_aes_encrypt(ctr,eb,key,(int)kl*8);
    uint32_t ch=len-off;if(ch>16)ch=16;
    for(uint32_t i=0;i<ch;i++)out[off+i]=in[off+i]^eb[i];
    off+=ch;
  }
  uint8_t ghash[16];IXSSL_MEMSET(ghash,0,16);
  if(aad&&al>0)ixssl__ghash_update(ghash,H,aad,al);
  const uint8_t* ct_ptr=encrypt?out:in;
  if(ct_ptr&&len>0)ixssl__ghash_update(ghash,H,ct_ptr,len);
  uint8_t lb[16];IXSSL_MEMSET(lb,0,16);
  uint64_t ab=(uint64_t)al*8,cb=(uint64_t)len*8;
  for(int i=0;i<8;i++){lb[7-i]=(uint8_t)(ab>>(i*8));lb[15-i]=(uint8_t)(cb>>(i*8));}
  for(int i=0;i<16;i++)ghash[i]^=lb[i];
  ixssl__gcm_gf_mul(ghash,H);
  for(int i=0;i<16;i++)tag[i]=ghash[i]^E0[i];
}

void ixssl_aes_gcm_encrypt(const uint8_t* key,uint32_t kl,const uint8_t* iv,uint32_t il,const uint8_t* aad,uint32_t al,const uint8_t* pt,uint32_t pl,uint8_t* ct,uint8_t tag[16]){
  ixssl__aes_gcm_core(key,kl,iv,il,aad,al,pt,pl,ct,tag,1);
}

int ixssl_aes_gcm_decrypt(const uint8_t* key,uint32_t kl,const uint8_t* iv,uint32_t il,const uint8_t* aad,uint32_t al,const uint8_t* ct,uint32_t cl,uint8_t* pt,const uint8_t tag[16]){
  uint8_t exp[16];
  ixssl__aes_gcm_core(key,kl,iv,il,aad,al,ct,cl,pt,exp,0);
  return IXSSL_MEMCMP(exp,tag,16)==0;
}

#endif

#ifdef __cplusplus
}
#endif

#endif