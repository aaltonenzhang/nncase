/* Copyright 2020 Canaan Inc.
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
#include <lyra/lyra.hpp>
#include <string>

namespace nncase::cli
{
class compile_command
{
public:
    compile_command(lyra::cli &cli);

private:
    void run();

private:
    std::string input_filename_;
    std::string output_filename_;
    std::string input_format_;
    std::string target_name_;
    std::string output_arrays_;
    std::string dump_dir_;
    std::string dataset_;
    std::string dataset_format_ = "image";
    bool dump_ir_ = false;
    bool dump_asm_ = false;
};
}