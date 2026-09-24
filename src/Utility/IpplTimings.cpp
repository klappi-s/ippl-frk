//
// Class IpplTimings
//   IpplTimings - a simple singleton class which lets the user create and
//   timers that can be printed out at the end of the program.
//
//   General usage
//    1) create a timer:
//       IpplTimings::TimerRef val = IpplTimings::getTimer("timer name");
//    This will either create a new one, or return a ref to an existing one
//
//    2) start a timer:
//       IpplTimings::startTimer(val);
//    This will start the referenced timer running.  If it is already running,
//    it will not change anything.
//
//    3) stop a timer:
//       IpplTimings::stopTimer(val);
//    This will stop the timer, assuming it was running, and add in the
//    time to the accumulating time for that timer.
//
//    4) print out the results:
//       IpplTimings::print();
//

#include "Ippl.h"

#include "Utility/IpplTimings.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "Utility/Inform.h"
#include "Utility/IpplInfo.h"

#ifdef IPPL_ENABLE_NSYS_PROFILER
#include "nvtx3/nvToolsExt.h"
const uint32_t colors[] = {0xff00ff00, 0xff0000ff, 0xffffff00, 0xffff00ff,
                           0xff00ffff, 0xffff0000, 0xffffffff};
const int num_colors    = sizeof(colors) / sizeof(uint32_t);
#define PUSH_RANGE(name, cid)                                              \
    {                                                                      \
        int color_id                      = cid;                           \
        color_id                          = color_id % num_colors;         \
        nvtxEventAttributes_t eventAttrib = {0};                           \
        eventAttrib.version               = NVTX_VERSION;                  \
        eventAttrib.size                  = NVTX_EVENT_ATTRIB_STRUCT_SIZE; \
        eventAttrib.colorType             = NVTX_COLOR_ARGB;               \
        eventAttrib.color                 = colors[color_id];              \
        eventAttrib.messageType           = NVTX_MESSAGE_TYPE_ASCII;       \
        eventAttrib.message.ascii         = name;                          \
        nvtxRangePushEx(&eventAttrib);                                     \
    }
#endif

// Static member initialization
Timing* IpplTimings::instance = new Timing();
std::stack<Timing*> IpplTimings::stashedInstance;
const std::vector<double> Timing::emptyMeasurements;

Timing::Timing()
    : TimerList()
    , TimerMap() {}

Timing::~Timing() {
    for (TimerMap_t::iterator it = TimerMap.begin(); it != TimerMap.end(); ++it) {
        it->second = 0;
    }
    TimerMap.clear();

    TimerList.clear();
}

// create a timer, or get one that already exists
Timing::TimerRef Timing::getTimer(const char* nm) {
    std::string s(nm);
    TimerInfo* tptr          = 0;
    TimerMap_t::iterator loc = TimerMap.find(s);
    if (loc == TimerMap.end()) {
        tptr              = new TimerInfo;
        tptr->indx        = TimerList.size();
        tptr->name        = s;
        tptr->sampleLimit = sampleLimit_m;
        TimerMap.insert(TimerMap_t::value_type(s, tptr));
        TimerList.push_back(my_auto_ptr<TimerInfo>(tptr));
    } else {
        tptr = (*loc).second;
    }
    return tptr->indx;
}

// start a timer
void Timing::startTimer(TimerRef t) {
    if (t >= TimerList.size())
        return;
#ifdef IPPL_ENABLE_NSYS_PROFILER
    PUSH_RANGE(TimerList[t]->name.c_str(), (int)t);
#endif
    TimerList[t]->start();
}

// stop a timer, and accumulate it's values
void Timing::stopTimer(TimerRef t) {
    if (t >= TimerList.size())
        return;
#ifdef IPPL_ENABLE_NSYS_PROFILER
    nvtxRangePop();
#endif
    TimerList[t]->stop();
}

// clear a timer, by turning it off and throwing away its time
void Timing::clearTimer(TimerRef t) {
    if (t >= TimerList.size())
        return;
    TimerList[t]->clear();
}

// Reset all timers - useful for warmup
void Timing::resetAllTimers() {
    for (unsigned int i = 0; i < TimerList.size(); ++i) {
        TimerList[i]->clearAll();
    }
}
// Get measurements for a specific timer by reference
const std::vector<double>& Timing::getMeasurements(TimerRef t) const {
    if (t >= TimerList.size())
        return emptyMeasurements;
    return TimerList[t]->measurements;
}

