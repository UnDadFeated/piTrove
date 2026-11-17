#pragma once
#include "chores.h"
#include <string>
namespace chores {
struct HttpResponse { int status=200; std::string body; };
// Auth is applied by the main router before entering this handler.
HttpResponse handle_http(Store&, const std::string& method, const std::string& path,
                         const std::string& request, const std::string& today, const std::string& timezone);
}
