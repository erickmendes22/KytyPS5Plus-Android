// Host-only independent guest ISA runner; does not use Kyty/SPIR-V.
#include "rocjitsu/vm/amdgpu/compute_unit.h"
#include "rocjitsu/vm/amdgpu/gpu_memory.h"
#include "rocjitsu/vm/amdgpu/l2_cache.h"
#include "rocjitsu/vm/amdgpu/wavefront.h"
#include "util/diagnostic.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace rocjitsu;
struct Range { uint64_t addr; uint64_t size; };
static void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
static std::vector<uint8_t> read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    require(bool(f), "file open");
    auto size = f.tellg();
    require(size > 0 && size <= 512ll*1024*1024, "file size");
    std::vector<uint8_t> data(static_cast<size_t>(size));
    f.seekg(0);
    require(bool(f.read(reinterpret_cast<char*>(data.data()), size)), "file read");
    return data;
}
static bool covers(const std::vector<Range>& ranges, uint64_t addr, uint64_t n) {
    if (!n || addr > UINT64_MAX - n) return false;
    uint64_t end = addr+n;
    for (auto range : ranges) {
        if (range.addr > addr) return false;
        if (addr < range.addr+range.size) {
            addr = std::min(end, range.addr+range.size);
            if (addr == end) return true;
        }
    }
    return false;
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: p8_guest_ref manifest.txt");
        std::ifstream in(argv[1]);
        std::string magic, isa, codepath;
        unsigned wave_size, nsgpr, nvgpr;
        uint64_t pc, exec, limit;
        uint32_t mode;
        in >> magic >> isa >> std::hex >> pc >> exec >> mode >> std::dec
           >> wave_size >> nsgpr >> nvgpr >> limit >> std::quoted(codepath);
        require(bool(in) && (magic=="P8ISA1" || magic=="P8ISA2"), "manifest header");
        require(isa=="rdna1" || isa=="rdna2", "ISA");
        require((wave_size==32 || wave_size==64) && nsgpr && nsgpr<=106 &&
            nvgpr && nvgpr<=256 && exec && (wave_size==64 || !(exec>>32)) &&
            limit && limit<=10000000, "wave configuration");
        auto code=read_file(codepath);
        require(!(pc&3) && !(code.size()&3) && pc<=UINT64_MAX-code.size(), "code range");
        // The manifest must explicitly specify the launch registers and every
        // input byte. No data is reconstructed from the native output.
        std::vector<uint32_t> sgpr(nsgpr), vgpr(nvgpr*wave_size);
        for (auto &v:sgpr) in >> std::hex >> v;
        for (auto &v:vgpr) in >> std::hex >> v;
        unsigned nmem; in >> std::dec >> nmem;
        require(bool(in) && nmem<=65536, "memory segment count");
        auto memory=std::make_unique<amdgpu::GpuMemory>("p8_memory");
        auto cache=std::make_unique<amdgpu::L2Cache>("p8_l2");
        cache->set_backing_memory(memory.get());
        std::vector<Range> ranges;
        for (unsigned i=0;i<nmem;++i) {
            uint64_t addr; std::string path;
            in >> std::hex >> addr >> std::quoted(path);
            require(bool(in), "memory segment");
            auto data=read_file(path);
            require(addr<=UINT64_MAX-data.size(), "memory overflow");
            ranges.push_back({addr,data.size()});
            memory->load_image(data.data(),data.size(),addr);
        }
        std::sort(ranges.begin(),ranges.end(),[](Range a,Range b){return a.addr<b.addr;});
        for (size_t i=1;i<ranges.size();++i)
            require(ranges[i-1].addr+ranges[i-1].size<=ranges[i].addr,"overlapping input segments");
        for (auto range:ranges)
            require(range.addr+range.size<=pc || pc+code.size()<=range.addr,"code/data overlap");
        memory->load_image(code.data(),code.size(),pc);
        unsigned nout; in >> std::dec >> nout;
        require(bool(in) && nout<=4096, "output range count");
        std::vector<Range> outputs;
        for(unsigned i=0;i<nout;++i) {
            Range x{}; in >> std::hex >> x.addr >> x.size;
            require(bool(in) && x.size<=64*1024*1024 && covers(ranges,x.addr,x.size),"output range absent");
            outputs.push_back(x);
        }
        // P8ISA2 optionally initializes the bounded interpolation LDS window.
        // The adapter must supply the actual coefficients and validate every
        // referenced attribute/primitive; missing data is not a zero input.
        std::vector<uint8_t> lds;
        if(magic=="P8ISA2") {
            std::string path;
            in >> std::quoted(path);
            require(bool(in),"missing LDS payload declaration");
            if(path!="-") {
                lds=read_file(path);
                require(lds.size()<=64*1024 && !(lds.size()&3),"LDS payload size");
            }
        }
        std::string trailing;
        require(!(in>>trailing),"trailing manifest fields");
        amdgpu::ComputeUnitCore::Config cfg{};
        cfg.arch=isa=="rdna1"?IsaFamily::Rdna1:IsaFamily::Rdna2;
        cfg.num_wf_slots=2; cfg.sgprs_per_wf=106; cfg.vgprs_per_wf=256; cfg.lds_size_kb=64;
        util::StringDiagnostic diagnostic;
        auto cu=amdgpu::ComputeUnitCore::create("p8_cu",cfg,memory.get(),cache.get(),
            simdojo::ExecMode::FUNCTIONAL,diagnostic.emitter());
        if(!cu) throw std::runtime_error(diagnostic.message());
        std::vector<amdgpu::ShaderExport> graphics_exports;
        // GFX10/11 use distinct export paths. RDNA1 EXP invokes the CU
        // graphics callback, not Wavefront::shader_exports(). Bind a real
        // destination and preserve every enabled channel and lane.
        cu->set_shader_export_handler([&](const amdgpu::Wavefront&,const amdgpu::ShaderExport& packet){
            require(graphics_exports.size()<4096,"too many graphics exports");
            graphics_exports.push_back(packet);
        });
        auto *wf=cu->dispatch_wf(0,pc,106,256,wave_size);
        require(wf!=nullptr,"wave allocation");
        wf->set_exec(exec);
        wf->set_mode_raw(mode);
        // Still single-wave. Interpolation input is explicit, not a general
        // implementation of compute workgroup LDS synchronization.
        wf->set_lds_base(0);
        wf->set_lds_size(static_cast<uint32_t>(lds.size()));
        if(!lds.empty())cu->lds().write(uint32_t{0},lds.data(),static_cast<uint32_t>(lds.size()));
        for(unsigned i=0;i<nsgpr;++i) cu->write_sgpr(wf->sgpr_alloc().base+i,sgpr[i]);
        for(unsigned i=0;i<nvgpr;++i)
            for(unsigned lane=0;lane<wave_size;++lane)
                cu->write_vgpr(wf->vgpr_alloc().base+i,lane,vgpr[i*wave_size+lane]);
        bool fault=false;
        uint64_t accesses=0;
        cu->set_debug_active(true);
        cu->set_watchpoint_handler([&](amdgpu::Wavefront&,uint64_t addr,uint32_t bytes,bool,bool){
            ++accesses;
            if(!covers(ranges,addr,bytes)){
                std::cerr<<"UNCAPTURED_ACCESS "<<std::hex<<addr<<" "<<bytes<<"\n";
                fault=true;
            }
            return false;
        });
        cu->set_illegal_inst_handler([&](amdgpu::Wavefront&){fault=true;return false;});
        cu->set_execution_failure_handler([&](amdgpu::Wavefront&){fault=true;return false;});
        cu->set_memory_violation_handler([&](amdgpu::Wavefront&,uint64_t,bool,uint64_t){fault=true;return false;});
        std::vector<amdgpu::ShaderExportRow> exports;
        bool end_seen=false;
        uint64_t steps=0;
        while(!wf->is_halted() && steps<limit && !fault) {
            require(wf->pc>=pc && wf->pc<=pc+code.size()-4,"PC outside captured code");
            const uint32_t opcode=memory->read32(wf->pc);
            if(opcode==0xbf810000u)end_seen=true;
            cu->step();
            ++steps;
            if(!wf->is_halted()) {
                if(wf->instruction_execution_failed())fault=true;
                auto rows=wf->shader_exports();
                exports.assign(rows.begin(),rows.end());
            }
        }
        require(!fault,"execution or operand coverage failure");
        require(wf->is_halted() && end_seen,"wave did not terminate at s_endpgm");
        require(cu->isa_diagnostic_count()==0 && cu->memory_wait_diagnostic_count()==0,
            "ISA or memory wait diagnostic");
        std::cout<<"P8ISA_RESULT1 "<<std::dec<<steps<<" "<<accesses<<"\n";
        for(auto range:outputs) {
            std::cout<<"MEM "<<std::hex<<range.addr<<" ";
            for(uint64_t i=0;i<range.size;++i)
                std::cout<<std::setw(2)<<std::setfill('0')<<unsigned(memory->read8(range.addr+i));
            std::cout<<"\n";
        }
        for(const auto& row:graphics_exports) {
            std::cout<<"GFXEXP "<<std::hex<<unsigned(row.target)<<" "<<unsigned(row.enabled)
                     <<" "<<unsigned(row.compressed)<<" "<<unsigned(row.valid_mask)<<" "
                     <<unsigned(row.done)<<" "<<row.lanes;
            for(unsigned lane=0;lane<wave_size;++lane)
                if((row.lanes>>lane)&1)
                    for(auto value:row.values[lane])std::cout<<" "<<value;
            std::cout<<"\n";
        }
        for(auto row:exports) {
            std::cout<<"EXP "<<std::hex<<unsigned(row.target)<<" "<<unsigned(row.row)<<" "
                <<unsigned(row.components)<<" "<<unsigned(row.done)<<" "<<row.valid_lanes;
            for(unsigned lane=0;lane<wave_size;++lane)
                if((row.valid_lanes>>lane)&1)
                    for(auto value:row.values[lane])std::cout<<" "<<value;
            std::cout<<"\n";
        }
        return 0;
    } catch(const std::exception &e) {
        std::cerr<<"P8ISA_INCOMPLETE "<<e.what()<<"\n";
        return 2;
    }
}
