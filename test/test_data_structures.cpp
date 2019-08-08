/*******************************************************************************
 * Copyright 2017-2019 Intel Corporation
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
 *******************************************************************************/

#include <stdlib.h>
#include <chrono>

#include "gtest/gtest.h"

#include "ngraph_bridge/ngraph_pipelined_tensors.h"

using namespace std;
namespace ng = ngraph;

namespace tensorflow {

namespace ngraph_bridge {

namespace testing {

TEST(IndexLibrary, SingleThreadTest1) {
  IndexLibrary idx_lib{3};
  set<int> expected{0, 1, 2};

  int i0 = idx_lib.get_index();
  ASSERT_TRUE(expected.find(i0) != expected.end());
  expected.erase(i0);  // 2 elements left, i0 checked out

  int i1 = idx_lib.get_index();
  ASSERT_TRUE(expected.find(i1) != expected.end());
  expected.erase(i1);  // 1 elements left, i0, i1 checked out

  idx_lib.return_index(i0);
  expected.insert(i0);  // 2 elements left, i1 checked out

  int i2 = idx_lib.get_index();
  ASSERT_TRUE(expected.find(i2) != expected.end());
  expected.erase(i2);  // 1 elements left, i1, i2 checked out

  int i3 = idx_lib.get_index();
  ASSERT_TRUE(expected.find(i3) != expected.end());
  expected.erase(i3);  // 0 elements left, i1, i2, i3 checked out

  int i4 = idx_lib.get_index();
  ASSERT_EQ(i4, -1)
      << "Expected index library to be empty, hence get_index should return -1";

  // Try to return an invalid index
  ASSERT_THROW(idx_lib.return_index(50), std::runtime_error);

  idx_lib.return_index(i1);
  expected.insert(i1);  // 1 elements left, i2, i3 checked out

  // Try to return an index that is already checkedin/returned
  ASSERT_THROW(idx_lib.return_index(i1), std::runtime_error);
}

TEST(IndexLibrary, SingleThreadTest2) {
  IndexLibrary idx_lib{0};

  // Since it is an empty library it will always return -1
  ASSERT_EQ(idx_lib.get_index(), -1);
}

// 2 threads run randomly and attempt to get and return indices from the same
// IndexLibrary 10 times.
// The test asserts if one of the threads managed to get an index i, then the
// other thread must not have that index i
// TODO: dump the get/return sequence in case of a test failure.
TEST(IndexLibrary, MultiThreadTest) {
  IndexLibrary idx_lib{5};

  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_real_distribution<> dis(0, 1);

  vector<shared_ptr<set<int>>> checked_out_collections = {
      make_shared<set<int>>(), make_shared<set<int>>()};

  // TODO: remove thread_id. Its there for debug prints only
  auto worker = [&idx_lib, &dis, &gen,
                 &checked_out_collections](size_t thread_id) {
    shared_ptr<set<int>> my_checked_out = checked_out_collections[thread_id];
    shared_ptr<set<int>> other_checked_out =
        checked_out_collections[1 - thread_id];
    int count_work = 0;
    while (true) {
      if (dis(gen) > 0.5) {
        int i = idx_lib.get_index();
        // cout << "thread_id: " << thread_id << ". Got: " << i << "\n";
        if (i >= 0) {
          my_checked_out->insert(i);
          count_work++;
          // No need to lock access to my_checked_out and other_checked_out
          // There is an implicit lock in between them from idx_lib
          ASSERT_TRUE(other_checked_out->find(i) == other_checked_out->end());
        }
      } else {
        if (my_checked_out->begin() != my_checked_out->end()) {
          int j = *(my_checked_out->begin());
          // cout << "thread_id: " << thread_id << ". trying to return: " << j
          //     << "\n";
          idx_lib.return_index(j);
          count_work++;
          my_checked_out->erase(j);
        }
      }
      // wait for 1 or 2 ms randomly
      std::chrono::milliseconds timespan((dis(gen) > 0.5) ? 1 : 2);
      std::this_thread::sleep_for(timespan);
      if (count_work >= 10) {
        break;
      }
    }
    // In the end return all indices
    for (auto i : *my_checked_out) {
      // cout << "thread_id: " << thread_id << ". [Final] trying to return: " <<
      // i
      //     << "\n";
      idx_lib.return_index(i);
    }
  };

  std::thread thread0(worker, 0);
  std::thread thread1(worker, 1);

  thread0.join();
  thread1.join();
}
}
}
}