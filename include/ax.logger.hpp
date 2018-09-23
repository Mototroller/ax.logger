#ifndef AX_LOGGER_HPP
#define AX_LOGGER_HPP

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <future>
#include <iomanip>
#include <ios>
#include <iostream>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>


namespace Ax { namespace Logger {
    
    using size_t = std::size_t;
    using ByteType = unsigned char;
    
    
    enum class LEVEL : ByteType {
        TRACE,
        DEBUG,
        INFO,
        WARN,
        HIGH,
        ERR,
        OFF
    };
    
    inline bool operator<(LEVEL lh, LEVEL rh) {
        return static_cast<ByteType>(lh) < static_cast<ByteType>(rh); }
    
    inline bool operator<=(LEVEL lh, LEVEL rh) {
        return static_cast<ByteType>(lh) <= static_cast<ByteType>(rh); }
    
    inline std::ostream& operator<<(std::ostream& os, LEVEL lvl) {
        switch(lvl) {
            case LEVEL::TRACE:  os << "TRACE";  break;
            case LEVEL::DEBUG:  os << "DEBUG";  break;
            case LEVEL::INFO:   os << "INFO";   break;
            case LEVEL::WARN:   os << "WARN";   break;
            case LEVEL::HIGH:   os << "HIGH";   break;
            case LEVEL::ERR:    os << "ERROR";  break;
            case LEVEL::OFF:    os << "OFF";    break;
            
            default: os << "WTF" << int(lvl);
        }
        return os;
    }
    
    inline std::istream& operator>>(std::istream& is, LEVEL& lvl) {
        static const std::unordered_map<std::string, LEVEL> strToLvl{
            {"ALL",     LEVEL::TRACE},
            {"TRACE",   LEVEL::TRACE},
            {"DEBUG",   LEVEL::DEBUG},
            {"INFO",    LEVEL::INFO},
            {"WARN",    LEVEL::WARN},
            {"HIGH",    LEVEL::HIGH},
            {"ERR",     LEVEL::ERR},
            {"ERROR",   LEVEL::ERR},
            {"OFF",     LEVEL::OFF},
            {"NONE",    LEVEL::OFF}};
        
        std::string str; is >> str;
        std::transform(str.begin(), str.end(), str.begin(), 
            [](unsigned char c){ return std::toupper(c); });
        
        auto it = strToLvl.find(str);
        lvl = (it != strToLvl.end()) ? it->second : LEVEL::TRACE;
        
        return is;
    }
    
    /// Predefined tags for type which can be logged from-the-box
    enum class ArgType { Default, Scalar, Literal };
    
    
    /// Utility class, allows to set/get current thread string name
    class ThreadName {
    public:
        static void setName(std::string name) {
            auto& reg = getRegistry();
            std::lock_guard<decltype(reg.first)> lock(reg.first);
            reg.second[std::this_thread::get_id()] = std::move(name);
        }
        
        static std::string getName(std::thread::id threadId) {
            auto& reg = getRegistry();
            std::lock_guard<decltype(reg.first)> lock(reg.first);
            auto it = reg.second.find(threadId);
            return (it != reg.second.end()) ? it->second : "";
        }
        
    private:
        using Registry = std::unordered_map<std::thread::id, std::string>;
        using GuardedRegistry = std::pair<std::mutex, Registry>;
        
        static GuardedRegistry& getRegistry() {
            static GuardedRegistry reg_;
            return reg_;
        }
    };
    
    
    namespace detail {
        
        /// Abstract interface used while deserialization
        struct VirtualCodec {
            virtual size_t alignment() const = 0;
            virtual size_t deserialize(const void*, std::ostream&) const = 0;
            
            /// Default formatted deserialization simply uses deserialize(const void*, std::ostream&)
            /// TODO: not implemented
            virtual size_t deserialize(const void* src, std::ostream& out, const char*) const {
                return deserialize(src, out); }
        };
        
        class CodecRegistry {
        public:
            /// Adds VirtualCodec for corresponding type_index to registry
            static void registerCodec(std::type_index idx, std::unique_ptr<VirtualCodec>&& ptr) {
                auto& reg = getRegistry();
                std::lock_guard<decltype(reg.first)> lock(reg.first);
                reg.second[idx] = std::move(ptr);
            }
            
