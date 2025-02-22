#include "PvaDetector.hh"

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <getopt.h>
#include <cassert>
#include <bitset>
#include <chrono>
#include <unistd.h>
#include <iostream>
#include <map>
#include <algorithm>
#include <limits>
#include <thread>
#include <Python.h>
#include "DataDriver.h"
#include "RunInfoDef.hh"
#include "xtcdata/xtc/Damage.hh"
#include "xtcdata/xtc/DescData.hh"
#include "xtcdata/xtc/ShapesData.hh"
#include "xtcdata/xtc/NamesLookup.hh"
#include "psdaq/service/kwargs.hh"
#include "psdaq/service/EbDgram.hh"
#include "psdaq/eb/TebContributor.hh"
#include "psalg/utils/SysLog.hh"
#include "psdaq/service/fast_monotonic_clock.hh"

#ifndef POSIX_TIME_AT_EPICS_EPOCH
#define POSIX_TIME_AT_EPICS_EPOCH 631152000u
#endif

using json = nlohmann::json;
using logging = psalg::SysLog;
using ms_t = std::chrono::milliseconds;
using ns_t = std::chrono::nanoseconds;

namespace Drp {

struct PvaParameters : public Parameters
{
    std::string pvName;
    std::string provider;
    std::string request;
    std::string field;
};

};

static const XtcData::TimeStamp TimeMax(std::numeric_limits<unsigned>::max(),
                                        std::numeric_limits<unsigned>::max());
static unsigned tsMatchDegree = 2;

//
//  Put all the ugliness of non-global timestamps here
//
static int _compare(const XtcData::TimeStamp& ts1,
                    const XtcData::TimeStamp& ts2) {
  int result = 0;

  if ((tsMatchDegree == 0) && !(ts2 == TimeMax))
      return result;

  if (tsMatchDegree == 1) {
    /*
    **  Mask out the fiducial
    */
    const uint64_t mask = 0xfffffffffffe0000ULL;
    uint64_t ts1m = ts1.value()&mask;
    uint64_t ts2m = ts2.value()&mask;

    const uint64_t delta = 10000000; // 10 ms!
    if      (ts1m > ts2m)  result = ts1m - ts2m > delta ?  1 : 0;
    else if (ts2m > ts1m)  result = ts2m - ts1m > delta ? -1 : 0;

    return result;
  }

  if      (ts1 > ts2) result = 1;
  else if (ts2 > ts1) result = -1;
  return result;
}

template<typename T>
static int64_t _deltaT(XtcData::TimeStamp& ts)
{
    auto now = std::chrono::system_clock::now();
    auto tns = std::chrono::seconds{ts.seconds() + POSIX_TIME_AT_EPICS_EPOCH}
             + std::chrono::nanoseconds{ts.nanoseconds()};
    std::chrono::system_clock::time_point tp{std::chrono::duration_cast<std::chrono::system_clock::duration>(tns)};
    return std::chrono::duration_cast<T>(now - tp).count();
}

namespace Drp {

static const XtcData::Name::DataType xtype[] = {
  XtcData::Name::UINT8 , // pvBoolean
  XtcData::Name::INT8  , // pvByte
  XtcData::Name::INT16 , // pvShort
  XtcData::Name::INT32 , // pvInt
  XtcData::Name::INT64 , // pvLong
  XtcData::Name::UINT8 , // pvUByte
  XtcData::Name::UINT16, // pvUShort
  XtcData::Name::UINT32, // pvUInt
  XtcData::Name::UINT64, // pvULong
  XtcData::Name::FLOAT , // pvFloat
  XtcData::Name::DOUBLE, // pvDouble
  XtcData::Name::CHARSTR, // pvString
};

PvaMonitor::PvaMonitor(const PvaParameters& para,
                       PvaDetector&         pvaDetector) :
    Pds_Epics::PvMonitorBase(para.pvName, para.provider, para.request, para.field),
    m_para                  (para),
    m_state                 (NotReady),
    m_pvaDetector           (pvaDetector),
    m_notifySocket          {&m_context, ZMQ_PUSH}
{
    // ZMQ socket for reporting errors
    m_notifySocket.connect({"tcp://" + m_para.collectionHost + ":" + std::to_string(CollectionApp::zmq_base_port + m_para.partition)});
}

void PvaMonitor::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_state = NotReady;
}

int PvaMonitor::getVarDef(XtcData::VarDef& varDef,
                          size_t&          payloadSize,
                          size_t           rankHack)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_state != Ready) {
        if (getParams(m_type, m_nelem, m_rank))  {
            const std::chrono::seconds tmo(3);
            m_condition.wait_for(lock, tmo, [this] { return m_state == Ready; });
            if (m_state != Ready) {
                auto msg("Failed to get parameters for PV "+ name());
                logging::error("getVardef: %s", msg.c_str());
                json jmsg = createAsyncErrMsg(m_para.alias, msg);
                m_notifySocket.send(jmsg.dump());
                return 1;
            }
        }
        else {
            m_state = Ready;
        }
    }

    size_t rank = m_rank;
    if (rankHack != size_t(-1))
    {
      rank = rankHack; // Revisit: Hack!
      logging::warning("%s rank overridden from %zu to %zu\n",
                       name().c_str(), m_rank, rank);
    }

    auto xtcType = xtype[m_type];
    varDef.NameVec.push_back(XtcData::Name(m_fieldName.c_str(), xtcType, rank));

    payloadSize = m_nelem * XtcData::Name::get_element_size(xtcType);

    return 0;
}

void PvaMonitor::onConnect()
{
    logging::info("PV %s connected", name().c_str());

    if (m_para.verbose) {
        if (printStructure())
            logging::error("onConnect: printStructure() failed");
    }
}

