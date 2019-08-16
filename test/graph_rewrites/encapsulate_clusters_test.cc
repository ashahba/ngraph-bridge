/*******************************************************************************
 * Copyright 2019 Intel Corporation
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

#include "gtest/gtest.h"

#include "tensorflow/core/graph/node_builder.h"
#include "version.h"

#include "ngraph_bridge/ngraph_cluster_manager.h"
#include "ngraph_bridge/ngraph_encapsulate_clusters.h"
#include "test/test_utilities.h"

using namespace std;
namespace ng = ngraph;

namespace tensorflow {

namespace ngraph_bridge {

namespace testing {

#define ASSERT_OK(x) ASSERT_EQ((x), ::tensorflow::Status::OK());
#define ASSERT_NOT_OK(x) ASSERT_NE((x), ::tensorflow::Status::OK());

TEST(EncapsulateClusters, PopulateLibrary) {
  NGraphClusterManager::EvictAllClusters();
  Graph g(OpRegistry::Global());

  Tensor t_input_0(DT_FLOAT, TensorShape{2, 3});
  Tensor t_input_1(DT_INT32, TensorShape{2});
  t_input_1.flat<int32>().data()[0] = 3;
  t_input_1.flat<int32>().data()[1] = 2;

  int cluster_idx = NGraphClusterManager::NewCluster();

  Node* node1;
  ASSERT_OK(NodeBuilder("node1", "Const")
                .Attr("dtype", DT_FLOAT)
                .Attr("value", t_input_0)
                .Attr("_ngraph_marked_for_clustering", true)
                .Attr("_ngraph_cluster", cluster_idx)
                .Attr("_ngraph_backend", "CPU")
                .Finalize(&g, &node1));

  Node* node2;
  ASSERT_OK(NodeBuilder("node2", "Const")
                .Attr("dtype", DT_FLOAT)
                .Attr("value", t_input_1)
                .Attr("_ngraph_marked_for_clustering", true)
                .Attr("_ngraph_cluster", cluster_idx)
                .Attr("_ngraph_backend", "CPU")
                .Finalize(&g, &node2));

  Node* node3;
  ASSERT_OK(NodeBuilder("node3", "Add")
                .Input(node1, 0)
                .Input(node2, 0)
                .Attr("T", DT_FLOAT)
                .Attr("_ngraph_marked_for_clustering", true)
                .Attr("_ngraph_cluster", cluster_idx)
                .Attr("_ngraph_backend", "CPU")
                .Finalize(&g, &node3));

  Node* source = g.source_node();
  Node* sink = g.sink_node();
  g.AddEdge(source, Graph::kControlSlot, node1, Graph::kControlSlot);
  g.AddEdge(source, Graph::kControlSlot, node2, Graph::kControlSlot);
  g.AddEdge(node3, Graph::kControlSlot, sink, Graph::kControlSlot);

  FunctionDefLibrary* fdeflib_new = new FunctionDefLibrary();

  std::unordered_map<std::string, std::string> config_map;
  config_map["ngraph_device_id"] = "";
  ASSERT_OK(EncapsulateClusters(&g, 0, fdeflib_new, config_map, {0, {}}));

  int num_encapsulates = 0;
  int num_tf_nodes = 0;
  for (auto itr : g.nodes()) {
    auto node_type = itr->type_string();
    num_encapsulates += (node_type == "NGraphEncapsulate" ? 1 : 0);
    num_tf_nodes += ((node_type == "Add" || node_type == "Const") ? 1 : 0);
  }

  // Number of encapsulates == number of functions
  ASSERT_EQ(num_encapsulates, fdeflib_new->function_size());

  // No Add or Const nodes left in the graph
  ASSERT_EQ(num_tf_nodes, 0);

  // In this case, only 1 function has been added in the library
  ASSERT_EQ(fdeflib_new->function_size(), 1);

  // Check the name of the signature of the first (and only) function
  auto first_func = fdeflib_new->function(0);
  ASSERT_EQ(first_func.signature().name(),
            ("ngraph_cluster_" + to_string(cluster_idx)));

  // The first function in the flib should have 3 nodes
  ASSERT_EQ(first_func.node_def_size(), 3);

  // Ensure that the function is made of 2 op types, Add, Const, Const
  auto present = multiset<string>{string(first_func.node_def(0).op()),
                                  string(first_func.node_def(1).op()),
                                  string(first_func.node_def(2).op())};
  auto expected = multiset<string>{"Const", "Add", "Const"};
  ASSERT_EQ(present, expected);
  free(fdeflib_new);
}

TEST(EncapsulateClusters, AOT) {
  NGraphClusterManager::EvictAllClusters();
  Graph g(OpRegistry::Global());

  int cluster_idx = NGraphClusterManager::NewCluster();

  Node* node1;
  Node* node2;

  if (ngraph_tf_is_grappler_enabled()) {
    ASSERT_OK(NodeBuilder("node1", "Placeholder")
                  .Attr("dtype", DT_FLOAT)
                  .Finalize(&g, &node1));
    ASSERT_OK(NodeBuilder("node2", "Placeholder")
                  .Attr("dtype", DT_FLOAT)
                  .Finalize(&g, &node2));
  } else {
    ASSERT_OK(NodeBuilder("node1", "_Arg")
                  .Attr("index", 0)
                  .Attr("T", DT_FLOAT)
                  .Finalize(&g, &node1));
    ASSERT_OK(NodeBuilder("node2", "_Arg")
                  .Attr("index", 0)
                  .Attr("T", DT_FLOAT)
                  .Finalize(&g, &node2));
  }

  Node* node3;
  ASSERT_OK(NodeBuilder("node3", "Add")
                .Input(node1, 0)
                .Input(node2, 0)
                .Attr("T", DT_FLOAT)
                .Attr("_ngraph_marked_for_clustering", true)
                .Attr("_ngraph_cluster", cluster_idx)
                .Attr("_ngraph_backend", "INTERPRETER")
                .Finalize(&g, &node3));
  Node* node4;
  if (ngraph_tf_is_grappler_enabled()) {
    std::vector<NodeBuilder::NodeOut> inputs;
    std::vector<DataType> input_types;
    inputs.push_back(NodeBuilder::NodeOut(node3, 0));
    input_types.push_back(node3->output_type(0));
    ASSERT_OK(NodeBuilder("node4", "IdentityN")
                  .Input(inputs)
                  .Attr("T", input_types)
                  .Finalize(&g, &node4));
  }

  Node* source = g.source_node();
  Node* sink = g.sink_node();
  g.AddEdge(source, Graph::kControlSlot, node1, Graph::kControlSlot);
  g.AddEdge(source, Graph::kControlSlot, node2, Graph::kControlSlot);
  g.AddEdge(ngraph_tf_is_grappler_enabled() ? node4 : node3,
            Graph::kControlSlot, sink, Graph::kControlSlot);

  FunctionDefLibrary* fdeflib_new = new FunctionDefLibrary();

  std::vector<std::set<ShapeHintMap>> node_shapes_hints_vect = {
      {}, {{{"node1", {2, 2}}, {"node2", {2, 2}}}}};
  std::vector<bool> did_aot = {false, true};
  int num_cases = node_shapes_hints_vect.size();
  for (int i = 0; i < num_cases; i++) {
    ASSERT_OK(EncapsulateClusters(&g, 0, fdeflib_new,
                                  {{"ngraph_device_id", ""}},
                                  make_pair(1, node_shapes_hints_vect[i])));

    int num_encapsulates = 0;
    int num_tf_nodes = 0;
    for (auto itr : g.nodes()) {
      auto node_type = itr->type_string();
      num_encapsulates += (node_type == "NGraphEncapsulate" ? 1 : 0);
      num_tf_nodes += ((node_type == "Add" || node_type == "Const") ? 1 : 0);
    }

    ASSERT_EQ(num_encapsulates, fdeflib_new->function_size());

    // No Add or Const nodes left in the graph
    ASSERT_EQ(num_tf_nodes, 0);

    // TODO: assert that a json is created
    for (auto itr : g.nodes()) {
      if (itr->type_string() == "NGraphEncapsulate") {
        string aot_info;
        // TODO: remove the hardcoded signature
        bool found_exec =
            GetNodeAttr(itr->attrs(), "_ngraph_aot_ngexec_2,2,;2,2,;/",
                        &aot_info) == tensorflow::Status::OK();
        bool found_function =
            GetNodeAttr(itr->attrs(), "_ngraph_aot_ngfunction_2,2,;2,2,;/",
                        &aot_info) == tensorflow::Status::OK();
        ASSERT_TRUE(found_exec == did_aot[i]);
        ASSERT_TRUE(found_function == did_aot[i]);
      }
    }
  }

  free(fdeflib_new);
}

// TODO: do unit tests for AOT level 1 and level 2

// TODO: more test cases:
// what of scalar inputs. placeholder shape is {}?
// add a test with 2 encs. should fail for now
// Shape hints that cause errors in TranslateGraph?. eg trying to add [2,2] with
// [2,4]?
// Encapsulate being fed by another enc
// 2 encapsulates, but both are attached to inputs, so we can AOT
// Have a test where enc is fed by a const and a placeholder
}
}
}