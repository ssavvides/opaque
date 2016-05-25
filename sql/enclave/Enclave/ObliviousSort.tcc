#include "ObliviousSort.h"

#include <algorithm>

#include "common.h"
#include "util.h"

int log_2(int value);
int pow_2(int value);

template<typename RecordType>
void sort_single_buffer(
  int op_code, uint8_t *buffer, uint32_t num_rows, SortPointer<RecordType> *sort_ptrs,
  uint32_t sort_ptrs_len) {

  check(sort_ptrs_len >= num_rows,
        "sort_single_buffer: sort_ptrs is not large enough (%d vs %d)\n", sort_ptrs_len, num_rows);

  RowReader r(buffer);
  for (uint32_t i = 0; i < num_rows; i++) {
    r.read(&sort_ptrs[i], op_code);
  }

  std::sort(
    sort_ptrs, sort_ptrs + num_rows,
    [op_code](const SortPointer<RecordType> &a, const SortPointer<RecordType> &b) {
      return a.less_than(&b, op_code);
    });

  RowWriter w(buffer);
  for (uint32_t i = 0; i < num_rows; i++) {
    w.write(&sort_ptrs[i]);
  }
  w.close();
}

template<typename RecordType>
void merge(
  int op_code, uint8_t *buffer1, uint32_t buffer1_rows, uint8_t *buffer2, uint32_t buffer2_rows,
  SortPointer<RecordType> *sort_ptrs, uint32_t sort_ptrs_len) {

  check(sort_ptrs_len >= buffer1_rows + buffer2_rows,
        "merge: sort_ptrs is not large enough (%d vs %d)\n",
        sort_ptrs_len, buffer1_rows + buffer2_rows);

  struct BufferVars {
    BufferVars(uint8_t *buffer) : r(buffer), rows_read(0), rec(), ptr(), ptr_is_empty(true) {
      ptr.init(&rec);
    }
    RowReader r;
    uint32_t rows_read;
    RecordType rec;
    SortPointer<RecordType> ptr;
    bool ptr_is_empty;
  } b1(buffer1), b2(buffer2);

  for (uint32_t i = 0; i < buffer1_rows + buffer2_rows; i++) {
    // Fill ptr1 and ptr2
    if (b1.ptr_is_empty && b1.rows_read < buffer1_rows) {
      b1.r.read(&b1.ptr, op_code);
      b1.ptr_is_empty = false;
      b1.rows_read++;
    }
    if (b2.ptr_is_empty && b2.rows_read < buffer2_rows) {
      b2.r.read(&b2.ptr, op_code);
      b2.ptr_is_empty = false;
      b2.rows_read++;
    }

    // Write out the smaller one and clear it
    if (!b1.ptr_is_empty && !b2.ptr_is_empty) {
      if (b1.ptr.less_than(&b2.ptr, op_code)) {
        sort_ptrs[i].set(&b1.ptr);
        b1.ptr_is_empty = true;
      } else {
        sort_ptrs[i].set(&b2.ptr);
        b2.ptr_is_empty = true;
      }
    } else if (!b1.ptr_is_empty) {
      sort_ptrs[i].set(&b1.ptr);
      b1.ptr_is_empty = true;
    } else if (!b2.ptr_is_empty) {
      sort_ptrs[i].set(&b2.ptr);
      b2.ptr_is_empty = true;
    } else {
      printf("merge: Violated assumptions - input exhausted before output full\n");
      assert(false);
    }
  }

  check(b1.ptr_is_empty && b2.ptr_is_empty,
        "merge: Violated assumptions - output is full but input remains\n");

  // Write the merged result back, splitting it across buffers 1 and 2
  RowWriter w1(buffer1);
  for (uint32_t r = 0; r < buffer1_rows; r++) {
    w1.write(&sort_ptrs[r]);
  }
  w1.close();

  RowWriter w2(buffer2);
  for (uint32_t r = buffer1_rows; r < buffer1_rows + buffer2_rows; r++) {
    w2.write(&sort_ptrs[r]);
  }
  w2.close();
}

template<typename RecordType>
void external_oblivious_sort(int op_code,
                             uint32_t num_buffers,
                             uint8_t **buffer_list,
                             uint32_t *num_rows) {
  int len = num_buffers;
  int log_len = log_2(len) + 1;
  int offset = 0;

  perf("external_oblivious_sort: Sorting %d buffers in %d rounds\n", num_buffers, log_len);

  // Maximum number of rows we will need to store in memory at a time: the contents of two buffers
  // (for merging)
  uint32_t max_list_length = num_rows[0];
  if (num_buffers > 1) {
    max_list_length += num_rows[1];
  }

  // Actual record data, in arbitrary and unchanging order
  RecordType *data = new RecordType[max_list_length];

  // Pointers to the record data. Only the pointers will be sorted, not the records themselves
  SortPointer<RecordType> *sort_ptrs = new SortPointer<RecordType>[max_list_length];
  for (uint32_t i = 0; i < max_list_length; i++) {
    sort_ptrs[i].init(&data[i]);
  }

  if (num_buffers == 1) {
    debug("Sorting single buffer with %d rows, opcode %d\n", num_rows[0], op_code);
    sort_single_buffer(op_code, buffer_list[0], num_rows[0], sort_ptrs, max_list_length);
  } else {
    // Sort each buffer individually
    for (uint32_t i = 0; i < num_buffers; i++) {
      debug("Sorting buffer %d with %d rows, opcode %d\n", i, num_rows[i], op_code);
      sort_single_buffer(op_code, buffer_list[i], num_rows[i], sort_ptrs, max_list_length);
    }

    // Merge sorted buffers pairwise
    for (int stage = 1; stage <= log_len; stage++) {
      for (int stage_i = stage; stage_i >= 1; stage_i--) {
        int part_size = pow_2(stage_i);
        int part_size_half = part_size / 2;
        for (int i = offset; i <= (offset + len - 1); i += part_size) {
          for (int j = 1; j <= part_size_half; j++) {
            int idx = i + j - 1;
            int pair_idx = i + part_size - j;
            if (pair_idx < offset + len) {
              debug("Merging buffers %d and %d with %d, %d rows\n",
                    idx, pair_idx, num_rows[idx], num_rows[pair_idx]);
              merge(op_code, buffer_list[idx], num_rows[idx], buffer_list[pair_idx],
                    num_rows[pair_idx], sort_ptrs, max_list_length);
            }
          }
        }
      }
    }
  }

  delete[] sort_ptrs;
  delete[] data;
}
