#include <noggit/RetailM2Animation.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>
using namespace Noggit::RetailM2;
void check(bool b) { if (!b) throw std::runtime_error("test failed"); }
template<class F> void rejects(F f) { bool failed = false; try { f(); } catch (std::runtime_error const&) { failed = true; } check(failed); }
Bytes chunk(std::string tag, Bytes data) { Bytes b(8); std::memcpy(b.data(), tag.data(), 4); put(b,4,static_cast<std::uint32_t>(data.size())); append(b,data); return b; }
Bytes fixture(std::uint32_t parent, std::uint16_t animation, std::uint32_t external, float value)
{
  Bytes bones(104); put<std::uint32_t>(bones,0,1); put<std::uint32_t>(bones,4,16); put<std::int16_t>(bones,24,-1);
  for (int t=0;t<3;++t) put<std::int16_t>(bones,16+16+t*20+2,-1);
  auto time = static_cast<std::uint32_t>(bones.size()); bones.resize(bones.size()+8); put<std::uint32_t>(bones,time,1);
  auto keys = static_cast<std::uint32_t>(bones.size()); bones.resize(bones.size()+8); put<std::uint32_t>(bones,keys,1);
  auto td = static_cast<std::uint32_t>(bones.size()); bones.resize(bones.size()+4); put<std::uint32_t>(bones,td,0);
  auto kd = static_cast<std::uint32_t>(bones.size()); bones.resize(bones.size()+12); put<float>(bones,kd,value);
  put<std::uint32_t>(bones,time+4,external?0:td); put<std::uint32_t>(bones,keys+4,external?4:kd);
  put<std::uint32_t>(bones,36,1); put(bones,40,time); put<std::uint32_t>(bones,44,1); put(bones,48,keys);
  Bytes sequences(96); put<std::uint32_t>(sequences,8,1); put<std::uint32_t>(sequences,12,32);
  put(sequences,32,animation); put<std::uint32_t>(sequences,36,100); put<std::uint32_t>(sequences,44,external?0:0x20);
  Bytes result; append(result,chunk("SKB1",bones)); append(result,chunk("SKS1",sequences));
  if(parent) { Bytes pd(16);put(pd,8,parent);append(result,chunk("SKPD",pd)); }
  if(external) { Bytes af(8);put(af,0,animation);put(af,4,external);append(result,chunk("AFID",af)); }
  return result;
}
int main(int argc,char** argv)
{
  if(argc==3 || argc==4)
  {
    auto read=[&](std::uint32_t id) { std::ifstream file(std::filesystem::path(argv[1])/(std::to_string(id)+".bin"),std::ios::binary); if(!file) throw std::runtime_error("missing fixture "+std::to_string(id)); return Bytes(std::istreambuf_iterator<char>(file),{}); };
    Bytes model(304);
    if (argc == 4)
    {
      std::ifstream source(argv[3],std::ios::binary);
      if(!source) throw std::runtime_error("Cannot read M2 fixture");
      Bytes container(std::istreambuf_iterator<char>(source),{});
      if(container.size()>=4 && !std::memcmp(container.data(),"MD21",4)) model=chunks(container).at("MD21");
      else model=std::move(container);
    }
    applySkeleton(model,static_cast<std::uint32_t>(std::stoul(argv[2])),read);
    std::cout << "Real SKEL validated: bones="<<get<std::uint32_t>(model,44)<<", animations="<<get<std::uint32_t>(model,28)<<", materialized bytes="<<model.size()<<'\n'; return 0;
  }
  Bytes raw(16);put<float>(raw,4,42);auto anim=chunk("AFSB",raw);append(anim,chunk("AFSA",Bytes(3,'a')));
  check(animationPayload(anim,"AFSB")==raw);check(animationPayload(anim,"AFSA")==Bytes(3,'a'));
  rejects([&]{animationPayload(anim,"AFM2");});check(animationPayload(chunk("AFM2",raw),"AFSB")==raw);
  rejects([&]{chunks(Bytes(7));}); rejects([&]{animationIds(Bytes(7));});
  auto broken=anim;put<std::uint32_t>(broken,4,10000);rejects([&]{chunks(broken);});
  std::map<std::uint32_t,Bytes> files{{1,fixture(0,0,0,10)},{2,fixture(1,0,3,0)},{3,anim}};
  int reads=0;auto read=[&](std::uint32_t id){++reads;return files.at(id);};Bytes model(304);applySkeleton(model,2,read);
  check(reads==3);check(get<std::uint32_t>(model,44)==1);check(get<std::uint32_t>(model,28)==1);
  auto bone=get<std::uint32_t>(model,48), kh=get<std::uint32_t>(model,bone+32), ko=get<std::uint32_t>(model,kh+4);check(get<float>(model,ko)==42);
  files[2]=fixture(1,1,3,0);model.assign(304,0);applySkeleton(model,2,read);check(get<std::uint32_t>(model,28)==2);
  bone=get<std::uint32_t>(model,48);kh=get<std::uint32_t>(model,bone+32);ko=get<std::uint32_t>(model,kh+4);check(get<float>(model,ko)==10);ko=get<std::uint32_t>(model,kh+12);check(get<float>(model,ko)==42);
  // Parent/child materialization preserves pre-existing M2 loop indices.
  model.assign(312,0); put<std::uint32_t>(model,20,2); put<std::uint32_t>(model,24,304);
  put<std::int32_t>(model,304,123); put<std::int32_t>(model,308,123); applySkeleton(model,2,read);
  check(get<std::uint32_t>(model,20)==2); auto go=get<std::uint32_t>(model,24);
  check(get<std::int32_t>(model,go)==123 && get<std::int32_t>(model,go+4)==123);
  auto parts=chunks(fixture(0,0,0,10)); auto& sq=parts.at("SKS1");
  auto alias=slice(sq,32,64); put<std::uint16_t>(alias,0,1);put<std::uint32_t>(alias,4,0);put<std::uint32_t>(alias,12,0x40);put<std::uint16_t>(alias,62,0);
  append(sq,alias); put<std::uint32_t>(sq,8,2); Bytes aliased;
  for(auto const& part:parts) append(aliased,chunk(part.first,part.second));
  files[4]=aliased;model.assign(304,0);applySkeleton(model,4,read);
  auto so=get<std::uint32_t>(model,32);check(get<std::uint32_t>(model,so+64+4)==100);
  auto nb=get<std::uint32_t>(model,48);auto tk=get<std::uint32_t>(model,nb+32);
  check(get<std::uint32_t>(model,tk+4)==get<std::uint32_t>(model,tk+12)); // aliases reuse bytes
  // A child may animate a different global loop or intentionally have no
  // keys. Metadata remains per sequence, rather than imposing parent's mode.
  auto varied=chunks(fixture(1,1,0,55));auto& vb=varied.at("SKB1");auto& vs=varied.at("SKS1");
  put<std::int16_t>(vb,32,1);put<std::int16_t>(vb,34,0);
  auto globalOff=static_cast<std::uint32_t>(vs.size());vs.resize(vs.size()+4);put<std::int32_t>(vs,globalOff,777);
  put<std::uint32_t>(vs,0,1);put(vs,4,globalOff);Bytes variant;
  for(auto const& part:varied) append(variant,chunk(part.first,part.second));files[5]=variant;
  model.assign(304,0);Animation::TrackModes modes;applySkeleton(model,5,read,&modes);
  nb=get<std::uint32_t>(model,48);auto th=get<std::uint32_t>(model,nb+24);auto const& sm=modes.at(th);
  check(sm.size()==2 && sm[0].interpolation==0 && sm[0].globalSequence==-1 && sm[1].interpolation==1 && sm[1].globalSequence==0);
  check(get<std::int32_t>(model,get<std::uint32_t>(model,24))==777);
  put<std::uint32_t>(vb,36,0);put<std::uint32_t>(vb,44,0);variant.clear();
  for(auto const& part:varied) append(variant,chunk(part.first,part.second));files[6]=variant;
  model.assign(304,0);modes.clear();applySkeleton(model,6,read,&modes);
  nb=get<std::uint32_t>(model,48);tk=get<std::uint32_t>(model,nb+32);
  check(get<std::uint32_t>(model,tk)==1 && get<std::uint32_t>(model,tk+8)==0);
  files[1]=fixture(2,0,0,10);rejects([&]{applySkeleton(model,2,read);});
  auto sk=skeleton(fixture(0,0,0,10));put<std::uint32_t>(sk.sequences,44,0x40);put<std::uint16_t>(sk.sequences,94,0);rejects([&]{resolveAlias(sk,0);});
  put<std::uint16_t>(sk.sequences,94,2);rejects([&]{resolveAlias(sk,0);});
  std::cout<<"Retail skeleton, parent override, external animation, alias and bounds checks passed\n";
}
