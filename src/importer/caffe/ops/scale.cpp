/* Copyright 2019-2020 Canaan Inc.
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
 */
#include "../caffe_importer.h"
// #include <nncase/ir/ops/binary.h>
// #include <nncase/ir/ops/constant.h>
#include <nncase/ir/ops/scale.h>

using namespace nncase;
using namespace nncase::importer;
using namespace nncase::ir;

DEFINE_CAFFE_LOWER(Scale)
{
    // auto &input = *output_tensors_.at(op.bottom(0));
    // auto &param = op.scale_param();

    // auto gamma = load_tensor<1>(op.blobs(0));
    
    // std::vector<float> gamma_vec_c(gamma.begin(), gamma.end());
    // std::vector<float> gamma_vec;
    // for (size_t n = 0; n < input.shape()[0]; n++)
    // {
    //     for (size_t c = 0; c < input.shape()[1]; c++)
    //     {
    //         for (size_t hw = 0; hw < input.shape()[2] * input.shape()[3]; hw++)
    //         {
    //             gamma_vec.push_back(gamma_vec_c[c]);
    //         }
    //     }
    // }
    // auto gamma_const = graph_.emplace<constant>(dt_float32, input.shape(), gamma_vec);
    // gamma_const->name(op.name() + "/gamma_const");
    // auto mul = graph_.emplace<binary>(binary_mul, input.shape(), gamma_const->output().shape(), value_range<float>::full());
    // mul->name(op.name() + "/mul");

    // mul->input_b().connect(gamma_const->output());

    // if (!param.has_bias_term())
    // {
    //     input_tensors_.emplace(&mul->input_a(), op.bottom(0));
    //     output_tensors_.emplace(op.top(0), &mul->output());
    // }
    // else
    // {
    //     auto beta = load_tensor<1>(op.blobs(1));
    //     std::vector<float> beta_vec_c(beta.begin(), beta.end());
    //     std::vector<float> beta_vec;
    //     for (size_t n = 0; n < input.shape()[0]; n++)
    //     {
    //         for (size_t c = 0; c < input.shape()[1]; c++)
    //         {
    //             for (size_t hw = 0; hw < input.shape()[2] * input.shape()[3]; hw++)
    //             {
    //                 beta_vec.push_back(beta_vec_c[c]);
    //             }
    //         }
    //     }
    //     auto beta_const = graph_.emplace<constant>(dt_float32, input.shape(), beta_vec);
    //     beta_const->name(op.name() + "/beta_const");
    //     auto add = graph_.emplace<binary>(binary_add, mul->output().shape(), beta_const->output().shape(), value_range<float>::full());
    //     add->name(op.name() + "/add");
    //     add->input_a().connect(gamma_const->output());
    //     add->input_b().connect(beta_const->output());
    //     input_tensors_.emplace(&mul->input_a(), op.bottom(0));
    //     output_tensors_.emplace(op.top(0), &add->output());
    // }

    auto &input = *output_tensors_.at(op.bottom(0));
    auto &param = op.scale_param();

    auto op_data = get_op_data(op, caffemodel);

    auto gamma = load_tensor<1>(op_data.blobs(0));
    std::vector<float> gamma_vec_c(gamma.begin(), gamma.end());
    std::vector<float> beta_vec_c(gamma.size(), 0.f);
    if (param.has_bias_term())
    {
        auto beta = load_tensor<1>(op_data.blobs(1));
        beta_vec_c.assign(beta.begin(), beta.end());
    }

    auto sc = graph_.emplace<scale>(dt_float32, input.shape(), gamma_vec_c, beta_vec_c);
    sc->name(op.name() + "/scale");

    input_tensors_.emplace(&sc->input(), op.bottom(0));
    output_tensors_.emplace(op.top(0), &sc->output());
}