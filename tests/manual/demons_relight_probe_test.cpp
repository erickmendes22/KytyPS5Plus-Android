#include "graphics/host_gpu/renderer/pipeline/demonsRelightProbe.h"
#include <cassert>
#include <fstream>
#include <string>
#include <iostream>

using namespace Libs::Graphics::DemonsRelightProbe;
static void E(std::vector<uint32_t>& v,spv::Op o,std::initializer_list<uint32_t> a){
    v.push_back((static_cast<uint32_t>(a.size()+1)<<16)|o);
    v.insert(v.end(),a);
}
static std::vector<uint32_t> Fixture(const Map& m){
    std::vector<uint32_t> v{0x07230203,0x00010500,0,4000,0};
    constexpr uint32_t f=20,u=1,uv2=1000,fv2=1001,fv3=1002,fv4=1003,
        pair=1031,uv4=1004,img=1005,samp=1006,ip=1007,dp=1008,dimg=1009,dsamp=1010,
        fetchimg=1011,fetchptr=1012,outimg=1013,outptr=1014,
        ci=1015,cd=1016,cf=1017,co=1018,c0=1019,fc0=1020,
        coord=1021,grad=1022,ic=1023,exportv=1024,ft=1025;
    E(v,spv::OpCapability,{spv::CapabilityShader});
    E(v,spv::OpMemoryModel,{spv::AddressingModelLogical,spv::MemoryModelGLSL450});
    E(v,spv::OpEntryPoint,{spv::ExecutionModelGLCompute,56,0x6e69616d,0,4,5,ci,cd,cf,co});
    E(v,spv::OpExecutionMode,{56,spv::ExecutionModeLocalSize,8,8,1});
    E(v,spv::OpDecorate,{4,spv::DecorationBuiltIn,spv::BuiltInWorkgroupId});
    E(v,spv::OpDecorate,{5,spv::DecorationBuiltIn,spv::BuiltInLocalInvocationId});
    uint32_t binding=0;
    for(auto a:{ci,cd,cf,co}){
        E(v,spv::OpDecorate,{a,spv::DecorationDescriptorSet,0});
        E(v,spv::OpDecorate,{a,spv::DecorationBinding,binding++});
    }
    E(v,spv::OpTypeInt,{u,32,0});E(v,spv::OpTypeFloat,{f,32});
    E(v,spv::OpTypeVector,{2,u,3});E(v,spv::OpTypeVector,{uv2,u,2});
    E(v,spv::OpTypeStruct,{pair,u,u});E(v,spv::OpTypeVector,{uv4,u,4});E(v,spv::OpTypeVector,{fv2,f,2});
    E(v,spv::OpTypeVector,{fv3,f,3});E(v,spv::OpTypeVector,{fv4,f,4});
    E(v,spv::OpTypeBool,{m.boolean});E(v,spv::OpTypeVoid,{177});
    E(v,spv::OpTypePointer,{3,spv::StorageClassInput,2});
    E(v,spv::OpTypePointer,{m.ptr,spv::StorageClassStorageBuffer,u});
    E(v,spv::OpTypeImage,{img,f,spv::Dim2D,0,1,0,1,spv::ImageFormatUnknown});
    E(v,spv::OpTypeSampledImage,{samp,img});
    E(v,spv::OpTypePointer,{ip,spv::StorageClassUniformConstant,samp});
    E(v,spv::OpTypeImage,{dimg,f,spv::Dim2D,1,1,0,1,spv::ImageFormatUnknown});
    E(v,spv::OpTypeSampledImage,{dsamp,dimg});
    E(v,spv::OpTypePointer,{dp,spv::StorageClassUniformConstant,dsamp});
    E(v,spv::OpTypeImage,{fetchimg,u,spv::Dim2D,0,1,0,1,spv::ImageFormatUnknown});
    E(v,spv::OpTypePointer,{fetchptr,spv::StorageClassUniformConstant,fetchimg});
    E(v,spv::OpTypeImage,{outimg,f,spv::Dim2D,0,0,0,2,spv::ImageFormatRgba32f});
    E(v,spv::OpTypePointer,{outptr,spv::StorageClassUniformConstant,outimg});
    E(v,spv::OpTypeFunction,{ft,177});
    E(v,spv::OpConstant,{u,c0,0});E(v,spv::OpConstant,{f,fc0,0});
    E(v,spv::OpConstantComposite,{pair,1032,c0,c0});
    E(v,spv::OpConstantComposite,{fv3,coord,fc0,fc0,fc0});
    E(v,spv::OpConstantComposite,{fv2,grad,fc0,fc0});
    E(v,spv::OpConstantComposite,{2,ic,c0,c0,c0});
    E(v,spv::OpConstantComposite,{fv4,exportv,fc0,fc0,fc0,fc0});
    E(v,spv::OpConstantComposite,{uv2,1030,c0,c0});
    E(v,spv::OpVariable,{3,4,spv::StorageClassInput});E(v,spv::OpVariable,{3,5,spv::StorageClassInput});
    E(v,spv::OpVariable,{ip,ci,spv::StorageClassUniformConstant});E(v,spv::OpVariable,{dp,cd,spv::StorageClassUniformConstant});
    E(v,spv::OpVariable,{fetchptr,cf,spv::StorageClassUniformConstant});E(v,spv::OpVariable,{outptr,co,spv::StorageClassUniformConstant});
    E(v,spv::OpFunction,{177,56,0,ft});E(v,spv::OpLabel,{1100});
    E(v,spv::OpBranch,{1099});E(v,spv::OpLabel,{1099});
    E(v,spv::OpPhi,{pair,m.ray,1032,1100});
    E(v,spv::OpCompositeExtract,{u,1098,m.ray,0});
    E(v,spv::OpCompositeExtract,{u,1101,m.ray,1});
    E(v,spv::OpLoad,{samp,1102,ci});E(v,spv::OpLoad,{dsamp,1103,cd});
    E(v,spv::OpLoad,{fetchimg,1104,cf});E(v,spv::OpLoad,{outimg,1105,co});
    for(uint32_t i=0;i<15;++i){
        if(i<4)E(v,spv::OpImageSampleExplicitLod,{fv4,1200+i,1102,coord,4,grad,grad});
        else if(i<8||i==12)E(v,spv::OpImageSampleDrefExplicitLod,{f,1200+i,1103,coord,fc0,2,fc0});
        else if(i==9||i==10)E(v,spv::OpImageFetch,{uv4,1200+i,1104,ic,2,c0});
        else E(v,spv::OpImageSampleExplicitLod,{fv4,1200+i,1102,coord,2,fc0});
    }
    E(v,spv::OpImageWrite,{1105,1030,exportv});E(v,spv::OpReturn,{});E(v,spv::OpFunctionEnd,{});
    return v;
}
static void Save(const std::string& path,const std::vector<uint32_t>& v){
    std::ofstream out(path,std::ios::binary);
    out.write(reinterpret_cast<const char*>(v.data()),v.size()*4);assert(out.good());
}
int main(int argc,char** argv){
    if(argc==4){
        std::ifstream in(argv[1],std::ios::binary|std::ios::ate);
        assert(in.good()&&in.tellg()>0&&in.tellg()%4==0);
        std::vector<uint32_t> v(static_cast<size_t>(in.tellg())/4);
        in.seekg(0);in.read(reinterpret_cast<char*>(v.data()),v.size()*4);
        assert(in.good()&&Instrument(v,std::stoull(argv[2],nullptr,16),55));Save(argv[3],v);return 0;
    }
    assert(argc==2);
    for(size_t k=0;k<Maps.size();++k){
        const auto& m=Maps[k];const auto original=Fixture(m);auto v=original;
        assert(!Instrument(v,m.fp^1,55)&&v==original);assert(Instrument(v,m.fp,55));
        uint32_t calls=0;
        for(size_t i=5;i<v.size();){
            const auto n=v[i]>>16;assert(n&&n<=v.size()-i);
            if((v[i]&65535u)==spv::OpFunctionCall){assert(n==17);++calls;}
            i+=n;
        }
        assert(calls==Sites);
        Save(std::string(argv[1])+std::to_string(k)+"-original.spv",original);
        Save(std::string(argv[1])+std::to_string(k)+"-instrumented.spv",v);
    }
    auto fifth=Fixture(Maps[0]);
    assert(Instrument(fifth,0x4690ec9f28ed697cull,55));
    Save(std::string(argv[1])+"fifth-instrumented.spv",fifth);
    std::cout<<"Relight trace: struct/phi ray pair, 17 sites, four maps and guarded fifth OK\n";
}
