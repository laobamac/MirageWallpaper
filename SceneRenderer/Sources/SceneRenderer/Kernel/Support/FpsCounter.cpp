module;

module sr.utils;
import sr.core;
import rstd.cppstd;

using namespace sr;
using namespace std::chrono;

FpsCounter::FpsCounter(): m_fps(0), m_frameCount(0), m_startTime(steady_clock::now()) {}

namespace
{
constexpr seconds timeout { 2 };
}

void FpsCounter::RegisterFrame() {
    auto now  = steady_clock::now();
    auto diff = now - m_startTime;

    m_frameCount++;
    if (diff > timeout) {
        m_fps        = (u32)(m_frameCount / duration<double>(diff).count());
        m_frameCount = 0;
        m_startTime  = now;
        std::cerr << m_fps << std::endl;
    }
}