void PvaMonitor::onDisconnect()
{
    auto msg("PV "+ name() + " disconnected");
    logging::error("%s", msg.c_str());
    json jmsg = createAsyncErrMsg(m_para.alias, msg);
    m_notifySocket.send(jmsg.dump());
}

void PvaMonitor::updated()
{
    if (m_state == Ready) {
        int64_t seconds;
        int32_t nanoseconds;
        getTimestampEpics(seconds, nanoseconds);
        XtcData::TimeStamp timestamp(seconds, nanoseconds);
        //static XtcData::TimeStamp ts_prv(0, 0);
        //
        //if (timestamp > ts_prv) {
        m_pvaDetector.process(timestamp);
        //}
        //else {
        //  printf("Updated: ts didn't advance: new %016lx  prv %016lx  d %ld\n",
        //         timestamp.value(), ts_prv.value(), timestamp.to_ns() - ts_prv.to_ns());
        //}
        //ts_prv = timestamp;
        //
        //if (nanoseconds > 1000000000) {
        //  printf("Updated: nsec > 1 second: %016lx  s %ld  ns %d\n",
        //         timestamp.value(), seconds, nanoseconds);
        //}
        //
        //if ((timestamp.to_ns() > ts_prv.to_ns()) &&
        //    !(timestamp.value() > ts_prv.value())) {
        //  printf("Updated: > disagreement: ts to_ns %016lx  val %016lx\n"
        //         "                        prv to_ns %016lx  val %016lx\n",
        //         timestamp.to_ns(), timestamp.value(),
        //         ts_prv.to_ns(), ts_prv.value());
        //}
    }
    else {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!getParams(m_type, m_nelem, m_rank))  {
            m_state = Ready;
        }
        m_condition.notify_one();
    }
}


class Pgp : public PgpReader
{
public:
    Pgp(const Parameters& para, DrpBase& drp, Detector* det, const bool& running) :
        PgpReader(para, drp.pool, MAX_RET_CNT_C, 32),
        m_det(det), m_tebContributor(drp.tebContributor()), m_running(running),
        m_available(0), m_current(0), m_nDmaRet(0)
    {
        m_nodeId = drp.nodeId();
        if (drp.pool.setMaskBytes(para.laneMask, 0)) {
            logging::error("Failed to allocate lane/vc");
        }
    }

    Pds::EbDgram* next(uint32_t& evtIndex);
    const uint64_t nDmaRet() { return m_nDmaRet; }
private:
    Pds::EbDgram* _handle(uint32_t& evtIndex);
    Detector* m_det;
    Pds::Eb::TebContributor& m_tebContributor;
    static const int MAX_RET_CNT_C = 100;
    const bool& m_running;
    int32_t m_available;
    int32_t m_current;
    unsigned m_nodeId;
    uint64_t m_nDmaRet;
};

Pds::EbDgram* Pgp::_handle(uint32_t& pebbleIndex)
{
    const Pds::TimingHeader* timingHeader = handle(m_det, m_current);
    if (!timingHeader)  return nullptr;

    uint32_t pgpIndex = timingHeader->evtCounter & (m_pool.nDmaBuffers() - 1);
    PGPEvent* event = &m_pool.pgpEvents[pgpIndex];
    // No need to check for a broken event since we don't get indices for those

    // make new dgram in the pebble
    // It must be an EbDgram in order to be able to send it to the MEB
    pebbleIndex = event->pebbleIndex;
    XtcData::Src src = m_det->nodeId;
    Pds::EbDgram* dgram = new(m_pool.pebble[pebbleIndex]) Pds::EbDgram(*timingHeader, src, m_para.rogMask);

    // Collect indices of DMA buffers that can be recycled and reset event
    freeDma(event);

    return dgram;
}

Pds::EbDgram* Pgp::next(uint32_t& evtIndex)
{
    // get new buffers
    if (m_current == m_available) {
        m_current = 0;
        m_available = read();
        m_nDmaRet = m_available;
        if (m_available == 0) {
            return nullptr;
        }
    }

    Pds::EbDgram* dgram = _handle(evtIndex);
    m_current++;
    return dgram;
}


PvaDetector::PvaDetector(PvaParameters& para,
                         DrpBase&       drp) :
    XpmDetector     (&para, &drp.pool),
    m_para          (para),
    m_drp           (drp),
    m_evtQueue      (drp.pool.nbuffers()),
    m_pvQueue       (drp.pool.nbuffers()),
    m_bufferFreelist(m_pvQueue.size()),
    m_terminate     (false),
    m_running       (false),
    m_firstDimKw    (0)
{
    if (para.kwargs.find("firstdim") != para.kwargs.end())
        m_firstDimKw = std::stoul(para.kwargs["firstdim"]);
}

unsigned PvaDetector::connect(std::string& msg)
{
    try {
        m_pvaMonitor = std::make_shared<Drp::PvaMonitor>(m_para, *this);
    }
    catch(std::string& error) {
        logging::error("Failed to create PvaMonitor( %s ): %s",
                       m_para.pvName.c_str(), error.c_str());
        m_pvaMonitor.reset();
        msg = error;
        return 1;
    }

    return 0;
}

unsigned PvaDetector::disconnect()
{
    m_pvaMonitor.reset();
    return 0;
}

