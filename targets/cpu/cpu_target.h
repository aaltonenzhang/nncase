/* Copyright 2019-2021 Canaan Inc.
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
#pragma once
#include <nncase/targets/neutral_target.h>

namespace nncase::targets
{
class cpu_target : public neutral_target
{
public:
    using neutral_target::neutral_target;

    void register_target_dependent_passes(const module_type_t &type, ir::transforms::pass_manager &pass_mgr, bool use_ptq, bool split_w_scale) override;
};
}
