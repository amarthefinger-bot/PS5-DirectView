#include "FrameGeneration.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
void require(bool ok) { if (!ok) std::abort(); }
int main() {
    using Step=FrameGenerationCadence::Step;
    FrameGenerationCadence c;
    require(c.Next(false,60,1)==Step::Passthrough);
    require(c.Next(true,30,2)==Step::Passthrough);
    require(c.Next(true,120,3)==Step::Passthrough);
    require(c.Next(true,std::numeric_limits<double>::quiet_NaN(),4)==Step::Passthrough);
    require(c.Next(true,59.94,5)==Step::Seed);
    require(c.Next(true,59.94,6)==Step::Endpoint);
    require(c.Next(true,59.94,7)==Step::Midpoint);
    require(c.Next(true,59.94,8)==Step::Endpoint);
    // A missed capture frame must discard the pair, not interpolate across it.
    require(c.Next(true,60,10)==Step::Seed);
    require(c.Next(true,60,11)==Step::Endpoint);
    require(c.Next(true,60,12)==Step::Midpoint);
    require(c.Next(true,60,1)==Step::Seed); // source restart
    c.Reset();
    require(c.Next(true,60,2)==Step::Seed);
    int generated=0;
    for (unsigned i=3;i<=62;++i) if(c.Next(true,60,i)==Step::Midpoint) ++generated;
    require(generated==30);
    puts("Frame generation cadence tests passed");
}