//std::string PvaDetector::sconfigure(const std::string& config_alias, XtcData::Xtc& xtc, const void* bufEnd)
unsigned PvaDetector::configure(const std::string& config_alias, XtcData::Xtc& xtc, const void* bufEnd)
{
    logging::info("PVA configure");

    if (XpmDetector::configure(config_alias, xtc, bufEnd))
        return 1;

    if (m_exporter)  m_exporter.reset();
    m_exporter = std::make_shared<Pds::MetricExporter>();
    if (m_drp.exposer()) {
        m_drp.exposer()->RegisterCollectable(m_exporter);
    }

    XtcData::Alg     rawAlg("raw", 1, 0, 0);
    XtcData::NamesId rawNamesId(nodeId, RawNamesIndex);
    XtcData::Names&  rawNames = *new(xtc, bufEnd) XtcData::Names(bufEnd,
                                                                 m_para.detName.c_str(), rawAlg,
                                                                 m_para.detType.c_str(), m_para.serNo.c_str(), rawNamesId);
    size_t           payloadSize;
    XtcData::VarDef  rawVarDef;
    size_t           rankHack = m_firstDimKw != 0 ? 2 : -1; // Revisit: Hack!
    if (m_pvaMonitor->getVarDef(rawVarDef, payloadSize, rankHack)) {
        return 1;
    }
    payloadSize += (sizeof(Pds::EbDgram)    + // An EbDgram is needed by the MEB
                    24                      + // Space needed by DescribedData
                    sizeof(XtcData::Shapes) + // Needed by DescribedData
                    sizeof(XtcData::Shape));  // Also need 1 of these per PV
    if (payloadSize > m_pool->pebble.bufferSize()) {
        logging::warning("Increase Pebble buffer size from %zd to %zd to avoid truncation of %s data",
                         m_pool->pebble.bufferSize(), payloadSize, m_pvaMonitor->name().c_str());
    }
    rawNames.add(xtc, bufEnd, rawVarDef);
    m_namesLookup[rawNamesId] = XtcData::NameIndex(rawNames);

    XtcData::Alg     infoAlg("epicsinfo", 1, 0, 0);
    XtcData::NamesId infoNamesId(nodeId, InfoNamesIndex);
    XtcData::Names&  infoNames = *new(xtc, bufEnd) XtcData::Names(bufEnd,
                                                                  "epicsinfo", infoAlg,
                                                                  "epicsinfo", "detnum1234", infoNamesId);
    XtcData::VarDef  infoVarDef;
    infoVarDef.NameVec.push_back({"keys", XtcData::Name::CHARSTR, 1});
    infoVarDef.NameVec.push_back({m_para.detName.c_str(), XtcData::Name::CHARSTR, 1});
    infoNames.add(xtc, bufEnd, infoVarDef);
    m_namesLookup[infoNamesId] = XtcData::NameIndex(infoNames);

    // add dictionary of information for each epics detname above.
    // first name is required to be "keys".  keys and values
    // are delimited by ",".
    XtcData::CreateData epicsInfo(xtc, bufEnd, m_namesLookup, infoNamesId);
    epicsInfo.set_string(0, "epicsname"); //  "," "provider");
    epicsInfo.set_string(1, (m_pvaMonitor->name()).c_str()); // + "," + provider).c_str());

    // (Re)initialize the queues
    m_pvQueue.startup();
    m_evtQueue.startup();
    m_bufferFreelist.startup();
    size_t bufSize = m_pool->pebble.bufferSize();
    m_buffer.resize(m_pvQueue.size() * bufSize);
    for(unsigned i = 0; i < m_pvQueue.size(); ++i) {
        m_bufferFreelist.push(reinterpret_cast<XtcData::Dgram*>(&m_buffer[i * bufSize]));
    }

    m_terminate.store(false, std::memory_order_release);

    m_workerThread = std::thread{&PvaDetector::_worker, this};

    //    return std::string();
    return 0;
}

unsigned PvaDetector::unconfigure()
{
    m_terminate.store(true, std::memory_order_release);
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    m_pvQueue.shutdown();
    m_evtQueue.shutdown();
    m_bufferFreelist.shutdown();
    if (m_pvaMonitor)  m_pvaMonitor->clear();   // Start afresh
    m_namesLookup.clear();   // erase all elements

    return 0;
}

void PvaDetector::event(XtcData::Dgram& dgram, const void* bufEnd, PGPEvent* pgpEvent)
{
    XtcData::NamesId namesId(nodeId, RawNamesIndex);
    XtcData::DescribedData desc(dgram.xtc, bufEnd, m_namesLookup, namesId);
    auto ohSize      = (sizeof(Pds::EbDgram)      +
                        dgram.xtc.sizeofPayload() + // = the '24' in configure()
                        sizeof(XtcData::Shapes)   +
                        sizeof(XtcData::Shape));
    auto payloadSize = m_pool->pebble.bufferSize() - ohSize; // Subtract overhead
    uint32_t shape[XtcData::MaxRank];
    auto     size    = m_pvaMonitor->getData(desc.data(), payloadSize, shape);
    if (size > payloadSize) {           // Check actual size vs available size
        logging::debug("Truncated: Pebble buffer of size %zu is too small for payload of size %zu for %s\n",
                       m_pool->pebble.bufferSize(), size + ohSize, m_pvaMonitor->name().c_str());
        dgram.xtc.damage.increase(XtcData::Damage::Truncated);
        size = payloadSize;
    }

    desc.set_data_length(size);

    if (m_pvaMonitor->rank() > 0) {
        if (m_firstDimKw != 0) {            // Revisit: Hack!
            shape[1] = shape[0] / m_firstDimKw;
            shape[0] = m_firstDimKw;
        }
        desc.set_array_shape(0, shape);
    }

    //size_t sz = (sizeof(dgram) + dgram.xtc.sizeofPayload()) >> 2;
    //uint32_t* payload = (uint32_t*)dgram.xtc.payload();
    //printf("sz = %zd, size = %zd, extent = %d, szofPyld = %d, pyldIdx = %ld\n", sz, size, dgram.xtc.extent, dgram.xtc.sizeofPayload(), payload - (uint32_t*)&dgram);
    //uint32_t* buf = (uint32_t*)&dgram;
    //for (unsigned i = 0; i < sz; ++i) {
    //  if (&buf[i] == (uint32_t*)&dgram)       printf(  "dgram:   ");
    //  if (&buf[i] == (uint32_t*)payload)      printf("\npayload: ");
    //  if (&buf[i] == (uint32_t*)desc.data())  printf("\ndata:    ");
    //  printf("%08x ", buf[i]);
    //}
    //printf("\n");
}

