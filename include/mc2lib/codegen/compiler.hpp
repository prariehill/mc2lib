/*
 * Copyright (c) 2014-2015, Marco Elver
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the
 *    distribution.
 *
 *  * Neither the name of the software nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef MC2LIB_CODEGEN_COMPILER_HPP_
#define MC2LIB_CODEGEN_COMPILER_HPP_

#include "../memconsistency/model14.hpp"
#include "../types.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mc2lib {
namespace codegen {

namespace mc = memconsistency;

class AssemblerState;
class Operation;

typedef std::shared_ptr<Operation> OperationPtr;
typedef std::unordered_map<types::Pid, std::vector<OperationPtr>> Threads;

class Operation {
  public:
    explicit Operation(types::Pid pid)
        : pid_(pid)
    {}

    virtual ~Operation()
    {}

    /**
     * Clone the instance.
     */
    virtual OperationPtr clone() const = 0;

    /**
     * Provide Reset, as emit functions may modify the state of an Operation to
     * store information to map instructions to events.
     */
    virtual void reset() = 0;

    /**
     * Prepares the operation for emit; common emit code.
     *
     * @param[in,out] asms Pointer to AssemblerState instance of calling Compiler.
     *
     * @return true if can emit; false otherwise.
     */
    virtual bool enable_emit(AssemblerState *asms) = 0;

    /**
     * Generate static program-order relation.
     *
     * @param before Pointer to last Operation; nullptr if none exists.
     * @param[in,out] asms Pointer to AssemblerState instance maintained by Compiler.
     * @param[out] ew Pointer to ExecWitness to be inserted into.
     *
     * @return Last event in program-order generated by this operation.
     */
    virtual void insert_po(const Operation *before,
                           AssemblerState *asms, mc::model14::ExecWitness *ew) = 0;

    /**
     * Emit X86-64 machine code; fill in architecture-dependent ordering
     * relations.
     *
     * @param start Instruction pointer to first instruction when executing.
     * @param[in,out] asms Pointer to AssemblerState instance of calling Compiler.
     * @param[out] arch Pointer to target memory consistency model Architecture.
     * @param[out] code Pointer to memory to be copied into.
     * @param len Maximum lenth of code.
     *
     * @return Size of emitted code.
     */
    virtual std::size_t emit_X86_64(types::InstPtr start,
                                    AssemblerState *asms, mc::model14::Arch_TSO *arch,
                                    void *code, std::size_t len)
    {
        // Provide default (nop), as not all operations may be implementable on
        // all architectures.
        return 0;
    }

    /**
     * Accessor for last event generated. Also use to insert additional
     * ordering based on passed next_event (typically fences).
     *
     * @param next_event The first event in program-order of the next Operation;
     *                   nullptr if none exists.
     *
     * @return Last event in program-order; nullptr if none exists.
     */
    virtual const mc::Event* last_event(const mc::Event *next_event) const = 0;

    /**
     * Insert dynamic ordering relations (read-from, coherence-order).
     *
     * @param ip Instruction pointer of instruction for which a value was observed.
     * @param addr Address for observed operation.
     * @param from_id Pointer to observed memory (WriteIDs).
     * @param size Total size of observed memory operations in from_id;
     *             implementation should assert expected size.
     * @param[in,out] asms Pointer to AssemblerState instance maintained by Compiler.
     * @param[out] ew Pointer to ExecWitness to be inserted into.
     *
     * @return Success or not.
     */
    virtual bool insert_from(types::InstPtr ip, types::Addr addr,
                             const types::WriteID *from_id, std::size_t size,
                             AssemblerState *asms, mc::model14::ExecWitness *ew) = 0;

    types::Pid pid() const
    { return pid_; }

    void set_pid(types::Pid pid)
    { pid_ = pid; }

  private:
    types::Pid pid_;
};

class AssemblerState {
  public:
    static constexpr std::size_t MAX_INST_SIZE = 8;
    static constexpr std::size_t MAX_INST_EVTS  = MAX_INST_SIZE / sizeof(types::WriteID);
    static constexpr types::WriteID INIT_WRITE = 0x00;
    static constexpr types::WriteID MIN_WRITE = INIT_WRITE + 1;
    static constexpr types::WriteID MAX_WRITE = 0xff - (MAX_INST_EVTS - 1);
    static constexpr types::Poi MIN_READ = 0x8000000000000000ULL;
    static constexpr types::Poi MAX_READ = 0xffffffffffffffffULL - (MAX_INST_EVTS - 1);

    explicit AssemblerState(mc::model14::ExecWitness *ew)
        : ew_(ew)
    {}

