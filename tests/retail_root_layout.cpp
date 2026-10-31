#include <noggit/formats/adt/RetailRootLayout.hpp>
using Bytes=std::vector<std::uint8_t>;
void check(bool b){if(!b)throw std::runtime_error("Retail root layout regression");}
std::uint32_t read(Bytes const& b,unsigned p){std::uint32_t n;std::memcpy(&n,b.data()+p,4);return n;}
void put(Bytes& b,unsigned p,std::uint32_t n){std::memcpy(b.data()+p,&n,4);}
void add(Bytes& b,char const* tag,unsigned n){auto p=b.size();b.resize(p+8+n);for(unsigned i=0;i<4;++i)b[p+i]=tag[3-i];put(b,p+4,n);}
int main()
{
  using Noggit::Formats::ADT::finalizeRetailRootLayout;
  Bytes b(128);
  add(b,"MCVT",580);add(b,"MCNR",435);add(b,"MCCV",580);add(b,"MCSE",0);
  put(b,88,99999);put(b,116,1167);
  auto fixed=finalizeRetailRootLayout(b);
  check(fixed.size()==b.size()+13 && read(fixed,720)==448);
  check(read(fixed,20)==136 && read(fixed,24)==724 && read(fixed,116)==1180);
  check(read(fixed,88)==0 && read(fixed,92)==0);
  check(finalizeRetailRootLayout(fixed)==fixed);
  put(b,0,0x10000);put(b,20,0xaabbccdd);put(b,24,0x12345678);
  fixed=finalizeRetailRootLayout(b);
  check(read(fixed,20)==0xaabbccdd && read(fixed,24)==0x12345678);
  b.pop_back();bool rejected=false;
  try{finalizeRetailRootLayout(b);}catch(std::runtime_error const&){rejected=true;}check(rejected);
}
