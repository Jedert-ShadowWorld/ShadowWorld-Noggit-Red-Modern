// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

// Retail chunk layouts and child-skeleton precedence follow wow.export.
// See docs/third-party/wow.export-MIT.txt for attribution.
#include "AnimationTrackMode.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace Noggit::RetailM2
{
  using Bytes = std::vector<char>;
  using Reader = std::function<Bytes(std::uint32_t)>;
  using AnimationIds = std::map<std::pair<std::uint16_t, std::uint16_t>, std::uint32_t>;
  inline void range(Bytes const& b, std::size_t off, std::size_t count, std::size_t stride = 1)
  {
    if (!stride || off > b.size() || count > (b.size() - off) / stride)
      throw std::runtime_error("Retail M2/SKEL animation array exceeds its payload");
  }
  template<class T> T get(Bytes const& b, std::size_t off)
  {
    range(b, off, sizeof(T)); T v; std::memcpy(&v, b.data() + off, sizeof(T)); return v;
  }
  template<class T> void put(Bytes& b, std::size_t off, T v)
  {
    range(b, off, sizeof(T)); std::memcpy(b.data() + off, &v, sizeof(T));
  }
  inline std::uint32_t append(Bytes& b, Bytes const& data)
  {
    if (b.size() + data.size() > 512u * 1024u * 1024u)
      throw std::runtime_error("Retail animation working data exceeds 512 MiB");
    auto off = static_cast<std::uint32_t>(b.size()); b.insert(b.end(), data.begin(), data.end()); return off;
  }
  inline Bytes slice(Bytes const& b, std::size_t off, std::size_t size)
  {
    range(b, off, size); return Bytes(b.begin() + off, b.begin() + off + size);
  }
  inline std::map<std::string, Bytes> chunks(Bytes const& b)
  {
    std::map<std::string, Bytes> result;
    for (std::size_t off = 0; off < b.size();)
    {
      range(b, off, 8); auto size = get<std::uint32_t>(b, off + 4);
      std::string tag(b.data() + off, 4);
      if (!result.emplace(tag, slice(b, off + 8, size)).second)
        throw std::runtime_error("Duplicate Retail animation chunk: " + tag);
      off += 8 + std::size_t(size);
    }
    return result;
  }
  inline AnimationIds animationIds(Bytes const& b)
  {
    if (b.size() % 8) throw std::runtime_error("Truncated AFID entry");
    AnimationIds ids;
    for (std::size_t i = 0; i < b.size(); i += 8)
      ids[{get<std::uint16_t>(b, i), get<std::uint16_t>(b, i + 2)}] = get<std::uint32_t>(b, i + 4);
    return ids;
  }
  inline Bytes animationPayload(Bytes const& b, std::string const& preferred)
  {
    if (b.size() < 4 || (std::memcmp(b.data(), "AFM2", 4) && std::memcmp(b.data(), "AFSB", 4) && std::memcmp(b.data(), "AFSA", 4)))
      return b; // Older raw .anim payloads remain valid in Retail.
    auto parts = chunks(b);
    auto it = parts.find(preferred);
    if (it == parts.end()) it = parts.find("AFM2");
    if (it == parts.end()) throw std::runtime_error("Animation container lacks " + preferred + " or AFM2");
    return it->second;
  }
  struct Skeleton
  {
    Bytes bones, sequences, attachments;
    std::uint32_t boneCount = 0, boneOffset = 0, sequenceCount = 0, sequenceOffset = 0;
    std::uint32_t parent = 0;
    AnimationIds ids;
    std::vector<std::int32_t> globals;
  };
  inline Skeleton skeleton(Bytes const& data)
  {
    auto parts = chunks(data); Skeleton s;
    if (parts.count("SKPD")) s.parent = get<std::uint32_t>(parts.at("SKPD"), 8);
    if (parts.count("SKB1"))
    {
      s.bones = parts.at("SKB1"); s.boneCount = get<std::uint32_t>(s.bones, 0); s.boneOffset = get<std::uint32_t>(s.bones, 4);
      range(s.bones, s.boneOffset, s.boneCount, 88);
    }
    if (parts.count("SKS1"))
    {
      s.sequences = parts.at("SKS1"); auto n = get<std::uint32_t>(s.sequences, 0), off = get<std::uint32_t>(s.sequences, 4);
      range(s.sequences, off, n, 4);
      for (std::uint32_t i = 0; i < n; ++i) s.globals.push_back(get<std::int32_t>(s.sequences, off + i * 4));
      s.sequenceCount = get<std::uint32_t>(s.sequences, 8); s.sequenceOffset = get<std::uint32_t>(s.sequences, 12);
      range(s.sequences, s.sequenceOffset, s.sequenceCount, 64);
    }
    if (parts.count("AFID")) s.ids = animationIds(parts.at("AFID"));
    if (parts.count("SKA1"))
    {
      s.attachments = parts.at("SKA1");
      range(s.attachments, get<std::uint32_t>(s.attachments, 4), get<std::uint32_t>(s.attachments, 0), 40);
      range(s.attachments, get<std::uint32_t>(s.attachments, 12), get<std::uint32_t>(s.attachments, 8), 2);
    }
    return s;
  }
  inline std::size_t resolveAlias(Skeleton const& s, std::size_t index)
  {
    std::set<std::size_t> visited;
    while (true)
    {
      if (index >= s.sequenceCount || !visited.insert(index).second)
        throw std::runtime_error("Retail animation alias is cyclic or out of range");
      auto off = s.sequenceOffset + index * 64;
      if (!(get<std::uint32_t>(s.sequences, off + 12) & 0x40)) return index;
      index = get<std::uint16_t>(s.sequences, off + 62);
    }
  }
  // Materialize bone tracks into the existing M2 reader's address space. Geometry,
  // collision, particle definitions and texture tracks keep their original offsets.
  inline void applySkeleton(Bytes& model, std::uint32_t id, Reader const& read, Animation::TrackModes* modes = nullptr)
  {
    std::vector<Skeleton> chain; std::set<std::uint32_t> seen;
    while (id)
    {
      if (chain.size() >= 16 || !seen.insert(id).second) throw std::runtime_error("SKPD parent cycle or excessive depth");
      chain.push_back(skeleton(read(id))); id = chain.back().parent;
    }
    if (chain.empty() || !chain.back().boneCount) throw std::runtime_error("External skeleton has no base bones");
    auto const& base = chain.back();
    std::vector<std::pair<std::size_t, std::size_t>> selected;
    std::map<std::pair<std::uint16_t, std::uint16_t>, std::size_t> positions;
    for (std::size_t rev = chain.size(); rev-- > 0;)
    {
      auto const& s = chain[rev];
      if (!s.boneCount) continue; // Parent-only metadata has no track overrides.
      for (std::size_t i = 0; i < s.sequenceCount; ++i)
      {
        auto off = s.sequenceOffset + i * 64;
        auto key = std::make_pair(get<std::uint16_t>(s.sequences, off), get<std::uint16_t>(s.sequences, off + 2));
        if (!positions.count(key)) { positions[key] = selected.size(); selected.emplace_back(rev, i); }
        else selected[positions[key]] = {rev, i};
      }
    }
    // Keep indices used by M2 texture/particle tracks. Additional skeleton-only
    // sequences follow those original entries.
    auto originalCount = get<std::uint32_t>(model,28), originalOffset = get<std::uint32_t>(model,32);
    range(model, originalOffset, originalCount, 64);
    std::vector<std::pair<std::size_t, std::size_t>> ordered;
    std::set<std::size_t> consumed;
    for (std::uint32_t i = 0; i < originalCount; ++i)
    {
      auto off = originalOffset + std::size_t(i) * 64;
      auto key = std::make_pair(get<std::uint16_t>(model, off), get<std::uint16_t>(model, off + 2));
      auto it = positions.find(key);
      if (it == positions.end()) throw std::runtime_error("M2 sequence missing from external skeleton");
      ordered.push_back(selected[it->second]); consumed.insert(it->second);
    }
    for (std::size_t i = 0; i < selected.size(); ++i) if (!consumed.count(i)) ordered.push_back(selected[i]);
    selected = std::move(ordered);
    if (selected.size() > 32767) throw std::runtime_error("Too many Retail animation sequences");
    Bytes sequences;
    std::vector<std::size_t> aliases;
    std::map<std::uint32_t, Bytes> external;
    for (auto const& selection : selected)
    {
      auto const& s = chain[selection.first]; auto resolved = resolveAlias(s, selection.second); aliases.push_back(resolved);
      auto record = slice(s.sequences, s.sequenceOffset + selection.second * 64, 64);
      put(record, 4, get<std::uint32_t>(s.sequences, s.sequenceOffset + resolved * 64 + 4));
      put(record, 12, (get<std::uint32_t>(record, 12) & ~0x40u) | 0x20u);
      append(sequences, record);

    }
    // Global loops are local to each skeleton; concatenate and remap each track.
    std::vector<std::vector<std::uint32_t>> globalRemaps; Bytes globals;
    std::map<std::int32_t, std::uint32_t> globalIndices;
    auto originalGlobals = get<std::uint32_t>(model,20), originalGlobalOffset = get<std::uint32_t>(model,24);
    range(model, originalGlobalOffset, originalGlobals, 4);
    globals = slice(model, originalGlobalOffset, std::size_t(originalGlobals) * 4);
    for (std::uint32_t i = 0; i < originalGlobals; ++i)
      globalIndices.emplace(get<std::int32_t>(globals, i * 4), i);
    for (auto const& s : chain)
    {
      globalRemaps.emplace_back();
      for (auto g : s.globals)
      {
        if (!globalIndices.count(g))
        {
          globalIndices[g] = static_cast<std::uint32_t>(globals.size() / 4);
          auto off = globals.size(); globals.resize(off + 4); put(globals, off, g);
        }
        globalRemaps.back().push_back(globalIndices.at(g));
      }
    }
    if (globals.size() / 4 > 32767) throw std::runtime_error("Too many global animation loops");
    std::map<std::tuple<std::uintptr_t, std::size_t, std::size_t>, std::uint32_t> copiedRanges;
    auto copyRange = [&](Bytes const& source, std::size_t offset, std::size_t size)
    {
      auto key = std::make_tuple(reinterpret_cast<std::uintptr_t>(&source), offset, size);
      auto it = copiedRanges.find(key);
      if (it != copiedRanges.end()) return it->second;
      auto result = append(model, slice(source, offset, size));
      copiedRanges.emplace(key, result); return result;
    };
    Bytes bones = slice(base.bones, base.boneOffset, std::size_t(base.boneCount) * 88);
    for (std::size_t bone = 0; bone < base.boneCount; ++bone)
    {
      // Hierarchy is always inherited from the base; child animation data must
      // refer to that same bone numbering.
      for (auto const& s : chain)
        if (s.boneCount && (s.boneCount != base.boneCount || get<std::int16_t>(s.bones, s.boneOffset + bone * 88 + 8) != get<std::int16_t>(bones, bone * 88 + 8)))
          throw std::runtime_error("Child skeleton bone hierarchy differs from parent");
      for (unsigned track = 0; track < 3; ++track)
      {
        auto blockOut = bone * 88 + 16 + track * 20;
        Bytes times, keys; std::int16_t interpolation = -1;
        std::vector<Animation::SequenceMode> sequenceModes;
        std::size_t count = selected.size();
        if (!count) count = 1;
        for (std::size_t a = 0; a < count; ++a)
        {
          auto sourceIndex = selected.empty() ? chain.size() - 1 : selected[a].first;
          if (!chain[sourceIndex].boneCount) sourceIndex = chain.size() - 1;
          auto const& s = chain[sourceIndex]; auto block = s.boneOffset + bone * 88 + 16 + track * 20;
          auto type = get<std::int16_t>(s.bones, block), seq = get<std::int16_t>(s.bones, block + 2);
          if (type < 0 || type > 2) throw std::runtime_error("Unsupported Retail bone interpolation");
          if (seq >= static_cast<std::int32_t>(s.globals.size()) || seq < -1) throw std::runtime_error("Invalid skeleton global sequence");
          auto remappedGlobal = seq < 0 ? -1 : static_cast<std::int32_t>(globalRemaps[sourceIndex][seq]);
          if (interpolation < 0) interpolation = type;
          sequenceModes.push_back({type, static_cast<std::int16_t>(remappedGlobal)});
          auto ai = seq >= 0 ? 0 : (selected.empty() ? 0 : aliases[a]);
          Bytes const* payload = &s.bones;
          std::uint32_t externalId = 0;
          if (seq < 0 && !selected.empty() && ai < s.sequenceCount)
          {
            auto ao = s.sequenceOffset + ai * 64;
            auto it = s.ids.find({get<std::uint16_t>(s.sequences, ao), get<std::uint16_t>(s.sequences, ao + 2)});
            if (!(get<std::uint32_t>(s.sequences, ao + 12) & 0x20) && it != s.ids.end()) externalId = it->second;
          }
          for (unsigned kind = 0; kind < 2; ++kind)
          {
            auto n = get<std::uint32_t>(s.bones, block + 4 + kind * 8), off = get<std::uint32_t>(s.bones, block + 8 + kind * 8);
            range(s.bones, off, n, 8); std::uint32_t entries = 0, outputOffset = 0;
            if (ai < n)
            {
              entries = get<std::uint32_t>(s.bones, off + ai * 8); auto dataOff = get<std::uint32_t>(s.bones, off + ai * 8 + 4);
              if (entries && externalId)
              {
                auto it = external.find(externalId);
                if (it == external.end()) it = external.emplace(externalId, animationPayload(read(externalId), "AFSB")).first;
                payload = &it->second;
              }
              if (entries && seq < 0 && ai < s.sequenceCount
                  && !(get<std::uint32_t>(s.sequences, s.sequenceOffset + ai * 64 + 12) & 0x20) && payload == &s.bones)
                throw std::runtime_error("External skeleton animation has no AFID payload");
              auto stride = kind == 0 ? 4u : (track == 1 ? 8u : 12u) * (type == 2 ? 3u : 1u);
              range(*payload, dataOff, entries, stride);
              outputOffset = copyRange(*payload, dataOff, std::size_t(entries) * stride);
            }
            auto& dest = kind == 0 ? times : keys; auto destOff = dest.size(); dest.resize(destOff + 8);
            put(dest, destOff, entries); put(dest, destOff + 4, outputOffset);
          }
        }
        put(bones, blockOut, interpolation); put<std::int16_t>(bones, blockOut + 2, -1);
        auto timesOffset = append(model, times);
        if (modes) modes->emplace(timesOffset, std::move(sequenceModes));
        put(bones, blockOut + 4, static_cast<std::uint32_t>(times.size() / 8)); put(bones, blockOut + 8, timesOffset);
        put(bones, blockOut + 12, static_cast<std::uint32_t>(keys.size() / 8)); put(bones, blockOut + 16, append(model, keys));
      }
    }
    put(model, 20, static_cast<std::uint32_t>(globals.size() / 4)); put(model, 24, append(model, globals));
    put(model, 28, static_cast<std::uint32_t>(selected.size())); put(model, 32, append(model, sequences));
    put(model, 44, base.boneCount); put(model, 48, append(model, bones));
  }
}