void PvaDetector::_worker()
{
    // setup monitoring
    std::map<std::string, std::string> labels{{"instrument", m_para.instrument},
                                              {"partition", std::to_string(m_para.partition)},
                                              {"detname", m_para.detName},
                                              {"detseg", std::to_string(m_para.detSegment)},
                                              {"alias", m_para.alias}};
    m_nEvents = 0;
    m_exporter->add("drp_event_rate", labels, Pds::MetricType::Rate,
                    [&](){return m_nEvents;});
    m_nUpdates = 0;
    m_exporter->add("drp_update_rate", labels, Pds::MetricType::Rate,
                    [&](){return m_nUpdates;});
    m_nMatch = 0;
    m_exporter->add("drp_match_count", labels, Pds::MetricType::Counter,
                    [&](){return m_nMatch;});
    m_nEmpty = 0;
    m_exporter->add("drp_empty_count", labels, Pds::MetricType::Counter,
                    [&](){return m_nEmpty;});
    m_nMissed = 0;
    m_exporter->add("drp_miss_count", labels, Pds::MetricType::Counter,
                    [&](){return m_nMissed;});
    m_nTooOld = 0;
    m_exporter->add("drp_tooOld_count", labels, Pds::MetricType::Counter,
                    [&](){return m_nTooOld;});
    m_nTimedOut = 0;
    m_exporter->add("drp_timeout_count", labels, Pds::MetricType::Counter,
                    [&](){return m_nTimedOut;});
    m_timeDiff = 0;
    m_exporter->add("drp_time_diff", labels, Pds::MetricType::Gauge,
                    [&](){return m_timeDiff;});

    m_exporter->add("drp_worker_input_queue", labels, Pds::MetricType::Gauge,
                    [&](){return m_evtQueue.guess_size();});
    m_exporter->constant("drp_worker_queue_depth", labels, m_evtQueue.size());

    // Borrow this for awhile
    m_exporter->add("drp_worker_output_queue", labels, Pds::MetricType::Gauge,
                    [&](){return m_pvQueue.guess_size();});

    Pgp pgp(m_para, m_drp, this, m_running);

    m_exporter->add("drp_num_dma_ret", labels, Pds::MetricType::Gauge,
                    [&](){return pgp.nDmaRet();});
    m_exporter->add("drp_pgp_byte_rate", labels, Pds::MetricType::Rate,
                    [&](){return pgp.dmaBytes();});
    m_exporter->add("drp_dma_size", labels, Pds::MetricType::Gauge,
                    [&](){return pgp.dmaSize();});
    m_exporter->add("drp_th_latency", labels, Pds::MetricType::Gauge,
                    [&](){return pgp.latency();});
    m_exporter->add("drp_num_dma_errors", labels, Pds::MetricType::Gauge,
                    [&](){return pgp.nDmaErrors();});
    m_exporter->add("drp_num_no_common_rog", labels, Pds::MetricType::Gauge,
                    [&](){return pgp.nNoComRoG();});
    m_exporter->add("drp_num_missing_rogs", labels, Pds::MetricType::Gauge,
                    [&](){return pgp.nMissingRoGs();});
    m_exporter->add("drp_num_th_error", labels, Pds::MetricType::Gauge,
                    [&](){return pgp.nTmgHdrError();});
    m_exporter->add("drp_num_pgp_jump", labels, Pds::MetricType::Gauge,
                    [&](){return pgp.nPgpJumps();});
    m_exporter->add("drp_num_no_tr_dgram", labels, Pds::MetricType::Gauge,
                    [&](){return pgp.nNoTrDgrams();});

    const uint64_t nsTmo = (m_para.kwargs.find("match_tmo_ms") != m_para.kwargs.end() ?
                            std::stoul(Detector::m_para->kwargs["match_tmo_ms"])      :
                            1500) * 1000000;

    enum TmoState { None, Started, Finished };
    TmoState tmoState(TmoState::None);
    const std::chrono::microseconds tmo(int(1.1 * m_drp.tebPrms().maxEntries * 14/13));
    auto tInitial = Pds::fast_monotonic_clock::now(CLOCK_MONOTONIC);

    while (true) {
        if (m_terminate.load(std::memory_order_relaxed)) {
            break;
        }

        uint32_t index;
        if (pgp.next(index)) {
            tmoState = TmoState::None;
            m_nEvents++;

            m_evtQueue.push(index);

            _matchUp();
        }
        else {
            // If there are any PGP datagrams stacked up, try to match them
            // up with any PV updates that may have arrived
            _matchUp();

            // Generate a timestamp in the past
            XtcData::TimeStamp timestamp(0, nsTmo);
            auto ns = _deltaT<ns_t>(timestamp);
            _timeout(timestamp.from_ns(ns));

            if (tmoState == TmoState::None) {
                tmoState = TmoState::Started;
                tInitial = Pds::fast_monotonic_clock::now(CLOCK_MONOTONIC);
            } else {
                if (Pds::fast_monotonic_clock::now(CLOCK_MONOTONIC) - tInitial > tmo) {
                  //if (tmoState != TmoState::Finished) {
                        m_drp.tebContributor().timeout();
                  //      tmoState = TmoState::Finished;
                  //}
                }
            }
        }
    }

    // Flush the DMA buffers
    pgp.flush();

    logging::info("Worker thread finished");
}

