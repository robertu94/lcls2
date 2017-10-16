#ifndef Pds_IovPool_hh
#define Pds_IovPool_hh

namespace Pds {

  class IovEntry : private struct iovec
  {
  public:
    IovEntry(void* payload, size_t size)
    {
      iov_base = payload;
      iov_size = size;
    }
    ~IovEntry();
  public:
    void* operator new(size_t, IovPool* pool) { return pool->alloc(); }
    void  operator delete(void*)              { IovPool::free();      }
  public:
    void*  payload() const { return iov_base; }
    size_t size()    const { return iov_len;  }
  public:
    void*  next()    const { return (char*)iov_base + iov_len; }
    void   extend(size_t size) { iov_len += size; }
  };

  class IovPool
  {
  public:
    IovPool(unsigned count) :
      _count(count),
      _index(0),
      _iovs(new IovEntry[count])
    {
    }
    ~IovPool()
    {
      delete [] _iovs;
      _index = 0;
      _count = 0;
    }
  public:
    void* alloc()                       // Allocate one at a time
    {
      return _index < count ? &_iovs[++_index] : NULL;
    }
    static void free() {}               // No deallocation
    void clear() { _index = 0; }
  public:
    IovEntry* iovs()    const { return _iovs;  }
    size_t    iovSize() const { return _index; }
  public:
    IovEntry& last()    const { return _iovs[_index]; }
  private:
    unsigned  _count;
    unsigned  _index;
    IovEntry* _iovs;
  };
}
