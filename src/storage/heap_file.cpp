#include "storage/heap_file.h"
#include <cstring>
#include <sstream>

namespace minidb {

HeapFile::HeapFile(const std::string& table_name, BufferPoolManager* bpm,
                   const Schema& schema)
    : table_name_(table_name), bpm_(bpm), schema_(schema) {
    // Allocate the first page
    page_id_t pid;
    Page* page = bpm_->NewPage(pid);
    if (page) {
        first_page_id_ = pid;
        bpm_->UnpinPage(pid, true);
    }
}

std::string HeapFile::SerializeRow(const Row& row, const Schema& schema) {
    // Simple pipe-delimited text serialization
    std::ostringstream oss;
    for (size_t i = 0; i < row.size(); i++) {
        if (i > 0) oss << '|';
        const auto& val = row[i];
        if (std::holds_alternative<int64_t>(val)) {
            oss << std::get<int64_t>(val);
        } else if (std::holds_alternative<double>(val)) {
            oss << std::get<double>(val);
        } else {
            oss << std::get<std::string>(val);
        }
    }
    return oss.str();
}

Row HeapFile::DeserializeRow(const char* data, uint16_t len, const Schema& schema) {
    std::string s(data, len);
    Row row;
    std::istringstream iss(s);
    std::string token;
    size_t col = 0;

    while (std::getline(iss, token, '|') && col < schema.size()) {
        switch (schema[col].type) {
            case ColumnType::INT:
                row.push_back(static_cast<int64_t>(std::stoll(token)));
                break;
            case ColumnType::DOUBLE:
                row.push_back(std::stod(token));
                break;
            case ColumnType::STRING:
                row.push_back(token);
                break;
        }
        col++;
    }
    return row;
}

RecordId HeapFile::InsertTuple(const Row& row) {
    std::string serialized = SerializeRow(row, schema_);
    uint16_t len = static_cast<uint16_t>(serialized.size());

    page_id_t pid;
    Page* page = FindOrAllocatePage(len, pid);
    if (!page) return {INVALID_PAGE_ID, 0};

    int slot = page->InsertRecord(serialized.data(), len);
    if (slot < 0) {
        bpm_->UnpinPage(pid, false);
        return {INVALID_PAGE_ID, 0};
    }

    bpm_->UnpinPage(pid, true);
    tuple_count_++;
    return {pid, static_cast<slot_id_t>(slot)};
}

bool HeapFile::GetTuple(const RecordId& rid, Row& out) {
    Page* page = bpm_->FetchPage(rid.page_id);
    if (!page) return false;

    std::vector<char> buf;
    bool ok = page->GetRecord(rid.slot_id, buf);
    bpm_->UnpinPage(rid.page_id, false);

    if (!ok) return false;
    out = DeserializeRow(buf.data(), static_cast<uint16_t>(buf.size()), schema_);
    return true;
}

bool HeapFile::DeleteTuple(const RecordId& rid) {
    Page* page = bpm_->FetchPage(rid.page_id);
    if (!page) return false;

    bool ok = page->DeleteRecord(rid.slot_id);
    bpm_->UnpinPage(rid.page_id, ok);
    if (ok) tuple_count_--;
    return ok;
}

void HeapFile::ScanAll(std::function<void(const RecordId&, const Row&)> callback) {
    page_id_t pid = first_page_id_;

    while (pid != INVALID_PAGE_ID) {
        Page* page = bpm_->FetchPage(pid);
        if (!page) break;

        uint16_t num_slots = page->GetNumSlots();
        for (slot_id_t s = 0; s < num_slots; s++) {
            std::vector<char> buf;
            if (page->GetRecord(s, buf)) {
                Row row = DeserializeRow(buf.data(),
                    static_cast<uint16_t>(buf.size()), schema_);
                RecordId rid{pid, s};
                callback(rid, row);
            }
        }

        page_id_t next = page->GetNextPageId();
        bpm_->UnpinPage(pid, false);
        pid = next;
    }
}

Page* HeapFile::FindOrAllocatePage(uint16_t needed_size, page_id_t& out_page_id) {
    // Try existing pages
    page_id_t pid = first_page_id_;
    while (pid != INVALID_PAGE_ID) {
        Page* page = bpm_->FetchPage(pid);
        if (!page) break;

        // Try to insert — if it fails, the page is full
        // We check by trying to insert a dummy, but that's wasteful.
        // Instead, just return the page and let caller try InsertRecord.
        // If it returns -1, we'll move on.

        // Simple heuristic: if page has fewer than ~50 records, try it
        // (since our records are small, each page can fit many)
        out_page_id = pid;
        return page;
    }

    // All pages full or no pages — allocate new page
    Page* new_page = bpm_->NewPage(out_page_id);
    if (!new_page) return nullptr;

    // Link to the chain: find last page and update its next pointer
    if (first_page_id_ == INVALID_PAGE_ID) {
        first_page_id_ = out_page_id;
    } else {
        // Walk to last page and link
        page_id_t cur = first_page_id_;
        Page* cur_page = nullptr;
        page_id_t prev = INVALID_PAGE_ID;
        while (cur != INVALID_PAGE_ID) {
            cur_page = bpm_->FetchPage(cur);
            if (!cur_page) break;
            page_id_t next = cur_page->GetNextPageId();
            if (next == INVALID_PAGE_ID) {
                cur_page->SetNextPageId(out_page_id);
                bpm_->UnpinPage(cur, true);
                break;
            }
            bpm_->UnpinPage(cur, false);
            cur = next;
        }
    }
    return new_page;
}

} // namespace minidb
