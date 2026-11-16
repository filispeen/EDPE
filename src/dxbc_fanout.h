#pragma once
// Restricted SM5 colour-output fanout and pixel-position remap utilities.
// Original arithmetic and texture reads are retained; unsupported programs
// decline without mutation.
// Token definitions: Microsoft DirectXShaderCompiler d3d12TokenizedProgramFormat.hpp.
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
void ComputeHashRetail(const BYTE*,UINT,BYTE*);
namespace edpe { namespace dxbc_fanout_detail {
static UINT word(const std::vector<BYTE>& b,size_t off){if(off>b.size()||b.size()-off<4)throw std::runtime_error("bounds");UINT n;memcpy(&n,b.data()+off,4);return n;}
static void put(std::vector<BYTE>& b,UINT v){auto p=(BYTE*)&v;b.insert(b.end(),p,p+4);}
static UINT instructionLength(const std::vector<UINT>& t,size_t p) {
    UINT len=(t[p]>>24)&127;
    // Immediate constant buffers carry their length in the next word,
    // followed by literal float4 data, not executable instructions.
    if((t[p]&0x7ff)==53) {
        if((t[p]>>11)!=3 || p+1>=t.size())throw std::runtime_error("unsupported custom data");
        len=t[p+1];
        if(len<2 || (len-2)%4)throw std::runtime_error("constant buffer length");
    }
    if(!len || len>t.size()-p)throw std::runtime_error("instruction length");
    return len;
}
static bool twoDestinations(UINT op) {
    // Integer multiply/divide, sincos, add/subtract-with-carry and swapc all
    // encode two destinations.  Either destination may be the NULL operand.
    return op==38||op==77||op==78||op==81||op==132||op==133||op==142;
}
static size_t firstOperand(const std::vector<UINT>& inst) {
    size_t p=1;
    while(p<inst.size()&&(inst[p-1]&0x80000000u))++p;
    if(p>=inst.size())throw std::runtime_error("missing operands");
    return p;
}
static size_t rewriteSimpleDestination(std::vector<UINT>& inst,size_t p,UINT temp,UINT& mask) {
    if(p>=inst.size())throw std::runtime_error("missing destination operand");
    const UINT token=inst[p];
    // The retained game corpus uses only the SDK's exact 0D NULL operand, or
    // a 4-component mask on a 1D immediate-index TEMP/OUTPUT operand.  Decline
    // extensions and relative/indexed forms instead of guessing their length.
    if(token==0x0000d000u)return p+1;
    const UINT type=(token>>12)&255;
    const UINT base=type==0?0x00100002u:type==2?0x00102002u:~0u;
    if(base==~0u||(token&~0xf0u)!=base||p+1>=inst.size())
        throw std::runtime_error("unsupported destination operand");
    if(type==2) {
        if(inst[p+1]!=0)throw std::runtime_error("output operand");
        mask|=(token>>4)&15;inst[p]&=~0x2000u;inst[p+1]=temp;
    }
    return p+2;
}
static void rewriteDestinations(std::vector<UINT>& inst,UINT op,UINT temp,UINT& mask) {
    if(twoDestinations(op)) {
        size_t p=firstOperand(inst);
        p=rewriteSimpleDestination(inst,p,temp,mask);
        rewriteSimpleDestination(inst,p,temp,mask);
        return;
    }
    if((op<88&&op!=62&&op!=53)||(op>=108&&op<=141)||op==165||op==167) {
        size_t dest=1;
        while(dest<inst.size()&&(inst[dest-1]&0x80000000u))++dest;
        if(dest>=inst.size())return;
        if(((inst[dest]>>12)&255)==2) {
            if(dest+1>=inst.size()||(inst[dest]&~0xf0u)!=0x00102002u||inst[dest+1]!=0)
                throw std::runtime_error("output operand");
            mask|=(inst[dest]>>4)&15;inst[dest]&=~0x2000u;inst[dest+1]=temp;
        }
    }
}
static std::vector<BYTE> patch(const std::vector<BYTE>& in){
 if(in.size()>1024*1024||in.size()<32||word(in,28)>128||memcmp(in.data(),"DXBC",4)||word(in,24)!=in.size())throw std::runtime_error("container");
 BYTE digest[16];ComputeHashRetail(in.data()+20,UINT(in.size()-20),digest);if(memcmp(digest,in.data()+4,16))throw std::runtime_error("input checksum");
 std::vector<std::pair<UINT,std::vector<BYTE>>> chunks;
 bool sig=false,program=false;
 for(UINT i=0,n=word(in,28);i<n;++i){
   UINT off=word(in,32+4*i),tag=word(in,off),size=word(in,off+4);
   if(off+8>in.size()||size>in.size()-(off+8))throw std::runtime_error("chunk bounds");
   std::vector<BYTE> b(in.begin()+off+8,in.begin()+off+8+size);
   if(tag==0x4e47534f){
     if(sig||word(b,0)!=1||word(b,4)!=8||word(b,12)!=0||word(b,20)!=3||word(b,24)!=0||(word(b,28)&0xffff)!=15)throw std::runtime_error("output signature");
     UINT name=word(b,8);if(name>=b.size()||b.size()-name<10||_strnicmp((char*)b.data()+name,"SV_TARGET",9)||b[name+9])throw std::runtime_error("output name");
     std::vector<BYTE> s;put(s,3);put(s,8);
     for(UINT j=0;j<3;++j){put(s,80);put(s,j);put(s,word(b,16));put(s,3);put(s,j);put(s,word(b,28));}
     const char semantic[]="SV_TARGET";s.insert(s.end(),semantic,semantic+sizeof(semantic));while(s.size()%4)s.push_back(0xab);
     b=std::move(s);sig=true;
   } else if(tag==0x58454853||tag==0x52444853){
     if(program||b.size()%4||word(b,0)!=0x50||word(b,4)!=b.size()/4)throw std::runtime_error("program header");
     std::vector<UINT> t(b.size()/4);memcpy(t.data(),b.data(),b.size());UINT temp=~0u,decls=0,returns=0;
     for(size_t p=2;p<t.size();){UINT op=t[p]&0x7ff,len=instructionLength(t,p);
       if(op==104){if(temp!=~0u||len!=2||t[p+1]>=4096)throw std::runtime_error("temps");temp=t[p+1];}
       if(op==101){if(len!=3||t[p+1]!=0x001020f2||t[p+2]!=0)throw std::runtime_error("output declaration");++decls;}
       if(op==62){if(len!=1||p+1!=t.size())throw std::runtime_error("early return");++returns;}
       const bool supportedNew=(op>=108&&op<=111)||(op>=121&&op<=142)||op==161||op==162||op==165||op==167;
       if(op==4||op==5||op==44||op==63||op==102||op==103||(op>=109&&!supportedNew))throw std::runtime_error("unsupported opcode "+std::to_string(op));
       p+=len;
     }
     if(decls!=1||returns!=1)throw std::runtime_error("missing declarations");
     const bool addTemps=temp==~0u;if(addTemps)temp=0;
     std::vector<UINT> out{t[0],0};UINT mask=0;
     for(size_t p=2;p<t.size();){UINT op=t[p]&0x7ff,len=instructionLength(t,p);
       std::vector<UINT> inst(t.begin()+p,t.begin()+p+len);
       if(op==104)inst[1]=temp+1;
       rewriteDestinations(inst,op,temp,mask);
       if(op==62){for(UINT r=0;r<3;++r){UINT mov[]={0x05000036,0x001020f2,r,r==2?0x00100ff6u:0x00100e46u,temp};out.insert(out.end(),mov,mov+5);}}
       out.insert(out.end(),inst.begin(),inst.end());
       if(op==101){for(UINT r=1;r<3;++r){UINT decl[]={0x03000065,0x001020f2,r};out.insert(out.end(),decl,decl+3);}if(addTemps){out.push_back(0x02000068);out.push_back(1);}}
       p+=len;
     }
     if(mask!=15)throw std::runtime_error("incomplete output");out[1]=UINT(out.size());b.resize(out.size()*4);memcpy(b.data(),out.data(),b.size());program=true;
   }
   chunks.push_back({tag,std::move(b)});
 }
 if(!sig||!program)throw std::runtime_error("missing chunks");
 std::vector<BYTE> out(in.begin(),in.begin()+32);out.resize(32+chunks.size()*4);UINT i=0;
 for(auto& c:chunks){UINT off=UINT(out.size());memcpy(out.data()+32+4*i++,&off,4);put(out,c.first);put(out,UINT(c.second.size()));out.insert(out.end(),c.second.begin(),c.second.end());}
 UINT sz=UINT(out.size());memcpy(out.data()+24,&sz,4);ComputeHashRetail(out.data()+20,sz-20,out.data()+4);return out;
}

enum class HoloRemapKind : uint8_t { kHologramDepth = 0, kFrostedBase = 1 };
struct HoloProgram { uint64_t ps; uint32_t bytes; HoloRemapKind kind; };
inline constexpr HoloProgram kHoloPrograms[] = {
    {0xEA02FAC2BD6C643Cull, 7092,  HoloRemapKind::kHologramDepth},
    {0xE95634B0F61D218Full, 11428, HoloRemapKind::kHologramDepth},
    {0x0146ABCC53240479ull, 2292,  HoloRemapKind::kFrostedBase},
};
struct HoloRecipe {
    uint32_t target[8]; size_t targetLength;
    uint32_t follow[17]; size_t followLength;
    uint32_t operand;
};
inline constexpr HoloRecipe kHoloRecipes[] = {
    // ftoi r1.xy, v2.xyxx  followed by  mov r1.zw, l(0,0,0,0); ld_aoffimmi(0,0,0) r1.x, r1.xyzw, t1.xyzw
    {{0x0500001b, 0x00100032, 1, 0x00101046, 2}, 5,
     {0x08000036, 0x001000c2, 1, 0x00004002, 0, 0, 0, 0,
      0x8900002d, 0x800000c2, 0x00155543, 0x00100012, 1, 0x00100e46, 1, 0x00107e46, 1}, 17,
     0x00100046},
    // mul r0.zw, v2.xxxy, cb1[332].zzzw  followed by  sample_indexable r1.xyz, r0.zwzz, t0.xyzw, s1
    {{0x08000038, 0x001000c2, 0, 0x00101406, 2, 0x00208ea6, 1, 332}, 8,
     {0x8b000045, 0x800000c2, 0x00155543, 0x00100072, 1, 0x00100ae6, 0, 0x00107e46, 0, 0x00106000, 1}, 11,
     0x00100406},
};
inline constexpr size_t kHoloReliefLen = 16;
inline constexpr uint32_t kHoloReliefStock[2][kHoloReliefLen] = {
    {0x08000034,0x00100012,1,0x0020802a,1,120,0x00004001,0x3f800000, 0x08000038,0x00100012,1,0x0010000a,1,0x0020801a,2,21},
    {0x08000034,0x00100082,1,0x0020802a,1,120,0x00004001,0x3f800000, 0x08000038,0x00100082,1,0x0010003a,1,0x0020801a,2,21},
};

static std::vector<UINT> patchHoloTokens(const std::vector<UINT>& t, HoloRemapKind kind) {
    const HoloRecipe& r = kHoloRecipes[static_cast<size_t>(kind)];
    const bool relief = (kind == HoloRemapKind::kHologramDepth);
    size_t target = 0, tempAt = 0, reliefAt = 0;
    UINT temp = 0, found = 0, reliefFound = 0;
    if (t.size() <= 2 || t[0] != 0x50 || t[1] != t.size()) throw std::runtime_error("pixel SM5 program");
    for (size_t a = 2; a < t.size(); a += instructionLength(t, a)) {
        const UINT op = t[a] & 2047, n = instructionLength(t, a);
        if (op == 89 && (n != 4 || t[a + 2] == 13)) throw std::runtime_error("b13 occupied");
        if (op == 104) {
            if (n != 2 || tempAt != 0) throw std::runtime_error("temps declaration");
            tempAt = a;
            temp = t[a + 1];
        }
        if (n == r.targetLength && std::equal(r.target, r.target + n, t.begin() + a)) {
            target = a;
            ++found;
        }
        if (relief && a + kHoloReliefLen <= t.size()) {
            for (const auto& v : kHoloReliefStock) {
                if (std::equal(v, v + kHoloReliefLen, t.begin() + a)) {
                    reliefAt = a;
                    ++reliefFound;
                }
            }
        }
    }
    if (found != 1 || !tempAt || temp >= 4096 || tempAt >= target) throw std::runtime_error("SV_Position target not found");
    if (target + r.targetLength + r.followLength > t.size() ||
        !std::equal(r.follow, r.follow + r.followLength, t.begin() + target + r.targetLength)) {
        throw std::runtime_error("SV_Position follow mismatch");
    }
    if (relief && (reliefFound != 1 || reliefAt <= target)) throw std::runtime_error("relief target not found");

    const UINT cbDecl[] = {0x04000059, 0x00208e46, 13, relief ? 2u : 1u};
    const UINT madInst[] = {0x0b000032, 0x00100032, temp, 0x00101046, 2, 0x00208046, 13, 0, 0x00208ae6, 13, 0};
    std::vector<UINT> out(t.begin(), t.begin() + tempAt);
    out.insert(out.end(), cbDecl, cbDecl + 4);
    out.insert(out.end(), t.begin() + tempAt, t.begin() + target);
    out[tempAt + 5] = temp + 1;
    out.insert(out.end(), madInst, madInst + 11);
    out.insert(out.end(), t.begin() + target, t.end());
    out[target + 18] = r.operand;
    out[target + 19] = temp;
    out[1] = static_cast<UINT>(out.size());
    if (relief) {
        const size_t at = reliefAt + 15;
        const UINT reliefOp[3] = {0x0020800a, 13, 1}; // cb13[1].x
        for (size_t k = 0; k < 3; ++k) out[at + 3 + k] = reliefOp[k];
        out[at + 7] = 0; // l(0.0)
    }
    return out;
}

template <typename PatchFn>
static std::vector<BYTE> patchShaderContainer(const std::vector<BYTE>& in, PatchFn&& patchTokens) {
    if (in.size() > 1024 * 1024 || in.size() < 32 || word(in, 28) > 128 ||
        memcmp(in.data(), "DXBC", 4) || word(in, 24) != in.size()) {
        throw std::runtime_error("container");
    }
    BYTE digest[16];
    ComputeHashRetail(in.data() + 20, UINT(in.size() - 20), digest);
    if (memcmp(digest, in.data() + 4, 16)) throw std::runtime_error("input checksum");

    std::vector<std::pair<UINT, std::vector<BYTE>>> chunks;
    bool program = false;
    for (UINT i = 0, n = word(in, 28); i < n; ++i) {
        UINT off = word(in, 32 + 4 * i), tag = word(in, off), size = word(in, off + 4);
        if (off + 8 > in.size() || size > in.size() - (off + 8)) throw std::runtime_error("chunk bounds");
        std::vector<BYTE> b(in.begin() + off + 8, in.begin() + off + 8 + size);
        if (tag == 0x58454853 || tag == 0x52444853) {
            if (program || b.size() % 4 || (word(b, 0) & 0xFFFFu) != 0x50u || word(b, 4) != b.size() / 4)
                throw std::runtime_error("program header");
            std::vector<UINT> t(b.size() / 4);
            memcpy(t.data(), b.data(), b.size());
            const auto outTokens = patchTokens(t);
            b.resize(outTokens.size() * 4);
            memcpy(b.data(), outTokens.data(), b.size());
            program = true;
        }
        chunks.push_back({tag, std::move(b)});
    }
    if (!program) throw std::runtime_error("missing program chunk");
    std::vector<BYTE> out(in.begin(), in.begin() + 32);
    out.resize(32 + chunks.size() * 4);
    UINT i = 0;
    for (auto& c : chunks) {
        UINT off = UINT(out.size());
        memcpy(out.data() + 32 + 4 * i++, &off, 4);
        put(out, c.first);
        put(out, UINT(c.second.size()));
        out.insert(out.end(), c.second.begin(), c.second.end());
    }
    UINT sz = UINT(out.size());
    memcpy(out.data() + 24, &sz, 4);
    ComputeHashRetail(out.data() + 20, sz - 20, out.data() + 4);
    return out;
}

static size_t operandLength(const std::vector<UINT>& t, size_t p) {
    if (p >= t.size()) throw std::runtime_error("operand bounds");
    const size_t start = p;
    const UINT tok = t[p++];
    if (tok & 0x80000000u) {
        UINT ext = tok;
        while (ext & 0x80000000u) {
            if (p >= t.size()) throw std::runtime_error("operand ext bounds");
            ext = t[p++];
        }
    }
    const UINT numComps = tok & 3u;
    const UINT type = (tok >> 12) & 255u;
    const UINT idxDim = (tok >> 20) & 3u;
    if (type == 4u) { // IMMEDIATE32
        const size_t words = (numComps == 2u) ? 4u : 1u;
        if (p + words > t.size()) throw std::runtime_error("imm32 bounds");
        return (p - start) + words;
    }
    if (type == 5u) { // IMMEDIATE64
        const size_t words = (numComps == 2u) ? 4u : 2u;
        if (p + words > t.size()) throw std::runtime_error("imm64 bounds");
        return (p - start) + words;
    }
    for (UINT d = 0; d < idxDim; ++d) {
        const UINT rep = (tok >> (22u + 2u * d)) & 3u;
        if (rep == 0u) {
            if (p >= t.size()) throw std::runtime_error("idx imm32 bounds");
            ++p;
        } else if (rep == 1u) {
            if (p + 2 > t.size()) throw std::runtime_error("idx imm64 bounds");
            p += 2;
        } else if (rep == 2u) {
            p += operandLength(t, p);
        } else {
            if (p >= t.size()) throw std::runtime_error("idx imm32+rel bounds");
            ++p;
            p += operandLength(t, p);
        }
    }
    return p - start;
}

static bool isSimple1dReg(const std::vector<UINT>& t, size_t p, UINT regType, UINT regIndex) {
    if (p + 1 >= t.size()) return false;
    const UINT tok = t[p];
    if (tok & 0x80000000u) return false;
    const UINT type = (tok >> 12) & 255u;
    const UINT idxDim = (tok >> 20) & 3u;
    const UINT rep0 = (tok >> 22) & 3u;
    return type == regType && idxDim == 1u && rep0 == 0u && t[p + 1] == regIndex;
}

static bool isVisorSampleOp(UINT op) {
    return op >= 69u && op <= 74u; // sample, sample_c, sample_c_lz, sample_l, sample_d, sample_b
}

// On-foot helmet visor VS: SV_VertexID-only fullscreen triangle (z=0.99, cb1[91], cb2[2]),
// computing screen-space o1.xy which stays unjittered when world cb1 carries Halton jitter.
static std::vector<UINT> patchVisorVertexTokens(const std::vector<UINT>& t) {
    if (t.size() <= 2 || t[0] != 0x00010050u || t[1] != t.size()) {
        throw std::runtime_error("visor VS version");
    }
    bool hasCb1 = false, hasCb2 = false, hasVertexId = false, hasSvPos = false, hasO1Xy = false;
    bool hasNearZ99 = false;
    size_t tempAt = 0;
    UINT temp = 0, o1Mask = 0;

    for (size_t a = 2; a < t.size();) {
        const UINT n = instructionLength(t, a);
        const UINT op = t[a] & 2047u;
        if (op == 89u && n == 4u && ((t[a + 1] & 0x003FF000u) == 0x00208000u)) {
            if (t[a + 2] == 13u) throw std::runtime_error("b13 occupied");
            if (t[a + 2] == 1u && t[a + 3] >= 91u) hasCb1 = true;
            if (t[a + 2] == 2u && t[a + 3] >= 2u) hasCb2 = true;
        } else if (op == 96u && n == 4u && t[a + 2] == 0u && t[a + 3] == 6u) {
            hasVertexId = true; // dcl_input_sgv v0.x, vertexid
        } else if (op == 101u && n == 3u && t[a + 2] == 1u && ((t[a + 1] >> 4) & 15u) == 3u) {
            hasO1Xy = true; // dcl_output o1.xy
        } else if (op == 103u && n == 4u && t[a + 2] == 3u && t[a + 3] == 1u) {
            hasSvPos = true; // dcl_output_siv o3.xyzw, position
        } else if (op == 104u) {
            if (n != 2u || tempAt != 0) throw std::runtime_error("temps declaration");
            tempAt = a;
            temp = t[a + 1];
        } else if (tempAt != 0 && op != 53u) {
            if (op == 54u) { // check for mov o3.zw, l(..., 0.99f, 1.0f)
                for (size_t k = a + 1; k < a + n; ++k) {
                    if (t[k] == 0x3f7d70a4u) hasNearZ99 = true;
                }
            }
            size_t p = a + 1;
            while (p < a + n && (t[p - 1] & 0x80000000u)) ++p;
            if (p < a + n && isSimple1dReg(t, p, 2u, 1u) && (t[p] & 0xC) == 0u) {
                const UINT mask = (t[p] >> 4) & 15u;
                if (mask == 0u || (mask & ~3u) != 0u || (o1Mask & mask) != 0u) {
                    throw std::runtime_error("unexpected o1 write");
                }
                o1Mask |= mask;
            }
        }
        a += n;
    }

    if (!hasCb1 || !hasCb2 || !hasVertexId || !hasO1Xy || !hasSvPos || !hasNearZ99 ||
        !tempAt || temp == 0 || temp >= 4096u || o1Mask != 3u) {
        throw std::runtime_error("not helmet visor VS");
    }

    std::vector<UINT> out;
    out.reserve(t.size() + 24);
    out.push_back(t[0]);
    out.push_back(0);
    bool inExec = false;
    for (size_t a = 2; a < t.size();) {
        const UINT n = instructionLength(t, a);
        const UINT op = t[a] & 2047u;
        if (a == tempAt) {
            const UINT cbDecl[] = {0x04000059u, 0x00208e46u, 13u, 1u};
            out.insert(out.end(), cbDecl, cbDecl + 4);
            out.push_back(t[a]);
            out.push_back(temp + 1u);
            inExec = true;
        } else if (inExec && op != 53u) {
            const size_t start = out.size();
            out.insert(out.end(), t.begin() + a, t.begin() + a + n);
            size_t p = 1;
            while (p < n && (t[a + p - 1] & 0x80000000u)) ++p;
            if (p < n && isSimple1dReg(t, a + p, 2u, 1u)) {
                const UINT mask = (t[a + p] >> 4) & 15u;
                out[start + p] = (out[start + p] & ~0x000FF000u); // o1 -> r{temp}
                out[start + p + 1] = temp;
                if (mask == 1u) {
                    const UINT addX[] = {0x08000000u, 0x00102012u, 1u, 0x0010000au, temp, 0x0020800au, 13u, 0u};
                    out.insert(out.end(), addX, addX + 8);
                } else if (mask == 2u) {
                    const UINT addY[] = {0x08000000u, 0x00102022u, 1u, 0x0010001au, temp, 0x0020801au, 13u, 0u};
                    out.insert(out.end(), addY, addY + 8);
                } else if (mask == 3u) {
                    const UINT addXy[] = {0x08000000u, 0x00102032u, 1u, 0x00100046u, temp, 0x00208046u, 13u, 0u};
                    out.insert(out.end(), addXy, addXy + 8);
                }
            }
        } else {
            out.insert(out.end(), t.begin() + a, t.begin() + a + n);
        }
        a += n;
    }
    out[1] = UINT(out.size());
    return out;
}

// On-foot helmet visor PS: dual-source (o0, o1), samples t0..t4 using unjittered v3.xy * cb1[332].zw
// and refracts pre-visor HDR t1. Shift v3 -> r{tempPos} = v3 + cb13[0].zw, while adding cb13[1].xy
// back before t1 reads so refracted jittered world t1 is not un-jittered.
static std::vector<UINT> patchVisorPixelTokens(const std::vector<UINT>& t) {
    if (t.size() <= 2 || t[0] != 0x00000050u || t[1] != t.size()) {
        throw std::runtime_error("visor PS version");
    }
    bool hasCb1 = false, hasCb2 = false, hasSvPosV3 = false, hasCb332Mul = false;
    UINT texCount = 0, outCount = 0, v3Reads = 0, t1Samples = 0, temp = 0;
    size_t tempAt = 0;

    for (size_t a = 2; a < t.size();) {
        const UINT n = instructionLength(t, a);
        const UINT op = t[a] & 2047u;
        if (op == 89u && n == 4u && ((t[a + 1] & 0x003FF000u) == 0x00208000u)) {
            if (t[a + 2] == 13u) throw std::runtime_error("b13 occupied");
            if (t[a + 2] == 1u && t[a + 3] >= 333u) hasCb1 = true;
            if (t[a + 2] == 2u && t[a + 3] >= 9u) hasCb2 = true;
        } else if (op == 88u) {
            ++texCount;
        } else if (op == 101u) {
            ++outCount;
        } else if (op == 100u && n >= 4u && isSimple1dReg(t, a + 1, 1u, 3u) && t[a + 3] == 1u) {
            hasSvPosV3 = true; // dcl_input_ps_siv v3.xy, position
        } else if (op == 104u) {
            if (n != 2u || tempAt != 0) throw std::runtime_error("temps declaration");
            tempAt = a;
            temp = t[a + 1];
        } else if (tempAt != 0 && op != 53u) {
            size_t p = a + 1;
            while (p < a + n && (t[p - 1] & 0x80000000u)) ++p;
            UINT opIdx = 0;
            size_t coordOff = 0;
            bool readsT1 = false;
            while (p < a + n) {
                const size_t oLen = operandLength(t, p);
                if (opIdx > 0 && ((t[p] >> 12) & 255u) == 1u && oLen == 2u && t[p + 1] == 3u) {
                    if (!isSimple1dReg(t, p, 1u, 3u)) throw std::runtime_error("complex v3 operand");
                    ++v3Reads;
                    if (op == 56u) { // mul ..., v3..., cb1[332]...
                        for (size_t k = a + 1; k + 2 < a + n; ++k) {
                            if ((t[k] & 0x003FF000u) == 0x00208000u && t[k + 1] == 1u && t[k + 2] == 332u) {
                                hasCb332Mul = true;
                            }
                        }
                    }
                }
                if (isVisorSampleOp(op)) {
                    if (opIdx == 1u) coordOff = p;
                    if (opIdx == 2u && isSimple1dReg(t, p, 7u, 1u)) readsT1 = true;
                }
                p += oLen;
                ++opIdx;
            }
            if (readsT1) {
                if (!coordOff || (t[coordOff] & 0xFFFFE00Fu) != 0x00100006u ||
                    operandLength(t, coordOff) != 2u) {
                    throw std::runtime_error("unsupported t1 coord");
                }
                ++t1Samples;
            }
        }
        a += n;
    }

    if (!hasCb1 || !hasCb2 || !hasSvPosV3 || !hasCb332Mul || outCount != 2u || texCount < 4u ||
        !tempAt || temp == 0 || temp + 2u >= 4096u || v3Reads == 0 || t1Samples == 0) {
        throw std::runtime_error("not helmet visor PS");
    }

    const UINT tempPos = temp;
    const UINT tempUv = temp + 1u;
    std::vector<UINT> out;
    out.reserve(t.size() + 32);
    out.push_back(t[0]);
    out.push_back(0);
    bool inExec = false;
    for (size_t a = 2; a < t.size();) {
        const UINT n = instructionLength(t, a);
        const UINT op = t[a] & 2047u;
        if (a == tempAt) {
            const UINT cbDecl[] = {0x04000059u, 0x00208e46u, 13u, 2u};
            out.insert(out.end(), cbDecl, cbDecl + 4);
            out.push_back(t[a]);
            out.push_back(temp + 2u);
            const UINT shiftPos[] = {0x08000000u, 0x00100032u, tempPos, 0x00101046u, 3u, 0x00208ae6u, 13u, 0u};
            out.insert(out.end(), shiftPos, shiftPos + 8);
            inExec = true;
        } else if (inExec && op != 53u) {
            std::vector<UINT> inst(t.begin() + a, t.begin() + a + n);
            size_t p = 1;
            while (p < n && (inst[p - 1] & 0x80000000u)) ++p;
            UINT opIdx = 0;
            size_t coordOff = 0;
            bool readsT1 = false;
            while (p < n) {
                const size_t oLen = operandLength(inst, p);
                if (opIdx > 0 && isSimple1dReg(inst, p, 1u, 3u)) {
                    inst[p] &= ~0x000FF000u; // v3 -> r{tempPos}
                    inst[p + 1] = tempPos;
                }
                if (isVisorSampleOp(op)) {
                    if (opIdx == 1u) coordOff = p;
                    if (opIdx == 2u && isSimple1dReg(inst, p, 7u, 1u)) readsT1 = true;
                }
                p += oLen;
                ++opIdx;
            }
            if (readsT1 && coordOff) {
                const UINT origCoordTok = inst[coordOff];
                const UINT origCoordReg = inst[coordOff + 1];
                const UINT addUv[] = {
                    0x08000000u, 0x00100032u, tempUv,
                    origCoordTok, origCoordReg,
                    0x00208046u, 13u, 1u
                };
                out.insert(out.end(), addUv, addUv + 8);
                inst[coordOff] = 0x00100046u; // r{tempUv}.xyxx
                inst[coordOff + 1] = tempUv;
            }
            out.insert(out.end(), inst.begin(), inst.end());
        } else {
            out.insert(out.end(), t.begin() + a, t.begin() + a + n);
        }
        a += n;
    }
    out[1] = UINT(out.size());
    return out;
}

static std::vector<BYTE> patchHoloContainer(const std::vector<BYTE>& in, HoloRemapKind kind) {
    return patchShaderContainer(in, [kind](const std::vector<UINT>& t) {
        return patchHoloTokens(t, kind);
    });
}
} // namespace dxbc_fanout_detail
inline bool colourFanout(const void* data,size_t bytes,std::vector<BYTE>& output,std::string& reason) {
    output.clear();reason.clear();
    if(!data || bytes<32 || bytes>1024*1024){reason="bytecode size";return false;}
    try {
        const auto* p=static_cast<const BYTE*>(data);
        output=dxbc_fanout_detail::patch(std::vector<BYTE>(p,p+bytes));return true;
    } catch(const std::exception& e){reason=e.what();return false;}
}

inline bool patchVisorVertexShader(const void* data, size_t bytes,
                                   std::vector<BYTE>& output, std::string& reason) {
    output.clear();
    reason.clear();
    if (!data || bytes < 32 || bytes > 1024 * 1024) {
        reason = "bytecode size";
        return false;
    }
    try {
        const auto* p = static_cast<const BYTE*>(data);
        output = dxbc_fanout_detail::patchShaderContainer(
            std::vector<BYTE>(p, p + bytes), dxbc_fanout_detail::patchVisorVertexTokens);
        return true;
    } catch (const std::exception& e) {
        reason = e.what();
        return false;
    }
}

inline bool patchVisorPixelShader(const void* data, size_t bytes,
                                  std::vector<BYTE>& output, std::string& reason) {
    output.clear();
    reason.clear();
    if (!data || bytes < 32 || bytes > 1024 * 1024) {
        reason = "bytecode size";
        return false;
    }
    try {
        const auto* p = static_cast<const BYTE*>(data);
        output = dxbc_fanout_detail::patchShaderContainer(
            std::vector<BYTE>(p, p + bytes), dxbc_fanout_detail::patchVisorPixelTokens);
        return true;
    } catch (const std::exception& e) {
        reason = e.what();
        return false;
    }
}

struct HoloRemapParams {
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float biasX = 0.0f;
    float biasY = 0.0f;
    float relief = 1.0f;
    float pad0 = 0.0f;
    float pad1 = 0.0f;
    float pad2 = 0.0f;
};
static_assert(sizeof(HoloRemapParams) == 32, "HoloRemapParams must be two float4 registers");

inline int findHoloProgramSlot(uint64_t psHash) {
    for (size_t i = 0; i < sizeof(dxbc_fanout_detail::kHoloPrograms) / sizeof(dxbc_fanout_detail::kHoloPrograms[0]); ++i) {
        if (dxbc_fanout_detail::kHoloPrograms[i].ps == psHash) return static_cast<int>(i);
    }
    return -1;
}

inline bool patchHoloPixelShader(const void* data, size_t bytes, uint64_t psHash,
                                 std::vector<BYTE>& output, std::string& reason) {
    output.clear();
    reason.clear();
    const int slot = findHoloProgramSlot(psHash);
    if (slot < 0 || !data || bytes != dxbc_fanout_detail::kHoloPrograms[slot].bytes) {
        reason = "unsupported hologram pixel shader";
        return false;
    }
    try {
        const auto* p = static_cast<const BYTE*>(data);
        output = dxbc_fanout_detail::patchHoloContainer(
            std::vector<BYTE>(p, p + bytes),
            dxbc_fanout_detail::kHoloPrograms[slot].kind);
        return true;
    } catch (const std::exception& e) {
        reason = e.what();
        return false;
    }
}
} // namespace edpe
