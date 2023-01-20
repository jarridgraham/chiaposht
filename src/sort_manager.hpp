// Copyright 2018 Chia Network Inc

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//    http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef SRC_CPP_FAST_SORT_ON_DISK_HPP_
#define SRC_CPP_FAST_SORT_ON_DISK_HPP_

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <chrono>

#include "chia_filesystem.hpp"

#include "./bits.hpp"
#include "./calculate_bucket.hpp"
#include "./disk.hpp"
#include "./quicksort.hpp"
#include "./uniformsort.hpp"
#include "./bucketsort.hpp"
#include "disk.hpp"
#include "exceptions.hpp"
#include "sorting_bucket.hpp"

class SortManager : public Disk {
public:
    SortManager(
        uint64_t const memory_size,
        uint32_t const num_buckets,
        uint32_t const log_num_buckets,
        uint16_t const entry_size,
        const std::string &tmp_dirname,
        const std::string &filename,
        uint32_t begin_bits,
        uint64_t const stripe_size,
				uint8_t k,
				uint8_t phase,
				uint32_t num_threads = 2)

        : memory_size_(memory_size)
        , entry_size_(entry_size)
        , begin_bits_(begin_bits)
        , log_num_buckets_(log_num_buckets)
        , prev_bucket_buf_size(
            2 * (stripe_size + 10 * (kBC / pow(2, kExtraBits))) * entry_size)
        // 7 bytes head-room for SliceInt64FromBytes()
        , entry_buf_(new uint8_t[entry_size + 7])
				, num_threads( num_threads )
				, subbucket_bits( std::max( (uint8_t)2, (uint8_t)(k - log_num_buckets - 11) ) )
    {
        // Cross platform way to concatenate paths, gulrak library.
				std::vector<fs::path> bucket_filenames = std::vector<fs::path>();

        buckets_.reserve(num_buckets);
        for (size_t bucket_i = 0; bucket_i < num_buckets; bucket_i++) {
            std::ostringstream bucket_number_padded;
            bucket_number_padded << std::internal << std::setfill('0') << std::setw(3) << bucket_i;

            fs::path const bucket_filename =
                fs::path(tmp_dirname) /
                fs::path(filename + ".sort_bucket_" + bucket_number_padded.str() + ".tmp");
						buckets_.emplace_back( SortingBucket( bucket_filename.string(), entry_size, begin_bits_ + log_num_buckets, subbucket_bits ) );
        }
    }

		inline void AddToCache(const Bits &entry)
    {
        entry.ToBytes(entry_buf_.get());
				AddToCache(entry_buf_.get());
    }

		inline void AddToCache(const uint8_t *entry)
    {
        uint64_t const bucket_index =
						Util::ExtractNum(entry, entry_size_, begin_bits_, log_num_buckets_ + subbucket_bits );
				buckets_[bucket_index>>subbucket_bits].AddEntry( entry, bucket_index & ( ( (uint64_t)1<<subbucket_bits)-1) );
		}


		uint8_t const* Read(uint64_t begin, uint64_t length) override
    {
        assert(length <= entry_size_);
        return ReadEntry(begin);
    }

    void Write(uint64_t, uint8_t const*, uint64_t) override
    {
        assert(false);
        throw InvalidStateException("Invalid Write() called on SortManager");
    }

    void Truncate(uint64_t new_size) override
    {
        if (new_size != 0) {
            assert(false);
            throw InvalidStateException("Invalid Truncate() called on SortManager");
        }

        FlushCache();
        FreeMemory();
    }

    std::string GetFileName() override
    {
        return "<SortManager>";
    }

    void FreeMemory() override
    {
        for (auto& b : buckets_) {
					// TODO: clear bucket memory
//            b.file.FreeMemory();
//            // the underlying file will be re-opened again on-demand
//            b.underlying_file.Close();
        }
        prev_bucket_buf_.reset();
        memory_start_.reset();
        final_position_end = 0;
        // TODO: Ideally, bucket files should be deleted as we read them (in the
        // last reading pass over them)
    }

    uint8_t *ReadEntry(uint64_t position)
    {
        if (position < this->final_position_start) {
            if (!(position >= this->prev_bucket_position_start)) {
                throw InvalidStateException("Invalid prev bucket start");
            }
            // this is allocated lazily, make sure it's here
            assert(prev_bucket_buf_);
            return (prev_bucket_buf_.get() + (position - this->prev_bucket_position_start));
        }

        while (position >= this->final_position_end) {
            SortBucket();
        }
        if (!(this->final_position_end > position)) {
            throw InvalidValueException("Position too large");
        }
        if (!(this->final_position_start <= position)) {
            throw InvalidValueException("Position too small");
        }
        assert(memory_start_);
        return memory_start_.get() + (position - this->final_position_start);
    }

