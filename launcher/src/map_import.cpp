#include "map_document.h"
#include <algorithm>
#include <bit>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
namespace mapedit {
namespace {
void need(bool v, const char *m) {
  if (!v)
    throw std::runtime_error(m);
}
uint32_t word(const std::vector<uint8_t> &b, uint64_t p) {
  need(p + 4 <= b.size(), "Import field outside source map");
  return uint32_t(b[p]) << 24 | uint32_t(b[p + 1]) << 16 |
         uint32_t(b[p + 2]) << 8 | b[p + 3];
}
std::vector<uint32_t> list(const std::vector<uint8_t> &b, uint32_t f) {
  std::vector<uint32_t> out;
  std::set<uint32_t> seen;
  for (auto p = word(b, f); p; p = word(b, p)) {
    need(p + uint64_t(4) <= b.size() && seen.insert(p).second &&
             out.size() < 50000,
         "Invalid import source list");
    out.push_back(p);
  }
  return out;
}
std::string text(const std::vector<uint8_t> &b, uint32_t p) {
  need(p < b.size(), "Import name outside source");
  std::string s;
  for (unsigned k = 0; k < 1024 && p + k < b.size(); ++k) {
    if (!b[p + k])
      return s;
    s += char(b[p + k]);
  }
  throw std::runtime_error("Invalid import name");
}
std::string lower(std::string s) {
  for (auto &c : s)
    if (c >= 'A' && c <= 'Z')
      c += 32;
  return s;
}
} // namespace
size_t Document::import_object(const Document &donor, size_t index,
                               std::array<float, 3> position) {
  need(&donor != this, "Use Duplicate for the current map");
  need(index < donor.objects_.size() && donor.can_duplicate(index),
       "Unsupported source-map template");
  need(donor.data_ == donor.base_ &&
           donor.relocations_ == donor.base_relocations_,
       "Import templates must come from an unchanged original map");
  if(donor.is_editor_logic(index)) {
    const auto& from=donor.objects_[index];auto spec=donor.logic_settings(word(donor.data_,from.offset+20));need(bool(spec),"Authored logic settings are invalid");uint32_t created=0;
    transaction([&]{
      uint32_t visual=0;
      if(from.type==4180){const auto target=word(donor.data_,word(donor.data_,from.offset+16)+12);auto it=std::find_if(donor.objects_.begin(),donor.objects_.end(),[&](const auto& o){return o.type==4200&&word(donor.data_,o.offset+20)==target;});need(it!=donor.objects_.end(),"Source button visual model is missing");auto model=import_object(donor,size_t(it-donor.objects_.begin()),position);visual=objects_[model].offset;}
      auto added=add_logic(position,*spec);auto p=objects_[added].offset;created=word(data_,p+20);
      if(visual){pointer(p+4,word(data_,visual+4));for(unsigned k=0;k<4;++k)data_[word(data_,p+16)+12+k]=uint8_t(word(data_,visual+20)>>(24-8*k));}
      // Settings are portable; receiver identifiers belong to the source map.
      // Deliberately start with empty outputs, then connect local receivers.
    },true);
    auto found=std::find_if(objects_.begin(),objects_.end(),[&](const auto& o){return word(data_,o.offset+20)==created;});need(found!=objects_.end(),"Imported logic was not linked");return size_t(found-objects_.begin());
  }
  need(objects_.size() < 10000, "Editable object limit reached");
  auto &b = donor.data_;
  auto source = donor.objects_[index].offset;
  auto entities = list(b, 0x40);
  auto type = word(b, source + 12), config = word(b, source + 16);
  if (type == 4040) {
    auto id = word(b, config + 20);
    source = 0;
    for (auto p : entities)
      if (word(b, p + 20) == id) {
        source = p;
        break;
      }
    need(source, "Source spawner target missing");
    type = word(b, source + 12);
    need(type == 5000 || (type >= 2000 && type < 4000),
         "Unsupported source spawner target");
    config = word(b, source + 16);
  }
  auto hash = donor.source_hash();
  uint32_t added = 0;
  transaction(
      [&] {
        auto cached = std::find_if(imports_.begin(), imports_.end(),
                                   [&](auto &m) { return m.hash == hash; });
        if (cached == imports_.end()) {
          need(imports_.size() < 64, "Imported-map limit reached");
          // Keep complete relocation-owned allocation spans, including aliases
          // into vertex/animation pools. Explicit sizes preserve headers split
          // by interior pointer targets. Only object/archive dependencies are
          // roots of this walk; donor terrain, game scripts, lights and actor
          // lists are never activated.
          std::set<uint32_t> targets;
          for (auto f : donor.relocations_)
            targets.insert(word(b, f));
          targets.insert(uint32_t(b.size()));
          std::set<uint32_t> blocked, seeds, visited;
          std::map<uint32_t, uint32_t> minimum, exact;
          auto min = [&](uint32_t p, uint64_t n) {
            if (!p)
              return;
            need(uint64_t(p) + n <= b.size(),
                 "Import dependency exceeds source map");
            minimum[p] = std::max(minimum[p], uint32_t(n));
          };
          auto raw = [&](uint32_t p, uint64_t n) {
            if (!p || !n)
              return;
            min(p, n);
            exact[p] = std::max(exact[p], uint32_t(n));
          };
          auto string = [&](uint32_t p) {
            if (p)
              raw(p, text(b, p).size() + 1);
          };
          auto array = [&](uint32_t p) {
            if (!p)
              return;
            min(p, 16);
            auto bytes = word(b, p + 4);
            need(uint64_t(word(b, p + 8)) * word(b, p + 12) == bytes,
                 "Invalid imported vertex array");
            raw(word(b, p), bytes);
          };
          auto hierarchy = [&](uint32_t p) {
            if (!p)
              return;
            min(p, 100);
            for (auto [field, count, stride] :
                 {std::array<unsigned, 3>{4, 60, 24},
                  {12, 72, 10},
                  {20, 80, 2},
                  {24, 88, 6}})
              raw(word(b, p + field), uint64_t(word(b, p + count)) * stride);
          };
          for (auto p : entities) {
            blocked.insert(p);
            min(p, 44);
            min(word(b, p + 4), 48);
            string(word(b, p + 8));
            auto t = word(b, p + 12);
            if ((t >= 2000 && t < 4000) || t == 5000 || t == 4040 ||
                t == 4140 || t == 4200 || t == 4210 || logic_template_type(t))
              seeds.insert(p);
            if(logic_template_type(t)) {
              auto c=word(b,p+16);min(c,logic_template_config_size(t));
              if(t==4120 && word(b,c+8)) {
                const auto first=word(b,c+8);std::set<uint32_t> keys;auto key=first;
                do{need(key&&keys.insert(key).second&&keys.size()<=4096,"Invalid imported camera path");min(key,60);min(word(b,key+20),48);key=word(b,key+24);}while(key!=first);
              }
            }
          }
          auto visuals = list(b, 0x58);
          for (auto p : visuals) {
            blocked.insert(p);
            min(p, 48);
            min(word(b, p + 4), 48);
            string(word(b, p + 12));
            array(word(b, p + 20));
            min(word(b, p + 24), 20);
            for (auto f : list(b, p + 44)) {
              min(f, 20);
              array(word(b, f + 12));
              min(word(b, f + 16), 20);
            }
          }
          auto sources = list(b, 0x5c);
          for (auto p : sources) {
            blocked.insert(p);
            min(p, 44);
            for (auto f : {16u, 20u, 24u, 28u})
              array(word(b, p + f));
            hierarchy(word(b, p + 36));
            for (auto s : list(b, p + 4))
              min(s, 80);
          }
          for (auto p : entities)
            if (word(b, p + 12) == 4200) {
              auto c = word(b, p + 16);
              if (c && word(b, c) < visuals.size())
                seeds.insert(visuals[word(b, c)]);
            }
          auto textures = list(b, 12);
          for (auto p : textures) {
            blocked.insert(p);
            min(p, 40);
          }
          for (auto field : {8u, 0x48u, 0x4cu})
            for (auto p : list(b, field))
              blocked.insert(p);
          auto archive = word(b, 0x7c);
          auto records =
              archive ? list(b, archive + 8) : std::vector<uint32_t>{};
          for (auto p : records) {
            blocked.insert(p);
            min(p, 32);
            string(word(b, p + 4));
            seeds.insert(p);
            auto asset = word(b, p + 12);
            if (word(b, p + 8) == 0) {
              min(asset, 80);
              for (auto f : {28u, 32u, 36u, 40u})
                array(word(b, asset + f));
              hierarchy(word(b, asset + 76));
              for (auto m : list(b, asset + 8))
                min(m, 80);
              for (auto bone : list(b, asset + 48)) {
                min(bone, 52);
                auto n = word(b, bone + 12), nn = word(b, bone + 40);
                raw(word(b, bone + 16), uint64_t(n) * 12);
                raw(word(b, bone + 20), uint64_t(n) * 4);
                raw(word(b, bone + 24), uint64_t(n) * 4);
                raw(word(b, bone + 44), uint64_t(nn) * 12);
                raw(word(b, bone + 48), uint64_t(nn) * 4);
              }
              for (auto clip : list(b, asset + 52)) {
                min(clip, 24);
                min(word(b, clip + 16), 20);
              }
              auto packed = word(b, asset + 68);
              if (packed) {
                min(packed, 20);
                raw(word(b, packed), uint64_t(word(b, packed + 4)) * 20);
              }
            } else
              min(asset, 12);
          }
          std::vector<std::pair<uint32_t, uint32_t>> ranges;
          std::vector<uint32_t> pending(seeds.begin(), seeds.end());
          while (!pending.empty()) {
            auto p = pending.back();
            pending.pop_back();
            if (!p || !visited.insert(p).second)
              continue;
            need(p >= 128 && p < b.size(),
                 "Import dependency points into source root");
            auto next = targets.upper_bound(p);
            need(next != targets.end(), "Import dependency has no extent");
            uint64_t end = exact.contains(p) ? uint64_t(p) + exact[p] : *next;
            end = std::max(end, uint64_t(p) + minimum[p]);
            need(end <= b.size() && end > p,
                 "Invalid import allocation extent");
            ranges.emplace_back(p, uint32_t(end));
            for (auto it = donor.relocations_.lower_bound(p);
                 it != donor.relocations_.end() && uint64_t(*it) + 4 <= end;
                 ++it)
              if (!blocked.contains(*it))
                pending.push_back(word(b, *it));
          }
          std::sort(ranges.begin(), ranges.end());
          ImportedMap package;
          package.hash = hash;
          package.size = uint32_t(b.size());
          for (auto [a, z] : ranges) {
            if (!package.segments.empty() && a <= package.segments.back().end)
              package.segments.back().end =
                  std::max(z, package.segments.back().end);
            else
              package.segments.push_back({a, z, 0});
          }
          uint64_t total = 0;
          for (auto &r : package.segments)
            total += r.end - r.source + 31;
          need(data_.size() + total < 128u * 1024 * 1024,
               "Import exceeds map size limit");
          for (auto &r : package.segments) {
            while (data_.size() % 32)
              data_.push_back(0);
            r.target = uint32_t(data_.size());
            data_.insert(data_.end(), b.begin() + r.source, b.begin() + r.end);
          }
          auto mapped = [&](uint32_t p) {
            auto it = std::upper_bound(
                package.segments.begin(), package.segments.end(), p,
                [](auto p, auto &r) { return p < r.source; });
            need(it != package.segments.begin(), "Unowned imported pointer");
            --it;
            need(p < it->end, "Import dependency was not copied");
            return it->target + p - it->source;
          };
          for (auto &r : package.segments)
            for (auto it = donor.relocations_.lower_bound(r.source);
                 it != donor.relocations_.end() && uint64_t(*it) + 4 <= r.end;
                 ++it)
              pointer(r.target + *it - r.source,
                      blocked.contains(*it) ? 0 : mapped(word(b, *it)));
          auto append = [&](uint32_t field, uint32_t node) {
            auto existing = list(data_, field);
            pointer(node, 0);
            pointer(existing.empty() ? field : existing.back(), node);
          };
          for (auto p : textures)
            if (visited.contains(p))
              append(12, mapped(p));
          for (auto p : sources)
            if (visited.contains(p))
              append(0x5c, mapped(p));
          if (!records.empty()) {
            auto to = word(data_, 0x7c);
            if (!to) {
              while (data_.size() % 4)
                data_.push_back(0);
              to = uint32_t(data_.size());
              data_.resize(data_.size() + 28);
              data_[to + 3] = 1;
              data_[to + 7] = 100;
              pointer(0x7c, to);
            }
            std::set<std::pair<uint32_t, std::string>> present;
            for (auto p : list(data_, to + 8))
              present.emplace(word(data_, p + 8),
                              lower(text(data_, word(data_, p + 4))));
            for (auto p : records)
              if (present
                      .emplace(word(b, p + 8), lower(text(b, word(b, p + 4))))
                      .second)
                append(to + 8, mapped(p));
          }
          imports_.push_back(std::move(package));
          cached = std::prev(imports_.end());
        }
        need(cached->size == b.size(), "Imported source identity mismatch");
        auto mapped = [&](uint32_t p) {
          auto it =
              std::upper_bound(cached->segments.begin(), cached->segments.end(),
                               p, [](auto p, auto &r) { return p < r.source; });
          need(it != cached->segments.begin(),
               "Template missing from import cache");
          --it;
          need(p < it->end &&
                   uint64_t(it->target) + it->end - it->source <= data_.size(),
               "Invalid import cache range");
          return it->target + p - it->source;
        };
        uint32_t visual = 0, destruction = 0;
        if (type == 4200) {
          auto nodes = list(b, 0x58);
          auto idx = word(b, config);
          need(idx < nodes.size(), "Source prop model missing");
          visual = mapped(nodes[idx]);
          auto id = word(b, config + 8);
          for (auto p : entities)
            if (word(b, p + 20) == id && word(b, p + 12) == 4210) {
              destruction = mapped(p);
              break;
            }
        }
        added =
            clone_entity(mapped(source), position, true, visual, destruction);
      },
      true);
  auto it = std::find_if(objects_.begin(), objects_.end(),
                         [&](auto &o) { return o.offset == added; });
  need(it != objects_.end(), "Imported object was not linked");
  return size_t(it - objects_.begin());
}
} // namespace mapedit
