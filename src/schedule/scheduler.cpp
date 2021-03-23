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
#include <nncase/ir/op_utils.h>
#include <nncase/ir/ops/bitcast.h>
#include <nncase/ir/ops/concat.h>
#include <nncase/ir/ops/constant.h>
#include <nncase/ir/visitor.h>
#include <nncase/schedule/scheduler.h>
#include <nncase/targets/target.h>
#include <unordered_map>

#include <xtensor/xarray.hpp>

using namespace nncase;
using namespace nncase::ir;
using namespace nncase::schedule;

namespace nncase::schedule
{
struct schedule_context : module_schedule_result
{
    module_type_t module_type;
    std::span<ir::output_node *> outputs;
    std::unordered_map<const ir::output_connector *, logical_buffer> logical_buffers;
    std::vector<physical_buffer> physical_buffers;

    void generate_compute_sequence();
    void make_logical_buffers();
    void analyze_buffer_alias();
    void fix_concat_indices();
    void fix_lifetime();
    void make_physical_buffers();
    void allocate_physical_buffers(target &target);
    void assign_allocations();
};
}

namespace
{
memory_location_t decide_memory_location(ir::output_connector &conn) noexcept
{
    auto &opcode = conn.owner().runtime_opcode();
    if (opcode == op_input_node)
        return mem_input;
    else if (opcode == op_constant)
        return mem_rdata;

    auto connections = conn.connections();
    if (std::any_of(connections.begin(), connections.end(), [](input_connector *conn) { return conn->owner().runtime_opcode() == op_output_node; }))
        return mem_output;
    return conn.memory_location();
}

shape_t to_strides(const shape_t &shape)
{
    shape_t strides(shape.size());
    xt::compute_strides(shape, xt::layout_type::row_major, strides);
    return strides;
}

class lifetime_recorder
{
public:
    lifetime_recorder(std::unordered_map<const ir::output_connector *, logical_buffer> &buffers)
        : buffers_(buffers)
    {
    }

    void allocate(ir::output_connector &conn)
    {
        auto it = buffers_.find(&conn);
        if (it == buffers_.end())
        {
            logical_buffer buffer(next_buffer_id_++, conn, decide_memory_location(conn));
            buffer.lifetime().birth = cnt_age_;
            buffer.lifetime().used_count = conn.connections().size();
            buffers_.emplace(&conn, buffer);
        }
    }

    void release(ir::output_connector &conn)
    {
        auto node = buffers_.find(&conn);
        if (node != buffers_.end())
        {
            auto &lifetime = node->second.lifetime();
            if (!lifetime.is_alive())
                throw std::runtime_error("Trying to free a released buffer");
            else
                lifetime.used_count--;
        }
    }

    void grow_age()
    {
        cnt_age_++;
        for (auto &b : buffers_)
        {
            auto &lifetime = b.second.lifetime();
            if (lifetime.is_alive())
                lifetime.age++;
        }
    }

private:
    size_t next_buffer_id_ = 0;
    size_t cnt_age_ = 0;
    std::unordered_map<const ir::output_connector *, logical_buffer> &buffers_;
};
}

void schedule_context::generate_compute_sequence()
{
    auto alloc_visitor = make_relay_ir_visitor([&](node &node) {
        if (node.attributes() & node_attr_action)
            compute_sequence.emplace_back(&node);
    });

    alloc_visitor.visit(outputs);
}

void schedule_context::make_logical_buffers()
{
    lifetime_recorder lr(logical_buffers);
    auto alloc_visitor = make_relay_ir_visitor([&](node &node) {
        for (auto out : node.outputs())
            lr.allocate(*out);

        lr.grow_age();

        for (auto in : node.inputs())
        {
            auto out = in->connection();
            assert(out);
            lr.release(*out);
        }
    });
    alloc_visitor.visit(outputs);
}

