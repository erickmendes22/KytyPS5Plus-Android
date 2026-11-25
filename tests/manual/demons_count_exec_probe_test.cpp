// Synthetic shader exercising the identical ID/type/control-flow contract.
// No commercial game code is embedded in the repository.
// Optionally pass a captured original SPIR-V file to validate that exact module.
#include "graphics/host_gpu/renderer/pipeline/demonsCountExecProbe.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <string>

using namespace Libs::Graphics::DemonsCountExecProbe;
static void E(std::vector<uint32_t>& v,spv::Op op,std::initializer_list<uint32_t> args) {
    v.push_back((static_cast<uint32_t>(args.size()+1)<<16)|op);
    v.insert(v.end(),args.begin(),args.end());
}
static std::vector<uint32_t> Fixture(uint32_t version) {
    std::vector<uint32_t> v{0x07230203,version,0,7000,0};
    E(v,spv::OpCapability,{spv::CapabilityShader});
    E(v,spv::OpCapability,{spv::CapabilityInt64});
    E(v,spv::OpMemoryModel,{spv::AddressingModelLogical,spv::MemoryModelGLSL450});
    E(v,spv::OpEntryPoint,{spv::ExecutionModelGLCompute,39,0x6e69616d,0,4});
    E(v,spv::OpExecutionMode,{39,spv::ExecutionModeLocalSize,64,1,1});
    E(v,spv::OpDecorate,{4,spv::DecorationBuiltIn,spv::BuiltInWorkgroupId});
    E(v,spv::OpTypeInt,{1,32,0});
    E(v,spv::OpTypeVector,{2,1,3});
    E(v,spv::OpTypePointer,{3,spv::StorageClassInput,2});
    E(v,spv::OpTypePointer,{6,spv::StorageClassInput,1});
    E(v,spv::OpTypeFloat,{19,32});
    E(v,spv::OpTypeInt,{31,64,0});
    E(v,spv::OpTypeBool,{113});
    E(v,spv::OpTypeVoid,{153});
    E(v,spv::OpTypePointer,{147,spv::StorageClassStorageBuffer,1});
    E(v,spv::OpTypeFunction,{154,153});
    E(v,spv::OpConstant,{1,10,0});
    E(v,spv::OpConstant,{1,11,1});
    E(v,spv::OpConstant,{19,12,0x3f800000});
    E(v,spv::OpConstant,{31,13,0,0});
    E(v,spv::OpConstantTrue,{113,14});
    E(v,spv::OpVariable,{3,4,spv::StorageClassInput});
    E(v,spv::OpFunction,{153,39,0,154});
    E(v,spv::OpLabel,{40});
    for(auto site:Sites) {
        if(site==187) E(v,spv::OpIAdd,{1,site,10,11});
        else if(site%4==0) E(v,spv::OpIAdd,{1,site,10,11});
        else if(site%4==1) E(v,spv::OpLogicalNot,{113,site,14});
        else if(site%4==2) E(v,spv::OpFAdd,{19,site,12,12});
        else E(v,spv::OpIAdd,{31,site,13,13});
    }
    E(v,spv::OpReturn,{});
    E(v,spv::OpFunctionEnd,{});
    return v;
}
static void Save(const std::string& path,const std::vector<uint32_t>& v) {
    std::ofstream out(path,std::ios::binary);
    out.write(reinterpret_cast<const char*>(v.data()),v.size()*4);
    assert(out.good());
}
int main(int argc,char** argv) {
    if(argc==3) {
        std::ifstream in(argv[1],std::ios::binary|std::ios::ate);
        assert(in.good() && in.tellg()>0 && in.tellg()%4==0);
        std::vector<uint32_t> v(static_cast<size_t>(in.tellg())/4);
        in.seekg(0);in.read(reinterpret_cast<char*>(v.data()),v.size()*4);
        assert(in.good() && Instrument(v,Fingerprint,55));
        Save(argv[2],v);
        return 0;
    }
    assert(argc==2);
    for(uint32_t version:{0x00010300u,0x00010400u}) {
        auto original=Fixture(version),v=original;
        assert(!Instrument(v,Fingerprint^1,55) && v==original);
        assert(Instrument(v,Fingerprint,55));
        auto alt=original;
        assert(Instrument(alt,AlternateFingerprint,55)&&alt==v);
        size_t entries=0;
        for(size_t i=5;i<v.size();) {
            const auto n=v[i]>>16;
            assert(n && n<=v.size()-i);
            if((v[i]&65535u)==spv::OpEntryPoint) {
                assert(n==(version<0x00010400u?6u:7u));
                ++entries;
            }
            i+=n;
        }
        assert(entries==1 && v[1]==version);
        const auto stem=std::string(argv[1])+(version==0x00010300u?"13":"14");
        Save(stem+"-original.spv",original);
        Save(stem+"-instrumented.spv",v);
    }
    std::cout<<"Count EXEC probe versioned interfaces OK\n";
}