            /// @returns pointer to VirtualCodec by type_index
            static const VirtualCodec* getCodec(std::type_index idx) {
                auto& reg = getRegistry();
                std::lock_guard<decltype(reg.first)> lock(reg.first);
                return reg.second[idx].get();
            }
            
        private:
            using Registry = std::unordered_map<std::type_index, std::unique_ptr<VirtualCodec>>;
            using GuardedRegistry = std::pair<std::mutex, Registry>;
            
            /// @returns reference to global codec registry
            static GuardedRegistry& getRegistry() {
                static GuardedRegistry reg_;
                return reg_;
            }
        };
        
        /// Auxiliary proxy, assumed to be used as static member variable,
        /// constructs and registers virtual codec instance inside constructor.
        template <class Codec>
        struct Initializer {
        private:
            struct SpecificCodec : public VirtualCodec {
                virtual size_t alignment() const override {
                    return Codec::alignment; }
                
                virtual size_t deserialize(const void* src, std::ostream& out) const override {
                    return Codec::deserialize(src, out); }
            };
            
        public:
            Initializer() {
                std::type_index idx(typeid(typename Codec::type));
                CodecRegistry::registerCodec(idx, std::make_unique<SpecificCodec>());
            }
        };
        
        
        /// Lightweight RAII holder, invokes T pseudodestructor
        /// within own destructor if hadn't been disabled.
        template <class T>
        struct Destructor {
            explicit Destructor(const T* ptr) :
                ptr_(ptr) {}
            
            void disable() {
                ptr_ = nullptr; }
            
            ~Destructor() {
                if(ptr_) ptr_->~T(); }
        
        private:
            const T* ptr_;
        };
        
        
        /// Instantiates for string literals, provides compile time info.
        /// TODO: filter non-character types.
        template <class>
        struct LiteralTraits;
        
        template <class T, size_t N>
        struct LiteralTraits<const T[N]> {
            using underlying_type = T;
            enum : size_t { size = N };
        };
        
        
        template <class T>
        struct ArgTraits {
        private:
            template <class U>
            static auto probeS(int) -> decltype(std::declval<std::ostream&>() << std::declval<U>(), std::true_type());

            template <class>
            static auto probeS(...) -> std::false_type;
            
            template <class U>
            static auto probeL(int) -> decltype(detail::LiteralTraits<U>::size, std::true_type());
            
            template <class U>
            static auto probeL(...) -> std::false_type;
            
        public:
            static constexpr bool is_streamable = decltype(probeS<T>(0))::value;
            static constexpr bool is_literal    = decltype(probeL<T>(0))::value;
            static constexpr bool is_scalar     = std::is_scalar<T>::value;
            
            static constexpr unsigned checksum = // mask with appropriate bit set
                ((is_streamable << 2) + (is_literal << 1) + (is_scalar));
        };
        
        
        template <class T>
        struct DefaultType {
            using Traits = ArgTraits<T>;
            static_assert(Traits::is_streamable,
                "Type is not loggable, please provide Codec<T> specialization "
                "or std::ostream& operator<<(std::ostream&, const T&) overload");
            
            constexpr static ArgType value =
                Traits::is_literal  ? ArgType::Literal  :
                Traits::is_scalar   ? ArgType::Scalar   : ArgType::Default;
        };
        
        
        /// Uses to mark failed log messages while serialization
        struct FailTag {};
        
        
        /// Lightweight std::decay, removes reference and cv-qualifiers
        template <class T>
        using simplify_t = std::remove_cv_t<std::remove_reference_t<T>>;
        
    } //detail
    
    
    /// Performs all necessary initialization for codec T when instantiated.
    /// Every codec MUST force instantiation with expression: `(void)(CodecManager<Codec>::init)`
    template <class Codec>
    struct CodecManager { const static detail::Initializer<Codec> init; };
    
