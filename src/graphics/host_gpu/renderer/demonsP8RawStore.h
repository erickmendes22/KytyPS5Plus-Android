#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Libs::Graphics::DemonsP8Reference {
// Every GPU observation has a unique transfer ID. Only the persisted
// identical payload may be shared; a hash collision is never equality.
class RawStore {
public:
    struct Saved { std::string file; const char* reason=nullptr; bool reused=false; };
    RawStore(uint64_t storage_budget,uint64_t copy_budget)
        : storage_budget_(storage_budget),copy_budget_(copy_budget) {}
    uint64_t ReserveCopy(uint64_t bytes) {
        std::scoped_lock lock(mutex_);
        if(!bytes||bytes>copy_budget_-copied_)return 0;
        copied_+=bytes;return ++serial_;
    }
    Saved Save(const std::filesystem::path& folder,uint64_t id,std::span<const uint8_t> data) {
        if(!id||data.empty())return {"","invalid_payload",false};
        uint64_t hash=14695981039346656037ull;
        for(const auto b:data){hash^=b;hash*=1099511628211ull;}
        std::scoped_lock lock(mutex_);
        auto& candidates=files_[{data.size(),hash}];
        for(const auto& filename:candidates)
            if(Equals(folder/filename,data))return {filename,nullptr,true};
        if(data.size()>storage_budget_-stored_)return {"","raw_storage_budget_exceeded",false};
        const auto filename="P8_FULL_"+std::to_string(id)+".bin";
        std::ofstream out(folder/filename,std::ios::binary|std::ios::trunc);
        if(!out)return {"","file_open_failed",false};
        out.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));
        out.close();
        if(!out)return {"","file_write_failed",false};
        stored_+=data.size();
        candidates.push_back(filename);
        return {filename,nullptr,false};
    }
private:
    static bool Equals(const std::filesystem::path& file,std::span<const uint8_t> data) {
        std::ifstream in(file,std::ios::binary);
        if(!in)return false;
        std::array<char,65536> buffer{};
        for(size_t at=0;at<data.size();){
            const auto count=std::min(buffer.size(),data.size()-at);
            in.read(buffer.data(),static_cast<std::streamsize>(count));
            if(in.gcount()!=static_cast<std::streamsize>(count)||
               std::memcmp(buffer.data(),data.data()+at,count)!=0)return false;
            at+=count;
        }
        return in.peek()==std::char_traits<char>::eof();
    }
    std::mutex mutex_;
    uint64_t storage_budget_,copy_budget_,stored_=0,copied_=0,serial_=0;
    std::map<std::pair<uint64_t,uint64_t>,std::vector<std::string>> files_;
};
}