    void reset()
    {
        last_write_id_ = MIN_WRITE - 1;
        last_read_id_ = MIN_READ - 1;

        writes_.clear();
    }

    bool exhausted() const
    { return last_write_id_ >= MAX_WRITE || last_read_id_ >= MAX_READ; }

    template <std::size_t max_size, class Func>
    std::array<const mc::Event*, max_size/sizeof(types::WriteID)>
    make_event(types::Pid pid, mc::Event::Type type,
               types::Addr addr, std::size_t size, Func mkevt)
    {
        static_assert(max_size <= MAX_INST_SIZE, "Invalid size!");
        static_assert(sizeof(types::WriteID) <= max_size, "Invalid size!");
        static_assert(max_size % sizeof(types::WriteID) == 0, "Invalid size!");
        assert(size <= max_size);
        assert(sizeof(types::WriteID) <= size);
        assert(size % sizeof(types::WriteID) == 0);

        assert(!exhausted());

        std::array<const mc::Event*, max_size/sizeof(types::WriteID)> result;

        for (std::size_t i = 0; i < size/sizeof(types::WriteID); ++i) {
            result[i] = mkevt(i * sizeof(types::WriteID));
        }

        return result;
    }

    template <std::size_t max_size>
    std::array<const mc::Event*, max_size/sizeof(types::WriteID)>
    make_read(types::Pid pid, mc::Event::Type type, types::Addr addr,
              std::size_t size = max_size)
    {
        return make_event<max_size>(pid, type, addr, size, [&](types::Addr offset) {
            const mc::Event event =
                mc::Event(type, addr + offset, mc::Iiid(pid, ++last_read_id_));

            return &ew_->events.insert(event, true);
        });
    }

    template <std::size_t max_size>
    std::array<const mc::Event*, max_size/sizeof(types::WriteID)>
    make_write(types::Pid pid, mc::Event::Type type, types::Addr addr,
               types::WriteID *data, std::size_t size = max_size)
    {
        return make_event<max_size>(pid, type, addr, size, [&](types::Addr offset) {
            const types::WriteID write_id = ++last_write_id_;

            const mc::Event event =
                mc::Event(type, addr + offset, mc::Iiid(pid, write_id));

            *(data + offset) = write_id;
            return (writes_[write_id] = &ew_->events.insert(event, true));
        });
    }

    template <std::size_t max_size>
    std::array<const mc::Event*, max_size/sizeof(types::WriteID)>
    get_write(const mc::Event& after, types::Addr addr,
              const types::WriteID *from_id, std::size_t size = max_size)
    {
        static_assert(max_size <= MAX_INST_SIZE, "Invalid size!");
        static_assert(sizeof(types::WriteID) <= max_size, "Invalid size!");
        static_assert(max_size % sizeof(types::WriteID) == 0, "Invalid size!");
        assert(size <= max_size);
        assert(sizeof(types::WriteID) <= size);
        assert(size % sizeof(types::WriteID) == 0);

        std::array<const mc::Event*, max_size/sizeof(types::WriteID)> result;

        for (std::size_t i = 0; i < size/sizeof(types::WriteID); ++i) {
            WriteID_EventPtr::const_iterator write;

            const bool valid = from_id[i] != INIT_WRITE &&
                               (write = writes_.find(from_id[i])) != writes_.end() &&
                               write->second->addr == addr &&
                               write->second->iiid != after.iiid;
            if (valid) {
                result[i] = write->second;
            } else {
                if (from_id[i] != INIT_WRITE) {
                    // While the checker works even if memory is not 0'ed out
                    // completely, as the chances of reading a write-id from a
                    // previous test that has already been used in this test is
                    // low and doesn't necessarily cause a false positive, it is
                    // recommended that memory is 0'ed out for every new test.
                    std::cerr << "warn: Invalid write, but not INIT_WRITE! "
                              << "Has memory been reset?" << std::endl;
                }

                auto initial = mc::Event(mc::Event::Write, addr, mc::Iiid(-1, addr));
                result[i] = &ew_->events.insert(initial);
            }

            addr += sizeof(types::WriteID);
        }

        return result;
    }

  private:
    typedef std::unordered_map<types::WriteID, const mc::Event*> WriteID_EventPtr;

    mc::model14::ExecWitness *ew_;

    WriteID_EventPtr writes_;

    types::WriteID last_write_id_;
    types::Poi last_read_id_;
};

template <class Backend>
class Compiler {
  public:
    explicit Compiler(mc::model14::Architecture *arch, mc::model14::ExecWitness *ew,
                      const Threads *threads = nullptr)
        : asms_(ew), backend_(arch), ew_(ew)
    {
        reset(threads);
    }