    template <class Codec>
    const detail::Initializer<Codec> CodecManager<Codec>::init;
    
    
    /// Base codec class, should be specialized for valid ("loggable", serializable) types,
    /// will be instantiated with compile time error for invalid (non-serializable) types.
    template <class T, class = void>
    struct Codec { static_assert(!sizeof(T), "Type is not loggable"); };
    
    
    /// Default general-purpose codec, heavy and slow, uses in-place std::string.
    /// Requires overloaded `std::ostream& operator<<(std::ostream&, const T&)`.
    template <class T>
    struct Codec<T, std::enable_if_t<detail::DefaultType<T>::value == ArgType::Default>> {
    private:
        using UnitType = std::string;
    
    public:
        using type = T;
        enum : size_t { alignment = alignof(UnitType) };
        
        /// @returns required binary size for serialization
        constexpr static size_t size(const type&) {
            return sizeof(UnitType); }
        
        /// Serializes argument to in-place constructed std::ostringstream,
        /// @returns occupied binary size
        static size_t serialize(void* dst, const type& obj) {
            /// Forced instantiation + initialization
            (void)(CodecManager<Codec>::init);
            
            std::ostringstream os; os << obj;
            new(dst) UnitType(os.str());
            return sizeof(UnitType);
        }

        /// Deserializes data from src to std::ostream,
        /// @returns binary read size
        static size_t deserialize(const void* src, std::ostream& out) {
            auto unitPtr = static_cast<UnitType const*>(src);
            detail::Destructor<UnitType> dtor(unitPtr);
            out << *unitPtr;
            return sizeof(UnitType);
        }
    };
    
    
    /// Common codec for scalar types (arithmetic, pointers, streamable enumerations)
    template <class T>
    struct Codec<T, std::enable_if_t<detail::DefaultType<T>::value == ArgType::Scalar>> {
        using type = T;
        enum : size_t { alignment = alignof(type) };
        
        constexpr static size_t size(const type&) {
            return sizeof(type); }

        static size_t serialize(void* dst, const type& obj) {
            (void)(CodecManager<Codec>::init);
            new(dst) type(obj);
            return sizeof(type);
        }

        static size_t deserialize(const void* src, std::ostream& out) {
            auto value = *static_cast<const type*>(src);
            out << value;
            return sizeof(type);
        }
        
        static size_t deserialize(const void* src, std::ostream& out, const char* fmt) {
            static_assert(!sizeof(type), "Not implemented");
            return sizeof(type);
        }
    };
    
    
    /// Specialization for string literals (const char[N])
    template <class T>
    struct Codec<T, std::enable_if_t<detail::DefaultType<T>::value == ArgType::Literal>> {
    private:
        using LitTraits = detail::LiteralTraits<T>;
        
    public:
        using type = T;
        enum : size_t { alignment = alignof(typename LitTraits::underlying_type) };
        
        constexpr static size_t size(const type&) {
            return LitTraits::size; }

        static size_t serialize(void* dst, const type& literal) {
            (void)(CodecManager<Codec>::init);
            
            constexpr auto sz = LitTraits::size * sizeof(typename LitTraits::underlying_type);
            std::memcpy(dst, literal, sz);
            return LitTraits::size;
        }

        static size_t deserialize(const void* src, std::ostream& out) {
            auto literalPtr = static_cast<const type*>(src);
            out << *literalPtr;
            return LitTraits::size;
        }
    };
    
    
    /// Specialization for c-style strings
    template <>
    struct Codec<const char*> {
        using type = const char*;
        enum : size_t { alignment = alignof(char) };
        
        static size_t size(const type& str) {
            return std::strlen(str) + 1; }

        static size_t serialize(void* dst, const type& str) {
            (void)(CodecManager<Codec>::init);
            auto sz = size(str);
            std::memcpy(dst, str, sz);
            return sz;
        }

        static size_t deserialize(const void* src, std::ostream& out) {
            auto str = static_cast<type>(src); out << str;
            return size(str);
        }
    };
    
    /// Non-const c-strings
    template <>
    struct Codec<char*> : Codec<const char*> {};
    
    
    /// Specialization for std::string
    template <>
    struct Codec<std::string> {
        using type = std::string;
        enum : size_t { alignment = alignof(char) };
        
        /// NOTE: std::strlen because of null-terminators
        static size_t size(const type& str) {
            return std::strlen(str.c_str()) + 1; }
        
