#include <cassert>
#include "darkmoon_policy.h"
int main(){
 using namespace wiiu_render;
 assert(darkmoon(0x0004000000055f00ULL));assert(!darkmoon(0));
 auto b=darkmoon_policy(0,720);assert(b.fps==30&&b.force==1&&b.scale==0.85f);
 auto q=darkmoon_policy(1,720);assert(q.fps==30&&q.force==0&&q.scale==1.0f);
 auto f=darkmoon_policy(2,720);assert(f.fps==0&&f.force==1&&f.scale==2.0f/3);
 for(int mode=0;mode<3;mode++){auto p=darkmoon_policy(mode,480);assert(p.force==0&&p.scale==1);}
 assert(darkmoon_policy(-1,720).scale==b.scale);assert(darkmoon_policy(9,720).fps==30);
 assert(darkmoon_policy(0,1080).force==-1);
}
