#include "kernel/sarosLazyFoldPolicy.h"
#include <cassert>
using Kernel::SarosLazyFoldPolicy::NeedActivation;
static_assert(NeedActivation(true,false,0x1000000000ull,512ull<<30u));
static_assert(!NeedActivation(true,true,0x1000000000ull,512ull<<30u));
static_assert(!NeedActivation(false,false,0x1000000000ull,512ull<<30u));
static_assert(!NeedActivation(true,false,0x1000000000ull,1ull<<30u));
int main(){assert(NeedActivation(true,false,0x1000000000ull,512ull<<30u));}
