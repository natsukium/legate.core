/* Copyright 2021 NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "mapping/instance_manager.h"
#include "utilities/dispatch.h"

namespace legate {
namespace mapping {

using namespace Legion;
using namespace Legion::Mapping;

using RegionGroupP = std::shared_ptr<RegionGroup>;

RegionGroup::RegionGroup(const std::vector<Region>& rs, const Domain bound)
  : regions(rs), bounding_box(bound)
{
}

RegionGroup::RegionGroup(std::vector<Region>&& rs, const Domain bound)
  : regions(std::move(rs)), bounding_box(bound)
{
}

bool InstanceSet::find_instance(Region region, Instance& result) const
{
  auto finder = groups_.find(region);
  if (finder == groups_.end()) return false;
  auto& group  = finder->second;
  auto ifinder = instances_.find(group);
  assert(ifinder != instances_.end());
  result = ifinder->second;
  return true;
}

// We define "too big" as the size of the "unused" points being bigger than the intersection
static inline bool too_big(size_t union_volume,
                           size_t my_volume,
                           size_t group_volume,
                           size_t intersect_volume)
{
  return (union_volume - (my_volume + group_volume - intersect_volume)) > intersect_volume;
}

struct construct_overlapping_region_group_fn {
  template <int32_t DIM>
  RegionGroupP operator()(
    const InstanceSet::Region& region,
    const InstanceSet::Domain& domain,
    const std::map<InstanceSet::RegionGroupP, InstanceSet::Instance>& instances)
  {
    auto bound = domain.bounds<DIM, coord_t>();
    std::vector<InstanceSet::Region> regions(1, region);

    for (const auto& pair : instances) {
      auto& group = pair.first;

      Rect<DIM> group_bbox = group->bounding_box;
      auto intersect       = bound.intersection(group_bbox);
      if (intersect.empty()) continue;

      // Don't merge if the unused space would be more than the space saved
      auto union_bbox  = bound.union_bbox(group_bbox);
      size_t bound_vol = bound.volume();
      size_t union_vol = union_bbox.volume();

      // If it didn't get any bigger then we can keep going
      if (bound_vol == union_vol) continue;

      // Only allow merging if it isn't "too big"
      if (too_big(union_vol, bound_vol, group_bbox.volume(), intersect.volume())) continue;

      regions.insert(regions.end(), group->regions.begin(), group->regions.end());
      bound = union_bbox;
    }

    return std::make_shared<RegionGroup>(std::move(regions), InstanceSet::Domain(bound));
  }
};

RegionGroupP InstanceSet::construct_overlapping_region_group(const Region& region,
                                                             const Domain& domain) const
{
  return dim_dispatch(
    domain.get_dim(), construct_overlapping_region_group_fn{}, region, domain, instances_);
}

std::set<InstanceSet::Instance> InstanceSet::record_instance(RegionGroupP group, Instance instance)
{
  assert(instances_.find(group) == instances_.end());
  instances_[group] = instance;

  std::set<Instance> replaced;
  for (auto& region : group->regions) {
    auto finder = groups_.find(region);
    if (finder == groups_.end())
      groups_[region] = group;
    else {
      replaced.insert(instances_[finder->second]);
      finder->second = group;
    }
  }
  replaced.erase(instance);
  return std::move(replaced);
}

bool InstanceSet::erase(PhysicalInstance inst)
{
  std::set<RegionGroupP> filtered_groups;
  for (auto it = instances_.begin(); it != instances_.end(); /*nothing*/) {
    if (it->second == inst) {
      auto to_erase = it++;
      filtered_groups.insert(to_erase->first);
      instances_.erase(to_erase);
    } else
      it++;
  }

  for (auto& group : filtered_groups)
    for (auto& region : group->regions) groups_.erase(region);

  return instances_.empty();
}

size_t InstanceSet::get_instance_size() const
{
  size_t sum = 0;
  for (auto& pair : instances_) sum += pair.second.get_instance_size();
  return sum;
}

bool InstanceManager::find_instance(Region region,
                                    FieldID field_id,
                                    Memory memory,
                                    Instance& result)
{
  auto finder = instance_sets_.find(FieldMemInfo(region.get_tree_id(), field_id, memory));
  return finder != instance_sets_.end() && finder->second.find_instance(region, result);
}

RegionGroupP InstanceManager::find_region_group(const Region& region,
                                                const Domain& domain,
                                                FieldID field_id,
                                                Memory memory)
{
  FieldMemInfo key(region.get_tree_id(), field_id, memory);

  auto finder = instance_sets_.find(key);
  if (finder == instance_sets_.end())
    return std::make_shared<RegionGroup>(std::vector<Region>({region}), domain);

  return finder->second.construct_overlapping_region_group(region, domain);
}

std::set<InstanceManager::Instance> InstanceManager::record_instance(RegionGroupP group,
                                                                     FieldID fid,
                                                                     Instance instance)
{
  const auto mem = instance.get_location();
  const auto tid = instance.get_tree_id();

  FieldMemInfo key(tid, fid, mem);
  return instance_sets_[key].record_instance(group, instance);
}

void InstanceManager::erase(PhysicalInstance inst)
{
  const auto mem = inst.get_location();
  const auto tid = inst.get_tree_id();

  for (auto fit = instance_sets_.begin(); fit != instance_sets_.end(); /*nothing*/) {
    if ((fit->first.memory != mem) || (fit->first.tid != tid)) {
      fit++;
      continue;
    }
    if (fit->second.erase(inst)) {
      auto to_erase = fit++;
      instance_sets_.erase(to_erase);
    } else
      fit++;
  }
}

std::map<Legion::Memory, size_t> InstanceManager::aggregate_instance_sizes() const
{
  std::map<Legion::Memory, size_t> result;
  for (auto& pair : instance_sets_) {
    auto& memory = pair.first.memory;
    if (result.find(memory) == result.end()) result[memory] = 0;
    result[memory] += pair.second.get_instance_size();
  }
  return result;
}

}  // namespace mapping
}  // namespace legate
