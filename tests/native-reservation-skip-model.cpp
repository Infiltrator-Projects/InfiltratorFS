// SPDX-License-Identifier: GPL-3.0-or-later
// Exhaustive native allocation-run skip proof for stable two-pass bitmaps.
// This checks search identity; real concurrent kernel allocation is tested
// separately in the filesystem integration suite.
#include <cstdint>
#include <iostream>
#include <stdexcept>

static int linear(std::uint32_t occupied,int count,int hint,int total){
    for(int pass=0;pass<2;pass++){
        const int lo=pass==0?hint:0;
        const int hi=pass==0?total:hint;
        for(int candidate=lo;candidate+count<=hi;candidate++){
            bool busy=false;
            for(int i=candidate;i<candidate+count;i++)
                if((occupied>>i)&1U){busy=true;break;}
            if(!busy)return candidate;
        }
    }
    return -1;
}
static int skip_blocker(std::uint32_t occupied,int count,int hint,int total){
    for(int pass=0;pass<2;pass++){
        const int lo=pass==0?hint:0;
        const int hi=pass==0?total:hint;
        int candidate=lo;
        while(candidate+count<=hi){
            int blocker=candidate+count;
            for(int i=candidate;i<candidate+count;i++)
                if((occupied>>i)&1U){blocker=i;break;}
            if(blocker<candidate+count){candidate=blocker+1;continue;}
            return candidate;
        }
    }
    return -1;
}
int main(){
    try{
        std::uint64_t count_cases=0;
        for(int n=1;n<12;n++)
            for(std::uint32_t bitmap=0;bitmap<(1U<<n);bitmap++)
                for(int count=1;count<=n;count++)
                    for(int hint=0;hint<=n;hint++){
                        const int expected=linear(bitmap,count,hint,n);
                        const int found=skip_blocker(bitmap,count,hint,n);
                        if(expected!=found)
                            throw std::runtime_error("reservation search mismatch n="+
                                std::to_string(n)+" bitmap="+std::to_string(bitmap)+
                                " count="+std::to_string(count)+
                                " hint="+std::to_string(hint)+
                                " expected="+std::to_string(expected)+
                                " actual="+std::to_string(found));
                        ++count_cases;
                    }
        std::cout<<"Native reservation skip model: PASS ("<<count_cases<<" states)\n";
        return 0;
    }catch(const std::exception& error){
        std::cerr<<error.what()<<'\n';return 1;
    }
}
