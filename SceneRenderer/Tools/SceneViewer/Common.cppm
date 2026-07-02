module;

export module viewer.common;

import rstd.cppstd;

export import :arg;

export namespace viewer
{

std::filesystem::path ExecutableDir(const char* argv0);

} // namespace viewer