void PvaDetector::process(const XtcData::TimeStamp& timestamp)
{
    // Protect against namesLookup not being stable before Enable
    if (m_running.load(std::memory_order_relaxed)) {
        ++m_nUpdates;
        logging::debug("%s updated @ %u.%09u", m_pvaMonitor->name().c_str(), timestamp.seconds(), timestamp.nanoseconds());

        XtcData::Dgram* dgram;
        if (m_bufferFreelist.try_pop(dgram)) { // If a buffer is available...
            //static uint64_t last_ts = 0;
            //uint64_t ts = timestamp.to_ns();
            //int64_t  dT = ts - last_ts;
            //printf("  PV:  %u.%09u, dT %9ld, ts %18lu, last %18lu\n", timestamp.seconds(), timestamp.nanoseconds(), dT, ts, last_ts);
            //if (dT > 0)  last_ts = ts;

            dgram->time = timestamp;           // Save the PV's timestamp
            dgram->xtc = {{XtcData::TypeId::Parent, 0}, {nodeId}};

            const void* bufEnd = (char*)dgram + m_pool->pebble.bufferSize();
            event(*dgram, bufEnd, nullptr);    // PGPEvent not needed in this case

            m_pvQueue.push(dgram);
        }
        else {
            ++m_nMissed;                       // Else count it as missed
        }
    }
}

void PvaDetector::_matchUp()
{
    while (true) {
        uint32_t pebbleIdx;
        if (!m_evtQueue.peek(pebbleIdx))  break;

        Pds::EbDgram* pebbleDg = reinterpret_cast<Pds::EbDgram*>(m_pool->pebble[pebbleIdx]);
        XtcData::TransitionId::Value service = pebbleDg->service();
        if (service != XtcData::TransitionId::L1Accept) {
            _handleTransition(pebbleIdx, pebbleDg);
            continue;
        }

        XtcData::Dgram* pvDg;
        if (!m_pvQueue.peek(pvDg))  break;

        m_timeDiff = pebbleDg->time.to_ns() - pvDg->time.to_ns();

        int result = _compare(pebbleDg->time, pvDg->time);

        logging::debug("PGP: %u.%09d, PV: %u.%09d, PGP - PV: %12ld ns, pid %014lx, svc %2d, compare %c, latency %ld",
      //printf        ("PGP: %u.%09d, PV: %u.%09d, PGP - PV: %12ld ns, pid %014lx, svc %2d, compare %c, latency %ld\n",
                       pebbleDg->time.seconds(), pebbleDg->time.nanoseconds(),
                       pvDg->time.seconds(), pvDg->time.nanoseconds(),
                       m_timeDiff, pebbleDg->pulseId(), pebbleDg->service(),
                       result == 0 ? '=' : (result < 0 ? '<' : '>'), _deltaT<ms_t>(pebbleDg->time));

        if      (result == 0)  _handleMatch  (*pvDg, *pebbleDg);
        else if (result  < 0)  _handleYounger(*pvDg, *pebbleDg);
        else                   _handleOlder  (*pvDg, *pebbleDg);
    }
}

void PvaDetector::_handleTransition(uint32_t pebbleIdx, Pds::EbDgram* pebbleDg)
{
    // Find the transition dgram in the pool and initialize its header
    Pds::EbDgram* trDgram = m_pool->transitionDgrams[pebbleIdx];
    if (trDgram) {                      // nullptr happen during shutdown
        *trDgram = *pebbleDg;

        XtcData::TransitionId::Value service = trDgram->service();
        if (service != XtcData::TransitionId::SlowUpdate) {
            // copy the temporary xtc created on phase 1 of the transition
            // into the real location
            XtcData::Xtc& trXtc = transitionXtc();
            trDgram->xtc = trXtc; // Preserve header info, but allocate to check fit
            const void* bufEnd = (char*)trDgram + m_para.maxTrSize;
            auto payload = trDgram->xtc.alloc(trXtc.sizeofPayload(), bufEnd);
            memcpy(payload, (const void*)trXtc.payload(), trXtc.sizeofPayload());

            if (service == XtcData::TransitionId::Enable) {
                m_running = true;
            }
            else if (service == XtcData::TransitionId::Disable) {
                m_running = false;
            }
        }
    }
    _sendToTeb(*pebbleDg, pebbleIdx);

    uint32_t evtIdx;
    m_evtQueue.try_pop(evtIdx);       // Actually consume the pebble index
    assert(evtIdx == pebbleIdx);
}

void PvaDetector::_handleMatch(const XtcData::Dgram& pvDg, Pds::EbDgram& pebbleDg)
{
    uint32_t pebbleIdx;
    m_evtQueue.try_pop(pebbleIdx);      // Actually consume the element

    pebbleDg.xtc.damage.increase(pvDg.xtc.damage.value());
    auto bufEnd  = (char*)&pebbleDg + m_pool->pebble.bufferSize();
    auto payload = pebbleDg.xtc.alloc(pvDg.xtc.sizeofPayload(), bufEnd);
    memcpy(payload, (const void*)pvDg.xtc.payload(), pvDg.xtc.sizeofPayload());

    ++m_nMatch;
    logging::debug("PV matches PGP!!  "
                   "TimeStamps: PV %u.%09u == PGP %u.%09u",
                   pvDg.time.seconds(), pvDg.time.nanoseconds(),
                   pebbleDg.time.seconds(), pebbleDg.time.nanoseconds());

    _sendToTeb(pebbleDg, pebbleIdx);

    XtcData::Dgram* dgram;
    m_pvQueue.try_pop(dgram);       // Actually consume the element
    m_bufferFreelist.push(dgram);   // Return buffer to freelist
}

