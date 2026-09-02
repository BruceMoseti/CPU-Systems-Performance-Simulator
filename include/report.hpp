// Human-readable rendering of a simulation result.
#pragma once

#include <string>

#include "simulator.hpp"

namespace perfsim {

std::string format_report(const Results& results);

}  // namespace perfsim
