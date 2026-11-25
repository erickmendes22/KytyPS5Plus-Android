#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>
#include <spirv/unified1/spirv.hpp>

namespace Libs::Graphics::DemonsCountExecProbe {
constexpr uint64_t Shader=0xfb18d110fab8234eull;
constexpr uint64_t Fingerprint=0x737447ebe010052eull;
// Exact module exported in 202757. 187 is the guest invocation index.
// All subgroup-any predicates/results, important float comparisons, BDA
// addresses/loads, and atomic operands/results. Values are RAW bits.
constexpr std::array<uint32_t,157> Sites{
187,208,210,211,230,246,247,249,250,251,266,582,824,931,932,938,939,945,946,951,962,971,972,977,978,984,985,990,1001,1142,1232,1233,1234,1235,1255,1256,1257,1258,1581,1730,1818,1819,1820,1821,1838,1839,1840,1841,2373,2374,2385,2421,2509,2510,2511,2512,2529,2530,2531,2532,2634,2635,2779,2780,3841,3845,3846,3847,3848,3904,3905,3953,3954,3955,3956,3979,3980,3981,3982,4097,4098,4099,4100,4101,4106,4121,4124,4125,4126,4127,4150,4154,4155,4156,4157,4199,4200,4317,4328,4348,4421,4422,4423,4511,4513,4514,4515,4591,4593,4594,4595,4772,4773,4774,4775,4843,4844,4845,4846,5748,5752,5753,5754,5755,5756,5776,5779,5780,5781,5782,5784,5798,5804,5805,6320,6324,6325,6326,6327,6329,6343,6346,6347,6348,6349,6351,6365,6371,6372,6490,6491,6496,6497,6503,6504,6509,6518};
constexpr uint32_t Lanes=64, WordsPerSite=3;
constexpr uint32_t WordCount=1+Lanes*Sites.size()*WordsPerSite;
inline bool Instrument(std::vector<uint32_t>& spirv,uint64_t fingerprint,uint32_t binding) {
    if (fingerprint!=Fingerprint || spirv.size()<5) return false;
    constexpr uint32_t u32=1,f32=19,u64=31,boolean=113,void_type=153,ptr_word=147,ptr_input_word=6;
    uint32_t next=spirv[3];
    const auto id=[&]{return next++;};
    const auto emit=[](std::vector<uint32_t>& out,spv::Op op,std::initializer_list<uint32_t> args){
        out.push_back((static_cast<uint32_t>(args.size()+1)<<16)|op);
        out.insert(out.end(),args);
    };
    size_t first_type=0;
    for(size_t i=5;i<spirv.size();){
        const auto n=spirv[i]>>16;
        if(!n || n>spirv.size()-i)return false;
        const auto op=static_cast<spv::Op>(spirv[i]&65535u);
        if(!first_type && op>=spv::OpTypeVoid && op<=spv::OpTypeForwardPointer)first_type=i;
        i+=n;
    }
    if(!first_type)return false;
    std::vector<uint32_t> annotations,decl,fn,result(spirv.begin(),spirv.begin()+5);
    const auto data=id(),block=id(),ptr_block=id(),buffer=id();
    emit(annotations,spv::OpDecorate,{data,spv::DecorationArrayStride,4});
    emit(annotations,spv::OpDecorate,{block,spv::DecorationBlock});
    emit(annotations,spv::OpMemberDecorate,{block,0,spv::DecorationOffset,0});
    emit(annotations,spv::OpDecorate,{buffer,spv::DecorationDescriptorSet,0});
    emit(annotations,spv::OpDecorate,{buffer,spv::DecorationBinding,binding});
    emit(decl,spv::OpTypeRuntimeArray,{data,u32});
    emit(decl,spv::OpTypeStruct,{block,data});
    emit(decl,spv::OpTypePointer,{ptr_block,spv::StorageClassStorageBuffer,block});
    emit(decl,spv::OpVariable,{ptr_block,buffer,spv::StorageClassStorageBuffer});
    std::array<uint32_t,Sites.size()+1> constants{};
    for(uint32_t i=0;i<constants.size();++i){constants[i]=id();emit(decl,spv::OpConstant,{u32,constants[i],i});}
    const auto c0=constants[0],c1=constants[1],c2=constants[2],c3=constants[3],c64=constants[64],c32=constants[32],cs=constants[Sites.size()];
    const auto fn_type=id(),rec=id(),lane=id(),site=id(),lo=id(),hi=id(),entry_label=id();
    emit(decl,spv::OpTypeFunction,{fn_type,void_type,u32,u32,u32,u32});
    emit(fn,spv::OpFunction,{void_type,rec,0,fn_type});
    for(auto p:{lane,site,lo,hi})emit(fn,spv::OpFunctionParameter,{u32,p});
    emit(fn,spv::OpLabel,{entry_label});
    const auto flag_ptr=id(),flag=id(),enabled=id(),inrange=id(),active=id(),write=id(),merge=id();
    emit(fn,spv::OpAccessChain,{ptr_word,flag_ptr,buffer,c0,c0});
    emit(fn,spv::OpLoad,{u32,flag,flag_ptr});
    emit(fn,spv::OpINotEqual,{boolean,enabled,flag,c0});
    emit(fn,spv::OpULessThan,{boolean,inrange,lane,c64});
    emit(fn,spv::OpLogicalAnd,{boolean,active,enabled,inrange});
    emit(fn,spv::OpSelectionMerge,{merge,0});
    emit(fn,spv::OpBranchConditional,{active,write,merge});
    emit(fn,spv::OpLabel,{write});
    const auto row=id(),offset=id(),words=id(),base=id();
    emit(fn,spv::OpIMul,{u32,row,lane,cs});
    emit(fn,spv::OpIAdd,{u32,offset,row,site});
    emit(fn,spv::OpIMul,{u32,words,offset,c3});
    emit(fn,spv::OpIAdd,{u32,base,words,c1});
    for(auto [off,value]:std::array<std::pair<uint32_t,uint32_t>,3>{{{c0,c1},{c1,lo},{c2,hi}}}){
        const auto at=id(),ptr=id();
        emit(fn,spv::OpIAdd,{u32,at,base,off});
        emit(fn,spv::OpAccessChain,{ptr_word,ptr,buffer,c0,at});
        emit(fn,spv::OpStore,{ptr,value});
    }
    emit(fn,spv::OpBranch,{merge});emit(fn,spv::OpLabel,{merge});
    emit(fn,spv::OpReturn,{});emit(fn,spv::OpFunctionEnd,{});
    uint32_t safe_lane=0,seen=0;
    bool main=false,globals=false,entry=false;
    std::vector<std::pair<uint32_t,uint32_t>> pending;
    const auto record=[&](uint32_t type,uint32_t value){
        if(!safe_lane)return false;
        const auto iter=std::lower_bound(Sites.begin(),Sites.end(),value);
        if(iter==Sites.end()||*iter!=value)return false;
        uint32_t bits=value,upper=c0;
        if(type==boolean){bits=id();emit(result,spv::OpSelect,{u32,bits,value,c1,c0});}
        else if(type==f32){bits=id();emit(result,spv::OpBitcast,{u32,bits,value});}
        else if(type==u64){
            bits=id();const auto shifted=id();upper=id();
            emit(result,spv::OpUConvert,{u32,bits,value});
            emit(result,spv::OpShiftRightLogical,{u64,shifted,value,c32});
            emit(result,spv::OpUConvert,{u32,upper,shifted});
        }else if(type!=u32)return false;
        emit(result,spv::OpFunctionCall,{void_type,id(),rec,safe_lane,constants[iter-Sites.begin()],bits,upper});
        ++seen;return true;
    };
    for(size_t i=5;i<spirv.size();){
        const auto n=spirv[i]>>16;const auto op=static_cast<spv::Op>(spirv[i]&65535u);
        if(i==first_type)result.insert(result.end(),annotations.begin(),annotations.end());
        if(op==spv::OpFunction){
            if(!globals){result.insert(result.end(),decl.begin(),decl.end());globals=true;}
            main=spirv[i+2]==39;
        }
        if(main && op!=spv::OpPhi && !pending.empty()){
            for(auto [type,value]:pending)if(!record(type,value))return false;
            pending.clear();
        }
        if(op==spv::OpEntryPoint){
            if(entry)return false;
            // SPIR-V <=1.3 permits only Input/Output variables in the
            // entry-point interface. >=1.4 requires all used globals.
            // 214450 exported version 0x00010300, so listing the new
            // StorageBuffer here made the diagnostic invalid.
            if (spirv[1]>=0x00010400u) {
                result.push_back(spirv[i]+(1u<<16));
                result.insert(result.end(),spirv.begin()+i+1,spirv.begin()+i+n);
                result.push_back(buffer);
            } else {
                result.insert(result.end(),spirv.begin()+i,spirv.begin()+i+n);
            }
            entry=true;
        }else result.insert(result.end(),spirv.begin()+i,spirv.begin()+i+n);
        if(main && n>=3){
            const auto type=spirv[i+1],value=spirv[i+2];
            if((type==u32||type==u64||type==f32||type==boolean) &&
               std::binary_search(Sites.begin(),Sites.end(),value)){
                if(value==187){
                    // Exact module's WorkgroupId input is %4. Discard all
                    // workgroups beyond X=0 and Y/Z=0, without touching guest EXEC.
                    const auto yp=id(),zp=id(),y=id(),z=id(),yz=id(),first=id();
                    safe_lane=id();
                    emit(result,spv::OpAccessChain,{ptr_input_word,yp,4,c1});
                    emit(result,spv::OpAccessChain,{ptr_input_word,zp,4,c2});
                    emit(result,spv::OpLoad,{u32,y,yp});emit(result,spv::OpLoad,{u32,z,zp});
                    emit(result,spv::OpBitwiseOr,{u32,yz,y,z});
                    emit(result,spv::OpIEqual,{boolean,first,yz,c0});
                    emit(result,spv::OpSelect,{u32,safe_lane,first,value,c64});
                }
                if(op==spv::OpPhi)pending.emplace_back(type,value);
                else if(!record(type,value))return false;
            }
        }
        if(op==spv::OpFunctionEnd)main=false;
        i+=n;
    }
    if(!entry||!globals||seen!=Sites.size()||!pending.empty())return false;
    result.insert(result.end(),fn.begin(),fn.end());result[3]=next;
    spirv=std::move(result);return true;
}
} // namespace Libs::Graphics::DemonsCountExecProbe
