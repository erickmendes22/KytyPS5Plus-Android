#pragma once
#include <array>
#include <cstdint>
#include <initializer_list>
#include <vector>
#include <utility>
#include <spirv/unified1/spirv.hpp>

namespace Libs::Graphics::DemonsVertexPositionProbe {
constexpr uint32_t Header=3, Stride=23, MaxVertices=65536;
constexpr uint32_t WordCount=Header+Stride*MaxVertices;
// 225647 proved the two small-mesh depth/material pairs equivalent.
 // Focus the expensive per-vertex capture on the actual dominant surface.
constexpr bool Material(uint64_t hash) { return hash==0xf825a988127faeb4ull || hash==0x74b55dea9562eda1ull; }
constexpr bool Watched(uint64_t hash) { return hash==0x34c51585c926a664ull || hash==0xa7208481a1f2b760ull || Material(hash); }
constexpr uint32_t PairIndex(uint64_t hash) { return hash==0xa7208481a1f2b760ull || hash==0x74b55dea9562eda1ull ? 1u : 0u; }
// Exact 221410 modules. Inputs are the actual SSA values used by the
// transform, not a re-read of a potentially different guest buffer.
struct Site {
    uint64_t hash,fingerprint;
    uint32_t main,boolean,void_type,ptr,vertex,instance,transform,position,store,clip;
    std::array<uint32_t,15> inputs; // position xyz, matrix rows
    uint32_t f32=8,vec4=9;
};
constexpr std::array<Site,13> Sites{{
 {0x34c51585c926a664ull,0x92e062bff851bfe3ull,39,60,43,99,71,73,97,593,602,597,
  {366,367,368,253,254,255,256,292,293,294,295,331,332,333,334}},
 {0x34c51585c926a664ull,0x5650cfdc24ed3f10ull,39,60,43,115,71,73,112,878,887,882,
  {413,415,417,279,281,283,285,325,327,329,331,371,373,375,377}},
 {0xa7208481a1f2b760ull,0x42cb3db63c2693b2ull,45,71,49,110,82,84,108,761,769,765,
  {378,379,380,264,265,266,267,304,305,306,307,343,344,345,346}},
 {0xa7208481a1f2b760ull,0xa338dff0ce2abb1aull,45,71,49,126,82,84,123,1059,1067,1063,
  {425,427,429,290,292,294,296,337,339,341,343,383,385,387,389}},
 // 223656 material position and clip-distance stores. Zero site means
 // not captured (do not pretend to have material transform operands).
 {0xf825a988127faeb4ull,0xdd6b59738d0d2030ull,40,65,44,104,76,78,0,841,850,845,{}},
 {0xf825a988127faeb4ull,0x46d9e68030e3e5a9ull,40,65,44,120,76,78,0,1195,1204,1199,{}},
 {0x74b55dea9562eda1ull,0x861d06190301aef3ull,46,73,50,112,84,86,0,986,994,990,{}},
 {0x74b55dea9562eda1ull,0x5ef07abfcab3a10cull,46,73,50,128,84,86,0,1353,1361,1357,{}},
 // 225647: dominant depth VS native loads at 0x80..0x98, and exact
 // export stores at these SSA sites. Material records its final export.
 {0xaff9c7ef505c6253ull,0x03110de384d15f6bull,30,47,34,103,58,60,101,489,499,493,
  {336,338,340,293,295,297,299,246,248,250,252,200,202,204,206}},
 {0xaff9c7ef505c6253ull,0x5c675684d6527d92ull,32,47,36,109,58,60,107,495,505,499,
  {342,344,346,299,301,303,305,252,254,256,258,206,208,210,212},10,11},
 {0xc96f3fcde8141266ull,0x30fa33e11ddb71c9ull,39,62,45,131,73,75,115,787,796,791,{}},
 // Exact material export sites verified from the 185122 modules.
 {0xf825a988127faeb4ull,0x0397926184a9f830ull,40,65,44,120,76,78,0,1195,1204,1199,{}},
 {0x74b55dea9562eda1ull,0x3998632d8b97d056ull,46,73,50,128,84,86,0,1353,1361,1357,{}}
}};
inline bool HasTransformInputs(uint64_t hash,uint64_t fingerprint) {
    for(const auto& x:Sites) if(x.hash==hash&&x.fingerprint==fingerprint) return x.inputs[0]!=0;
    return false;
}
// A new specialization may move SSA IDs. Find its actual position export,
// not a guessed operand map. Only exact site maps certify transform inputs.
inline bool DiscoverExport(const std::vector<uint32_t>& w,Site& site) {
    if(w.size()<5||w[3]>100000||w[3]<2)return false;
    const auto bound=w[3];
    std::vector<uint32_t> builtin(bound,UINT32_MAX),member0(bound,UINT32_MAX),pointee(bound),
        variable_type(bound),constant(bound,UINT32_MAX),load(bound),base(bound),index(bound,UINT32_MAX);
    uint32_t u32=0,f32=0,vec4=0,vertex_var=0,instance_var=0,clip_var=0;
    for(size_t i=5;i<w.size();){
        const auto n=w[i]>>16;const auto op=static_cast<spv::Op>(w[i]&65535u);
        if(!n||n>w.size()-i)return false;
        const auto* a=w.data()+i;
        if(op==spv::OpEntryPoint&&n>=4){
            if(site.main||a[1]!=spv::ExecutionModelVertex)return false;
            site.main=a[2];
        }
        if(op==spv::OpTypeInt&&n==4&&a[2]==32&&a[3]==0)u32=a[1];
        if(op==spv::OpTypeFloat&&n==3&&a[2]==32)f32=a[1];
        if(op==spv::OpTypeVector&&n==4&&a[2]==f32&&a[3]==4)vec4=a[1];
        if(op==spv::OpTypeVoid&&n==2)site.void_type=a[1];
        if(op==spv::OpTypeBool&&n==2)site.boolean=a[1];
        if(op==spv::OpTypePointer&&n==4&&a[1]<bound){
            pointee[a[1]]=a[3];
            if(a[2]==spv::StorageClassStorageBuffer&&a[3]==1)site.ptr=a[1];
        }
        if(op==spv::OpDecorate&&n==4&&a[1]<bound&&a[2]==spv::DecorationBuiltIn)builtin[a[1]]=a[3];
        if(op==spv::OpMemberDecorate&&n==5&&a[1]<bound&&a[2]==0&&a[3]==spv::DecorationBuiltIn)member0[a[1]]=a[4];
        if(op==spv::OpVariable&&n>=4&&a[2]<bound)variable_type[a[2]]=a[1];
        if(op==spv::OpConstant&&n==4&&a[2]<bound)constant[a[2]]=a[3];
        i+=n;
    }
    if(u32!=1||!site.main||!site.void_type||!site.boolean||!site.ptr||!f32||!vec4)return false;
    for(uint32_t v=1;v<bound;++v){
        if(builtin[v]==spv::BuiltInVertexIndex){if(vertex_var)return false;vertex_var=v;}
        if(builtin[v]==spv::BuiltInInstanceIndex){if(instance_var)return false;instance_var=v;}
        if(builtin[v]==spv::BuiltInClipDistance){if(clip_var)return false;clip_var=v;}
    }
    if(!vertex_var||!instance_var||!clip_var)return false;
    bool main=false;
    for(size_t i=5;i<w.size();){
        const auto n=w[i]>>16;const auto op=static_cast<spv::Op>(w[i]&65535u);const auto* a=w.data()+i;
        if(op==spv::OpFunction&&n>=3)main=a[2]==site.main;
        if(op==spv::OpFunctionEnd)main=false;
        if(main){
            if(op==spv::OpLoad&&n>=4&&a[2]<bound)load[a[2]]=a[3];
            if(op==spv::OpBitcast&&n==4&&a[1]==u32&&a[3]<bound){
                if(load[a[3]]==vertex_var){if(site.vertex)return false;site.vertex=a[2];}
                if(load[a[3]]==instance_var){if(site.instance)return false;site.instance=a[2];}
            }
            if((op==spv::OpAccessChain||op==spv::OpInBoundsAccessChain)&&n==5&&a[2]<bound&&a[4]<bound){
                base[a[2]]=a[3];index[a[2]]=constant[a[4]];
            }
            if(op==spv::OpStore&&n==3&&a[1]<bound){
                const auto b=base[a[1]];
                if(b==clip_var&&index[a[1]]==0){if(site.clip)return false;site.clip=a[2];}
                bool pos=builtin[a[1]]==spv::BuiltInPosition;
                if(b&&b<bound&&index[a[1]]==0&&variable_type[b]<bound&&pointee[variable_type[b]]<bound)
                    pos|=member0[pointee[variable_type[b]]]==spv::BuiltInPosition;
                if(pos){
                    if(site.store||!site.clip||!site.vertex||!site.instance)return false;
                    site.store=a[1];site.position=a[2];
                }
            }
        }
        i+=n;
    }
    site.f32=f32;site.vec4=vec4;
    return site.store&&site.position;
}
inline bool Instrument(std::vector<uint32_t>& spirv,uint64_t hash,uint64_t fingerprint,uint32_t binding,bool discover=false) {
    const Site* found=nullptr;
    Site fallback{};
    for (const auto& x:Sites) if(x.hash==hash && x.fingerprint==fingerprint) found=&x;
    if(!found&&discover&&Watched(hash)&&DiscoverExport(spirv,fallback))found=&fallback;
    if(!found || spirv.size()<5)return false;
    const auto& site=*found;
    constexpr uint32_t u32=1;
    const auto f32=site.f32,vec4=site.vec4;
    uint32_t next=spirv[3];
    const auto id=[&]{return next++;};
    const auto emit=[](std::vector<uint32_t>& out,spv::Op op,std::initializer_list<uint32_t> args){
        out.push_back((static_cast<uint32_t>(args.size()+1)<<16)|op);
        out.insert(out.end(),args);
    };
    std::vector<uint32_t> annotations,decl,fn,result(spirv.begin(),spirv.begin()+5);
    size_t first_type=0;
    for(size_t i=5;i<spirv.size();){
        const auto n=spirv[i]>>16;
        if(!n || n>spirv.size()-i)return false;
        const auto op=static_cast<spv::Op>(spirv[i]&65535u);
        if(!first_type && op>=spv::OpTypeVoid && op<=spv::OpTypeForwardPointer) first_type=i;
        i+=n;
    }
    if(!first_type)return false;
    const auto data=id(),block=id(),ptr_block=id(),buf=id();
    emit(annotations,spv::OpDecorate,{data,spv::DecorationArrayStride,4});
    emit(annotations,spv::OpDecorate,{block,spv::DecorationBlock});
    emit(annotations,spv::OpMemberDecorate,{block,0,spv::DecorationOffset,0});
    emit(annotations,spv::OpDecorate,{buf,spv::DecorationDescriptorSet,0});
    emit(annotations,spv::OpDecorate,{buf,spv::DecorationBinding,binding});
    emit(decl,spv::OpTypeRuntimeArray,{data,u32});
    emit(decl,spv::OpTypeStruct,{block,data});
    emit(decl,spv::OpTypePointer,{ptr_block,spv::StorageClassStorageBuffer,block});
    emit(decl,spv::OpVariable,{ptr_block,buf,spv::StorageClassStorageBuffer});
    std::array<uint32_t,Stride+1> c{};
    for(uint32_t i=0;i<c.size();++i){c[i]=id();emit(decl,spv::OpConstant,{u32,c[i],i});}
    const auto fn_type=id(),rec=id(),vertex=id(),instance=id(),transform=id(),position=id(),clip=id();
    std::array<uint32_t,15> inputs{};
    for(auto& a:inputs)a=id();
    // Function type: void(uint vertex, uint instance, uint transform,
    // vec4 position, float clip, 15 uint input words).
    decl.push_back((23u<<16)|spv::OpTypeFunction);
    decl.insert(decl.end(),{fn_type,site.void_type,u32,u32,u32,vec4,f32});
    decl.insert(decl.end(),inputs.size(),u32);
    emit(fn,spv::OpFunction,{site.void_type,rec,0,fn_type});
    for(auto p:{vertex,instance,transform})emit(fn,spv::OpFunctionParameter,{u32,p});
    emit(fn,spv::OpFunctionParameter,{vec4,position});
    emit(fn,spv::OpFunctionParameter,{f32,clip});
    for(auto p:inputs)emit(fn,spv::OpFunctionParameter,{u32,p});
    emit(fn,spv::OpLabel,{id()});
    const auto read=[&](uint32_t index){
        const auto p=id(),v=id();
        emit(fn,spv::OpAccessChain,{site.ptr,p,buf,c[0],c[index]});
        emit(fn,spv::OpLoad,{u32,v,p});return v;
    };
    const auto enable=read(0),base=read(1),count=read(2);
    const auto off=id(),enabled=id(),inrange=id(),active=id(),write=id(),merge=id();
    emit(fn,spv::OpISub,{u32,off,vertex,base});
    emit(fn,spv::OpINotEqual,{site.boolean,enabled,enable,c[0]});
    emit(fn,spv::OpULessThan,{site.boolean,inrange,off,count});
    emit(fn,spv::OpLogicalAnd,{site.boolean,active,enabled,inrange});
    emit(fn,spv::OpSelectionMerge,{merge,0});
    emit(fn,spv::OpBranchConditional,{active,write,merge});
    emit(fn,spv::OpLabel,{write});
    const auto row=id(),start=id();
    emit(fn,spv::OpIMul,{u32,row,off,c[Stride]});
    emit(fn,spv::OpIAdd,{u32,start,row,c[Header]});
    std::array<uint32_t,Stride> values{};
    values[0]=c[1];values[1]=instance;values[2]=transform;
    for(uint32_t j=0;j<4;++j){
        const auto f=id();values[3+j]=id();
        emit(fn,spv::OpCompositeExtract,{f32,f,position,j});
        emit(fn,spv::OpBitcast,{u32,values[3+j],f});
    }
    values[7]=id();emit(fn,spv::OpBitcast,{u32,values[7],clip});
    for(uint32_t j=0;j<inputs.size();++j)values[8+j]=inputs[j];
    for(uint32_t j=0;j<values.size();++j){
        const auto offset=id(),p=id();
        emit(fn,spv::OpIAdd,{u32,offset,start,c[j]});
        emit(fn,spv::OpAccessChain,{site.ptr,p,buf,c[0],offset});
        // Indexed draws can execute a vertex more than once. Atomic writes
        // avoid races between identical invocations; only one instance sampled.
        emit(fn,spv::OpAtomicExchange,{u32,id(),p,c[1],c[0],values[j]});
    }
    emit(fn,spv::OpBranch,{merge});emit(fn,spv::OpLabel,{merge});
    emit(fn,spv::OpReturn,{});emit(fn,spv::OpFunctionEnd,{});
    bool entry=false,globals=false,main=false,inserted=false;
    for(size_t i=5;i<spirv.size();){
        const auto n=spirv[i]>>16;
        const auto op=static_cast<spv::Op>(spirv[i]&65535u);
        if(i==first_type)result.insert(result.end(),annotations.begin(),annotations.end());
        if(op==spv::OpFunction){
            if(!globals){result.insert(result.end(),decl.begin(),decl.end());globals=true;}
            main=spirv[i+2]==site.main;
        }
        if(op==spv::OpEntryPoint){
            if(entry)return false;
            if(spirv[1]>=0x00010400u){
                result.push_back(spirv[i]+(1u<<16));
                result.insert(result.end(),spirv.begin()+i+1,spirv.begin()+i+n);
                result.push_back(buf);
            }else result.insert(result.end(),spirv.begin()+i,spirv.begin()+i+n);
            entry=true;
        }else result.insert(result.end(),spirv.begin()+i,spirv.begin()+i+n);
        if(main && op==spv::OpStore && n==3 && spirv[i+1]==site.store && spirv[i+2]==site.position){
            if(inserted)return false;
            result.push_back((24u<<16)|spv::OpFunctionCall);
            result.insert(result.end(),{site.void_type,id(),rec,site.vertex,site.instance,
                site.transform?site.transform:c[0],site.position,site.clip});
            for(auto operand:site.inputs)result.push_back(operand?operand:c[0]);
            inserted=true;
        }
        if(op==spv::OpFunctionEnd)main=false;
        i+=n;
    }
    if(!entry||!globals||!inserted)return false;
    result.insert(result.end(),fn.begin(),fn.end());result[3]=next;
    spirv=std::move(result);return true;
}
} // namespace Libs::Graphics::DemonsVertexPositionProbe
