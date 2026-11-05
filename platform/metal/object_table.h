#ifndef CANE_PLATFORM_METAL_OBJECT_TABLE_H_
#define CANE_PLATFORM_METAL_OBJECT_TABLE_H_

#include <cstdint>
#include <memory>
#include <vector>

namespace cane::gl_metal {

// GL names start at 1; name 0 is "no object". Records are heap allocated so
// pointers to them stay valid while the table grows.
template <typename Record>
class ObjectTable {
 public:
  uint32_t Create() {
    uint32_t name;
    if (!free_.empty()) {
      name = free_.back();
      free_.pop_back();
    } else {
      name = static_cast<uint32_t>(records_.size());
      records_.emplace_back();
    }
    records_[name] = std::make_unique<Record>();
    return name;
  }

  void Destroy(uint32_t name) {
    if (name == 0 || name >= records_.size() || !records_[name]) return;
    records_[name].reset();
    free_.push_back(name);
  }

  Record* Find(uint32_t name) {
    if (name == 0 || name >= records_.size()) return nullptr;
    return records_[name].get();
  }

  const Record* Find(uint32_t name) const {
    if (name == 0 || name >= records_.size()) return nullptr;
    return records_[name].get();
  }

  bool Exists(uint32_t name) const { return Find(name) != nullptr; }

  // Textures and buffers may be bound by name before glGen*; GL creates them
  // on first bind. Returns the record, creating it when absent.
  Record* FindOrCreate(uint32_t name) {
    if (name == 0) return nullptr;
    if (name >= records_.size()) records_.resize(name + 1);
    if (!records_[name]) records_[name] = std::make_unique<Record>();
    return records_[name].get();
  }

  template <typename Fn>
  void ForEach(Fn&& fn) {
    for (auto& record : records_) {
      if (record) fn(*record);
    }
  }

 private:
  std::vector<std::unique_ptr<Record>> records_{1};
  std::vector<uint32_t> free_;
};

}  // namespace cane::gl_metal

#endif  // CANE_PLATFORM_METAL_OBJECT_TABLE_H_
