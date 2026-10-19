#pragma once
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#define AVS_WINDOWS 1
#define AVS_FORCEINLINE __forceinline
using SFLOAT=double;
inline void* avs_malloc(size_t bytes,size_t) {auto* p=std::calloc(1,bytes);if(!p)throw std::bad_alloc();return p;}
inline void avs_free(void* p) {std::free(p);}
// The historical constructors use raw arrays. Until construction succeeds,
// keep each allocation owned here: a derived destructor is not called when
// its constructor throws. Successful objects retain their original cleanup.
class SsrcAllocations {
    std::vector<void*> blocks_;
public:
    explicit SsrcAllocations(size_t count) {blocks_.reserve(count);}
    SsrcAllocations(const SsrcAllocations&)=delete;
    SsrcAllocations& operator=(const SsrcAllocations&)=delete;
    ~SsrcAllocations() {for(auto* p:blocks_)avs_free(p);}
    void* allocate(size_t bytes,size_t alignment) {
        auto* p=avs_malloc(bytes,alignment);
        try {blocks_.push_back(p);}catch(...){avs_free(p);throw;}
        return p;
    }
    void commit() noexcept {blocks_.clear();}
};
struct mem_block {enum {ALLOC_FAST_DONTGODOWN};};
template<class T> class mem_block_t {
    std::vector<T> data_;
public:
    void set_mem_logic(int){}
    void check_size(int size){if(size<0)throw std::bad_alloc();if(data_.size()<static_cast<size_t>(size))data_.resize(size);}
    operator T*(){return data_.data();}
    T* operator+(int index){return data_.data()+index;}
};
template<class T> struct mem_ops {
    static void copy(T* to,const T* from,size_t count){if(count)std::memcpy(to,from,count*sizeof(T));}
    static void move(T* to,const T* from,size_t count){if(count)std::memmove(to,from,count*sizeof(T));}
};