    bool CloseToNewBucket(uint64_t position) const
    {
        if (!(position <= this->final_position_end)) {
            return this->next_bucket_to_sort < buckets_.size();
        };
        return (
            position + prev_bucket_buf_size / 2 >= this->final_position_end &&
            this->next_bucket_to_sort < buckets_.size());
    }

    void TriggerNewBucket(uint64_t position)
    {
        if (!(position <= this->final_position_end)) {
            throw InvalidValueException("Triggering bucket too late");
        }
        if (!(position >= this->final_position_start)) {
            throw InvalidValueException("Triggering bucket too early");
        }

        if (memory_start_) {
            // save some of the current bucket, to allow some reverse-tracking
            // in the reading pattern,
            // position is the first position that we need in the new array
            uint64_t const cache_size = (this->final_position_end - position);
            prev_bucket_buf_.reset(new uint8_t[prev_bucket_buf_size]);
            memset(prev_bucket_buf_.get(), 0x00, this->prev_bucket_buf_size);
            memcpy(
                prev_bucket_buf_.get(),
                memory_start_.get() + position - this->final_position_start,
                cache_size);
        }

        SortBucket();
        this->prev_bucket_position_start = position;
    }

    void FlushCache()
    {
        for (auto& b : buckets_) {
						b.Flush();
        }
        final_position_end = 0;
        memory_start_.reset();
    }

    ~SortManager()
    {
        // Close and delete files in case we exit without doing the sort
				for (auto& b : buckets_){
					b.Remove();
				}
    }

private:

    // The buffer we use to sort buckets in-memory
    std::unique_ptr<uint8_t[]> memory_start_;
    // Size of the whole memory array
    uint64_t memory_size_;
    // Size of each entry
    uint16_t entry_size_;
    // Bucket determined by the first "log_num_buckets" bits starting at "begin_bits"
    uint32_t begin_bits_;
    // Log of the number of buckets; num bits to use to determine bucket
    uint32_t log_num_buckets_;

		std::vector<SortingBucket> buckets_;

    uint64_t prev_bucket_buf_size;
    std::unique_ptr<uint8_t[]> prev_bucket_buf_;
    uint64_t prev_bucket_position_start = 0;

    bool done = false;

    uint64_t final_position_start = 0;
    uint64_t final_position_end = 0;
    uint64_t next_bucket_to_sort = 0;
    std::unique_ptr<uint8_t[]> entry_buf_;
		uint32_t num_threads;
		const uint8_t subbucket_bits;

    void SortBucket()
    {
        if (!memory_start_) {
            // we allocate the memory to sort the bucket in lazily. It'se freed
            // in FreeMemory() or the destructor
            memory_start_.reset(new uint8_t[memory_size_]);
        }

        this->done = true;
        if (next_bucket_to_sort >= buckets_.size()) {
            throw InvalidValueException("Trying to sort bucket which does not exist.");
        }
        uint64_t const bucket_i = this->next_bucket_to_sort;
				SortingBucket& b = buckets_[bucket_i];
        uint64_t const entries_fit_in_memory = this->memory_size_ / entry_size_;

				if( b.Count() > entries_fit_in_memory ) {
            throw InsufficientMemoryException(
                "Not enough memory for sort in memory. Need to sort " +
								std::to_string(b.Size() / (1024.0 * 1024.0 * 1024.0)) +
                "GiB");
        }

				double const have_ram = entry_size_ * entries_fit_in_memory / (1024.0 * 1024.0 * 1024.0);
				double const qs_ram = entry_size_ * b.Count() / (1024.0 * 1024.0 * 1024.0);

				std::cout << "\tBucket " << bucket_i << " Ram: " << std::fixed
									<< std::setprecision(3) << have_ram << "GiB, size: " <<  qs_ram << "GiB" << std::flush;

				auto start_time = std::chrono::high_resolution_clock::now();

				b.SortToMemory( memory_start_.get(), num_threads );

				auto end_time = std::chrono::high_resolution_clock::now();
				std::cout << ", sort time: " << (end_time - start_time)/std::chrono::milliseconds(1)/1000.0 << "s" << std::endl;

        this->final_position_start = this->final_position_end;
				this->final_position_end += b.Size();
				this->next_bucket_to_sort += 1;

				// Deletes the bucket file and clean ram
				b.Remove();
		}
};

#endif  // SRC_CPP_FAST_SORT_ON_DISK_HPP_