void schedule_context::analyze_buffer_alias()
{
    auto alias_visitor = make_relay_ir_visitor([&](node &node) {
        // 1. bitcast
        if (auto b = node_cast<bitcast>(node))
        {
            auto &input = *b->input().connection();
            auto &in_buf = logical_buffers.at(&input);
            auto &out_buf = logical_buffers.at(&b->output());

            if (out_buf.memory_location() == mem_output && in_buf.memory_location() == mem_data)
                in_buf.memory_location() = mem_output;

            // input & rdata should be copied to output
            if (out_buf.memory_location() != mem_output
                || (in_buf.memory_location() != mem_input && in_buf.memory_location() != mem_rdata))
            {
                shape_t begin(input.shape().size(), 0);
                out_buf.parent() = { &logical_buffers.at(&input), begin };
                b->attributes(b->attributes() & ~node_attr_action);
            }
        }

        // 2. concat
        else if (auto c = node_cast<concat>(node))
        {
            auto inputs = c->inputs();
            auto outputs = c->output().connections();

            // simple & exlusive concat
            // input & rdata should be copied to output
            if ((c->axis() == 0 || std::all_of(inputs[0]->shape().begin(), inputs[0]->shape().begin() + c->axis(), [](size_t dim) { return dim == 1; }))
                && std::all_of(inputs.begin(), inputs.end(), [this](input_connector *in) {
                       auto &in_buf = logical_buffers.at(in->connection());
                       return (in_buf.memory_location() != mem_input && in_buf.memory_location() != mem_rdata)
                           && in->connection()->owner().runtime_opcode() != op_slice;
                   })
                && std::count_if(outputs.begin(), outputs.end(), [](input_connector *in) {
                       return in->owner().runtime_opcode() == op_concat;
                   })
                    < 2)
            {
                // Fix parent later
                c->attributes(c->attributes() & ~node_attr_action);
            }
        }
    });
    alias_visitor.visit(outputs);
}

void schedule_context::fix_concat_indices()
{
    auto fix_concat_visitor = make_relay_ir_visitor([&](node &node) {
        if (auto c = node_cast<concat>(node))
        {
            if (c->attributes() & node_attr_action)
                return;

            // 1. Init indices
            {
                auto axis = c->axis();
                auto &out_buf = logical_buffers.at(&c->output());
                shape_t cnt_begin(c->input_at(0).shape().size(), 0);
                for (auto in : c->inputs())
                {
                    auto &in_buf = logical_buffers.at(in->connection());
                    in_buf.parent() = { &out_buf, cnt_begin };
                    cnt_begin[axis] += in->shape()[axis];
                }
            }

            // 2. Iterate parent
            auto child = c;
            while (true)
            {
                auto parent = try_get_direct_child<concat>(*child);
                if (!parent || parent->attributes() & node_attr_action)
                    break;
                auto index = get_input_index(*parent, child->output());
                auto axis = parent->axis();
                shape_t child_begin(child->output().shape().size(), 0);
                child_begin[axis] += std::accumulate(parent->concat_dims().begin(), parent->concat_dims().begin() + index, 0);

                auto &in_buf = logical_buffers.at(&child->output());
                auto &out_buf = logical_buffers.at(&parent->output());
                in_buf.parent() = { &out_buf, child_begin };
                for (auto &in : c->inputs())
                {
                    auto &in_buf = logical_buffers.at(in->connection());
                    auto &desc = *in_buf.parent();
                    desc.parent = &out_buf;
                    desc.begin += child_begin;
                }

                child = parent;
            }
        }
    });
    fix_concat_visitor.visit(outputs);
}

void schedule_context::fix_lifetime()
{
    // Assign parent
    for (auto &bp : logical_buffers)
    {
        auto &p = bp.second.parent();
        if (p)
        {
            while (p->parent->parent())
                p = p->parent->parent();
        }
    }

    // Extend lifetime
    for (auto &bp : logical_buffers)
    {
        auto &lifetime = bp.second.lifetime();
        if (bp.second.parent())
        {
            auto &p_liftime = bp.second.parent()->parent->lifetime();
            auto birth = std::min(lifetime.birth, p_liftime.birth);
            auto end = std::max(lifetime.end(), p_liftime.end());
            p_liftime.birth = birth;
            p_liftime.age = end - birth;
        }
    }
}

