#include "dump_serialize.hpp"

#include <fmt/core.h>

#include <cereal/archives/binary.hpp>
#include <cereal/types/set.hpp>
#include <fstream>
#include <phosphor-logging/log.hpp>

namespace phosphor
{
namespace dump
{
namespace elog
{

using namespace phosphor::logging;

void serialize(const ElogList& list, const fs::path& dir)
{
    std::ofstream os(dir.c_str(), std::ios::binary);
    cereal::BinaryOutputArchive oarchive(os);
    oarchive(list);
}

bool deserialize(const fs::path& path, ElogList& list)
{
    try
    {
        if (fs::exists(path))
        {
            std::ifstream is(path.c_str(), std::ios::in | std::ios::binary);
            cereal::BinaryInputArchive iarchive(is);
            iarchive(list);
            return true;
        }
        return false;
    }
    catch (cereal::Exception& e)
    {
<<<<<<< HEAD
        log<level::ERR>(e.what());
        fs::remove(path);
||||||| 0af74a5
        log<level::ERR>(e.what());
        std::filesystem::remove(path);
=======
        log<level::ERR>(
            fmt::format("Failed to deserialize, errormsg({})", e.what())
                .c_str());
        std::filesystem::remove(path);
>>>>>>> origin/master
        return false;
    }
}

} // namespace elog
} // namespace dump
} // namespace phosphor
