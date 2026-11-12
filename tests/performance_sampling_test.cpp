#include "ssne/performance_detail.h"
#include <cassert>
#include <iostream>
#include <vector>

int main(){
    using ssne::perf::detail::select_sample;
    assert(!select_sample(1,0) && !select_sample(0,16));
    for(unsigned n:{1u,2u,3u,16u,256u}){
        std::vector<unsigned> offsets(n);
        for(unsigned block=0;block<n;++block){
            unsigned selected=0;
            for(unsigned offset=0;offset<n;++offset)
                if(select_sample(uint64_t(block)*n+offset+1,n)){++selected;++offsets[offset];}
            assert(selected==1);
        }
        for(auto seen:offsets)assert(seen==1);
    }
    // The old fixed sample always sees the same cost for these workloads.
    // Rotation must recover the actual average over a complete phase cycle.
    for(const std::vector<unsigned> costs: {std::vector<unsigned>{1,11}, {1,4,8,11}}){
        unsigned all=0,sampled=0,count=0;
        for(unsigned i=0;i<256;++i){all+=costs[i%costs.size()];
            if(select_sample(i+1,16)){sampled+=costs[i%costs.size()];++count;}}
        assert(count==16 && sampled*256==all*count);
    }
    std::cout<<"PASS sample budget, complete offset coverage, alternating and four-phase workload averages\n";
}
