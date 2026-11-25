#include "graphics/host_gpu/renderer/pipeline/demonsVertexPositionProbe.h"
#include <cassert>
#include <fstream>
#include <string>
#include <iostream>
using namespace Libs::Graphics::DemonsVertexPositionProbe;
static void E(std::vector<uint32_t>& v,spv::Op op,std::initializer_list<uint32_t> args) {
    v.push_back((static_cast<uint32_t>(args.size()+1)<<16)|op);
    v.insert(v.end(),args.begin(),args.end());
}
static std::vector<uint32_t> Fixture(const Site& s,uint32_t version){
    constexpr uint32_t outptr=2000,functype=2001,label=2002,one=2003,zero=2004;
    std::vector<uint32_t> v{0x07230203,version,0,3000,0};
    E(v,spv::OpCapability,{spv::CapabilityShader});
    E(v,spv::OpMemoryModel,{spv::AddressingModelLogical,spv::MemoryModelGLSL450});
    E(v,spv::OpEntryPoint,{spv::ExecutionModelVertex,s.main,0x6e69616d,0,s.store});
    E(v,spv::OpDecorate,{s.store,spv::DecorationBuiltIn,spv::BuiltInPosition});
    E(v,spv::OpTypeInt,{1,32,0});
    E(v,spv::OpTypeFloat,{8,32});
    E(v,spv::OpTypeVector,{9,8,4});
    E(v,spv::OpTypeBool,{s.boolean});
    E(v,spv::OpTypeVoid,{s.void_type});
    E(v,spv::OpTypePointer,{s.ptr,spv::StorageClassStorageBuffer,1});
    E(v,spv::OpTypePointer,{outptr,spv::StorageClassOutput,9});
    E(v,spv::OpTypeFunction,{functype,s.void_type});
    E(v,spv::OpConstant,{8,one,0x3f800000});
    E(v,spv::OpConstant,{8,zero,0});
    E(v,spv::OpConstant,{1,s.vertex,0});
    E(v,spv::OpConstant,{1,s.instance,0});
    if(s.transform)E(v,spv::OpConstant,{1,s.transform,0});
    E(v,spv::OpConstant,{8,s.clip,0});
    for(auto i:s.inputs)if(i)E(v,spv::OpConstant,{1,i,0});
    E(v,spv::OpConstantComposite,{9,s.position,zero,zero,zero,one});
    E(v,spv::OpVariable,{outptr,s.store,spv::StorageClassOutput});
    E(v,spv::OpFunction,{s.void_type,s.main,0,functype});
    E(v,spv::OpLabel,{label});
    E(v,spv::OpStore,{s.store,s.position});
    E(v,spv::OpReturn,{});
    E(v,spv::OpFunctionEnd,{});
    return v;
}
static void Save(const std::string& path,const std::vector<uint32_t>& v){
    std::ofstream out(path,std::ios::binary);
    out.write(reinterpret_cast<const char*>(v.data()),v.size()*sizeof(uint32_t));
    assert(out.good());
}
int main(int argc,char** argv){
    assert(argc==2);
    for(size_t k=0;k<Sites.size();++k){
        const auto& s=Sites[k];
        for(uint32_t version:{0x00010300u,0x00010400u}){
            const auto src=Fixture(s,version);auto v=src;
            assert(!Instrument(v,s.hash,s.fingerprint^1,55)&&v==src);
            assert(Instrument(v,s.hash,s.fingerprint,55));
            size_t calls=0,stores=0;
            for(size_t i=5;i<v.size();){
                const auto n=v[i]>>16;
                assert(n && n<=v.size()-i);
                if((v[i]&65535u)==spv::OpFunctionCall){assert(n==24);++calls;}
                if((v[i]&65535u)==spv::OpAtomicExchange)++stores;
                i+=n;
            }
            assert(calls==1&&stores==Stride);
            Save(std::string(argv[1])+std::to_string(k)+"-"+std::to_string(version)+".spv",v);
        }
    }
    std::cout<<"Vertex position recorder: all eight site maps and SPIR-V versions assembled\n";
}