void schedule_context::make_physical_buffers()
{
    std::unordered_map<logical_buffer *, size_t> physical_ids;
    for (auto &bp : logical_buffers)
    {
        if (!bp.second.parent())
        {
            auto id = physical_ids.size();
            physical_ids.emplace(&bp.second, id);
            physical_buffers.emplace_back(id, bp.second);
        }
    }

    // Assign parents
    for (auto &bp : logical_buffers)
    {
        auto parent = bp.second.parent() ? bp.second.parent()->parent : &bp.second;
        bp.second.physical() = &physical_buffers.at(physical_ids.at(parent));
    }
}

void schedule_context::allocate_physical_buffers(target &target)
{
    allocator_map_t allocators;
    std::vector<std::shared_ptr<buffer_allocator>> allocator_holder;
    target.register_allocators(module_type, allocators, allocator_holder);

    for (auto &usage_p : max_usages)
    {
        // All of rdata live through the module lifetime
        if (usage_p.first == mem_rdata)
        {
            auto it = allocators.find(usage_p.first);
            if (it != allocators.end())
                it->second->base_offset(usage_p.second);
        }
    }

    std::vector<physical_buffer *> orders;
    orders.reserve(physical_buffers.size());
    for (auto &b : physical_buffers)
        orders.emplace_back(&b);
    std::sort(orders.begin(), orders.end(), [](const physical_buffer *lhs, const physical_buffer *rhs) {
        return lhs->lifetime().birth < rhs->lifetime().birth;
    });

    for (auto &b : orders)
        allocators.at(b->owner().memory_location())->mark(*b);

    for (auto &alloc : allocators)
    {
        alloc.second->finish();
        max_usages.emplace(alloc.first, alloc.second->max_usage());
    }

    for (auto &b : physical_buffers)
        b.allocation() = memory_span { allocators.at(b.owner().memory_location())->allocations().at(&b) };
}

void schedule_context::assign_allocations()
{
    auto alloc_visitor = make_relay_ir_visitor([&](node &node) {
        for (auto out : node.outputs())
        {
            auto &lbuf = logical_buffers.at(out);
            auto &owner = lbuf.physical()->owner();
            auto &memory = lbuf.physical()->allocation();

            // TODO: take account of subbuffer
            buffer_allocation alloc;
            alloc.memory_location = owner.memory_location();
            alloc.type = lbuf.type();
            alloc.size = ir::get_bytes(lbuf.type(), lbuf.shape());
            alloc.shape = lbuf.shape();
            if (lbuf.parent() && node.runtime_opcode() != op_bitcast)
                alloc.parent_shape = owner.shape();
            else
                alloc.parent_shape = lbuf.shape();
            alloc.strides = to_strides(alloc.parent_shape);
            alloc.start = memory.start;
            if (lbuf.parent())
            {
                auto &begin = lbuf.parent()->begin;
                alloc.start += ir::get_bytes(lbuf.type()) * xt::element_offset<size_t>(alloc.strides, begin.begin(), begin.end());
            }

            allocations.emplace(out, alloc);
        }
    });
    alloc_visitor.visit(outputs);
}

schedule_result scheduler::schedule(bool skip_buffer_alias)
{
    auto schedule_module = [&](ir::graph &graph, std::span<ir::output_node *> outputs, module_schedule_result &result) {
        schedule_context context;
        context.graph = &graph;
        context.module_type = graph.module_type();
        context.outputs = outputs;

        context.make_logical_buffers();
        if (!skip_buffer_alias)
            context.analyze_buffer_alias();
        context.fix_concat_indices();
        context.fix_lifetime();
        context.generate_compute_sequence();
        context.make_physical_buffers();
        context.allocate_physical_buffers(target_);
        context.assign_allocations();
        result = module_schedule_result { context };
    };

    schedule_result result;
    result.main_module = &main_graph_;
    result.graph_orders.reserve(main_graph_.subgraphs().size() + 1);

    // 1. main graph
    schedule_module(*result.main_module, outputs_, result.modules[result.main_module]);
    result.graph_orders.emplace_back(result.main_module);

    // 2. subgraphs
    for (auto &subgraph : main_graph_.subgraphs())
    {
        schedule_module(*subgraph, subgraph->outputs(), result.modules[subgraph.get()]);
        result.graph_orders.emplace_back(subgraph.get());
    }

    return result;
}