// Get measurements for a specific timer by name
const std::vector<double>& Timing::getMeasurements(const std::string& name) const {
    TimerMap_t::const_iterator loc = TimerMap.find(name);
    if (loc == TimerMap.end())
        return emptyMeasurements;
    return loc->second->measurements;
}

// Get measurement count for a timer
size_t Timing::getMeasurementCount(TimerRef t) const {
    if (t >= TimerList.size())
        return 0;
    return TimerList[t]->measurements.size();
}

// Get all timer names
std::vector<std::string> Timing::getTimerNames() const {
    std::vector<std::string> names;
    names.reserve(TimerList.size());
    for (unsigned int i = 0; i < TimerList.size(); ++i) {
        names.push_back(TimerList[i]->name);
    }
    return names;
}

namespace {
    // Both raw and summary exports use the same bounded, collective I/O path.
    void writeRankOrderedCSV(const std::string& filename, const std::string& localStr,
                             const std::string& header) {
        const int rank     = ippl::Comm->rank();
        const int numRanks = ippl::Comm->size();
        // Export rank by rank in bounded chunks. A single Gatherv uses int
        // displacements and can overflow even when every rank's count fits in int.
        // Keep the CSV order and avoid allocating the entire export on rank zero.
        // A private communicator prevents these messages from consuming application tags.
        MPI_Comm exportComm;
        MPI_Comm_dup(ippl::Comm->getCommunicator(), &exportComm);
        MPI_Aint localSize = static_cast<MPI_Aint>(localStr.size());
        std::vector<MPI_Aint> sizes(numRanks, 0);
        MPI_Gather(&localSize, 1, MPI_AINT, sizes.data(), 1, MPI_AINT, 0, exportComm);

        constexpr size_t chunkBytes = 1024 * 1024;
        std::vector<char> buffer(rank == 0 ? chunkBytes : 0);
        std::ofstream outFile;
        int outputError = 0;
        if (rank == 0) {
            outFile.open(filename);
            outputError = !outFile;
        }
        MPI_Bcast(&outputError, 1, MPI_INT, 0, exportComm);
        if (outputError) {
            MPI_Comm_free(&exportComm);
            throw std::runtime_error("Timing::dumpToCSV: cannot open " + filename);
        }
        if (rank == 0) {
            outFile << header;
            for (int source = 0; source < numRanks; ++source) {
                for (MPI_Aint offset = 0; offset < sizes[source];) {
                    const int count  = static_cast<int>(std::min<MPI_Aint>(
                        sizes[source] - offset, static_cast<MPI_Aint>(chunkBytes)));
                    const char* data = localStr.data() + (source == 0 ? offset : 0);
                    if (source != 0) {
                        MPI_Recv(buffer.data(), count, MPI_CHAR, source, 0, exportComm,
                                 MPI_STATUS_IGNORE);
                        data = buffer.data();
                    }
                    // Continue receiving after an I/O failure so no sender is stranded.
                    outFile.write(data, count);
                    offset += count;
                }
            }
            outFile.close();
            outputError = !outFile;
        } else {
            for (MPI_Aint offset = 0; offset < localSize;) {
                const int count = static_cast<int>(
                    std::min<MPI_Aint>(localSize - offset, static_cast<MPI_Aint>(chunkBytes)));
                MPI_Send(localStr.data() + offset, count, MPI_CHAR, 0, 0, exportComm);
                offset += count;
            }
        }
        MPI_Bcast(&outputError, 1, MPI_INT, 0, exportComm);
        MPI_Comm_free(&exportComm);
        if (outputError)
            throw std::runtime_error("Timing::dumpToCSV: cannot write " + filename);
    }

}  // namespace

// Dump all measurements to CSV (default format)
void Timing::dumpToCSV(const std::string& filename) {
    dumpToCSV(filename, ",", true);
}

