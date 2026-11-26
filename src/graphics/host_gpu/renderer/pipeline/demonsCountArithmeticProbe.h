#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>
#include <spirv/unified1/spirv.hpp>

namespace Libs::Graphics::DemonsCountArithmeticProbe {
constexpr uint64_t Shader=0xcb3b9c1ea64fe876ull;
constexpr uint64_t Three=0xac3603ad3ba167b7ull,Two=0x3d1cc19be27b4002ull;
enum Site : uint32_t { SecLimit,SecIndex,SecLength,SecOffset,SecInRange,SecLoad,SecValue,
    PriIndex,PriLength,PriOffset,PriInRange,PriLoad,PriLimit,PriValue,Count,Groups,
    StoreGroup,StoreY,StoreCount,StoreSecondary,StoreZ,SiteCount };
constexpr uint32_t WordCount=1+2*SiteCount;
struct Map {
    uint32_t boolean,ptr_word;
    std::array<uint32_t,SiteCount> ids;
};
constexpr Map Layout(uint64_t fp) {
    // The final five IDs are actual OpStore pointer IDs; record only when
    // those stores execute, including their original value operand.
    if(fp==Three)return {44,40,{42,48,54,56,57,63,66,68,74,76,77,82,85,86,87,93,111,127,143,159,174}};
    if(fp==Two)return {42,38,{40,0,0,0,0,0,47,50,55,57,58,64,67,68,69,76,94,110,126,142,157}};
    return {};
}
constexpr bool Supported(uint64_t fp){return fp==Three||fp==Two;}
inline bool Instrument(std::vector<uint32_t>& spirv,uint64_t fp,uint32_t binding) {
    if(!Supported(fp)||spirv.size()<5)return false;
    const auto map=Layout(fp);
    constexpr uint32_t u32=1,vec3=2,void_type=28,ptr_block=18,entry_id=20,workgroup_id=4;
    uint32_t next=spirv[3];
    auto id=[&]{return next++;};
    const auto emit=[](std::vector<uint32_t>& out,spv::Op op,std::initializer_list<uint32_t> args){
        out.push_back((static_cast<uint32_t>(args.size()+1)<<16)|op);
        out.insert(out.end(),args);
    };
    size_t first_type=0;
    for(size_t i=5;i<spirv.size();){
        const auto n=spirv[i]>>16;
        if(!n||n>spirv.size()-i)return false;
        const auto op=static_cast<spv::Op>(spirv[i]&65535u);
        if(!first_type && op>=spv::OpTypeVoid && op<=spv::OpTypeForwardPointer)first_type=i;
        i+=n;
    }
    if(!first_type)return false;
    std::vector<uint32_t> annotations,decl,fn,out(spirv.begin(),spirv.begin()+5);
    const auto debug=id();
    emit(annotations,spv::OpDecorate,{debug,spv::DecorationDescriptorSet,0});
    emit(annotations,spv::OpDecorate,{debug,spv::DecorationBinding,binding});
    // Both exact modules already declare this StorageBuffer block pointer.
    emit(decl,spv::OpVariable,{ptr_block,debug,spv::StorageClassStorageBuffer});
    std::array<uint32_t,WordCount> constants{};
    for(uint32_t j=0;j<constants.size();++j){constants[j]=id();emit(decl,spv::OpConstant,{u32,constants[j],j});}
    const auto zero=constants[0],one=constants[1],two=constants[2];
    const auto ft=id(),rec=id(),site=id(),value=id(),label=id();
    emit(decl,spv::OpTypeFunction,{ft,void_type,u32,u32});
    emit(fn,spv::OpFunction,{void_type,rec,0,ft});
    emit(fn,spv::OpFunctionParameter,{u32,site});
    emit(fn,spv::OpFunctionParameter,{u32,value});
    emit(fn,spv::OpLabel,{label});
    const auto flagptr=id(),flag=id(),enabled=id(),wg=id(),x=id(),y=id(),z=id(),xy=id(),xyz=id(),first=id(),active=id(),write=id(),merge=id();
    emit(fn,spv::OpAccessChain,{map.ptr_word,flagptr,debug,zero,zero});
    emit(fn,spv::OpLoad,{u32,flag,flagptr});
    emit(fn,spv::OpINotEqual,{map.boolean,enabled,flag,zero});
    emit(fn,spv::OpLoad,{vec3,wg,workgroup_id});
    emit(fn,spv::OpCompositeExtract,{u32,x,wg,0});
    emit(fn,spv::OpCompositeExtract,{u32,y,wg,1});
    emit(fn,spv::OpCompositeExtract,{u32,z,wg,2});
    emit(fn,spv::OpBitwiseOr,{u32,xy,x,y});emit(fn,spv::OpBitwiseOr,{u32,xyz,xy,z});
    emit(fn,spv::OpIEqual,{map.boolean,first,xyz,zero});
    emit(fn,spv::OpLogicalAnd,{map.boolean,active,enabled,first});
    emit(fn,spv::OpSelectionMerge,{merge,0});emit(fn,spv::OpBranchConditional,{active,write,merge});
    emit(fn,spv::OpLabel,{write});
    const auto offset=id(),base=id(),valpos=id(),p=id(),q=id();
    emit(fn,spv::OpIMul,{u32,offset,site,two});
    emit(fn,spv::OpIAdd,{u32,base,offset,one});
    emit(fn,spv::OpIAdd,{u32,valpos,base,one});
    emit(fn,spv::OpAccessChain,{map.ptr_word,p,debug,zero,base});
    emit(fn,spv::OpStore,{p,one});
    emit(fn,spv::OpAccessChain,{map.ptr_word,q,debug,zero,valpos});
    emit(fn,spv::OpStore,{q,value});
    emit(fn,spv::OpBranch,{merge});emit(fn,spv::OpLabel,{merge});emit(fn,spv::OpReturn,{});emit(fn,spv::OpFunctionEnd,{});
    bool main=false,globals=false,entry=false;
    std::array<bool,SiteCount> recorded{};
    std::vector<std::array<uint32_t,3>> pending;
    const auto record=[&](uint32_t index,uint32_t type,uint32_t v){
        if(recorded[index])return false;
        if(type==map.boolean){const auto bits=id();emit(out,spv::OpSelect,{u32,bits,v,one,zero});v=bits;}
        else if(type!=u32)return false;
        emit(out,spv::OpFunctionCall,{void_type,id(),rec,constants[index],v});
        recorded[index]=true;return true;
    };
    for(size_t i=5;i<spirv.size();){
        const auto n=spirv[i]>>16;
        const auto op=static_cast<spv::Op>(spirv[i]&65535u);
        if(i==first_type)out.insert(out.end(),annotations.begin(),annotations.end());
        if(op==spv::OpFunction){
            if(!globals){out.insert(out.end(),decl.begin(),decl.end());globals=true;}
            main=spirv[i+2]==entry_id;
        }
        if(main&&op!=spv::OpPhi&&!pending.empty()){
            for(auto a:pending)if(!record(a[0],a[1],a[2]))return false;
            pending.clear();
        }
        if(op==spv::OpEntryPoint){
            if(entry)return false;
            if(spirv[1]>=0x00010400u){
                out.push_back(spirv[i]+(1u<<16));
                out.insert(out.end(),spirv.begin()+i+1,spirv.begin()+i+n);out.push_back(debug);
            }else out.insert(out.end(),spirv.begin()+i,spirv.begin()+i+n);
            entry=true;
        }else out.insert(out.end(),spirv.begin()+i,spirv.begin()+i+n);
        if(main&&n>=3){
            if(op==spv::OpStore){
                for(uint32_t j=StoreGroup;j<SiteCount;++j)
                    if(spirv[i+1]==map.ids[j]&&!record(j,u32,spirv[i+2]))return false;
            }else{
                const auto type=spirv[i+1],v=spirv[i+2];
                if(type==u32||type==map.boolean)for(uint32_t j=0;j<StoreGroup;++j){
                    if(!map.ids[j]||v!=map.ids[j])continue;
                    if(op==spv::OpPhi)pending.push_back({j,type,v});
                    else if(!record(j,type,v))return false;
                }
            }
        }
        if(op==spv::OpFunctionEnd)main=false;
        i+=n;
    }
    if(!entry||!globals||!pending.empty())return false;
    for(uint32_t j=0;j<SiteCount;++j)if(bool(map.ids[j])!=recorded[j])return false;
    out.insert(out.end(),fn.begin(),fn.end());out[3]=next;spirv=std::move(out);return true;
}
}
