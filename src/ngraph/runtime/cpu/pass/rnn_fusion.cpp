/*******************************************************************************
* Copyright 2017-2018 Intel Corporation
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
#include <typeindex>
#include <typeinfo>

#include <algorithm>
#include <iostream>
#include <numeric>
#include <unordered_set>
#include "ngraph/graph_util.hpp"
#include "ngraph/log.hpp"
#include "ngraph/op/add.hpp"
#include "ngraph/op/add.hpp"
#include "ngraph/op/batch_norm.hpp"
#include "ngraph/op/broadcast.hpp"
#include "ngraph/op/broadcast.hpp"
#include "ngraph/op/concat.hpp"
#include "ngraph/op/constant.hpp"
#include "ngraph/op/convolution.hpp"
#include "ngraph/op/divide.hpp"
#include "ngraph/op/dot.hpp"
#include "ngraph/op/exp.hpp"
#include "ngraph/op/get_output_element.hpp"
#include "ngraph/op/multiply.hpp"
#include "ngraph/op/negative.hpp"
#include "ngraph/op/pad.hpp"
#include "ngraph/op/parameter.hpp"
#include "ngraph/op/relu.hpp"
#include "ngraph/op/reshape.hpp"
#include "ngraph/op/result.hpp"
#include "ngraph/op/slice.hpp"
#include "ngraph/op/sqrt.hpp"
#include "ngraph/op/subtract.hpp"
#include "ngraph/op/sum.hpp"
#include "ngraph/op/tanh.hpp"
#include "ngraph/pattern/matcher.hpp"
#include "ngraph/pattern/op/label.hpp"
#include "ngraph/pattern/op/skip.hpp"
#include "ngraph/runtime/cpu/op/batch_norm_relu.hpp"
#include "ngraph/runtime/cpu/op/conv_bias.hpp"
#include "ngraph/runtime/cpu/op/conv_relu.hpp"
#include "ngraph/runtime/cpu/op/lstm.hpp"
#include "ngraph/runtime/cpu/op/matmul_bias.hpp"
#include "ngraph/runtime/cpu/op/rnn.hpp"
#include "ngraph/runtime/cpu/op/sigmoid.hpp"
#include "rnn_fusion.hpp"

using namespace ngraph;
void ngraph::runtime::cpu::pass::LSTMFusion::construct_sigmoid()
{
    //construct variance
    auto input = std::make_shared<pattern::op::Label>(element::f32, Shape{3, 4});
    auto neg_input = std::make_shared<op::Negative>(input);
    auto exp_neg_input = std::make_shared<op::Exp>(neg_input);

    // broadcast input
    auto constant = std::make_shared<pattern::op::Label>(element::f32, Shape{});
    auto broadcast_constant = std::make_shared<op::Broadcast>(constant, Shape{3, 4}, AxisSet{0, 1});

    auto add_exp = std::make_shared<op::Add>(exp_neg_input, broadcast_constant);
    auto divide_1_over_exp = std::make_shared<op::Divide>(broadcast_constant, add_exp);

    //Define a call back that needs to called once the DFG matches the pattern
    ngraph::pattern::graph_rewrite_callback callback = [input](pattern::Matcher& m) {
        NGRAPH_DEBUG << "In a callback for construct_fprop_sigmoid pattern against "
                     << m.match_root()->get_name();
        auto pattern_map = m.get_pattern_map();

        if (m.match_root()->get_element_type() != element::f32)
        {
            NGRAPH_DEBUG << "mpattern = " << m.match_root()->get_name() << " type is not float!";
            return false;
        }

        if (m.match_root()->get_outputs().size() != pattern_map[input]->get_outputs().size())
        {
            NGRAPH_DEBUG << "mpattern = " << m.match_root()->get_name()
                         << "input= " << pattern_map[input]->get_name() << "size dont match!";
            return false;
        }

        auto sigmoid_node = std::make_shared<op::Sigmoid>(pattern_map[input]);
        ngraph::replace_node(m.match_root(), sigmoid_node);
        return true;
    };

    auto m = std::make_shared<ngraph::pattern::Matcher>(divide_1_over_exp, callback);
    NGRAPH_DEBUG << "Sigmoid: " << m;
    this->add_matcher(m);
}

void ngraph::runtime::cpu::pass::LSTMFusion::construct_lstm_fprop()
{
    // param1_1 -> ht_1 (src_iter)
    auto param1_1 = std::make_shared<pattern::op::Label>(element::f32, Shape{10, 100});
    auto broadcast_pred_1 = [](std::shared_ptr<Node> n) {
        return static_cast<bool>(std::dynamic_pointer_cast<op::Broadcast>(n));
    };
    auto skip_param_1_1 = std::make_shared<pattern::op::Skip>(param1_1, broadcast_pred_1);
    // param1_2 -> h2h weights (weights_iter)
    auto param1_2 = std::make_shared<pattern::op::Label>(element::f32, Shape{400, 100});
    auto param1_2_reshape =
        std::make_shared<op::Reshape>(param1_2, AxisVector{1, 0}, Shape{100, 400});
    auto dot_1 = std::make_shared<op::Dot>(skip_param_1_1, param1_2_reshape);

    auto bias1 = std::make_shared<pattern::op::Label>(element::f32, Shape{400});
    auto broadcast_bias1 = std::make_shared<op::Broadcast>(bias1, Shape{10, 400}, AxisSet{0});
    auto add_1 = std::make_shared<op::Add>(dot_1, broadcast_bias1);

    // param2_1 -> xt (src_layer)
    auto param2_1 = std::make_shared<pattern::op::Label>(element::f32, Shape{10, 50});
    // param2_2 -> i2h weights (weights_layer)
    auto param2_2 = std::make_shared<pattern::op::Label>(element::f32, Shape{400, 50});
    auto param2_2_reshape =
        std::make_shared<op::Reshape>(param2_2, AxisVector{1, 0}, Shape{50, 400});
    auto dot_2 = std::make_shared<op::Dot>(param2_1, param2_2_reshape);
    auto bias2 = std::make_shared<pattern::op::Label>(element::f32, Shape{400});
    auto broadcast_bias2 = std::make_shared<op::Broadcast>(bias2, Shape{10, 400}, AxisSet{0});
    auto add_2 = std::make_shared<op::Add>(dot_2, broadcast_bias2);

    auto X = std::make_shared<op::Add>(add_2, add_1);
    // construct forget gate
    auto input_slice_0 = std::make_shared<op::Slice>(X, Coordinate{0, 0}, Coordinate{10, 100});
    auto forget_gate = std::make_shared<op::Sigmoid>(input_slice_0);

    //ct-1 -> cell state (src_iter -> {ht | ct-1}
    auto ct_1 = std::make_shared<pattern::op::Label>(element::f32, Shape{10, 100});
    auto multiply_forget_gate_ct_1 = std::make_shared<op::Multiply>(forget_gate, ct_1);

    // construct input gate
    auto input_slice_1 = std::make_shared<op::Slice>(X, Coordinate{0, 100}, Coordinate{10, 200});
    auto input_gate = std::make_shared<op::Sigmoid>(input_slice_1);
    auto input_slice_2 = std::make_shared<op::Slice>(X, Coordinate{0, 200}, Coordinate{10, 300});
    auto tanh_1 = std::make_shared<op::Tanh>(input_slice_2);
    auto multiply_input_gate_tanh_1 = std::make_shared<op::Multiply>(input_gate, tanh_1);

    auto add_ct_1_input_gate_tanh_1 =
        std::make_shared<op::Add>(multiply_forget_gate_ct_1, multiply_input_gate_tanh_1);
    auto ct_label = std::make_shared<pattern::op::Label>(
        add_ct_1_input_gate_tanh_1, nullptr, NodeVector{add_ct_1_input_gate_tanh_1});

    // construct output gate
    auto input_slice_3 = std::make_shared<op::Slice>(X, Coordinate{0, 300}, Coordinate{10, 400});
    auto output_gate = std::make_shared<op::Sigmoid>(input_slice_3);
    auto tanh_2 = std::make_shared<op::Tanh>(ct_label);
    auto ht = std::make_shared<op::Multiply>(output_gate, tanh_2);
    auto ht_label = std::make_shared<pattern::op::Label>(ht, nullptr, NodeVector{ht});

    //Define a call back that needs to called once the DFG matches the pattern
    pattern::graph_rewrite_callback callback =
        [ct_label, param1_1, param1_2, param2_1, param2_2, bias1, bias2, ct_1](
            pattern::Matcher& m) {
            NGRAPH_DEBUG << "In a callback for construct_fprop_lstm pattern against "
                         << m.match_root()->get_name();

            auto pattern_map = m.get_pattern_map();
            NGRAPH_DEBUG << "In Lstm fprop call back";

            if (m.match_root()->get_element_type() != element::f32)
            {
                NGRAPH_DEBUG << "mpattern = " << m.match_root()->get_name()
                             << " type is not float!";
                return false;
            }

            if (m.match_root()->get_shape() != pattern_map[param2_1]->get_shape())
            {
                NGRAPH_DEBUG << "matched_node_shape: " << join(m.match_root()->get_shape())
                             << " hidden state shape: " << join(pattern_map[param2_1]->get_shape());
                return false;
            }
            Shape ct_shape{pattern_map[ct_label]->get_shape()};
            auto lstm = std::make_shared<op::Lstm>(pattern_map[param1_1],
                                                   pattern_map[param1_2],
                                                   pattern_map[param2_1],
                                                   pattern_map[param2_2],
                                                   pattern_map[bias1],
                                                   pattern_map[bias2],
                                                   pattern_map[ct_1],
                                                   ct_shape);

            auto ht_output = std::make_shared<op::GetOutputElement>(lstm, 0);
            auto ct_output = std::make_shared<op::GetOutputElement>(lstm, 1);

            std::vector<std::shared_ptr<Node>> new_args;
            for (auto node : pattern_map[ct_label]->get_users())
            {
                if (std::dynamic_pointer_cast<op::Multiply>(node))
                {
                    NGRAPH_DEBUG << "node_name: " << node->get_name();
                    for (size_t i = 0; i < node->get_input_size(); i++)
                    {
                        if (node->get_argument(i) == pattern_map[ct_label])
                        {
                            new_args.push_back(ct_output);
                        }
                        else
                        {
                            new_args.push_back(node->get_argument(i));
                        }
                        NGRAPH_DEBUG << "Multiply_input's shape: " << join(new_args[i]->get_shape())
                                     << " " << new_args[i]->get_name();
                    }
                    auto new_ct_node = node->copy_with_new_args(new_args);
                    NGRAPH_DEBUG << "node: " << node->get_name() << " replaced with  "
                                 << new_ct_node->get_name();
                    ;
                    ngraph::replace_node(node, new_ct_node);
                    new_args.clear();
                }
            }
            ngraph::replace_node(m.match_root(), ht_output);
            return true;
        };
    auto m = std::make_shared<pattern::Matcher>(ht, callback);
    NGRAPH_DEBUG << "lstm: " << m;
    this->add_matcher(m);
}

static std::shared_ptr<ngraph::Node>
    compute_rnn_args(std::vector<std::shared_ptr<pattern::op::Label>>& rnn_labels,
                     pattern::RecurrentMatcher& m,
                     bool input_symbol = false)
{
    NGRAPH_DEBUG << "Inside compute arg " << rnn_labels.size();
    std::set<std::shared_ptr<Node>> unique_params;
    NodeVector concat_args;

    // src_layer -> concatenate input symbols from different LSTM cells belonging to same RNN layer in the order 0, 1, 2... t time slice
    if (input_symbol && rnn_labels.size() == 1)
    {
        auto node_labels = m.get_bound_nodes_for_pattern(rnn_labels[0]);
        std::reverse(node_labels.begin(), node_labels.end());
        return std::make_shared<op::Concat>(node_labels, 0);
    }

    // src_iter -> concatenate ht_1|ct_1 of the first LSTM cells belonging to same RNN layer
    if (rnn_labels.size() == 2)
    {
        for (size_t i = 0; i < rnn_labels.size(); i++)
        {
            auto node_labels = m.get_bound_nodes_for_pattern(rnn_labels[i]);
            if (std::dynamic_pointer_cast<op::GetOutputElement>(
                    node_labels[node_labels.size() - 1]))
            {
                throw ngraph_error(
                    "pattern matcher error, ht_1|ct_1 of the first LSTM cell should not match "
                    "intermediate LSTM outputs");
            }
            concat_args.push_back(node_labels[node_labels.size() - 1]);
        }
        return std::make_shared<op::Concat>(concat_args, 0);
    }
    // i2h or h2h weights shared between LSTN cells
    else
    {
        auto node_labels = m.get_bound_nodes_for_pattern(rnn_labels[0]);
        return node_labels[node_labels.size() - 1];
    }
}

static bool is_unreachable(std::shared_ptr<ngraph::Node> node)
{
    std::unordered_set<std::shared_ptr<ngraph::Node>> instances_seen;
    std::deque<std::shared_ptr<ngraph::Node>> stack;
    stack.push_front(node);

    while (stack.size() > 0)
    {
        std::shared_ptr<ngraph::Node> n = stack.front();
        if (instances_seen.count(n) == 0)
        {
            if (n->is_output())
            {
                return false;
            }
            instances_seen.insert(n);
        }
        stack.pop_front();
        for (auto arg : n->get_users())
        {
            if (instances_seen.count(arg) == 0)
            {
                stack.push_front(arg);
            }
        }
    }
    return true;
}

void ngraph::runtime::cpu::pass::RNNFusion::construct_rnn_lstm_fprop()
{
    auto rpattern_ht_1 = std::make_shared<pattern::op::Label>(element::f32, Shape{32, 100});
    auto weights_h2h = std::make_shared<pattern::op::Label>(element::f32, Shape{400, 100});
    auto xt = std::make_shared<pattern::op::Label>(element::f32, Shape{32, 200});
    auto weights_i2h = std::make_shared<pattern::op::Label>(element::f32, Shape{400, 100});
    auto bias1 = std::make_shared<pattern::op::Label>(element::f32, Shape{400});
    auto bias2 = std::make_shared<pattern::op::Label>(element::f32, Shape{400});
    auto ct_1 = std::make_shared<pattern::op::Label>(element::f32, Shape{32, 100});

    auto lstm = std::make_shared<op::Lstm>(
        xt, weights_i2h, rpattern_ht_1, weights_h2h, bias1, bias2, ct_1, Shape{32, 100});
    auto goe = std::make_shared<op::GetOutputElement>(lstm, 0);
    auto lstm_node_label = std::make_shared<pattern::op::Label>(goe, nullptr, NodeVector{goe});

    pattern::recurrent_graph_rewrite_callback callback = [lstm_node_label,
                                                          rpattern_ht_1,
                                                          weights_h2h,
                                                          xt,
                                                          weights_i2h,
                                                          bias1,
                                                          bias2,
                                                          ct_1](pattern::RecurrentMatcher& m) {

        NGRAPH_DEBUG << " In recurrent RNN fusion callback";

        auto ht_1_label = m.get_bound_nodes_for_pattern(rpattern_ht_1);

        std::vector<std::shared_ptr<pattern::op::Label>> src_iter_labels{rpattern_ht_1, ct_1};
        auto src_iter = compute_rnn_args(src_iter_labels, m);

        std::vector<std::shared_ptr<pattern::op::Label>> weights_layer_labels{weights_i2h};
        auto weights_layer = compute_rnn_args(weights_layer_labels, m);

        std::vector<std::shared_ptr<pattern::op::Label>> weights_iter_labels{weights_h2h};
        auto weights_iter = compute_rnn_args(weights_iter_labels, m);

        std::vector<std::shared_ptr<pattern::op::Label>> src_layer_labels{xt};
        auto src_layer = compute_rnn_args(src_layer_labels, m, true);

        auto bias_i2h_label = m.get_bound_nodes_for_pattern(bias2);
        auto bias_h2h_label = m.get_bound_nodes_for_pattern(bias1);
        auto bias = std::make_shared<op::Add>(bias_i2h_label[0], bias_h2h_label[0]);

        auto num_of_lstm_matched = m.get_number_of_recurrent_matches();
        size_t num_gates_in_lstm = 4;
        // TODO: assert for batch_size, sequence length and num_of_lstm's fused
        size_t batch_size = src_layer->get_shape()[0] / num_of_lstm_matched;
        size_t sequence_len = num_of_lstm_matched;
        size_t src_layer_feature_size = src_layer->get_shape()[1];
        size_t feature_size = ht_1_label[0]->get_shape()[1];
        // number of states for LSTM is 2
        size_t num_rnn_cell_states = 2;
        size_t rnn_direction = 1;
        size_t num_fused_rnn_layers = 1;

        NGRAPH_DEBUG << "src_layer: " << join(src_layer->get_shape());
        NGRAPH_DEBUG << "src_iter: " << join(src_iter->get_shape());
        NGRAPH_DEBUG << "weights_layer: " << join(weights_layer->get_shape());
        NGRAPH_DEBUG << "weights_iter: " << join(weights_iter->get_shape());
        NGRAPH_DEBUG << "bias: " << join(bias->get_shape());
        NGRAPH_DEBUG << "src_seq_len: " << sequence_len;
        NGRAPH_DEBUG << "batch_size: " << batch_size;
        NGRAPH_DEBUG << "feature_size: " << feature_size;

        if ((src_layer->get_arguments().size()) != sequence_len)
        {
            throw ngraph_error(
                "number of lstm inputs captured in the RNN fusion is not equal to "
                "src_sequence_length");
        }

        if ((src_iter->get_arguments().size()) != num_rnn_cell_states)
        {
            throw ngraph_error("number of states for RNN op is not equal to (ht_1|ct_1)");
        }
        auto rnn = std::make_shared<op::Rnn>(src_layer,
                                             src_iter,
                                             weights_layer,
                                             weights_iter,
                                             bias,
                                             num_of_lstm_matched,
                                             num_gates_in_lstm,
                                             sequence_len,
                                             src_layer_feature_size,
                                             feature_size,
                                             num_rnn_cell_states,
                                             rnn_direction,
                                             num_fused_rnn_layers);

        std::vector<std::shared_ptr<op::Slice>> ht_slice_per_timestep(num_of_lstm_matched, nullptr);
        auto rnn_ht_out = std::make_shared<op::GetOutputElement>(rnn, 0);
        auto rnn_ct_out = std::make_shared<op::GetOutputElement>(rnn, 1);

        //slice the rnn ht's
        size_t start_index = 0;
        size_t end_index = batch_size;
        // capture the slices in the reverse order, so it corrosponds to lstm_goes order captured by the Pattern matcher
        for (size_t i = 0; i < num_of_lstm_matched; i++)
        {
            ht_slice_per_timestep[i] = (std::make_shared<op::Slice>(
                rnn_ht_out, Coordinate{start_index, 0}, Coordinate{end_index, feature_size}));
            start_index += batch_size;
            end_index += batch_size;
        }
        std::reverse(ht_slice_per_timestep.begin(), ht_slice_per_timestep.end());

        NGRAPH_DEBUG << "rnn_time_slice: " << ht_slice_per_timestep.size();

        // find the lstm's nodes captured in PM
        auto lstm_goes = m.get_bound_nodes_for_pattern(lstm_node_label);
        std::vector<std::shared_ptr<ngraph::Node>> lstm_nodes;

        // we need to collect LSTM from GOE's, in order to deterministicaly determine
        // the individaual time slice output ht. lstm_goes will hold the GOE in the decreasing
        // order of the time slices
        for (size_t i = 0; i < lstm_goes.size(); i++)
        {
            // lstm's will be the input to GOE's
            lstm_nodes.push_back(lstm_goes[i]->get_arguments()[0]);
        }

        if (sequence_len != lstm_nodes.size())
        {
            throw ngraph_error(" Number of lstm nodes in RNN layer is not equal to time slices");
        }

        if (lstm_nodes.size() != lstm_goes.size() &&
            lstm_goes.size() != ht_slice_per_timestep.size())
        {
            throw ngraph_error(
                "Number of slices of rnn output ht is not equal to the time slices in RNN layer");
        }

        // collect all the consumers of LSTM goe's (ht)
        std::set<std::shared_ptr<ngraph::Node>> lstm_goe0_user;
        std::unordered_map<std::shared_ptr<Node>, std::shared_ptr<Node>> map_goe_to_lstm_slices;
        std::shared_ptr<Node> goe_0;
        for (size_t index = 0; index < lstm_nodes.size(); index++)
        {
            // now get the GOE0 which is the first output of lstm (ht)
            for (auto& goes : lstm_nodes[index]->get_outputs().at(0).get_inputs())
            {
                auto goe_node = std::dynamic_pointer_cast<op::GetOutputElement>(goes->get_node());
                // first output node of lstm
                if (goe_node->get_n() == 0)
                {
                    goe_0 = goes->get_node();
                }
            }

            for (auto goe0_user : goe_0->get_users())
            {
                if (std::find(lstm_nodes.begin(), lstm_nodes.end(), goe0_user) ==
                        lstm_nodes.end() &&
                    !is_unreachable(goe0_user))
                {
                    lstm_goe0_user.insert(goe0_user);
                    map_goe_to_lstm_slices[goe_0] = ht_slice_per_timestep[index];
                    NGRAPH_DEBUG << "ht_slice: " << ht_slice_per_timestep[index]->get_name()
                                 << " goe0_user " << goe0_user->get_name() << " ";
                }
            }
        }

        //now go through the lstm consumers and replace them with the slice
        std::vector<std::shared_ptr<Node>> new_args;
        for (auto& node : lstm_goe0_user)
        {
            for (auto& node_args : node->get_arguments())
            {
                if (std::find(lstm_goes.begin(), lstm_goes.end(), node_args) != lstm_goes.end())
                {
                    NGRAPH_DEBUG << " args_shape " << join(node_args->get_shape())
                                 << "name: " << node_args->get_name();
                    new_args.push_back(map_goe_to_lstm_slices[node_args]);
                }
                else
                {
                    NGRAPH_DEBUG << " args_shape " << join(node_args->get_shape())
                                 << "name: " << node_args->get_name();
                    new_args.push_back(node_args);
                }
            }
            NGRAPH_DEBUG << "node bring replaced " << node->get_name();
            auto new_node = node->copy_with_new_args(new_args);
            if (!std::dynamic_pointer_cast<op::Result>(node))
            {
                ngraph::replace_node(node, new_node);
            }
            NGRAPH_DEBUG << "node: " << node->get_name() << " replaced with  "
                         << new_node->get_name();
            new_args.clear();
        }
        NGRAPH_DEBUG << "End of recurrent fusion call back "
                     << "matched_node: " << m.get_match_root()->get_name();
        ngraph::replace_node(m.get_match_root(), ht_slice_per_timestep[0]);
        return true;

    };

    std::set<std::shared_ptr<pattern::op::Label>> empty_correlated_matches;
    auto m = std::make_shared<pattern::RecurrentMatcher>(
        lstm_node_label, rpattern_ht_1, empty_correlated_matches, callback);
    this->add_matcher(m);
}

void ngraph::runtime::cpu::pass::RecurrentRNNFusion::construct_superfused_rnn_fprop()
{
    auto src_layer_label = std::make_shared<pattern::op::Label>(element::f32, Shape{30, 100});
    auto src_iter_label = std::make_shared<pattern::op::Label>(element::f32, Shape{20, 100});
    auto weights_layer_label = std::make_shared<pattern::op::Label>(element::f32, Shape{400, 100});
    auto weights_iter_label = std::make_shared<pattern::op::Label>(element::f32, Shape{400, 100});
    auto bias_label = std::make_shared<pattern::op::Label>(element::f32, Shape{400});

    size_t number_of_timesteps = 3;
    size_t number_of_gates_per_cell = 4;
    size_t src_seq_length = 3;
    size_t src_layer_feature_size = 100;
    size_t feature_size = 100;
    size_t num_rnn_cell_states = 2;
    size_t rnn_direction = 1;
    size_t num_of_rnn_fused_layer = 1;
    size_t batch_size = 10;

    auto rnn_node = std::make_shared<op::Rnn>(src_layer_label,
                                              src_iter_label,
                                              weights_layer_label,
                                              weights_iter_label,
                                              bias_label,
                                              number_of_timesteps,
                                              number_of_gates_per_cell,
                                              src_seq_length,
                                              src_layer_feature_size,
                                              feature_size,
                                              num_rnn_cell_states,
                                              rnn_direction,
                                              num_of_rnn_fused_layer);

    NodeVector ht_slice_per_timestep;
    auto rnn_ht_out = std::make_shared<op::GetOutputElement>(rnn_node, 0);
    auto rnn_ht_label =
        std::make_shared<pattern::op::Label>(rnn_ht_out, nullptr, NodeVector{rnn_ht_out});
    auto rnn_ct_out = std::make_shared<op::GetOutputElement>(rnn_node, 1);

    //slice the rnn ht's
    size_t start_index = 0;
    size_t end_index = batch_size;
    // capture the slices in the reverse order, so it corrosponds to lstm_goes order captured by the Pattern matcher
    for (size_t i = 0; i < number_of_timesteps; i++)
    {
        ht_slice_per_timestep.push_back((std::make_shared<op::Slice>(
            rnn_ht_out, Coordinate{start_index, 0}, Coordinate{end_index, feature_size})));
        start_index += batch_size;
        end_index += batch_size;
    }
    std::reverse(ht_slice_per_timestep.begin(), ht_slice_per_timestep.end());
    auto reshape_pred = [](std::shared_ptr<Node> n) {
        return static_cast<bool>(std::dynamic_pointer_cast<op::Reshape>(n));
    };
    auto skip_reshape_3 =
        std::make_shared<pattern::op::Skip>(ht_slice_per_timestep[0], reshape_pred);
    auto skip_reshape_2 =
        std::make_shared<pattern::op::Skip>(ht_slice_per_timestep[1], reshape_pred);
    auto skip_reshape_1 =
        std::make_shared<pattern::op::Skip>(ht_slice_per_timestep[2], reshape_pred);

    auto input_for_next_layer =
        std::make_shared<op::Concat>(NodeVector{skip_reshape_3, skip_reshape_2, skip_reshape_1}, 0);
    auto rnn_node_label = std::make_shared<pattern::op::Label>(
        input_for_next_layer, nullptr, NodeVector{input_for_next_layer});

    pattern::recurrent_graph_rewrite_callback callback =
        [src_layer_label,
         src_iter_label,
         weights_layer_label,
         weights_iter_label,
         bias_label,
         rnn_ht_label](pattern::RecurrentMatcher& m) {
            auto scr_nodes = m.get_bound_nodes_for_pattern(src_layer_label);
            auto rnn_ht_out_nodes = m.get_bound_nodes_for_pattern(rnn_ht_label);
            auto number_of_rnn_cell_matched = m.get_number_of_recurrent_matches();
            std::cout << "########## In Recurrent RNN super fusion ############ " << std::endl;
            std::cout << "Number of RNN's Matched: " << number_of_rnn_cell_matched << std::endl;
            std::cout << "matched_root: " << m.get_match_root()->get_name() << std::endl;
            std::cout << "src_layer_node: " << scr_node[0]->get_name() << std::endl;

            // // we can fuse across different RNN layers only if SLC != DLC
            // for (size_t i=0; i< number_of_rnn_cell_matched; i++)
            // {
            //     if(src_nodes[i]->get_shape()[1] != rnn_ht_out_nodes[i]->get_shape()[1])
            //     {
            //         return false;
            //     }

            // }
            // std::vector<std::shared_ptr<pattern::op::Label>> src_iter_labels{src_iter};
            // auto src_iter = compute_rnn_args(src_iter_labels, m);

            // std::vector<std::shared_ptr<pattern::op::Label>> weights_layer_labels{weights_layer};
            // auto weights_layer = compute_rnn_args(weights_layer_labels, m);

            // std::vector<std::shared_ptr<pattern::op::Label>> weights_iter_labels{weights_iter};
            // auto weights_iter = compute_rnn_args(weights_iter_labels, m);

            // std::vector<std::shared_ptr<pattern::op::Label>> src_layer_labels{src_layer};
            // auto src_layer = compute_rnn_args(src_layer_labels, m, true);

            return false;

        };

    std::set<std::shared_ptr<pattern::op::Label>> empty_correlated_matches;
    auto m = std::make_shared<pattern::RecurrentMatcher>(
        rnn_node_label, src_layer_label, empty_correlated_matches, callback);
    this->add_matcher(m);
}