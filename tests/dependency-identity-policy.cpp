// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only native dependency identity and mounted-evidence policy.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>
namespace fs=std::filesystem;
static std::string read(const fs::path& path) {
    std::ifstream file(path,std::ios::binary);
    if(!file)throw std::runtime_error("Cannot read "+path.string());
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
static bool has(const std::string& body,const std::string& token) {
    return body.find(token)!=std::string::npos;
}
int main(int argc,char** argv) {
    try{
        if(argc>2)throw std::runtime_error("Usage: dependency-identity-policy [REPOSITORY_ROOT]");
        const fs::path root=argc==2?fs::path(argv[1]):fs::path(".");
        const fs::path workflows=root/".github/workflows";
        if(!fs::is_directory(workflows))throw std::runtime_error("Missing GitHub workflow directory");
        const std::regex action(R"(^\s*uses:\s*([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)(/[^@\s#]+)?@([^\s#]+))");
        const std::regex immutable(R"(^[0-9a-f]{40}$)");
        std::vector<fs::path> paths;
        for(const auto& item:fs::directory_iterator(workflows))
            if(item.is_regular_file()&&item.path().extension()==".yml")paths.push_back(item.path());
        std::sort(paths.begin(),paths.end());
        std::vector<std::string> failures;
        for(const auto& path:paths){
            std::ifstream stream(path);
            if(!stream)throw std::runtime_error("Cannot read "+path.string());
            std::string line;
            std::size_t number=0;
            while(std::getline(stream,line)){
                ++number;
                std::smatch match;
                if(std::regex_search(line,match,action)&&
                   !std::regex_match(match[3].str(),immutable))
                    failures.push_back(path.string()+":"+std::to_string(number)+
                        ": remote action is not pinned to an immutable commit SHA");
            }
            if(stream.bad())throw std::runtime_error("Failed to read "+path.string());
        }
        const std::string cmake=read(root/"CMakeLists.txt");
        const auto start=cmake.find("FetchContent_Declare(infilfs_lz4");
        const auto end=start==std::string::npos?std::string::npos:
            cmake.find(")\nFetchContent_MakeAvailable",start);
        if(start==std::string::npos||end==std::string::npos){
            failures.push_back("CMakeLists.txt: retained LZ4 source block missing");
        }else{
            const auto lz4=cmake.substr(start,end-start);
            if(!has(lz4,"GIT_TAG ebb370ca83af193212df4dcbadcc5d87bc0de2f0 # v1.10.0"))
                failures.push_back("CMakeLists.txt: retained LZ4 source not pinned to qualified commit");
            if(!has(lz4,"GIT_SHALLOW FALSE"))
                failures.push_back("CMakeLists.txt: exact LZ4 arbitrary-commit fetch must be non-shallow");
        }
        const auto kernel=read(workflows/"kernel-module.yml");
        if(has(kernel,"load/mount test will be skipped")||
           has(kernel,R"POL(printf 'runtime_kdir=\n')POL"))
            failures.push_back("kernel-module.yml: mounted qualification has skip-success fallback");
        if(!has(kernel,"mounted native qualification cannot proceed"))
            failures.push_back("kernel-module.yml: missing explicit fail-closed running-kernel gate");
        if(has(kernel,"if: steps.headers.outputs.runtime_kdir"))
            failures.push_back("kernel-module.yml: obsolete mounted-test skip guards remain");
        if(has(kernel,"runtime load/mount skipped"))
            failures.push_back("kernel-module.yml: obsolete compile-only success reporting remains");
        for(const auto& failure:failures)
            std::cerr<<"dependency identity policy: FAIL: "<<failure<<'\n';
        if(!failures.empty())return 1;
        std::cout<<"Dependency identity and mounted-evidence policy: PASS\n";
        return 0;
    }catch(const std::exception& e){
        std::cerr<<"dependency identity policy: FAIL: "<<e.what()<<'\n';return 1;
    }
}