void PvaDetector::_handleYounger(const XtcData::Dgram& pvDg, Pds::EbDgram& pebbleDg)
{
    uint32_t pebbleIdx;
    m_evtQueue.try_pop(pebbleIdx);      // Actually consume the element

    // No corresponding PV data so mark event damaged
    pebbleDg.xtc.damage.increase(XtcData::Damage::MissingData);

    ++m_nEmpty;
    logging::debug("PV too young!!    "
                   "TimeStamps: PV %u.%09u > PGP %u.%09u",
                   pvDg.time.seconds(), pvDg.time.nanoseconds(),
                   pebbleDg.time.seconds(), pebbleDg.time.nanoseconds());

    _sendToTeb(pebbleDg, pebbleIdx);
}

void PvaDetector::_handleOlder(const XtcData::Dgram& pvDg, Pds::EbDgram& pebbleDg)
{
    ++m_nTooOld;
    logging::debug("PV too old!!      "
                   "TimeStamps: PV %u.%09u < PGP %u.%09u [0x%08x%04x.%05x < 0x%08x%04x.%05x]",
                   pvDg.time.seconds(), pvDg.time.nanoseconds(),
                   pebbleDg.time.seconds(), pebbleDg.time.nanoseconds(),
                   pvDg.time.seconds(), (pvDg.time.nanoseconds()>>16)&0xfffe, pvDg.time.nanoseconds()&0x1ffff,
                   pebbleDg.time.seconds(), (pebbleDg.time.nanoseconds()>>16)&0xfffe, pebbleDg.time.nanoseconds()&0x1ffff);

    XtcData::Dgram* dgram;
    m_pvQueue.try_pop(dgram);           // Actually consume the element
    m_bufferFreelist.push(dgram);       // Return buffer to freelist
}

void PvaDetector::_timeout(const XtcData::TimeStamp& timestamp)
{
    // Time out older PV updates
    XtcData::Dgram* pvDg;
    if (m_pvQueue.peek(pvDg)) {
      if (!(pvDg->time > timestamp)) {   // pvDg is newer than the timeout timestamp
            m_pvQueue.try_pop(pvDg);     // Actually consume the element
            m_bufferFreelist.push(pvDg); // Return buffer to freelist
        }
    }

    // Time out older pending PGP datagrams
    uint32_t index;
    if (!m_evtQueue.peek(index))  return;

    Pds::EbDgram& dgram = *reinterpret_cast<Pds::EbDgram*>(m_pool->pebble[index]);
    if (dgram.time > timestamp)  return; // dgram is newer than the timeout timestamp

    uint32_t idx;
    m_evtQueue.try_pop(idx);             // Actually consume the element
    assert(idx == index);

    if (dgram.service() == XtcData::TransitionId::L1Accept) {
        // No PVA data so mark event as damaged
        dgram.xtc.damage.increase(XtcData::Damage::TimedOut);
        ++m_nTimedOut;
        //printf("TO: %u.%09u, PGP: %u.%09u, PGP - TO: %10ld ns, svc %2d  Timeout\n",
        //       timestamp.seconds(), timestamp.nanoseconds(),
        //       dgram.time.seconds(), dgram.time.nanoseconds(),
        //       dgram.time.to_ns() - timestamp.to_ns(),
        //       dgram.service());
        logging::debug("Event timed out!! "
                       "TimeStamps: timeout %u.%09u > PGP %u.%09u [0x%08x%04x.%05x > 0x%08x%04x.%05x]",
                       timestamp.seconds(), timestamp.nanoseconds(),
                       dgram.time.seconds(), dgram.time.nanoseconds(),
                       timestamp.seconds(), (timestamp.nanoseconds()>>16)&0xfffe, timestamp.nanoseconds()&0x1ffff,
                       dgram.time.seconds(), (dgram.time.nanoseconds()>>16)&0xfffe, dgram.time.nanoseconds()&0x1ffff);
    }

    _sendToTeb(dgram, index);
}

void PvaDetector::_sendToTeb(const Pds::EbDgram& dgram, uint32_t index)
{
    // Make sure the datagram didn't get too big
    const size_t size = sizeof(dgram) + dgram.xtc.sizeofPayload();
    const size_t maxSize = (dgram.service() == XtcData::TransitionId::L1Accept)
                         ? m_pool->pebble.bufferSize()
                         : m_para.maxTrSize;
    if (size > maxSize) {
        logging::critical("%s Dgram of size %zd overflowed buffer of size %zd", XtcData::TransitionId::name(dgram.service()), size, maxSize);
        throw "Dgram overflowed buffer";
    }

    auto l3InpBuf = m_drp.tebContributor().fetch(index);
    Pds::EbDgram* l3InpDg = new(l3InpBuf) Pds::EbDgram(dgram);
    if (l3InpDg->isEvent()) {
        auto triggerPrimitive = m_drp.triggerPrimitive();
        if (triggerPrimitive) { // else this DRP doesn't provide input
            const void* bufEnd = (char*)l3InpDg + sizeof(*l3InpDg) + triggerPrimitive->size();
            triggerPrimitive->event(*m_pool, index, dgram.xtc, l3InpDg->xtc, bufEnd); // Produce
        }
    }
    m_drp.tebContributor().process(l3InpDg);
}


PvaApp::PvaApp(PvaParameters& para) :
    CollectionApp(para.collectionHost, para.partition, "drp", para.alias),
    m_drp(para, context()),
    m_para(para),
    m_pvaDetector(std::make_unique<PvaDetector>(para, m_drp)),
    m_det(m_pvaDetector.get()),
    m_unconfigure(false)
{
    Py_Initialize();                    // for use by configuration

    if (m_det == nullptr) {
        logging::critical("Error !! Could not create Detector object for %s", m_para.detType.c_str());
        throw "Could not create Detector object for " + m_para.detType;
    }
    logging::info("Ready for transitions");
}

