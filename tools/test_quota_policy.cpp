// Boundary and repeated-checkpoint storage accounting, independently bounded.
#include "../port/wiiu/quota_policy.h"
#include <cassert>
#include <iostream>
using namespace wiiu::quota_policy;
int main(){
  constexpr uint64_t mib=1ull<<20;
  // A checkpoint worker writes a separate temporary file while the last state
  // remains intact. It must budget both, including a retained backup.
  auto q=required(16*mib,15*mib,0,96*mib);
  assert(q>=98*mib && q%unit==0);
  assert(required(q,q-97*mib,0,96*mib)>q);
  // Replacing a file must not charge its existing extent twice.
  assert(required(16*mib,2*mib,10*mib,11*mib)==16*mib);
  assert(required(16*mib,17*mib,0,1)==0);
  assert(required(maximum,0,0,1)==0);
  assert(required(16*mib,16*mib,0,~uint64_t{})==0);
  for(uint64_t quota=4*mib;quota<=256*mib;quota+=unit)
    for(uint64_t free=0;free<=quota;free+=mib)
      for(uint64_t incoming : {uint64_t{0},mib,95*mib,256*mib}){
        const auto result=required(quota,free,0,incoming);
        assert(result>=quota && result<=maximum && result%unit==0);
        assert(result-quota+free>=incoming+headroom);
      }
  std::cout<<"quota growth: checkpoint overlap, overwrite and capacity boundaries passed\n";
}
