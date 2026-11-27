#include "graphics/host_gpu/renderer/pipeline/demonsVertexPositionProbe.h"
#include <cassert>
#include <fstream>
#include <string>
#include <iostream>
#include <cstring>
#include <algorithm>
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
    E(v,spv::OpTypeFloat,{s.f32,32});
    E(v,spv::OpTypeVector,{s.vec4,s.f32,4});
    E(v,spv::OpTypeBool,{s.boolean});
    E(v,spv::OpTypeVoid,{s.void_type});
    E(v,spv::OpTypePointer,{s.ptr,spv::StorageClassStorageBuffer,1});
    E(v,spv::OpTypePointer,{outptr,spv::StorageClassOutput,s.vec4});
    E(v,spv::OpTypeFunction,{functype,s.void_type});
    E(v,spv::OpConstant,{s.f32,one,0x3f800000});
    E(v,spv::OpConstant,{s.f32,zero,0});
    E(v,spv::OpConstant,{1,s.vertex,0});
    E(v,spv::OpConstant,{1,s.instance,0});
    if(s.transform)E(v,spv::OpConstant,{1,s.transform,0});
    E(v,spv::OpConstant,{s.f32,s.clip,0});
    for(auto i:s.inputs)if(i)E(v,spv::OpConstant,{1,i,0});
    E(v,spv::OpConstantComposite,{s.vec4,s.position,zero,zero,zero,one});
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

