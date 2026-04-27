#include "kdump_manager.hpp"

#include <sdbusplus/bus.hpp>

int main()
{
    auto bus = sdbusplus::bus::new_default();
    phosphor::dump::kdump::Manager manager(bus);
    return 0;
}
