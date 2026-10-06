#pragma once

#include <string>
#include <string_view>

namespace gygax::service {

bool evaluateExpression(std::string_view text, double& result, std::string& error);

}