// No production hash, SSA site, uint type ID or recorder pointer is reused.
// Optional built-in block places Position at member 1, not member 0.
static std::vector<uint32_t> Generic(uint32_t version,bool block_position,bool unsigned_inputs=false){
    enum : uint32_t {U=101,I,F,V,Void,InputPtr,FloatOutPtr,VecOutPtr,OneU,ZeroU,
        ClipArray,ClipOutPtr,Block,BlockPtr,VertexVar,InstanceVar,ClipVar,PositionVar,
        FnType,Fn,Label,VertexS,InstanceS,VertexU,InstanceU,ZeroF,OneF,Position,ClipPtr,PositionPtr};
    std::vector<uint32_t> v{0x07230203,version,0,256,0};
    E(v,spv::OpCapability,{spv::CapabilityShader});
    E(v,spv::OpCapability,{spv::CapabilityClipDistance});
    E(v,spv::OpMemoryModel,{spv::AddressingModelLogical,spv::MemoryModelGLSL450});
    E(v,spv::OpEntryPoint,{spv::ExecutionModelVertex,Fn,0x6e69616d,0,VertexVar,InstanceVar,ClipVar,PositionVar});
    E(v,spv::OpDecorate,{VertexVar,spv::DecorationBuiltIn,spv::BuiltInVertexIndex});
    E(v,spv::OpDecorate,{InstanceVar,spv::DecorationBuiltIn,spv::BuiltInInstanceIndex});
    E(v,spv::OpDecorate,{ClipVar,spv::DecorationBuiltIn,spv::BuiltInClipDistance});
    if(block_position){
        E(v,spv::OpDecorate,{Block,spv::DecorationBlock});
        E(v,spv::OpMemberDecorate,{Block,0,spv::DecorationBuiltIn,spv::BuiltInPointSize});
        E(v,spv::OpMemberDecorate,{Block,1,spv::DecorationBuiltIn,spv::BuiltInPosition});
    }else E(v,spv::OpDecorate,{PositionVar,spv::DecorationBuiltIn,spv::BuiltInPosition});
    E(v,spv::OpTypeInt,{U,32,0});
    E(v,spv::OpTypeInt,{I,32,1});
    E(v,spv::OpTypeFloat,{F,32});
    E(v,spv::OpTypeVector,{V,F,4});
    E(v,spv::OpTypeVoid,{Void});
    E(v,spv::OpTypePointer,{InputPtr,spv::StorageClassInput,unsigned_inputs?U:I});
    E(v,spv::OpTypePointer,{FloatOutPtr,spv::StorageClassOutput,F});
    E(v,spv::OpTypePointer,{VecOutPtr,spv::StorageClassOutput,V});
    E(v,spv::OpConstant,{U,OneU,1});
    E(v,spv::OpConstant,{U,ZeroU,0});
    E(v,spv::OpTypeArray,{ClipArray,F,OneU});
    E(v,spv::OpTypePointer,{ClipOutPtr,spv::StorageClassOutput,ClipArray});
    if(block_position){
        E(v,spv::OpTypeStruct,{Block,F,V});
        E(v,spv::OpTypePointer,{BlockPtr,spv::StorageClassOutput,Block});
    }
    E(v,spv::OpVariable,{InputPtr,VertexVar,spv::StorageClassInput});
    E(v,spv::OpVariable,{InputPtr,InstanceVar,spv::StorageClassInput});
    E(v,spv::OpVariable,{ClipOutPtr,ClipVar,spv::StorageClassOutput});
    E(v,spv::OpVariable,{block_position?BlockPtr:VecOutPtr,PositionVar,spv::StorageClassOutput});
    E(v,spv::OpTypeFunction,{FnType,Void});
    E(v,spv::OpConstant,{F,ZeroF,0});
    E(v,spv::OpConstant,{F,OneF,0x3f800000});
    E(v,spv::OpConstantComposite,{V,Position,ZeroF,ZeroF,ZeroF,OneF});
    E(v,spv::OpFunction,{Void,Fn,0,FnType});
    E(v,spv::OpLabel,{Label});
    E(v,spv::OpLoad,{unsigned_inputs?U:I,VertexS,VertexVar});
    E(v,spv::OpLoad,{unsigned_inputs?U:I,InstanceS,InstanceVar});
    if(!unsigned_inputs){
        E(v,spv::OpBitcast,{U,VertexU,VertexS});
        E(v,spv::OpBitcast,{U,InstanceU,InstanceS});
    }
    E(v,spv::OpAccessChain,{FloatOutPtr,ClipPtr,ClipVar,ZeroU});
    E(v,spv::OpStore,{ClipPtr,ZeroF});
    if(block_position)E(v,spv::OpAccessChain,{VecOutPtr,PositionPtr,PositionVar,OneU});
    E(v,spv::OpStore,{block_position?PositionPtr:PositionVar,Position});
    E(v,spv::OpReturn,{});
    E(v,spv::OpFunctionEnd,{});
    return v;
}

static std::vector<std::vector<uint32_t>> Instructions(const std::vector<uint32_t>& v){
    std::vector<std::vector<uint32_t>> out;
    for(std::size_t i=5;i<v.size();){
        const auto n=v[i]>>16;
        assert(n&&n<=v.size()-i);
        out.emplace_back(v.begin()+i,v.begin()+i+n);
        i+=n;
    }
    return out;
}
static void Preserved(const std::vector<uint32_t>& source,const std::vector<uint32_t>& result){
    assert(source[0]==result[0]&&source[1]==result[1]&&source[2]==result[2]&&source[4]==result[4]);
    assert(result[3]>source[3]);
    const auto original=Instructions(source),instrumented=Instructions(result);
    std::size_t cursor=0;
    for(const auto& op:original){
        bool found=false;
        while(cursor<instrumented.size()){
            const auto& candidate=instrumented[cursor++];
            if((op[0]&65535u)==spv::OpEntryPoint&&source[1]>=0x00010400u){
                found=candidate.size()==op.size()+1&&candidate[0]==op[0]+(1u<<16)&&
                    std::equal(op.begin()+1,op.end(),candidate.begin()+1);
            }else found=candidate==op;
            if(found)break;
        }
        assert(found);
    }
    // Only the new recorder function performs writes; the original output
    // stores remain exactly the same in number, pointer and value.
    std::vector<std::vector<uint32_t>> before,after;
    for(const auto& op:original)if((op[0]&65535u)==spv::OpStore)before.push_back(op);
    for(const auto& op:instrumented)if((op[0]&65535u)==spv::OpStore)after.push_back(op);
    assert(before==after);
}