        static size_t serialize(void* dst, const type& str) {
            (void)(CodecManager<Codec>::init);
            auto sz = size(str);
            std::memcpy(dst, str.c_str(), sz);
            return sz;
        }
        
        static size_t deserialize(const void* src, std::ostream& out) {
            auto str = static_cast<const char*>(src); out << str;
            return size(str);
        }
    };
    
    
    namespace detail {
        
        struct MetaCodec {
        private:
            using Header = std::type_index;
            
            static void* shift(void*& ptr, size_t offset) {
                ptr = static_cast<ByteType*>(ptr) + offset;
                return ptr;
            }
            
            static void* align(void*& ptr, size_t alignment) {
                size_t space = std::numeric_limits<size_t>::max();
                return std::align(alignment, 1, ptr, space);
            }
            
            static size_t distance(void* bgn, void* end) {
                return static_cast<ByteType*>(end) - static_cast<ByteType*>(bgn); }
            
        public:
            template <class T>
            static size_t size(T&& arg) {
                using ArgCodec = Codec<detail::simplify_t<T>>;
                std::aligned_storage_t<1, alignof(Header)> header;
                
                void* const begin = &header;
                void* dst = begin;
                
                // Header "writing":
                shift(dst, sizeof(Header));
                
                // Aligning to begining of data:
                if(!align(dst, ArgCodec::alignment))
                    return 0;
                
                // Data "writing":
                shift(dst, ArgCodec::size(arg));
                
                // Aligning to next header:
                if(!align(dst, alignof(Header)))
                    return 0;
                
                // Total size used:
                return distance(begin, dst);
            }
            
            template <class T>
            static size_t serialize(void* dst, T&& arg) {
                using ArgCodec = Codec<detail::simplify_t<T>>;
                void* const begin = dst;
                
                if(!align(dst, alignof(Header)))
                    return 0;
                
                auto idxPtr = new(dst) Header(typeid(detail::FailTag));
                // TODO: Destructor<Header>(idxPtr)?
                shift(dst, sizeof(Header));
                
                if(!align(dst, ArgCodec::alignment))
                    return 0;
                
                auto written = ArgCodec::serialize(dst, std::forward<T>(arg));
                shift(dst, written);
                
                // Serialization done, finalizing header:
                *idxPtr = Header(typeid(typename ArgCodec::type));
                return distance(begin, dst);
            }
            
            static size_t deserialize(const void* src, std::ostream& out) {
                const auto begin = const_cast<void*>(src);
                auto ptr = begin;
                
                if(!align(ptr, alignof(Header)))
                    return 0;
                
                auto idxPtr = static_cast<const Header*>(ptr);
                detail::Destructor<Header> indexDtor(idxPtr);
                if(*idxPtr == Header(typeid(detail::FailTag)))
                    return 0;
                
                shift(ptr, sizeof(Header));
                
                auto vCodec = detail::CodecRegistry::getCodec(*idxPtr);
                if(!vCodec || !align(ptr, vCodec->alignment()))
                    return 0;
                
                auto read = vCodec->deserialize(ptr, out);
                shift(ptr, read);
                
                return distance(begin, ptr);
            }
            
            template<class... T>
            static size_t totalSize(T&&... args) {
                size_t current = 0;
                size_t total = 0;
                bool ok = true;
                
                // Iterating through parameter pack:
                (void)std::initializer_list<int>{((
                    current = (ok ? size(args) : 0),
                    ok = (ok && current != 0),
                    total += current
                ), 0)...};
                
                return ok ? total : 0;
            }
            
            template<class... T>
            static size_t totalSerialize(void* dst, T&&... args) {
                void* const begin = dst;
                
                size_t current = 0;
                bool ok = true;
                
                // Iterating through parameter pack:
                std::initializer_list<int>{((
                    current = (ok ? serialize(dst, std::forward<T>(args)) : 0),
                    ok = (ok && current != 0),
                    shift(dst, current)
                ), 0)...};
                
                // TODO: destruction rollback on exception?
                return ok ? distance(begin, dst) : 0;
            }
            
