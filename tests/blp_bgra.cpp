#include <noggit/formats/BlpBgra.hpp>
#include <fstream>
#include <iostream>
#include <iterator>

void check(bool ok,char const* message){if(!ok)throw std::runtime_error(message);}
void word(std::vector<char>& data,std::size_t offset,std::uint32_t value)
{for(unsigned i=0;i<4;++i)data[offset+i]=char((value>>(8*i))&255);}
std::vector<char> fixture()
{
  std::vector<char> bytes(164);word(bytes,0,0x32504c42);word(bytes,4,1);bytes[8]=3;bytes[9]=8;bytes[10]=8;
  word(bytes,12,3);word(bytes,16,1);word(bytes,20,148);word(bytes,84,12);word(bytes,24,160);word(bytes,88,4);
  unsigned char const pixels[]={1,2,3,0, 17,31,63,128, 10,20,30,255, 90,80,70,60};
  std::copy(pixels,pixels+16,bytes.begin()+148);return bytes;
}
int main(int argc,char** argv)
{
  try
  {
    auto bytes=fixture();auto decoded=Noggit::Format::read_bgra_blp(bytes.data(),bytes.size());
    check(decoded.width==3&&decoded.height==1&&decoded.mipmaps.size()==2,"Rectangular/tiny BGRA mip layout wrong");
    check(decoded.mipmaps.at(0)==std::vector<std::uint32_t>({0x00010203,0x80111f3f,0xff0a141e}),"BGRA channels or alpha changed");
    check(decoded.mipmaps.at(1).at(0)==0x3c5a5046,"1x1 BGRA mip decoded incorrectly");
    auto rejects=[](std::vector<char> data){bool failed=false;try{Noggit::Format::read_bgra_blp(data.data(),data.size());}catch(std::runtime_error const&){failed=true;}check(failed,"Malformed BGRA input accepted");};
    auto bad=bytes;bad.resize(147);rejects(bad);
    bad=bytes;word(bad,12,0);rejects(bad);
    bad=bytes;word(bad,12,0xffffffff);rejects(bad);
    bad=bytes;word(bad,20,0xfffffff0);rejects(bad);
    bad=bytes;word(bad,84,11);rejects(bad);
    bad=bytes;word(bad,24,152);rejects(bad);
    bad=bytes;word(bad,20,0);word(bad,84,0);rejects(bad);
    bad=bytes;bad[8]=2;rejects(bad);
    if(argc==2)
    {
      std::ifstream file(argv[1],std::ios::binary);check(bool(file),"Cannot open local BLP fixture");
      std::vector<char> actual((std::istreambuf_iterator<char>(file)),{});
      auto const image=Noggit::Format::read_bgra_blp(actual.data(),actual.size());
      auto read=[&](std::size_t at){std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=std::uint32_t(static_cast<unsigned char>(actual[at+i]))<<(i*8);return v;};
      std::size_t pixels=0;
      for(auto const& mip:image.mipmaps)
      {
        auto offset=read(20+4*mip.first);
        for(std::size_t i=0;i<mip.second.size();++i)
        {
          auto const* bgra=reinterpret_cast<unsigned char const*>(actual.data()+offset+i*4);
          auto const rgba=mip.second[i];
          check((rgba&255)==bgra[2]&&((rgba>>8)&255)==bgra[1]&&((rgba>>16)&255)==bgra[0]&&(rgba>>24)==bgra[3],"Actual client BGRA pixel differs");
          ++pixels;
        }
      }
      std::cout<<"PASS: actual BLP "<<image.width<<'x'<<image.height<<", "<<image.mipmaps.size()<<" mips, "<<pixels<<" pixels preserve RGBA\n";
    }
    std::cout<<"PASS: BGRA BLP pixel channels/alpha, tiny and rectangular mips, malformed bounds\n";return 0;
  }
  catch(std::exception const& e){std::cerr<<e.what()<<'\n';return 1;}
}
