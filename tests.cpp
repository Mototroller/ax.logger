#include <vector>

#include <ax.logger.hpp>

template <class... Args>
void test(unsigned nThreads, unsigned nRows, const Args&... args) {
    using namespace std::chrono;
    using namespace Ax::Logger;
    
    using Clock = steady_clock;
    
    constexpr auto fmtLen = sizeof...(Args) * 3 + 2;
    static char fmtStr[fmtLen] = {0};
    
    for(unsigned i = 0; i < sizeof...(Args); ++i) {
        auto dst = fmtStr + i * 3;
        std::strcpy(dst, "%% ");
    }
    
    fmtStr[fmtLen - 2] = '\n';
    
    
    std::atomic<unsigned> ready(0);
    std::atomic<bool> start(false);
    
    std::vector<std::future<nanoseconds>> jobs;
    jobs.reserve(nThreads);
    
    for(unsigned id = 0; id < nThreads; ++id) {
        jobs.emplace_back(std::async(std::launch::async, [&, id]{
            ThreadName::setName(std::to_string(id));
            Clock::time_point bgn, end;
            
            ++ready;
            
            while(!start);
            
            // Warmup
            bgn = Clock::now();
            for(unsigned i = 0; i < nRows/10 + 1; ++i)
                print<LEVEL::TRACE>(__func__, fmtStr, args...);
            end = Clock::now();
            
            // Control
            bgn = Clock::now();
            for(unsigned i = 0; i < nRows; ++i)
                print<LEVEL::TRACE>(__func__, fmtStr, args...);
            end = Clock::now();
            
            return duration_cast<nanoseconds>(end - bgn)/nRows;
        }));
    }
    
    while(ready != nThreads)
        std::this_thread::yield();
    
    start = true;
    std::this_thread::yield();
    
    std::ostringstream ss;
    (void)std::initializer_list<int>{((ss << args << " "), 0)...};
    std::cout << nThreads << " threads, args = " << ss.str() << std::endl;
    
    for(auto& job : jobs) {
        auto nsPerRow = job.get();
        std::cout << nsPerRow.count() << " ns/row" << std::endl;
    }
}

int main() {
    using namespace Ax::Logger;
    
    ThreadName::setName("main");
    
    {
        RotatingFileTransport::Config trCfg{};
        auto transport = std::make_unique<RotatingFileTransport>(trCfg);
        addTransport(std::move(transport), LEVEL::TRACE);
        
        print<LEVEL::TRACE>(__func__, "Hello, %s\n", "global!");
    }
    
    for(unsigned i = 1; i <= 4; ++i) {
        test(i, 1000);
        test(i, 1000, "Single literal 1");
        test(i, 1000, 1, 2.0, '3', "4", 0x5, 6, 7.0, '8', "9", 0xA);
    }
    
    return 0;
}