            static size_t debugDeserialize(const void* src, size_t nArgs, std::ostream& out) {
                const auto begin = const_cast<void*>(src);
                auto ptr = begin;
                
                for(size_t i = 0; i < nArgs; ++i) {
                    auto read = deserialize(ptr, out);
                    if(read == 0)
                        return 0;
                    
                    shift(ptr, read);
                }
                
                return distance(begin, ptr);
            }
        };
        
        
        class LogTransport {
        public:
            virtual std::ostream& stream() = 0;
            virtual void flush() = 0;
        };
        
        class CoutTransport : public LogTransport {
        public:
            CoutTransport() {}
            
            virtual std::ostream& stream() override {
                return std::cout; }
            
            virtual void flush() override {
                std::cout.flush(); }
        };
        
        class RotatingFileTransport : public LogTransport {
        public:
            struct Config {
                std::string directory   {"./"};
                std::string filePrefix  {"log_trace"};
                size_t maxFileSizeMB    {100};
            };
            
            RotatingFileTransport(const Config& cfg) :
                cfg_(cfg) { rotateFile(); }
            
            virtual std::ostream& stream() override {
                return *file_; }
            
            virtual void flush() override {
                file_->flush();
                
                auto fileSize = getFileSize(curName_);
                auto fileSizeMB = fileSize/(1024*1024);
                
                if(fileSizeMB > cfg_.maxFileSizeMB)
                    rotateFile();
                
                file_->flush();
            }
            
        private:
            Config cfg_;
            std::string curName_;
            std::unique_ptr<std::ofstream> file_;
            
            void rotateFile() {
                if(file_ && file_->is_open())
                    file_->close();
                
                // TODO: valgrind reports memory leak here
                
                auto timestamp = getUTCTimestamp();
                std::string newName; newName
                    .append(cfg_.directory) .append("/")
                    .append(cfg_.filePrefix).append("_")
                    .append(timestamp)      .append(".txt");
                
                auto newFile = std::make_unique<std::ofstream>(newName);
                if(!newFile->is_open())
                    throw std::runtime_error("Cannot open log file");
                
                curName_.swap(newName);
                file_.swap(newFile);
            }
            
            static std::string getUTCTimestamp() {
                using namespace std::chrono;
                
                auto now = system_clock::now();
                auto unixTime = system_clock::to_time_t(now);
                std::tm tm = *std::gmtime(&unixTime);
                
                auto us = duration_cast<microseconds>(now.time_since_epoch()).count();
                us = us % 1000000;
                
                std::ostringstream os; os << std::setfill('0')
                    << std::setw(4) << (tm.tm_year + 1900)
                    << std::setw(2) << (tm.tm_mon + 1)
                    << std::setw(2) << tm.tm_mday
                    << 'T'
                    << std::setw(2) << tm.tm_hour
                    << std::setw(2) << tm.tm_min
                    << std::setw(2) << tm.tm_sec
                    << 'Z'
                    << std::setw(6) << us;
                return os.str();
            }
            
            static size_t getFileSize(const std::string& filename) {
                std::ifstream tmp(filename, std::ios::ate | std::ios::binary);
                if(!tmp.is_open())
                    return 0;
                
                std::streampos size = tmp.tellg();
                return size;
            }
        };
        
        class TailFileTransport : public LogTransport {
            using Clock = std::chrono::steady_clock;
        public:
            TailFileTransport(std::string fileName, std::chrono::milliseconds renewPeriod) :
                lastRenew_(Clock::now()), renewPeriod_(renewPeriod), fileName_(fileName)
            { rotateFile(); }
            
            virtual std::ostream& stream() override {
                return file_; }
            
            virtual void flush() override {
                file_.flush();
                
                auto now = Clock::now();
                if(lastRenew_ + renewPeriod_ < now) {
                    rotateFile();
                    lastRenew_ = now;
                }
                
                file_.flush();
            }
                
        private:
            std::ofstream file_;
            Clock::time_point lastRenew_;
            
            const std::chrono::milliseconds renewPeriod_;
            const std::string fileName_;
            
