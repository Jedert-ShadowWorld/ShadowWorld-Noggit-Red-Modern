#include <noggit/formats/RetailMapTexture.hpp>
#include <vector>
#include <iostream>
int main()
{
  for (unsigned encoding : {0u,1u,7u})
  {
    unsigned block=encoding==0?8:16;
    std::vector<unsigned char> b(148+block*2);
    std::memcpy(b.data(),"BLP2",4);
    auto put=[&](unsigned p,unsigned n){std::memcpy(b.data()+p,&n,4);};
    put(4,1); b[8]=2; b[10]=encoding; b[11]=2;
    put(12,2);put(16,2);put(20,148);put(24,148+block);put(84,block);put(88,block);
    auto before=b;
    Noggit::Formats::finalizeRetailMapTexture(b.data(),b.size());
    if(b[9]!=(encoding==0?0:8) || b[11]!=1) return 1;
    for(unsigned i=0;i<b.size();++i) if(i!=9 && i!=11 && b[i]!=before[i]) return 2;
    put(88,block-1);
    bool rejected=false;
    try { Noggit::Formats::finalizeRetailMapTexture(b.data(),b.size()); }
    catch(std::runtime_error const&) { rejected=true; }
    if(!rejected) return 3;
  }
  std::cout<<"Retail BLP decoder metadata and mip bounds verified\n";
}