// Dump all measurements to CSV with custom options
void Timing::dumpToCSV(const std::string& filename, const std::string& delimiter,
                       bool includeHeader) {
    const int rank = ippl::Comm->rank();

    // Each rank serialises its measurements into a local buffer.
    std::ostringstream localData;
    for (unsigned int i = 0; i < TimerList.size(); ++i) {
        TimerInfo* tptr                         = TimerList[i].get();
        const std::string& timerName            = tptr->name;
        const std::vector<double>& measurements = tptr->measurements;
        for (size_t j = 0; j < measurements.size(); ++j) {
            localData << timerName << delimiter << rank << delimiter << j << delimiter
                      << std::setprecision(12) << measurements[j] << "\n";
        }
    }
    const std::string localStr = localData.str();

    const std::string header = includeHeader
                                   ? "timer_name" + delimiter + "rank" + delimiter
                                         + "measurement_id" + delimiter + "duration_seconds\n"
                                   : "";
    writeRankOrderedCSV(filename, localStr, header);
}

void Timing::setSampleLimit(size_t limit) {
    sampleLimit_m = limit;
    for (auto& timer : TimerList) {
        timer->sampleLimit = limit;
        std::vector<double>().swap(timer->measurements);
    }
}

double Timing::getLastMeasurement(TimerRef t) const {
    return t < TimerList.size() ? TimerList[t]->lastMeasurement : 0.0;
}

size_t Timing::getCallCount(TimerRef t) const {
    return t < TimerList.size() ? TimerList[t]->callCount : 0;
}

void Timing::dumpSummaryToCSV(const std::string& filename) {
    std::ostringstream localData;
    localData << std::setprecision(17);
    for (const auto& timer : TimerList) {
        const auto count = timer->callCount;
        const double stddev =
            count > 1 ? std::sqrt(std::max(0.0, timer->squaredDeviations / (count - 1))) : 0.0;
        localData << timer->name << ',' << ippl::Comm->rank() << ',' << count << ','
                  << timer->wallTime << ',' << timer->minMeasurement << ',' << timer->maxMeasurement
                  << ',' << timer->meanMeasurement << ',' << stddev << ','
                  << timer->measurements.size() << '\n';
    }
    writeRankOrderedCSV(filename, localData.str(),
                        "timer_name,rank,count,total_seconds,min_seconds,max_seconds,mean_seconds,"
                        "stddev_seconds,stored_samples\n");
}

// print out the timing results
void Timing::print() {
    if (TimerList.size() < 1)
        return;

    // report the average time for each timer
    Inform msg("Timings");
    msg << level1 << "---------------------------------------------";
    msg << "\n";
    msg << "     Timing results for " << ippl::Comm->size() << " rank(s):"
        << "\n";
    msg << "---------------------------------------------";
    msg << "\n";

    {
        TimerInfo* tptr  = TimerList[0].get();
        double walltotal = 0.0;
        ippl::Comm->reduce(tptr->wallTime, walltotal, 1, std::greater<double>());
        size_t lengthName = std::min(tptr->name.length(), 19lu);
        msg << tptr->name.substr(0, lengthName) << std::string().assign(20 - lengthName, '.')
            << " Wall tot = " << std::setw(10) << walltotal << "\n"
            << "\n";
    }

    for (unsigned int i = 1; i < TimerList.size(); ++i) {
        TimerInfo* tptr = TimerList[i].get();
        double wallmax = 0.0, wallmin = 0.0;
        double wallavg = 0.0;
        ippl::Comm->reduce(tptr->wallTime, wallmax, 1, std::greater<double>());
        ippl::Comm->reduce(tptr->wallTime, wallmin, 1, std::less<double>());
        ippl::Comm->reduce(tptr->wallTime, wallavg, 1, std::plus<double>());
        size_t lengthName = std::min(tptr->name.length(), 19lu);

        msg << tptr->name.substr(0, lengthName) << std::string().assign(20 - lengthName, '.')
            << " Wall max = " << std::setw(10) << wallmax << "\n"
            << std::string().assign(20, ' ') << " Wall avg = " << std::setw(10)
            << wallavg / ippl::Comm->size() << "\n"
            << std::string().assign(20, ' ') << " Wall min = " << std::setw(10) << wallmin << "\n"
            << "\n";
    }

    msg << "---------------------------------------------\n";
    msg << "     Measurement counts:\n";
    msg << "---------------------------------------------\n";
    for (unsigned int i = 0; i < TimerList.size(); ++i) {
        TimerInfo* tptr   = TimerList[i].get();
        size_t lengthName = std::min(tptr->name.length(), 19lu);
        msg << tptr->name.substr(0, lengthName) << std::string().assign(20 - lengthName, '.')
            << " Count = " << std::setw(10) << tptr->callCount << "\n";
    }

    msg << "---------------------------------------------";
    msg << endl;

    // Per-occurrence statistics
    msg << "---------------------------------------------\n";
    msg << "     Per-occurrence statistics:\n";
    msg << "---------------------------------------------\n";
    for (unsigned int i = 0; i < TimerList.size(); ++i) {
        TimerInfo* tptr    = TimerList[i].get();
        const size_t count = tptr->callCount;
        if (count == 0)
            continue;
        const double mean   = tptr->meanMeasurement;
        const double minVal = tptr->minMeasurement, maxVal = tptr->maxMeasurement;
        const double stddev =
            count > 1 ? std::sqrt(std::max(0.0, tptr->squaredDeviations / (count - 1))) : 0.0;

        size_t lengthName = std::min(tptr->name.length(), 19lu);
        msg << tptr->name.substr(0, lengthName) << std::string().assign(20 - lengthName, '.')
            << " n=" << std::setw(5) << count << "  mean=" << std::setw(10) << mean
            << "  std=" << std::setw(10) << stddev << "  min=" << std::setw(10) << minVal
            << "  max=" << std::setw(10) << maxVal << "\n";
    }
    msg << "---------------------------------------------" << endl;
}