PvaApp::~PvaApp()
{
    // Try to take things down gracefully when an exception takes us off the
    // normal path so that the most chance is given for prints to show up
    handleReset(json({}));

    Py_Finalize();                      // for use by configuration
}

void PvaApp::_disconnect()
{
    m_drp.disconnect();
    m_det->shutdown();
    m_pvaDetector->disconnect();
}

void PvaApp::_unconfigure()
{
    m_drp.pool.shutdown();  // Release Tr buffer pool
    m_drp.unconfigure();    // TebContributor must be shut down before the worker
    m_pvaDetector->unconfigure();
    m_unconfigure = false;
}

json PvaApp::connectionInfo()
{
    std::string ip = m_para.kwargs.find("ep_domain") != m_para.kwargs.end()
                   ? getNicIp(m_para.kwargs["ep_domain"])
                   : getNicIp(m_para.kwargs["forceEnet"] == "yes");
    logging::debug("nic ip  %s", ip.c_str());
    json body = {{"connect_info", {{"nic_ip", ip}}}};
    json info = m_det->connectionInfo();
    body["connect_info"].update(info);
    json bufInfo = m_drp.connectionInfo(ip);
    body["connect_info"].update(bufInfo);
    return body;
}

void PvaApp::connectionShutdown()
{
    m_drp.shutdown();
}

void PvaApp::_error(const std::string& which, const nlohmann::json& msg, const std::string& errorMsg)
{
    json body = json({});
    body["err_info"] = errorMsg;
    json answer = createMsg(which, msg["header"]["msg_id"], getId(), body);
    reply(answer);
}

void PvaApp::handleConnect(const nlohmann::json& msg)
{
    std::string errorMsg = m_drp.connect(msg, getId());
    if (!errorMsg.empty()) {
        logging::error("Error in DrpBase::connect");
        logging::error("%s", errorMsg.c_str());
        _error("connect", msg, errorMsg);
        return;
    }

    m_det->nodeId = m_drp.nodeId();
    m_det->connect(msg, std::to_string(getId()));

    unsigned rc = m_pvaDetector->connect(errorMsg);
    if (!errorMsg.empty()) {
        if (!rc) {
            logging::warning(("PvaDetector::connect: " + errorMsg).c_str());
            json warning = createAsyncWarnMsg(m_para.alias, errorMsg);
            reply(warning);
        }
        else {
            logging::error(("PvaDetector::connect: " + errorMsg).c_str());
            _error("connect", msg, errorMsg);
            return;
        }
    }

    json body = json({});
    json answer = createMsg("connect", msg["header"]["msg_id"], getId(), body);
    reply(answer);
}

void PvaApp::handleDisconnect(const json& msg)
{
    // Carry out the queued Unconfigure, if there was one
    if (m_unconfigure) {
        _unconfigure();
    }

    _disconnect();

    json body = json({});
    reply(createMsg("disconnect", msg["header"]["msg_id"], getId(), body));
}

void PvaApp::handlePhase1(const json& msg)
{
    std::string key = msg["header"]["key"];
    logging::debug("handlePhase1 for %s in PvaDetectorApp", key.c_str());

    XtcData::Xtc& xtc = m_det->transitionXtc();
    xtc = {{XtcData::TypeId::Parent, 0}, {m_det->nodeId}};
    auto bufEnd = m_det->trXtcBufEnd();

    json phase1Info{ "" };
    if (msg.find("body") != msg.end()) {
        if (msg["body"].find("phase1Info") != msg["body"].end()) {
            phase1Info = msg["body"]["phase1Info"];
        }
    }

    json body = json({});

    if (key == "configure") {
        if (m_unconfigure) {
            _unconfigure();
        }

        std::string errorMsg = m_drp.configure(msg);
        if (!errorMsg.empty()) {
            errorMsg = "Phase 1 error: " + errorMsg;
            logging::error("%s", errorMsg.c_str());
            _error(key, msg, errorMsg);
            return;
        }

        std::string config_alias = msg["body"]["config_alias"];
        unsigned error = m_det->configure(config_alias, xtc, bufEnd);
        if (error) {
            std::string errorMsg = "Failed transition phase 1";
            logging::error("%s", errorMsg.c_str());
            _error(key, msg, errorMsg);
            return;
        }

        m_drp.runInfoSupport(xtc, bufEnd, m_det->namesLookup());
        m_drp.chunkInfoSupport(xtc, bufEnd, m_det->namesLookup());
    }
    else if (key == "unconfigure") {
        // "Queue" unconfiguration until after phase 2 has completed
        m_unconfigure = true;
    }
    else if (key == "beginrun") {
        RunInfo runInfo;
        std::string errorMsg = m_drp.beginrun(phase1Info, runInfo);
        if (!errorMsg.empty()) {
            body["err_info"] = errorMsg;
            logging::error("%s", errorMsg.c_str());
        }
        else {
            m_drp.runInfoData(xtc, bufEnd, m_det->namesLookup(), runInfo);
        }
    }
    else if (key == "endrun") {
        std::string errorMsg = m_drp.endrun(phase1Info);
        if (!errorMsg.empty()) {
            body["err_info"] = errorMsg;
            logging::error("%s", errorMsg.c_str());
        }
    }
    else if (key == "enable") {
        bool chunkRequest;
        ChunkInfo chunkInfo;
        std::string errorMsg = m_drp.enable(phase1Info, chunkRequest, chunkInfo);
        if (!errorMsg.empty()) {
            body["err_info"] = errorMsg;
            logging::error("%s", errorMsg.c_str());
        } else if (chunkRequest) {
            logging::debug("handlePhase1 enable found chunkRequest");
            m_drp.chunkInfoData(xtc, bufEnd, m_det->namesLookup(), chunkInfo);
        }
        logging::debug("handlePhase1 enable complete");
    }

    json answer = createMsg(key, msg["header"]["msg_id"], getId(), body);
    reply(answer);
}

