#include "ssne/custom_level_mount.h"
#include "gc/bytes.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

unsigned checks=0;
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
void put(std::vector<uint8_t>& b,size_t p,uint32_t v){for(unsigned n=0;n<4;++n)b.at(p+n)=uint8_t(v>>(24-n*8));}
void run(const std::vector<uint8_t>& old) {
    const auto count=gc::be32(old.data()+8);
    uint64_t end=0;
    for(unsigned n=1;n<count;++n)if(!old[n*12])
        end=std::max(end,uint64_t(gc::be32(old.data()+n*12+4))+gc::be32(old.data()+n*12+8));
    const auto offset=uint32_t((end+31)&~uint64_t(31));
    const std::string alias="Custom_0123456789abcdef01234567";
    std::vector<uint8_t> next;uint32_t index=0;std::string error;
    check(ssne::append_custom_level_fst(old,alias,offset,640,next,index,error),"Append valid map to running FST");
    check(index==count && gc::be32(next.data()+8)==count+1,"Append preserves every prior entry index");
    for(unsigned n=0;n<count*12;++n)if(n<8 || n>=12)
        check(old[n]==next[n],"Prior FST records and extents are byte identical");
    check(std::equal(old.begin()+count*12,old.end(),next.begin()+(count+1)*12),"Existing string bytes retain all name offsets");
    check(gc::be32(next.data()+count*12+4)==offset && gc::be32(next.data()+count*12+8)==640,"New entry maps only the immutable appended extent");
    std::vector<uint8_t> second;
    check(ssne::append_custom_level_fst(next,"Custom_abcdef0123456789abcdef01",offset+640,128,second,index,error) && index==count+1,
          "Repeated activation appends without shifting first received map");
    check(!ssne::append_custom_level_fst(next,alias,offset+640,128,second,index,error),"Duplicate alias private record is rejected");
    check(!ssne::append_custom_level_fst(old,"../outside",offset,128,second,index,error),"Alias traversal rejected");
    if(offset)check(!ssne::append_custom_level_fst(old,alias,0,128,second,index,error),"Existing file overlap rejected");
    auto bad=old;put(bad,8,100001);
    check(!ssne::append_custom_level_fst(bad,alias,offset,128,second,index,error),"Unbounded FST count rejected");
    bad=old;put(bad,12,0x01000001);put(bad,16,0);put(bad,20,count+1);
    check(!ssne::append_custom_level_fst(bad,alias,offset,128,second,index,error),"Crossing directory extent rejected");
}
int main(int argc,char** argv){try{
    std::vector<uint8_t> fst(4*12);
    put(fst,0,0x01000000);put(fst,8,4);
    put(fst,12,0x01000001);put(fst,20,3);
    put(fst,24,8);put(fst,28,0x10000);put(fst,32,640);
    put(fst,36,17);put(fst,40,0x11000);put(fst,44,128);
    const char names[]="\0Levels\0base.ssw\0after.bin\0";
    fst.insert(fst.end(),names,names+sizeof(names)-1);run(fst);
    if(argc>1){std::ifstream in(argv[1],std::ios::binary);std::vector<uint8_t> real{std::istreambuf_iterator<char>(in),{}};check(!real.empty(),"Read owned game FST");run(real);}
    std::cout<<"PASS custom_level_fst_checks="<<checks<<"\n";
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
