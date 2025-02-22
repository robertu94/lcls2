#pragma once

#include <thread>
#include <vector>
#include <cstdint>
#include <map>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <string>
#include "spscqueue.hh"

#define PGP_MAX_LANES 8

namespace Pds {
    class EbDgram;
};

namespace Drp {

enum NamesIndex
{
   BASE         = 0,
   CHUNKINFO    = 252,
   STEPINFO     = 253,
   OFFSETINFO   = 254,
   RUNINFO      = 255,
};

struct Parameters
{
    Parameters() :
        partition(-1),
        detSegment(0),
        laneMask(0x1),
        loopbackPort(0),
        verbose(0)
    {
    }
    unsigned partition;
    unsigned nworkers;
    unsigned batchSize;
    unsigned detSegment;
    uint8_t laneMask;
    std::string alias;
    std::string detName;
    std::string device;
    std::string outputDir;
    std::string instrument;
    std::string detType;
    std::string serNo;
    std::string collectionHost;
    std::string prometheusDir;
    std::map<std::string,std::string> kwargs;
    uint32_t rogMask;
    int loopbackPort;
    unsigned verbose;
    size_t maxTrSize;
};

struct DmaBuffer
{
    int32_t size;
    uint32_t index;
};

struct PGPEvent
{
    DmaBuffer buffers[PGP_MAX_LANES];
    uint8_t mask = 0;
    unsigned pebbleIndex;
};

class Pebble
{
public:
    ~Pebble() {
        if (m_buffer) {
            delete m_buffer;
            m_buffer = nullptr;
        }
    }
    void create(unsigned nL1Buffers, size_t l1BufSize, unsigned nTrBuffers, size_t trBufSize);

    inline uint8_t* operator [] (unsigned index) {
        uint64_t offset = index*m_bufferSize;
        return &m_buffer[offset];
    }
    size_t size() const {return m_size;}
    size_t bufferSize() const {return m_bufferSize;}
private:
    size_t   m_size;
    size_t   m_bufferSize;
    uint8_t* m_buffer;
};

class MemPool
{
public:
    MemPool(Parameters& para);
    ~MemPool();
    Pebble pebble;
    std::vector<PGPEvent> pgpEvents;
    std::vector<Pds::EbDgram*> transitionDgrams;
    void** dmaBuffers;
    unsigned nDmaBuffers() const {return m_nDmaBuffers;}
    unsigned dmaSize() const {return m_dmaSize;}
    unsigned nbuffers() const {return m_nbuffers;}
    size_t bufferSize() const {return pebble.bufferSize();}
    int fd() const {return m_fd;}
    void shutdown();
    Pds::EbDgram* allocateTr();
    void freeTr(Pds::EbDgram* dgram) { m_transitionBuffers.push(dgram); }
    unsigned countDma();
    unsigned allocate();
    void freeDma(std::vector<uint32_t>& indices, unsigned count);
    void freePebble();
    const int64_t dmaInUse() const { return m_dmaAllocs.load(std::memory_order_relaxed) -
                                            m_dmaFrees.load(std::memory_order_relaxed); }
    const int64_t inUse() const { return m_allocs.load(std::memory_order_relaxed) -
                                         m_frees.load(std::memory_order_relaxed); }
    void resetCounters();
    int setMaskBytes(uint8_t laneMask, unsigned virtChan);
private:
    unsigned m_nDmaBuffers;
    unsigned m_nbuffers;
    unsigned m_dmaSize;
    int m_fd;
    bool m_setMaskBytesDone;
    SPSCQueue<void*> m_transitionBuffers;
    std::atomic<uint64_t> m_dmaAllocs;
    std::atomic<uint64_t> m_dmaFrees;
    std::atomic<uint64_t> m_allocs;
    std::atomic<uint64_t> m_frees;
    std::mutex m_lock;
    std::condition_variable m_condition;
};

}