void PvaApp::handleReset(const nlohmann::json& msg)
{
    unsubscribePartition();    // ZMQ_UNSUBSCRIBE
    _unconfigure();
    _disconnect();
    connectionShutdown();
}

} // namespace Drp


int main(int argc, char* argv[])
{
    Drp::PvaParameters para;
    std::string kwargs_str;
    int c;
    while((c = getopt(argc, argv, "p:o:l:D:S:C:d:u:k:P:T::M:01v")) != EOF) {
        switch(c) {
            case 'p':
                para.partition = std::stoi(optarg);
                break;
            case 'o':
                para.outputDir = optarg;
                break;
            case 'l':
                para.laneMask = std::stoul(optarg, nullptr, 16);
                break;
            case 'D':
                para.detType = optarg;  // Defaults to 'pv'
                break;
            case 'S':
                para.serNo = optarg;
                break;
            case 'u':
                para.alias = optarg;
                break;
            case 'C':
                para.collectionHost = optarg;
                break;
            case 'd':
                para.device = optarg;
                break;
            case 'k':
                kwargs_str = kwargs_str.empty()
                           ? optarg
                           : kwargs_str + ", " + optarg;
                break;
            case 'P':
                para.instrument = optarg;
                break;
            case 'M':
                para.prometheusDir = optarg;
                break;
            //  Indicate level of timestamp matching (ugh)
            case '0':
                tsMatchDegree = 0;
                break;
            case '1':
                fprintf(stderr, "Option -1 is disabled\n");  exit(EXIT_FAILURE);
                tsMatchDegree = 1;
                break;
            case 'v':
                ++para.verbose;
                break;
            default:
                return 1;
        }
    }

    switch (para.verbose) {
        case 0:  logging::init(para.instrument.c_str(), LOG_INFO);   break;
        default: logging::init(para.instrument.c_str(), LOG_DEBUG);  break;
    }
    logging::info("logging configured");
    if (para.instrument.empty()) {
        logging::warning("-P: instrument name is missing");
    }
    // Check required parameters
    if (para.partition == unsigned(-1)) {
        logging::critical("-p: partition is mandatory");
        return 1;
    }
    if (para.device.empty()) {
        logging::critical("-d: device is mandatory");
        return 1;
    }
    if (para.alias.empty()) {
        logging::critical("-u: alias is mandatory");
        return 1;
    }

    // Only one lane is supported by this DRP
    if (std::bitset<PGP_MAX_LANES>(para.laneMask).count() != 1) {
        logging::critical("-l: lane mask must have only 1 bit set");
        return 1;
    }

    // Allow detType to be overridden, but generally, psana will expect 'pv'
    if (para.detType.empty()) {
      para.detType = "pv";
    }

    // Alias must be of form <detName>_<detSegment>
    size_t found = para.alias.rfind('_');
    if ((found == std::string::npos) || !isdigit(para.alias.back())) {
        logging::critical("-u: alias must have _N suffix");
        return 1;
    }
    para.detName = para.alias.substr(0, found);
    para.detSegment = std::stoi(para.alias.substr(found+1, para.alias.size()));

    // Provider is "pva" (default) or "ca"
    if (optind < argc)                  // [<provider>/]<PV name>[.<field>]
    {
        para.pvName = argv[optind++];

        if (optind < argc)
        {
            logging::error("Unrecognized argument:");
            while (optind < argc)
                logging::error("  %s ", argv[optind++]);
            return 1;
        }
    }
    else {
        logging::critical("A PV ([<provider>/]<PV name>[.<field>]) is mandatory");
        return 1;
    }

    para.maxTrSize = 256 * 1024;
    try {
        get_kwargs(kwargs_str, para.kwargs);
        for (const auto& kwargs : para.kwargs) {
            if (kwargs.first == "forceEnet")      continue;
            if (kwargs.first == "ep_fabric")      continue;
            if (kwargs.first == "ep_domain")      continue;
            if (kwargs.first == "ep_provider")    continue;
            if (kwargs.first == "sim_length")     continue;  // XpmDetector
            if (kwargs.first == "timebase")       continue;  // XpmDetector
            if (kwargs.first == "pebbleBufSize")  continue;  // DrpBase
            if (kwargs.first == "pebbleBufCount") continue;  // DrpBase
            if (kwargs.first == "batching")       continue;  // DrpBase
            if (kwargs.first == "directIO")       continue;  // DrpBase
            if (kwargs.first == "firstdim")       continue;
            if (kwargs.first == "match_tmo_ms")   continue;
            logging::critical("Unrecognized kwarg '%s=%s'\n",
                              kwargs.first.c_str(), kwargs.second.c_str());
            return 1;
        }

        para.provider = "pva";
        para.field    = "value";
        auto pos = para.pvName.find("/", 0);
        if (pos != std::string::npos) { // Parse provider
            para.provider = para.pvName.substr(0, pos);
            para.pvName   = para.pvName.substr(pos+1);
        }
        pos = para.pvName.find(".", 0);
        if (pos != std::string::npos) { // Parse field name
            para.field  = para.pvName.substr(pos+1);
            para.pvName = para.pvName.substr(0, pos);
        }
        para.request = para.provider == "pva" ? "field(value,timeStamp,dimension)"
                                              : "field(value,timeStamp)";

        Drp::PvaApp(para).run();
        return 0;
    }
    catch (std::exception& e)  { logging::critical("%s", e.what()); }
    catch (std::string& e)     { logging::critical("%s", e.c_str()); }
    catch (char const* e)      { logging::critical("%s", e); }
    catch (...)                { logging::critical("Default exception"); }
    return EXIT_FAILURE;
}