static void Rejected(const std::vector<uint32_t>& source,const char* expected){
    auto v=source;
    const char* reason=nullptr;
    assert(!Instrument(v,0x557d65da5b786842ull,123,55,true,&reason));
    assert(v==source&&reason&&std::strcmp(reason,expected)==0);
}
int main(int argc,char** argv){
    assert(argc>=2); // output prefix, followed by optional captured native VS modules
    const std::array<uint32_t,5> mixed{17,19,0x80000400u,0x80000402u,17};
    const auto mixed_windows=VertexWindows::Build(mixed,0);
    assert(mixed_windows.base[0]==17&&mixed_windows.count[0]==3);
    assert(mixed_windows.base[1]==0x80000400u&&mixed_windows.count[1]==3);
    assert(mixed_windows.Total()==6&&mixed_windows.Offset(17)==0&&mixed_windows.Offset(19)==2);
    assert(mixed_windows.Offset(0x80000400u)==3&&mixed_windows.Offset(0x80000402u)==5);
    assert(mixed_windows.Offset(0x800003ffu)==UINT32_MAX);
    assert(mixed_windows.Offset(20)==UINT32_MAX);
    const std::array<uint32_t,2> wrap{0,1};
    const auto wrapped=VertexWindows::Build(wrap,-1);
    assert(wrapped.Total()==2&&wrapped.Offset(UINT32_MAX)==1&&wrapped.Offset(0)==0);
    const std::array<uint32_t,2> sparse{1,65537};
    assert(!VertexWindows::Build(sparse,0).Total());
    const std::array<uint32_t,2> signed_limit{0x7fffffffu,0x80000000u};
    assert(VertexWindows::Build(signed_limit,0).Total()==2);
    static_assert(HasProjectionOperands(0x868546d85ce5090aull,0x7843cbd43b48179bull));
    assert(!HasTransformInputs(0x868546d85ce5090aull,0x7843cbd43b48179bull));
    static_assert(FocusPair(0x868546d85ce5090aull,0x31e660625354d953ull,12765)==1);
    static_assert(FocusPair(0x13f07ce83dc1cc67ull,0xe31a5473e9ee7503ull,12765)==1);
    static_assert(FocusPair(0x868546d85ce5090aull,0x31e660625354d953ull,57066)==2);
    static_assert(FocusPair(0xc96f3fcde8141266ull,0x552d823dcfcf2deeull,36)==4);
    static_assert(FocusPair(0x34c51585c926a664ull,0x3dce2bd78d58db62ull,17760)==FocusPairCount);
    for(size_t k=0;k<Sites.size();++k){
        const auto& s=Sites[k];
        for(uint32_t version:{0x00010300u,0x00010400u}){
            const auto src=Fixture(s,version);auto v=src;
            assert(!Instrument(v,s.hash,s.fingerprint^1,55)&&v==src);
            assert(Instrument(v,s.hash,s.fingerprint,55));
            Preserved(src,v);
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
    for(uint32_t version:{0x00010300u,0x00010400u})for(bool block:{false,true})for(bool unsigned_inputs:{false,true}){
        const auto src=Generic(version,block,unsigned_inputs);
        auto v=src;
        Site discovered{};
        const char* reason=nullptr;
        assert(DiscoverExport(v,discovered,&reason));
        assert(discovered.u32!=1&&!discovered.ptr&&!discovered.boolean);
        // Both the newly seen material hash and an arbitrary unseen hash
        // follow the same structural path. No 557d site/allowlist is added.
        const auto hash=block?0x557d65da5b786842ull:0x1234567890abcdefull;
        assert(!Instrument(v,hash,123,55)&&v==src);
        assert(Instrument(v,hash,123,55,true,&reason));
        assert(std::strcmp(reason,"instrumented_structural_position_export")==0);
        Preserved(src,v);
        uint32_t limit=0;unsigned range_checks=0;
        for(const auto& op:Instructions(v)){
            if((op[0]&65535u)==spv::OpConstant&&op.size()==4&&op[3]==MaxVertices)limit=op[2];
            if((op[0]&65535u)==spv::OpULessThan){
                ++range_checks;
                if(range_checks==3)assert(op[4]==limit);
            }
        }
        assert(limit&&range_checks==3);
        Save(std::string(argv[1])+"generic-"+std::to_string(version)+"-"+std::to_string(block)+"-"+
            std::to_string(unsigned_inputs)+".spv",v);
    }
    const auto generic=Generic(0x00010400u,false);
    auto bad=generic;
    bad.pop_back();
    bad.back()=0;
    Rejected(bad,"malformed_instruction");
    bad=generic;bad[0]=0;
    Rejected(bad,"invalid_module_header_or_id_bound");
    bad=generic;bad[3]=UINT32_MAX;
    Rejected(bad,"invalid_module_header_or_id_bound");
    bad=generic;
    for(std::size_t i=5;i<bad.size();i+=bad[i]>>16)if((bad[i]&65535u)==spv::OpEntryPoint){
        bad[i+1]=spv::ExecutionModelFragment;break;
    }
    Rejected(bad,"expected_one_vertex_entry_point");
    bad=generic;
    for(std::size_t i=5;i<bad.size();i+=bad[i]>>16)if((bad[i]&65535u)==spv::OpDecorate&&bad[i+3]==spv::BuiltInClipDistance){
        bad[i+3]=spv::BuiltInCullDistance;break;
    }
    Rejected(bad,"vertex_instance_or_clip_output_absent");
    bad=generic;
    for(std::size_t i=5;i<bad.size();i+=bad[i]>>16)if((bad[i]&65535u)==spv::OpConstant&&bad[i+3]==1){
        bad[i+3]=2;break;
    }
    Rejected(bad,"expected_one_f32_clip_plane");
    bad=generic;
    for(std::size_t i=5;i<bad.size();i+=bad[i]>>16)if((bad[i]&65535u)==spv::OpStore){
        const std::vector<uint32_t> op(bad.begin()+i,bad.begin()+i+(bad[i]>>16));
        bad.insert(bad.begin()+i,op.begin(),op.end());break;
    }
    Rejected(bad,"unsupported_or_multiple_clip_stores");
    bad=generic;
    for(std::size_t i=5;i<bad.size();i+=bad[i]>>16)if((bad[i]&65535u)==spv::OpDecorate&&bad[i+3]==spv::BuiltInPosition){
        bad[i+3]=spv::BuiltInPointSize;break;
    }
    Rejected(bad,"position_export_absent");
    for(int argument=2;argument<argc;++argument){
        std::ifstream input(argv[argument],std::ios::binary|std::ios::ate);
        assert(input.good());
        const auto bytes=input.tellg();
        assert(bytes>=20&&bytes%4==0);
        std::vector<uint32_t> source(static_cast<std::size_t>(bytes)/4);
        input.seekg(0);input.read(reinterpret_cast<char*>(source.data()),bytes);
        assert(input.good());
        auto instrumented=source;
        const char* reason=nullptr;
        // Force structural discovery even for a known historical module.
        assert(Instrument(instrumented,0x1234567890abcdefull,0,4095,true,&reason));
        Preserved(source,instrumented);
        Save(std::string(argv[1])+"captured-"+std::to_string(argument-2)+".spv",instrumented);
        std::cout<<"Captured VS preserved: "<<argv[argument]<<" ("<<reason<<")\n";
    }
    std::cout<<"Vertex recorder: 14 exact sites, generic exports, bounds and original instructions verified\n";
}
