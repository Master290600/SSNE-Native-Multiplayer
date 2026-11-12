#pragma once
#include <map>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <cstring>
#include <bit>
#include <algorithm>
#include <stdexcept>
#include <utility>
// The owning tree retains std::map iterator/reference stability. The side
// index caches names only; property values always live in their original node.
namespace port::coop {
template<class T> class IndexedPropertyMap {
    using Tree=std::map<std::string,T>;
public:
    using key_type=std::string;using mapped_type=T;using value_type=typename Tree::value_type;
    using iterator=typename Tree::iterator;using const_iterator=typename Tree::const_iterator;using size_type=typename Tree::size_type;
private:
    using Prefix=std::array<uint64_t,2>;
    struct Entry {Prefix prefix;iterator node;};
    Tree nodes_;std::vector<Entry> index_;
    static constexpr uint64_t swap_bytes(uint64_t x)noexcept {
        x=((x&0x00FF00FF00FF00FFull)<<8)|((x>>8)&0x00FF00FF00FF00FFull);
        x=((x&0x0000FFFF0000FFFFull)<<16)|((x>>16)&0x0000FFFF0000FFFFull);
        return (x<<32)|(x>>32);
    }
    static Prefix prefix(std::string_view key)noexcept {
        Prefix p{};const auto first=std::min<size_t>(key.size(),8);if(first)std::memcpy(&p[0],key.data(),first);
        if(key.size()>8)std::memcpy(&p[1],key.data()+8,std::min<size_t>(key.size()-8,8));
        if constexpr(std::endian::native==std::endian::little){p[0]=swap_bytes(p[0]);p[1]=swap_bytes(p[1]);}return p;
    }
    auto lower(std::string_view key,const Prefix& p)const {
        return std::lower_bound(index_.begin(),index_.end(),key,[&](const Entry& e,std::string_view value){return e.prefix!=p?e.prefix<p:std::string_view(e.node->first)<value;});
    }
    void add_index(iterator node){const auto p=prefix(node->first);auto at=lower(node->first,p);try{index_.insert(at,Entry{p,node});}catch(...){nodes_.erase(node);throw;}}
    void rebuild(){index_.reserve(nodes_.size());for(auto it=nodes_.begin();it!=nodes_.end();++it)index_.push_back({prefix(it->first),it});}
public:
    IndexedPropertyMap()=default;
    IndexedPropertyMap(std::initializer_list<value_type> values){for(const auto& v:values)emplace(v);}
    IndexedPropertyMap(const IndexedPropertyMap& other):nodes_(other.nodes_){rebuild();}
    IndexedPropertyMap& operator=(const IndexedPropertyMap& other){if(this!=&other){IndexedPropertyMap temp(other);swap(temp);}return *this;}
    IndexedPropertyMap(IndexedPropertyMap&& other)noexcept:nodes_(std::move(other.nodes_)),index_(std::move(other.index_)){other.clear();}
    IndexedPropertyMap& operator=(IndexedPropertyMap&& other)noexcept{if(this!=&other){nodes_=std::move(other.nodes_);index_=std::move(other.index_);other.clear();}return *this;}
    IndexedPropertyMap& operator=(std::initializer_list<value_type> values){IndexedPropertyMap temp(values);swap(temp);return *this;}
    void swap(IndexedPropertyMap& other)noexcept{nodes_.swap(other.nodes_);index_.swap(other.index_);}
    bool empty()const noexcept{return nodes_.empty();}size_type size()const noexcept{return nodes_.size();}
    iterator begin()noexcept{return nodes_.begin();}const_iterator begin()const noexcept{return nodes_.begin();}const_iterator cbegin()const noexcept{return nodes_.cbegin();}
    iterator end()noexcept{return nodes_.end();}const_iterator end()const noexcept{return nodes_.end();}const_iterator cend()const noexcept{return nodes_.cend();}
    auto rbegin()noexcept{return nodes_.rbegin();}auto rbegin()const noexcept{return nodes_.rbegin();}auto rend()noexcept{return nodes_.rend();}auto rend()const noexcept{return nodes_.rend();}
    iterator find(std::string_view key){const auto p=prefix(key);const auto at=lower(key,p);return at!=index_.end()&&at->prefix==p&&at->node->first==key?at->node:nodes_.end();}
    const_iterator find(std::string_view key)const{const auto p=prefix(key);const auto at=lower(key,p);return at!=index_.end()&&at->prefix==p&&at->node->first==key?const_iterator(at->node):nodes_.end();}
    bool contains(std::string_view key)const{return find(key)!=end();}size_type count(std::string_view key)const{return contains(key)?1:0;}
    T& at(std::string_view key){const auto it=find(key);if(it==end())throw std::out_of_range("map::at: key not found");return it->second;}
    const T& at(std::string_view key)const{const auto it=find(key);if(it==end())throw std::out_of_range("map::at: key not found");return it->second;}
    template<class... Args> std::pair<iterator,bool> emplace(Args&&... args){auto result=nodes_.emplace(std::forward<Args>(args)...);if(result.second)add_index(result.first);return result;}
    template<class... Args> iterator emplace_hint(const_iterator hint,Args&&... args){const auto old=nodes_.size();const auto it=nodes_.emplace_hint(hint,std::forward<Args>(args)...);if(nodes_.size()!=old)add_index(it);return it;}
    template<class... Args> std::pair<iterator,bool> try_emplace(Args&&... args){auto result=nodes_.try_emplace(std::forward<Args>(args)...);if(result.second)add_index(result.first);return result;}
    T& operator[](const std::string& key){return try_emplace(key).first->second;}T& operator[](std::string&& key){return try_emplace(std::move(key)).first->second;}
    template<class Key,class Value> std::pair<iterator,bool> insert_or_assign(Key&& key,Value&& value){auto result=nodes_.insert_or_assign(std::forward<Key>(key),std::forward<Value>(value));if(result.second)add_index(result.first);return result;}
    std::pair<iterator,bool> insert(const value_type& value){return emplace(value);}std::pair<iterator,bool> insert(value_type&& value){return emplace(std::move(value));}
    template<class It> void insert(It first,It last){for(;first!=last;++first)emplace(*first);}
    iterator erase(const_iterator at){const auto p=prefix(at->first);const auto found=lower(at->first,p);index_.erase(found);return nodes_.erase(at);}
    iterator erase(iterator at){return erase(const_iterator(at));}
    size_type erase(std::string_view key){const auto it=find(key);if(it==end())return 0;erase(it);return 1;}
    iterator erase(const_iterator first,const_iterator last){while(first!=last)first=erase(first);return first==nodes_.end()?nodes_.end():nodes_.find(first->first);}
    void clear()noexcept{index_.clear();nodes_.clear();}
    bool operator==(const IndexedPropertyMap& other)const{return nodes_==other.nodes_;}
};

} // namespace port::coop