// save the timing results into a file
void Timing::print(const std::string& fn, const std::map<std::string, unsigned int>& problemSize) {
    std::ofstream* timer_stream;
    Inform* msg;

    if (TimerList.size() < 1)
        return;

    timer_stream = new std::ofstream;
    timer_stream->open(fn.c_str(), std::ios::out);
    msg = new Inform(0, *timer_stream, 0);

    if (problemSize.size() > 0) {
        *msg << "Problem size:\n";
        for (auto it : problemSize) {
            *msg << "    " << std::setw(10) << it.first << ": " << it.second << "\n";
        }
        *msg << endl;
    }

    *msg << std::setw(27) << "ranks" << std::setw(11) << "Wall tot\n"
         << std::string().assign(37, '=') << "\n";
    {
        TimerInfo* tptr  = TimerList[0].get();
        double walltotal = 0.0;
        ippl::Comm->reduce(tptr->wallTime, walltotal, 1, std::greater<double>());
        size_t lengthName = std::min(tptr->name.length(), 19lu);
        *msg << tptr->name.substr(0, lengthName);
        for (int j = lengthName; j < 20; ++j) {
            *msg << ".";
        }
        *msg << " " << std::setw(6) << ippl::Comm->size() << " " << std::setw(9)
             << std::setprecision(4) << walltotal << "\n";
    }

    *msg << "\n"
         << std::setw(27) << "ranks" << std::setw(10) << "Wall max" << std::setw(10) << "Wall min"
         << std::setw(11) << "Wall avg\n"
         << std::string().assign(57, '=') << "\n";
    for (unsigned int i = 0; i < TimerList.size(); ++i) {
        TimerInfo* tptr = TimerList[i].get();
        double wallmax = 0.0, wallmin = 0.0;
        double wallavg = 0.0;
        ippl::Comm->reduce(tptr->wallTime, wallmax, 1, std::greater<double>());
        ippl::Comm->reduce(tptr->wallTime, wallmin, 1, std::less<double>());
        ippl::Comm->reduce(tptr->wallTime, wallavg, 1, std::plus<double>());
        size_t lengthName = std::min(tptr->name.length(), 19lu);
        *msg << tptr->name.substr(0, lengthName);
        for (int j = lengthName; j < 20; ++j) {
            *msg << ".";
        }
        *msg << " " << std::setw(6) << ippl::Comm->size() << " " << std::setw(9)
             << std::setprecision(4) << wallmax << " " << std::setw(9) << std::setprecision(4)
             << wallmin << " " << std::setw(9) << std::setprecision(4)
             << wallavg / ippl::Comm->size() << endl;
    }
    timer_stream->close();
    delete msg;
    delete timer_stream;
}

IpplTimings::IpplTimings() {}
IpplTimings::~IpplTimings() {}

void IpplTimings::stash() {
    PAssert_EQ(stashedInstance.size(), 0);

    stashedInstance.push(instance);
    instance = new Timing();
    instance->setSampleLimit(stashedInstance.top()->getSampleLimit());
}

void IpplTimings::pop() {
    PAssert_GT(stashedInstance.size(), 0);

    delete instance;
    instance = stashedInstance.top();
    stashedInstance.pop();
}