            void rotateFile() {
                if(file_ && file_.is_open())
                    file_.close();
                
                file_.clear();
                file_.open(fileName_, std::ios::out | std::ios::trunc);
                
                if(!file_.is_open())
                    throw std::runtime_error("Cannot open log file");
            }
        };
        
        
        // Desired:
        // LOG(TRACE) << "Ololo!" << 123;
        // logger::print<TRACE>("Ololo! %%", 123);
        // logger::stream<TRACE>() << "Ololo!" << 123;
        
        /// Implementation
        class logAux {
            using SystemClock = std::chrono::system_clock;
            
            /// Single log message
            struct LogEntry {
                SystemClock::time_point timestamp;
                std::thread::id threadId;
                
                char location[23] = {0};
                LEVEL logLevel = LEVEL::OFF;
                
                const char* fmtPtr = nullptr;
                size_t argsN = 0;
                
                /// Beginning of payload: [h|arg1]...[h|arg2]...
                std::aligned_storage_t<1, alignof(std::type_index)> data;
                
                
                LogEntry(SystemClock::time_point when, const char* where, LEVEL lvl) :
                    timestamp(when), threadId(std::this_thread::get_id()), logLevel(lvl)
                {
                    auto size = std::min(sizeof(location) - 1, std::strlen(where));
                    std::memcpy(location, where, size);
                }
            };
            
            /// POD single unit for LogEntry placing
            using PodEntry = std::aligned_storage_t<sizeof(LogEntry), alignof(LogEntry)>;
            
            /// Unit holding aligned PODs
            using StorageUnit = std::unique_ptr<PodEntry[]>;
            
            /// Invisible proxy, prints message to log at d-tor
            template <LEVEL lvl>
            class ProxyStream { friend class logAux;
            public:
                ProxyStream(ProxyStream&& rh) :
                    this_(rh.this_), location_(rh.location_)
                {
                    acc_.swap(rh.acc_);
                    rh.this_ = nullptr;
                }
                
                ~ProxyStream() {
                    if(this_)
                        this_->print<lvl>(location_.c_str(), "%%", acc_.str());
                }
                
                template <class T>
                std::ostream& operator<<(const T& obj) {
                    return acc_ << obj; }
                
            private:
                ProxyStream(logAux* out, const std::string& location) :
                    this_(out), location_(location) {}
                
                logAux* this_;
                std::string location_;
                std::ostringstream acc_;
            };
            
        public:
            struct Config { std::chrono::milliseconds flushPeriod{500}; };
            
            logAux(const Config& cfg) :
                // NOTE: force ThreadName::Registry construction
                cfg_((ThreadName::getName(std::this_thread::get_id()), cfg)),
                fmtSpec_(
                    R"((%%|%[-+#0]?\d*(?:\.\d*)?(?:hh|h|ll|l|j|z|t|L|n)?[diuoxXfFeEgGaAcsp]))",
                    std::regex::optimize),
                logLevel_(LEVEL::TRACE),
                stop_(false),
                flushJob_(std::async(std::launch::async, &logAux::flushCycle, this))
            {}
            
            ~logAux() {
                stop();
                try {
                    if(flushJob_.valid())
                        flushJob_.get();
                    
                    // Flush rest of entries:
                    consumeAndFlush();
                } catch(std::exception& ex) {
                    std::cerr << "Logger exception: " << ex.what() << std::endl;
                } catch(...) {
                    std::cerr << "Logger unknown exception" << std::endl;
                }
            }
            
            logAux(const logAux&)               = delete;
            logAux& operator=(const logAux&)    = delete;
            
            logAux(logAux&&)            = delete;
            logAux& operator=(logAux&&) = delete;
            
            
            void setLevel(LEVEL lvl) {
                logLevel_.store(lvl, std::memory_order_release); }
            
            LEVEL getLevel() const {
                return logLevel_.load(std::memory_order_acquire); }
            
            void addTransport(std::unique_ptr<LogTransport>&& transport, LEVEL lvl) {
                std::lock_guard<decltype(transportMtx_)> lock(transportMtx_);
                transports_.emplace_back(lvl, std::move(transport));
            }
            
            void stop() noexcept {
                stop_.store(true, std::memory_order_release);
                cv_.notify_one();
            }
            
