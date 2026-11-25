#pragma once
#include <array>
#include <cstdint>
#include <initializer_list>
#include <vector>
#include <utility>
#include <spirv/unified1/spirv.hpp>

namespace Libs::Graphics::DemonsRelightProbe {
constexpr uint64_t Shader=0xbcbcc6ba1e68c974ull;
constexpr uint32_t Header=3, Sites=17, Events=8, EventWords=12,
    SiteWords=1+Events*EventWords, Rays=256, Probes=2,
    WordCount=Header+Probes*Rays*Sites*SiteWords;
struct Map {uint64_t fp; uint32_t ray,boolean,ptr;};
constexpr std::array<Map,5> Maps{{
    {0x2746d5f7af6311deull,379,228,238},
    {0x97ea10ade6976354ull,374,223,233},
    {0xb611c71dabffe99cull,374,223,233},
    {0xf93c8d74323b5ffaull,373,222,232},
    {0x4690ec9f28ed697cull,378,227,237}
}};
// Exact observed module: 8x8 local size, 2x2 workgroups per probe.
// Site 0 is raw ray pair, 1..15 are actual image operations in program
// order, 16 is the final image store. Each event retains coordinate,
// sampled value (or exported value), and LOD/gradient/Dref operands.
// No guest data, EXEC, sampling operation or arithmetic is changed.
inline bool Instrument(std::vector<uint32_t>& spirv,uint64_t fp,uint32_t binding) {
    Map discovered{fp,0,0,0};
    const Map* m=nullptr;
    for(const auto& a:Maps)if(a.fp==fp)m=&a;
    // All five known modules were captured in 112246. Only the relight shader
    // invokes this helper; unknown variants must prove the same atlas binding below.
    if(spirv.size()<5||spirv[3]>100000)return false;
    const auto bound=spirv[3];
    constexpr uint32_t u=1,f=20,vec3=2,voidt=177,mainid=56;
    struct Type{uint32_t component=0,count=0;};
    std::vector<Type> types(bound);
    std::vector<uint32_t> value_types(bound);
    size_t first_type=0;
    bool local=false;
    uint32_t bool_count=0, ptr_count=0;
    std::vector<bool> pair_struct(bound), pair_phi(bound);
    uint32_t previous_pair=0, candidate_count=0;
    std::vector<uint32_t> binding_of(bound,UINT32_MAX), constants(bound,UINT32_MAX), load_from(bound), chain_base(bound), chain_index(bound), sampled_image(bound);
    uint32_t image_sites=0;bool atlas_layout=false;
    for(size_t i=5;i<spirv.size();){
        const auto n=spirv[i]>>16;
        if(!n||n>spirv.size()-i)return false;
        const auto o=static_cast<spv::Op>(spirv[i]&65535u);
        if(!first_type&&o>=spv::OpTypeVoid&&o<=spv::OpTypeForwardPointer)first_type=i;
        if(o==spv::OpTypeInt&&n==4&&spirv[i+1]<bound&&spirv[i+2]==32)types[spirv[i+1]]={spirv[i+1],1};
        if(o==spv::OpTypeFloat&&n==3&&spirv[i+1]<bound&&spirv[i+2]==32)types[spirv[i+1]]={spirv[i+1],1};
        if(o==spv::OpTypeVector&&n==4&&spirv[i+1]<bound&&spirv[i+2]<bound&&spirv[i+3]<=4&&types[spirv[i+2]].count==1)
            types[spirv[i+1]]={spirv[i+2],spirv[i+3]};
        if(o==spv::OpExecutionMode&&n==6&&spirv[i+1]==mainid&&spirv[i+2]==17&&spirv[i+3]==8&&spirv[i+4]==8&&spirv[i+5]==1)local=true;
        if(o==spv::OpDecorate&&n==4&&spirv[i+1]<bound&&spirv[i+2]==spv::DecorationBinding)binding_of[spirv[i+1]]=spirv[i+3];
        if(o==spv::OpConstant&&n==4&&(spirv[i+1]==u||spirv[i+1]==f)&&spirv[i+2]<bound)constants[spirv[i+2]]=spirv[i+3];
        if(o==spv::OpLoad&&n>=4&&spirv[i+2]<bound)load_from[spirv[i+2]]=spirv[i+3];
        if(o==spv::OpAccessChain&&n==5&&spirv[i+2]<bound){chain_base[spirv[i+2]]=spirv[i+3];chain_index[spirv[i+2]]=spirv[i+4];}
        if(o==spv::OpSampledImage&&n==5&&spirv[i+2]<bound)sampled_image[spirv[i+2]]=spirv[i+3];
        if(o==spv::OpImageSampleExplicitLod||o==spv::OpImageSampleDrefExplicitLod||o==spv::OpImageFetch){
            if(++image_sites==12&&o==spv::OpImageSampleExplicitLod&&n==7&&spirv[i+5]==2&&spirv[i+6]<bound&&constants[spirv[i+6]]==0){
                const auto s=spirv[i+3];const auto im=s<bound?sampled_image[s]:0;
                const auto ptr=im<bound?load_from[im]:0;
                const auto base=ptr<bound?chain_base[ptr]:0,ix=ptr<bound?chain_index[ptr]:0;
                atlas_layout=base&&base<bound&&ix&&ix<bound&&binding_of[base]==4&&constants[ix]==2;
            }
        }
        if(o==spv::OpTypeBool&&n==2){discovered.boolean=spirv[i+1];++bool_count;}
        if(o==spv::OpTypePointer&&n==4&&spirv[i+2]==spv::StorageClassStorageBuffer&&spirv[i+3]==u){discovered.ptr=spirv[i+1];++ptr_count;}
        // AMD buffer loads lower to a struct {uint,uint}, not a vector.
        if(o==spv::OpTypeStruct&&n==4&&spirv[i+1]<bound&&spirv[i+2]==u&&spirv[i+3]==u){
            types[spirv[i+1]]={u,2};pair_struct[spirv[i+1]]=true;
        }
        if(o==spv::OpPhi&&n>=5&&spirv[i+1]<bound&&spirv[i+2]<bound&&pair_struct[spirv[i+1]])pair_phi[spirv[i+2]]=true;
        if(o==spv::OpCompositeExtract&&n==5&&spirv[i+1]==u&&spirv[i+3]<bound&&pair_phi[spirv[i+3]]){
            if(spirv[i+4]==1&&previous_pair==spirv[i+3]){discovered.ray=previous_pair;++candidate_count;}
            previous_pair=spirv[i+4]==0?spirv[i+3]:0;
        }else previous_pair=0;
        // Values needed here are constants, loads, phis, arithmetic and
        // composites. Exclude non-result instructions (stores/branches).
        const auto code=uint32_t(o);
        if(n>=3&&spirv[i+1]<bound&&spirv[i+2]<bound&&types[spirv[i+1]].count&&
           ((code>=41&&code<=52)||code==61||(code>=77&&code<=98)||(code>=109&&code<=205)||code==245||code==12))
            value_types[spirv[i+2]]=spirv[i+1];
        i+=n;
    }
    if(!first_type||!local||types[u].count!=1||types[f].count!=1)return false;
    if(!m){
        if(bool_count!=1||ptr_count!=1||candidate_count!=1||!atlas_layout)return false;
        m=&discovered;
    }
    if(m->ray>=bound||m->boolean>=bound||m->ptr>=bound)return false;
    uint32_t next=bound;
    const auto id=[&]{return next++;};
    const auto emit=[](std::vector<uint32_t>& v,spv::Op o,std::initializer_list<uint32_t> a){
        v.push_back((static_cast<uint32_t>(a.size()+1)<<16)|o);v.insert(v.end(),a);
    };
    std::vector<uint32_t> ann,decl,fn,out(spirv.begin(),spirv.begin()+5);
    const auto data=id(),block=id(),ptr=id(),buf=id();
    emit(ann,spv::OpDecorate,{data,spv::DecorationArrayStride,4});
    emit(ann,spv::OpDecorate,{block,spv::DecorationBlock});
    emit(ann,spv::OpMemberDecorate,{block,0,spv::DecorationOffset,0});
    emit(ann,spv::OpDecorate,{buf,spv::DecorationDescriptorSet,0});
    emit(ann,spv::OpDecorate,{buf,spv::DecorationBinding,binding});
    emit(decl,spv::OpTypeRuntimeArray,{data,u});
    emit(decl,spv::OpTypeStruct,{block,data});
    emit(decl,spv::OpTypePointer,{ptr,spv::StorageClassStorageBuffer,block});
    emit(decl,spv::OpVariable,{ptr,buf,spv::StorageClassStorageBuffer});
    std::array<uint32_t,257> c{};
    for(uint32_t i=0;i<c.size();++i){c[i]=id();emit(decl,spv::OpConstant,{u,c[i],i});}
    const auto ft=id(),rec=id(),site=id();
    std::array<uint32_t,EventWords> params{};
    for(auto& p:params)p=id();
    decl.push_back((16u<<16)|spv::OpTypeFunction);
    decl.insert(decl.end(),{ft,voidt,u});decl.insert(decl.end(),params.size(),u);
    emit(fn,spv::OpFunction,{voidt,rec,0,ft});
    emit(fn,spv::OpFunctionParameter,{u,site});
    for(auto p:params)emit(fn,spv::OpFunctionParameter,{u,p});
    emit(fn,spv::OpLabel,{id()});
    const auto read=[&](uint32_t index){
        const auto p=id(),v=id();emit(fn,spv::OpAccessChain,{m->ptr,p,buf,c[0],c[index]});
        emit(fn,spv::OpLoad,{u,v,p});return v;
    };
    const auto enable=read(0),z0=read(1),z1=read(2);
    const auto group=id(),localv=id();
    emit(fn,spv::OpLoad,{vec3,group,4});
    emit(fn,spv::OpLoad,{vec3,localv,5});
    std::array<uint32_t,3> g{},l{};
    for(uint32_t i=0;i<3;++i){g[i]=id();l[i]=id();emit(fn,spv::OpCompositeExtract,{u,g[i],group,i});emit(fn,spv::OpCompositeExtract,{u,l[i],localv,i});}
    const auto en=id(),a=id(),b=id(),zok=id(),xok=id(),yok=id(),xy=id(),match=id(),active=id(),record=id(),done=id();
    emit(fn,spv::OpINotEqual,{m->boolean,en,enable,c[0]});
    emit(fn,spv::OpIEqual,{m->boolean,a,g[2],z0});
    emit(fn,spv::OpIEqual,{m->boolean,b,g[2],z1});
    emit(fn,spv::OpLogicalOr,{m->boolean,zok,a,b});
    emit(fn,spv::OpULessThan,{m->boolean,xok,g[0],c[2]});
    emit(fn,spv::OpULessThan,{m->boolean,yok,g[1],c[2]});
    emit(fn,spv::OpLogicalAnd,{m->boolean,xy,xok,yok});
    emit(fn,spv::OpLogicalAnd,{m->boolean,match,xy,zok});
    emit(fn,spv::OpLogicalAnd,{m->boolean,active,en,match});
    emit(fn,spv::OpSelectionMerge,{done,0});
    emit(fn,spv::OpBranchConditional,{active,record,done});
    emit(fn,spv::OpLabel,{record});
    const auto xg=id(),yg=id(),x=id(),y=id(),y16=id(),lane=id(),pidx=id(),pbase=id(),row=id(),rowbase=id(),si=id(),off=id(),countptr=id(),count=id(),countnext=id();
    emit(fn,spv::OpIMul,{u,xg,g[0],c[8]});emit(fn,spv::OpIMul,{u,yg,g[1],c[8]});
    emit(fn,spv::OpIAdd,{u,x,xg,l[0]});emit(fn,spv::OpIAdd,{u,y,yg,l[1]});
    emit(fn,spv::OpIMul,{u,y16,y,c[16]});emit(fn,spv::OpIAdd,{u,lane,y16,x});
    emit(fn,spv::OpSelect,{u,pidx,a,c[0],c[1]});
    emit(fn,spv::OpIMul,{u,pbase,pidx,c[Rays]});emit(fn,spv::OpIAdd,{u,row,pbase,lane});
    emit(fn,spv::OpIMul,{u,rowbase,row,c[Sites]});emit(fn,spv::OpIAdd,{u,si,rowbase,site});
    emit(fn,spv::OpIMul,{u,off,si,c[SiteWords]});
    const auto base=id();emit(fn,spv::OpIAdd,{u,base,off,c[Header]});
    emit(fn,spv::OpAccessChain,{m->ptr,countptr,buf,c[0],base});
    emit(fn,spv::OpLoad,{u,count,countptr});emit(fn,spv::OpIAdd,{u,countnext,count,c[1]});
    emit(fn,spv::OpStore,{countptr,countnext});
    const auto room=id(),write=id(),merge=id();
    emit(fn,spv::OpULessThan,{m->boolean,room,count,c[Events]});
    emit(fn,spv::OpSelectionMerge,{merge,0});emit(fn,spv::OpBranchConditional,{room,write,merge});
    emit(fn,spv::OpLabel,{write});
    const auto ev=id(),start=id();emit(fn,spv::OpIMul,{u,ev,count,c[EventWords]});
    emit(fn,spv::OpIAdd,{u,start,base,ev});
    for(uint32_t j=0;j<EventWords;++j){
        const auto at=id(),p=id();emit(fn,spv::OpIAdd,{u,at,start,c[j+1]});
        emit(fn,spv::OpAccessChain,{m->ptr,p,buf,c[0],at});emit(fn,spv::OpStore,{p,params[j]});
    }
    emit(fn,spv::OpBranch,{merge});emit(fn,spv::OpLabel,{merge});emit(fn,spv::OpBranch,{done});
    emit(fn,spv::OpLabel,{done});emit(fn,spv::OpReturn,{});emit(fn,spv::OpFunctionEnd,{});
    const auto words=[&](uint32_t v,std::array<uint32_t,4>& dst,uint32_t start=0){
        if(!v)return true;
        if(v>=bound||!value_types[v])return false;
        const auto t=types[value_types[v]];
        if(!t.count||start+t.count>dst.size())return false;
        for(uint32_t j=0;j<t.count;++j){
            auto word=v;
            if(t.count>1){word=id();emit(out,spv::OpCompositeExtract,{t.component,word,v,j});}
            if(t.component!=u){auto bits=id();emit(out,spv::OpBitcast,{u,bits,word});word=bits;}
            dst[start+j]=word;
        }
        return true;
    };
    const auto capture=[&](uint32_t site_index,uint32_t coord,uint32_t val,uint32_t aux0,uint32_t aux1){
        std::array<uint32_t,4> coords{},values{},aux{};
        coords.fill(c[0]);values.fill(c[0]);aux.fill(c[0]);
        if(!words(coord,coords)||!words(val,values)||!words(aux0,aux)||
           !words(aux1,aux,aux0?types[value_types[aux0]].count:0))return false;
        out.push_back((17u<<16)|spv::OpFunctionCall);
        out.insert(out.end(),{voidt,id(),rec,c[site_index]});
        out.insert(out.end(),coords.begin(),coords.end());
        out.insert(out.end(),values.begin(),values.end());
        out.insert(out.end(),aux.begin(),aux.end());
        return true;
    };
    bool entry=false,globals=false,main=false,raw=false,store=false;
    uint32_t sample=0;
    for(size_t i=5;i<spirv.size();){
        const auto n=spirv[i]>>16;const auto o=static_cast<spv::Op>(spirv[i]&65535u);
        if(i==first_type)out.insert(out.end(),ann.begin(),ann.end());
        if(o==spv::OpFunction){
            if(!globals){out.insert(out.end(),decl.begin(),decl.end());globals=true;}
            main=spirv[i+2]==mainid;
        }
        if(o==spv::OpEntryPoint){
            if(entry)return false;
            if(spirv[1]>=0x00010400u){
                out.push_back(spirv[i]+(1u<<16));out.insert(out.end(),spirv.begin()+i+1,spirv.begin()+i+n);out.push_back(buf);
            }else out.insert(out.end(),spirv.begin()+i,spirv.begin()+i+n);
            entry=true;
        }else out.insert(out.end(),spirv.begin()+i,spirv.begin()+i+n);
        if(main&&o==spv::OpCompositeExtract&&n==5&&spirv[i+3]==m->ray&&spirv[i+4]==1){
            if(raw||!capture(0,0,m->ray,0,0))return false;
            raw=true;
        }
        if(main&&(o==spv::OpImageSampleExplicitLod||o==spv::OpImageSampleDrefExplicitLod||o==spv::OpImageFetch)){
            if(++sample>=Sites-1)return false;
            uint32_t a0=0,a1=0;
            if(o==spv::OpImageSampleDrefExplicitLod){if(n!=8||spirv[i+6]!=2)return false;a0=spirv[i+5];a1=spirv[i+7];}
            else if(n==8&&spirv[i+5]==4){a0=spirv[i+6];a1=spirv[i+7];}
            else if(n==7&&spirv[i+5]==2){a0=spirv[i+6];}
            else return false;
            if(!capture(sample,spirv[i+4],spirv[i+2],a0,a1))return false;
        }
        if(main&&o==spv::OpImageWrite){
            if(store||n!=4||!capture(Sites-1,spirv[i+2],spirv[i+3],0,0))return false;
            store=true;
        }
        if(o==spv::OpFunctionEnd)main=false;
        i+=n;
    }
    if(!entry||!globals||!raw||!store||sample!=Sites-2)return false;
    out.insert(out.end(),fn.begin(),fn.end());out[3]=next;spirv=std::move(out);return true;
}
} // namespace Libs::Graphics::DemonsRelightProbe
