#include "graphics/presentation/videoOutResolutionPolicy.h"

using Libs::VideoOut::OutputResolutionPolicy::Select;

static_assert(Select(1280, 720, false, false) == 2u); // Legacy non-detectable PS5 title
static_assert(Select(1280, 720, false, true) == 1u);  // UFC6 HD output policy
static_assert(Select(1920, 1080, false, true) == 1u);
static_assert(Select(2560, 1440, false, true) == 2u); // Must not lie about UHD output
static_assert(Select(3840, 2160, true, true) == 2u);
static_assert(Select(1280, 720, true, false) == 1u); // Existing ATTRIBUTE3 behaviour
static_assert(Select(0, 0, true, true) == 2u);       // Invalid dimensions are not HD

int main() { return 0; }