    void reset(const Threads *threads = nullptr)
    {
        threads_ = threads;

        if (threads_ != nullptr) {
            // Must ensure all Operation instances have been reset.
            for (const auto& thread : (*threads_)) {
                for(const auto& op : thread.second) {
                    op->reset();
                }
            }
        }

        asms_.reset();
        backend_.reset();
        ew_->clear();
        ip_to_op_.clear();
    }

    const Threads* threads()
    { return threads_; }

    const AssemblerState* asms() const
    { return &asms_; }

    std::size_t emit(types::InstPtr base, Operation *op, void *code, std::size_t len,
                     const Operation **last_op) {
        // Prepare op for emit.
        if (!op->enable_emit(&asms_)) {
            return 0;
        }

        // Generate program-order.
        if (last_op != nullptr) {
            op->insert_po(*last_op, &asms_, ew_);
            *last_op = op;
        } else {
            op->insert_po(nullptr, &asms_, ew_);
        }

        // Generate code and architecture-specific ordering relations.
        const std::size_t op_len = backend_(base, op, &asms_, code, len);
        assert(op_len != 0);

        // Base IP must be unique!
        assert(ip_to_op_.find(base) == ip_to_op_.end());
        // Insert IP to Operation mapping.
        ip_to_op_[base] = std::make_pair(base + op_len, op);

        return op_len;
    }

    std::size_t emit(types::Pid pid, types::InstPtr base, void *code, std::size_t len)
    {
        assert(threads_ != nullptr);

        auto thread = threads_->find(pid);

        if (thread == threads_->end()) {
            return 0;
        }

        std::size_t emit_len = 0;
        const Operation *last_op = nullptr;

        for (const auto& op : thread->second) {
            // Generate code and architecture-specific ordering relations.
            const std::size_t op_len = emit(base + emit_len, op.get(), code,
                                            len - emit_len, &last_op);

            emit_len += op_len;
            assert(emit_len <= len);

            code = static_cast<char*>(code) + op_len;
        }

        return emit_len;
    }

    bool insert_from(types::InstPtr ip, types::Addr addr,
                     const types::WriteID *from_id, std::size_t size)
    {
        auto op = ip_to_op(ip);

        if (op == nullptr) {
            return false;
        }

        return op->insert_from(ip, addr, from_id, size, &asms_, ew_);
    }

    Operation* ip_to_op(types::InstPtr ip)
    {
        if (ip_to_op_.empty()) {
            // Can be legally empty if no code has yet been emitted, i.e. right
            // after host system startup. By not faulting here, the host can
            // still use ip_to_op to check if an instruction needs to be
            // treated specially: before any code has been emitted, no
            // instructions will be treated specially.
            return nullptr;
        }

        auto e = --ip_to_op_.upper_bound(ip);

        if (!(e->first <= ip && ip < e->second.first)) {
            return nullptr;
        }

        return e->second.second;
    }

  private:
    typedef std::map<types::InstPtr, std::pair<types::InstPtr, Operation*>> InstPtr_Op;

    AssemblerState asms_;
    Backend backend_;
    const Threads *threads_;

    mc::model14::ExecWitness *ew_;

    // Each processor executes unique code, hence IP must be unique.  Only
    // stores the start IP of Op-sequence
    InstPtr_Op ip_to_op_;
};

class Backend_X86_64 {
  public:
    explicit Backend_X86_64(mc::model14::Architecture *arch)
        : arch_(dynamic_cast<mc::model14::Arch_TSO*>(arch))
    {
        assert(arch_ != nullptr);
    }

    void reset()
    {
        arch_->clear();
    }

    std::size_t operator ()(types::InstPtr start, Operation *op,
                            AssemblerState *asms,
                            void *code, std::size_t len) const
    {
        return op->emit_X86_64(start, asms, arch_, code, len);
    }

  private:
    mc::model14::Arch_TSO *arch_;
};

template <class T>
inline Threads
threads_extract(T *container)
{
    Threads result;
    std::unordered_set<Operation*> used;

    for (auto& op : (*container)) {
        assert(op.get() != nullptr);

        if (used.insert(op.get()).second) {
            // Using same instance of Operation multiple times is not
            // permitted.
            op = op->clone();
        }

        result[op->pid()].emplace_back(op);
    }

    return result;
}

inline std::size_t
threads_size(const Threads& threads)
{
    std::size_t result = 0;

    for (const auto& thread : threads) {
        result += thread.second.size();
    }

    return result;
}

} /* namespace codegen */
} /* namespace mc2lib */

#endif /* MC2LIB_CODEGEN_COMPILER_HPP_ */

/* vim: set ts=4 sts=4 sw=4 et : */
