#include "storage/page.h"
#include <algorithm>
#include <cstring>

namespace minidb {

Page::Page() {
    std::memset(data_, 0, PAGE_SIZE);
}

void Page::Init(page_id_t pid) {
    std::memset(data_, 0, PAGE_SIZE);
    page_id_      = pid;
    num_records_  = 0;
    free_offset_  = HEADER_SIZE;
    next_page_id_ = INVALID_PAGE_ID;
    slots_.clear();
    SerializeHeader();
}

uint16_t Page::FreeSpace() const {
    // Space between end of slot directory and the next record offset
    uint16_t slot_dir_end = HEADER_SIZE
        + static_cast<uint16_t>(slots_.size()) * 6; // 6 bytes per slot entry (2+2+2)
    if (free_offset_ < slot_dir_end) return 0;
    return PAGE_SIZE - free_offset_ - static_cast<uint16_t>(slots_.size()) * 6;
}

int Page::InsertRecord(const char* data, uint16_t len) {
    // We need space for: the record data + one new slot entry (6 bytes)
    uint16_t needed = len + 6;
    
    // Calculate available space
    uint16_t slot_dir_end = HEADER_SIZE
        + static_cast<uint16_t>(slots_.size()) * 6;
    uint16_t data_start = free_offset_;
    
    // Records are stored starting from free_offset_ going forward
    uint16_t available = (PAGE_SIZE > data_start + slot_dir_end + 6) 
                         ? (PAGE_SIZE - data_start - static_cast<uint16_t>(slots_.size() + 1) * 6 - HEADER_SIZE)
                         : 0;
    
    // Simple check: can we fit the record?
    uint16_t total_used = HEADER_SIZE 
        + static_cast<uint16_t>(slots_.size() + 1) * 6 
        + (free_offset_ - HEADER_SIZE) + len;
    
    if (total_used > PAGE_SIZE) {
        return -1; // no space
    }

    // Check for a reusable deleted slot
    int reuse_slot = -1;
    for (int i = 0; i < static_cast<int>(slots_.size()); i++) {
        if (!slots_[i].valid) {
            reuse_slot = i;
            break;
        }
    }

    // Write record data at free_offset_
    uint16_t record_offset = free_offset_;
    std::memcpy(data_ + record_offset, data, len);
    free_offset_ += len;

    if (reuse_slot >= 0) {
        slots_[reuse_slot] = {record_offset, len, true};
        num_records_++;
        SerializeHeader();
        return reuse_slot;
    }

    // Add new slot
    slots_.push_back({record_offset, len, true});
    num_records_++;
    SerializeHeader();
    return static_cast<int>(slots_.size()) - 1;
}

bool Page::GetRecord(slot_id_t slot, std::vector<char>& out) const {
    if (slot >= slots_.size() || !slots_[slot].valid) return false;
    const auto& s = slots_[slot];
    out.resize(s.length);
    std::memcpy(out.data(), data_ + s.offset, s.length);
    return true;
}

bool Page::DeleteRecord(slot_id_t slot) {
    if (slot >= slots_.size() || !slots_[slot].valid) return false;
    slots_[slot].valid = false;
    num_records_--;
    SerializeHeader();
    return true;
}

void Page::SerializeHeader() {
    // Write header fields into data_ buffer for disk persistence
    uint16_t off = 0;
    std::memcpy(data_ + off, &num_records_, 2);  off += 2;
    std::memcpy(data_ + off, &free_offset_, 2);  off += 2;
    std::memcpy(data_ + off, &next_page_id_, 4); off += 4;

    // Slot directory follows header
    // We'll store slot count + entries in a reserved area at end of page
    // Actually, for simplicity we serialize the full page from our in-memory
    // structures when writing to disk. The raw data_ already has records
    // embedded. We just need to save/restore the slot directory.
    
    // Store slot count at offset PAGE_SIZE - 2
    uint16_t num_slots = static_cast<uint16_t>(slots_.size());
    std::memcpy(data_ + PAGE_SIZE - 2, &num_slots, 2);

    // Store slot entries at PAGE_SIZE - 2 - num_slots * 6
    uint16_t slot_base = PAGE_SIZE - 2 - num_slots * 6;
    for (uint16_t i = 0; i < num_slots; i++) {
        uint16_t pos = slot_base + i * 6;
        std::memcpy(data_ + pos, &slots_[i].offset, 2);
        std::memcpy(data_ + pos + 2, &slots_[i].length, 2);
        uint16_t v = slots_[i].valid ? 1 : 0;
        std::memcpy(data_ + pos + 4, &v, 2);
    }
}

void Page::DeserializeHeader() {
    uint16_t off = 0;
    std::memcpy(&num_records_,  data_ + off, 2); off += 2;
    std::memcpy(&free_offset_,  data_ + off, 2); off += 2;
    std::memcpy(&next_page_id_, data_ + off, 4); off += 4;

    // Read slot count from end of page
    uint16_t num_slots = 0;
    std::memcpy(&num_slots, data_ + PAGE_SIZE - 2, 2);

    slots_.resize(num_slots);
    uint16_t slot_base = PAGE_SIZE - 2 - num_slots * 6;
    for (uint16_t i = 0; i < num_slots; i++) {
        uint16_t pos = slot_base + i * 6;
        std::memcpy(&slots_[i].offset, data_ + pos, 2);
        std::memcpy(&slots_[i].length, data_ + pos + 2, 2);
        uint16_t v = 0;
        std::memcpy(&v, data_ + pos + 4, 2);
        slots_[i].valid = (v != 0);
    }
}

} // namespace minidb
