// Draw-owned bytes. Normal resize zeroes new bytes, including alignment gaps.
// Only a producer that writes EVERY new byte may request overwrite growth.
#pragma once
#include <vector>
#include <cstdint>
#include <memory>
#include <cstring>
#include <utility>
#include <algorithm>
#include <initializer_list>
namespace ssne::gx {
template<class T> struct OverwriteAllocator {
    using value_type=T;
    OverwriteAllocator()=default;
    template<class U> OverwriteAllocator(const OverwriteAllocator<U>&)noexcept{}
    T* allocate(size_t n){return std::allocator<T>{}.allocate(n);}
    void deallocate(T* p,size_t n)noexcept{std::allocator<T>{}.deallocate(p,n);}
    template<class U,class... Args> void construct(U* p,Args&&... args){
        if constexpr(sizeof...(Args)==0)::new(static_cast<void*>(p))U;
        else ::new(static_cast<void*>(p))U(std::forward<Args>(args)...);
    }
    template<class U> bool operator==(const OverwriteAllocator<U>&)const noexcept{return true;}
};
class VertexBytes {
    std::vector<uint8_t,OverwriteAllocator<uint8_t>> bytes_;
public:
    using value_type=uint8_t;
    VertexBytes()=default;
    VertexBytes(const VertexBytes&)=default;
    VertexBytes(VertexBytes&&)noexcept=default;
    VertexBytes& operator=(const VertexBytes&)=default;
    VertexBytes& operator=(VertexBytes&&)noexcept=default;
    VertexBytes& operator=(std::initializer_list<uint8_t> v){bytes_.assign(v);return *this;}
    VertexBytes& operator=(const std::vector<uint8_t>& v){bytes_.assign(v.begin(),v.end());return *this;}
    size_t size()const noexcept{return bytes_.size();}
    size_t capacity()const noexcept{return bytes_.capacity();}
    bool empty()const noexcept{return bytes_.empty();}
    uint8_t* data()noexcept{return bytes_.data();}
    const uint8_t* data()const noexcept{return bytes_.data();}
    auto begin()noexcept{return bytes_.begin();}auto end()noexcept{return bytes_.end();}
    auto begin()const noexcept{return bytes_.begin();}auto end()const noexcept{return bytes_.end();}
    auto& operator[](size_t n)noexcept{return bytes_[n];}const auto& operator[](size_t n)const noexcept{return bytes_[n];}
    void clear()noexcept{bytes_.clear();}
    void reserve(size_t n){bytes_.reserve(n);}
    void resize(size_t n){const auto old=bytes_.size();bytes_.resize(n);if(n>old)std::memset(bytes_.data()+old,0,n-old);}
    void resize(size_t n,uint8_t value){bytes_.resize(n,value);}
    void resize_for_overwrite(size_t n){bytes_.resize(n);}
    template<class It> void assign(It first,It last){bytes_.assign(first,last);}
    void assign(size_t n,uint8_t value){bytes_.assign(n,value);}
    void swap(VertexBytes& other)noexcept{bytes_.swap(other.bytes_);}
    bool operator==(const VertexBytes& other)const noexcept{return size()==other.size() && (empty() || !std::memcmp(data(),other.data(),size()));}
    template<class A> bool operator==(const std::vector<uint8_t,A>& other)const noexcept{return size()==other.size() && (empty() || !std::memcmp(data(),other.data(),size()));}
};
static_assert(sizeof(VertexBytes)==sizeof(std::vector<uint8_t>));
}
