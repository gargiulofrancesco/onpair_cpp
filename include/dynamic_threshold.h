#ifndef DYNAMIC_THRESHOLD_H
#define DYNAMIC_THRESHOLD_H

#include <algorithm>
#include <cstddef>
#include <cstdio>

// Adaptively adjusts the frequency threshold for merging token pairs based on
// the observed rate of dictionary entry creation compared to the rate needed
// to fill the dictionary within the scan budget.
//
// It compares recent_rate vs target_rate every ~0.8% of capacity.
// If ratio > 2 the threshold increases; if ratio < 0.5 it decreases (min 2).
class DynamicThreshold {
public:
    DynamicThreshold(size_t capacity, size_t total_bytes, double scan_fraction = 1.0)
        : capacity_(capacity)
        , threshold_(2)
        , entries_created_(0)
        , bytes_scanned_(0)
        , bytes_at_last_check_(0)
        , entries_at_last_check_(0)
        , check_count_(0)
    {
        scan_budget_ = static_cast<size_t>(total_bytes * scan_fraction);
        check_interval_ = std::max<size_t>(capacity / 128, 64);
        next_checkpoint_ = check_interval_;
    }

    // Returns the current threshold value.  Completely branchless.
    size_t get() const { return threshold_; }

    // Returns true when the scan budget has been used up.
    bool budget_exhausted() const { return bytes_scanned_ >= scan_budget_; }

    // Accumulate bytes after finishing one input string / record.
    void on_bytes_scanned(size_t n) { bytes_scanned_ += n; }

    // Notify the controller that a new dictionary entry was created.
    // Internally triggers a checkpoint evaluation every ~capacity/128 entries.
    void on_entry_created() {
        ++entries_created_;
        if (entries_created_ < next_checkpoint_) return;
        rebalance();
    }

    // Accessors for diagnostics / downstream use.
    size_t entries_created() const { return entries_created_; }
    size_t bytes_scanned()   const { return bytes_scanned_; }
    size_t capacity()        const { return capacity_; }
    size_t scan_budget()     const { return scan_budget_; }

private:
    void rebalance() {
        ++check_count_;

        size_t delta_entries = entries_created_ - entries_at_last_check_;
        size_t delta_bytes   = bytes_scanned_   - bytes_at_last_check_;

        double recent_rate = (delta_bytes > 0) ? static_cast<double>(delta_entries) / delta_bytes : 1e9;

        size_t entries_remaining = (capacity_ > entries_created_) ? capacity_ - entries_created_ : 1;
        size_t bytes_remaining = (scan_budget_ > bytes_scanned_) ? scan_budget_ - bytes_scanned_ : 1;

        double target_rate = static_cast<double>(entries_remaining) / bytes_remaining;
        double ratio = (target_rate > 0) ? recent_rate / target_rate : 1e9;

        size_t old_threshold = threshold_;

        if (ratio > 2.0) threshold_ += 1;
        else if (ratio < 0.5) threshold_ = (threshold_ > 2) ? threshold_ - 1 : 2;

        entries_at_last_check_ = entries_created_;
        bytes_at_last_check_   = bytes_scanned_;
        next_checkpoint_ = entries_created_ + check_interval_;
    }

    size_t capacity_;
    size_t scan_budget_;
    size_t threshold_;

    size_t entries_created_;
    size_t bytes_scanned_;

    size_t check_interval_;
    size_t next_checkpoint_;
    size_t bytes_at_last_check_;
    size_t entries_at_last_check_;
    size_t check_count_;
};

#endif // DYNAMIC_THRESHOLD_H