            template <LEVEL lvl = LEVEL::TRACE, size_t fmtSize, class... Args>
            void print(const char* location, const char (&fmt)[fmtSize], Args&&... args) {
                if(lvl < logLevel_.load(std::memory_order_acquire))
                    return;
                
                auto now = SystemClock::now();
                auto argsSize = MetaCodec::totalSize(args...);
                
                auto unit = makeUnit(now, location, lvl, argsSize);
                auto& entry = *reinterpret_cast<LogEntry*>(&unit[0]);
                detail::Destructor<LogEntry> entryDtor(&entry);
                
                entry.fmtPtr = &fmt[0];
                entry.argsN = sizeof...(Args);
                
                auto written = MetaCodec::totalSerialize(&entry.data, std::forward<Args>(args)...);
                if(entry.argsN != 0 && written == 0)
                    return;
                
                std::lock_guard<decltype(queueMtx_)> lock(queueMtx_);
                queue_.emplace_back(std::move(unit));
                entryDtor.disable();
            }
            
            template <LEVEL lvl>
            [[deprecated("TODO: apply filter")]]
            ProxyStream<lvl> stream(const std::string& location) {
                // TODO: filter by logLevel_ before stream construction
                return {this, location};
            }
            
        private:
            const Config cfg_;
            const std::regex fmtSpec_;
            std::atomic<LEVEL> logLevel_;
            
            std::mutex queueMtx_;
            std::deque<StorageUnit> queue_;
            
            std::mutex transportMtx_;
            using TransportUnit = std::pair<LEVEL, std::unique_ptr<LogTransport>>;
            std::vector<TransportUnit> transports_;
            
            std::mutex jobMtx_;
            std::condition_variable cv_;
            std::atomic<bool> stop_;
            std::future<void> flushJob_;
            
            
            void consumeAndFlush() {
                decltype(queue_) slice;
                
                {
                    std::lock_guard<decltype(queueMtx_)> lock(queueMtx_);
                    slice.swap(queue_);
                }
                
                if(!slice.empty()) {
                    // Restoring timestamp order (async logging may cause entries mixing):
                    std::sort(slice.begin(), slice.end(), [](const auto& lh, const auto& rh){
                        return asLogEntry(lh).timestamp < asLogEntry(rh).timestamp; });
                    
                    std::ostringstream ss;
                    std::string resultStr;
                    
                    for(const auto& unit : slice) {
                        const auto& entry = asLogEntry(unit);
                        detail::Destructor<LogEntry> entryDtor(&entry);
                        
                        ss.clear(); ss.str("");
                        deserializeEntry(entry, ss);
                        resultStr.assign(ss.str());
                        
                        std::lock_guard<decltype(transportMtx_)> lock(transportMtx_);
                        for(auto& tr : transports_)
                            if(tr.first <= entry.logLevel)
                                tr.second->stream() << resultStr;
                    }
                }
                
                {
                    std::lock_guard<decltype(transportMtx_)> lock(transportMtx_);
                    for(auto& tr : transports_)
                        tr.second->flush();
                }
            }
            
            void flushCycle() {
                using namespace std::chrono;
                using SteadyClock = std::chrono::steady_clock;
                
                auto flushPeriod = cfg_.flushPeriod;
                auto lastFlush = SteadyClock::now();
                
                while(!stop_.load(std::memory_order_acquire)) {
                    auto sleepUntil = lastFlush + flushPeriod;
                    
                    std::unique_lock<decltype(jobMtx_)> lock(jobMtx_);
                    auto status = cv_.wait_until(lock, sleepUntil);
                    lock.unlock();
                    
                    auto now = SteadyClock::now();
                    
                    if(status == std::cv_status::timeout) {
                        try {
                            consumeAndFlush();
                            lastFlush = now;
                        } catch(std::exception& ex) {
                            std::cerr << "Flush exception: " << ex.what() << std::endl;
                        } catch(...) {
                            std::cerr << "Flush unknown exception" << std::endl;
                        }
                    }
                }
            }
            
