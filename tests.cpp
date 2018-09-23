#include <ax.logger.hpp>

int main() {
    using namespace Ax::Logger;
    
    Ax::Logger::detail::logAux::Config cfg;
    Ax::Logger::detail::logAux logger(cfg);
    
    RotatingFileTransport::Config trCfg{};
    std::unique_ptr<RotatingFileTransport> fileTransport(new RotatingFileTransport(trCfg));
    logger.addTransport(std::move(fileTransport), LEVEL::TRACE);
    
    logger.print<LEVEL::TRACE>(__func__, "Hello, %%!", "world");
    
    return 0;
}