            bool deserializeEntry(const LogEntry& entry, std::ostream& out) {
                {
                    using namespace std::chrono;
                    
                    auto t = SystemClock::to_time_t(entry.timestamp);
                    std::tm tm = *std::gmtime(&t);
                    
                    auto us = duration_cast<microseconds>(entry.timestamp.time_since_epoch()).count();
                    us = us % 1000000;
                    
                    std::ostringstream os; os << std::setfill('0')
                        << std::setw(2) << (tm.tm_year - 100) << '.'
                        << std::setw(2) << (tm.tm_mon + 1) << '.'
                        << std::setw(2) << tm.tm_mday      << ' '
                        << std::setw(2) << tm.tm_hour      << ':'
                        << std::setw(2) << tm.tm_min       << ':'
                        << std::setw(2) << tm.tm_sec       << '.'
                        << std::setw(6) << us;
                    out << os.str();
                }
                
                out << std::left << " ["
                    << std::setw(6) << entry.logLevel << "]["
                    << std::setw(6) << threadIdToString(entry.threadId, 6) << "]["
                    << std::setw(6) << ThreadName::getName(entry.threadId) << "]["
                    << entry.location << "] ";
                
                auto fmt = entry.fmtPtr;
                size_t argsLeft = entry.argsN;
                auto data = reinterpret_cast<const ByteType*>(&entry.data);
                
                std::cmatch match;
                bool argsOverflow = false;
                
                while(argsLeft > 0) {
                    if(std::regex_search(fmt, match, fmtSpec_)) {
                        std::copy(fmt, match[0].first, std::ostreambuf_iterator<char>(out));
                        fmt += match.position() + match.length();
                    } else if(!argsOverflow) {
                        out << fmt << "<ERR_ARGS_OVERFLOW>";
                        fmt += std::strlen(fmt);
                        argsOverflow = true;
                    }
                    
                    // Deserialize + append unconditionally:
                    auto readSz = MetaCodec::deserialize(data, out);
                    
                    if(readSz == 0) {
                        argsLeft = 0;
                        out << "<ERR_DESERIALIZE>";
                    } else {
                        --argsLeft;
                        data += readSz;
                    }
                }
                
                // Rest of format string:
                // TODO: detect args underflow
                out << fmt;
                return true;
            }
            
            static std::string threadIdToString(std::thread::id id, size_t len) {
                std::ostringstream os; os << id;
                auto res = os.str();
                
                if(res.size() <= len)
                    res.resize(len, ' ');
                else
                    res = res.substr(res.size() - len);
                
                return res;
            }
            
            static size_t unitsRequired(size_t payloadSz) {
                auto total = sizeof(PodEntry) + alignof(std::type_index) + payloadSz;
                return (total - 1) / sizeof(PodEntry) + 1;
            }
            
            static StorageUnit makeUnit(SystemClock::time_point when, const char* where, LEVEL lvl, size_t payloadSz) {
                auto uptr = std::make_unique<PodEntry[]>(unitsRequired(payloadSz));
                new(&uptr[0]) LogEntry(when, where, lvl);
                return uptr;
            }
            
            static const LogEntry& asLogEntry(const StorageUnit& unit) {
                return *reinterpret_cast<LogEntry const*>(&unit[0]); }
        };
        
        inline logAux& globalLogger() {
            static logAux globalLogger_(logAux::Config{});
            return globalLogger_;
        }
        
    } // detail
    
    using detail::LogTransport;
    using detail::CoutTransport;
    using detail::RotatingFileTransport;
    using detail::TailFileTransport;
    
    inline void addTransport(std::unique_ptr<LogTransport>&& transport, LEVEL lvl) {
        detail::globalLogger().addTransport(std::move(transport), lvl); }
    
    inline void setLevel(Logger::LEVEL lvl) {
        detail::globalLogger().setLevel(lvl); }
    
    template <Logger::LEVEL lvl, class... Args>
    inline void print(Args&&... args) {
        detail::globalLogger().print<lvl>(std::forward<Args>(args)...); }
    
    /*/! Make it more useful by filter
    template <Logger::LEVEL lvl>
    inline decltype(auto) logStream(const std::string& location) {
        return detail::globalLogger().stream<lvl>(location); }
    //*/
    
} // Logger
} // Ax

#endif // guard AX_LOGGER_HPP
