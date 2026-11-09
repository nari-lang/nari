#include "bytecode/bytecode.h"
#include "jit/trace_jit.h"
#include <cstdint>
#include <cstring>

namespace nari {
namespace bytecode {

#ifndef DISABLE_JIT
// trace recording: called once per instruction while recording is active
void VM::trace_record_step(OpCode op, const uint8_t *insn_base, size_t insn_pc, uint32_t func_idx) {
    using Kind = jit::TraceStep::Kind;
    using TraceType = jit::TraceType;
    auto &rec = trace_recorder;
    if (rec.aborted || !rec.recording) {
        return;
    }
    // skip instructions from inlined function bodies.
    // When OP_CALL is successfully inlined (e.g. IntAdd), the interpreter still enters the called function,
    // so we track call depth and ignore all instructions until the matching RETURN pops us back to the recording
    // function.
    if (rec.inline_depth > 0) {
        if (op == OpCode::OP_CALL || op == OpCode::OP_CALL_METHOD || op == OpCode::OP_NEW_INSTANCE) {
            ++rec.inline_depth;
        } else if (op == OpCode::OP_RETURN) {
            --rec.inline_depth;
        }
        return; // skip recording this instruction
    }
    if (func_idx != rec.func_idx) {
        rec.aborted = true;
        rec.recording = false;
        return;
    }

    // re-read uint16 operand from instruction bytes
    auto op16 = [&]() -> uint16_t { return (uint16_t(insn_base[1]) << 8) | uint16_t(insn_base[2]); };

    // determine TraceType from a live Value tag
    auto vtype = [](const Value &v) -> TraceType {
        if (v.is_int()) {
            return TraceType::Int;
        }
        if (v.is_float()) {
            return TraceType::Float;
        }
        if (v.is_bool()) {
            return TraceType::Bool;
        }
        return TraceType::Unknown;
    };

    auto abort_recording = [&]() {
        rec.aborted = true;
        rec.pending_inline_func = nullptr;
    };

    switch (op) {
        // loads
        case OpCode::OP_LOAD_VAR: {
            uint16_t slot = op16();
            TraceType t = vtype(peek());
            if (t == TraceType::Int) {
                jit::TraceStep s = { Kind::LoadIntVar };
                s.slot = slot;
                rec.steps.push_back(s);
                rec.type_vstack.push_back(TraceType::Int);
            } else if (t == TraceType::Float) {
                jit::TraceStep s = { Kind::LoadFloatVar };
                s.slot = slot;
                rec.steps.push_back(s);
                rec.type_vstack.push_back(TraceType::Float);
            } else if (peek().is_object()) {
                jit::TraceStep s = { Kind::LoadObjVar };
                s.slot = slot;
                ObjectObj *oobj = static_cast<ObjectObj *>(peek().heap_ptr());
                s.obj_ptr = oobj;
                s.shape_ptr = oobj->shape;
                s.shape_ver = oobj->shape_version;
                rec.steps.push_back(s);
                rec.type_vstack.push_back(TraceType::Obj);
            } else if (peek().is_array()) {
                // Array trace: identity guard on ArrayObj* + size guard on (finish - start) at entry.
                // Trace body can access elements via ArrayGetIdx / ArraySetIdx.
                // Any op that could grow the array (push, spread, OOB store) aborts recording
                ArrayObj *aobj = static_cast<ArrayObj *>(peek().heap_ptr());
                jit::TraceStep s = { Kind::LoadArrayVar };
                s.slot = slot;
                s.obj_ptr = aobj;
                s.int_val = static_cast<int64_t>(aobj->v.size() * sizeof(Value));
                rec.steps.push_back(s);
                rec.type_vstack.push_back(TraceType::Array);
            } else if (peek().is_function()) {
                // deferred closure call, record the local slot, not the current capture cell
                auto &fd = peek().get_function();
                if ((fd.jit_inline_kind == JitInlineKind::ClosureInc || fd.jit_inline_kind == JitInlineKind::ClosureAddConst) &&
                    fd.jit_capture0_raw) {
                    rec.pending_closure_capture0 = fd.jit_capture0_raw;
                    rec.pending_closure_slot = slot;
                    rec.pending_closure_kind = fd.jit_inline_kind;
                    rec.pending_closure_imm = fd.jit_inline_imm;
                    rec.type_vstack.push_back(TraceType::Unknown);
                } else {
                    abort_recording();
                }
            } else {
                abort_recording();
            }
            break;
        }

        case OpCode::OP_STORE_VAR: {
            // STORE_VAR = peek, no pop: interpreter stack unchanged
            uint16_t slot = op16();
            if (rec.type_vstack.empty()) {
                abort_recording();
                break;
            }
            TraceType t = rec.type_vstack.back(); // type of stored value (remains on stack)
            if (t == TraceType::Int) {
                jit::TraceStep s = { Kind::StoreIntVar };
                s.slot = slot;
                rec.steps.push_back(s);
            } else if (t == TraceType::Float) {
                jit::TraceStep s = { Kind::StoreFloatVar };
                s.slot = slot;
                rec.steps.push_back(s);
            } else {
                abort_recording();
            }
            // type_vstack unchanged (value still on interpreter stack)
            break;
        }

        case OpCode::OP_POP:
            if (rec.type_vstack.empty()) {
                abort_recording();
                break;
            }
            rec.type_vstack.pop_back();
            rec.steps.push_back({ Kind::Pop });
            break;

        case OpCode::OP_DUP:
            if (rec.type_vstack.empty()) {
                abort_recording();
                break;
            }
            rec.type_vstack.push_back(rec.type_vstack.back());
            rec.steps.push_back({ Kind::Dup });
            break;

        case OpCode::OP_LOAD_CONST: {
            uint16_t idx = op16();
            const auto &c = current_function()->constants[idx];
            if (c.type == Constant::Type::INT) {
                jit::TraceStep s{ Kind::LoadIntConst };
                s.int_val = c.as_int;
                rec.steps.push_back(s);
                rec.type_vstack.push_back(TraceType::Int);
            } else if (c.type == Constant::Type::FLOAT) {
                jit::TraceStep s{ Kind::LoadFloatConst };
                s.dbl_val = c.as_float;
                rec.steps.push_back(s);
                rec.type_vstack.push_back(TraceType::Float);
            } else {
                abort_recording();
            }
            break;
        }

        case OpCode::OP_LOAD_ZERO:
            rec.steps.push_back({ Kind::LoadZeroConst });
            rec.type_vstack.push_back(TraceType::Int);
            break;

        case OpCode::OP_LOAD_ONE:
            rec.steps.push_back({ Kind::LoadOneConst });
            rec.type_vstack.push_back(TraceType::Int);
            break;

        case OpCode::OP_ADD:
        case OpCode::OP_SUB:
        case OpCode::OP_MUL:
        case OpCode::OP_MOD:
        case OpCode::OP_LT:
        case OpCode::OP_LE:
        case OpCode::OP_GT:
        case OpCode::OP_GE:
        case OpCode::OP_EQ:
        case OpCode::OP_NE: {
            if (rec.type_vstack.size() < 2) {
                abort_recording();
                break;
            }
            TraceType rhs = rec.type_vstack.back();
            rec.type_vstack.pop_back();
            TraceType lhs = rec.type_vstack.back();
            rec.type_vstack.pop_back();
            Kind kind;
            bool comparison = false;
            if (lhs == TraceType::Int && rhs == TraceType::Int) {
                switch (op) {
                    case OpCode::OP_ADD: kind = Kind::IntAdd; break;
                    case OpCode::OP_SUB: kind = Kind::IntSub; break;
                    case OpCode::OP_MUL: kind = Kind::IntMul; break;
                    case OpCode::OP_MOD: kind = Kind::IntMod; break;
                    case OpCode::OP_LT: kind = Kind::IntLt; comparison = true; break;
                    case OpCode::OP_LE: kind = Kind::IntLe; comparison = true; break;
                    case OpCode::OP_GT: kind = Kind::IntGt; comparison = true; break;
                    case OpCode::OP_GE: kind = Kind::IntGe; comparison = true; break;
                    case OpCode::OP_EQ: kind = Kind::IntEq; comparison = true; break;
                    default: kind = Kind::IntNe; comparison = true; break;
                }
            } else if (lhs == TraceType::Float && rhs == TraceType::Float) {
                if (op == OpCode::OP_MOD) {
                    abort_recording();
                    break;
                }
                switch (op) {
                    case OpCode::OP_ADD: kind = Kind::FloatAdd; break;
                    case OpCode::OP_SUB: kind = Kind::FloatSub; break;
                    case OpCode::OP_MUL: kind = Kind::FloatMul; break;
                    case OpCode::OP_LT: kind = Kind::FloatLt; comparison = true; break;
                    case OpCode::OP_LE: kind = Kind::FloatLe; comparison = true; break;
                    case OpCode::OP_GT: kind = Kind::FloatGt; comparison = true; break;
                    case OpCode::OP_GE: kind = Kind::FloatGe; comparison = true; break;
                    case OpCode::OP_EQ: kind = Kind::FloatEq; comparison = true; break;
                    default: kind = Kind::FloatNe; comparison = true; break;
                }
            } else {
                abort_recording();
                break;
            }
            if (!comparison && ((lhs == TraceType::Int && !peek().is_int()) || (lhs == TraceType::Float && !peek().is_float()))) {
                abort_recording();
                break;
            }
            jit::TraceStep step = jit::TraceStep::with_resume(kind, insn_pc);
            rec.steps.push_back(step);
            rec.type_vstack.push_back(comparison ? TraceType::Bool : lhs);
            break;
        }

        case OpCode::OP_JUMP_IF_FALSE: {
            if (rec.type_vstack.empty() || rec.type_vstack.back() != TraceType::Bool) {
                abort_recording();
                break;
            }
            rec.type_vstack.pop_back();
            if (!rec.type_vstack.empty()) {
                abort_recording();
                break;
            }
            int16_t offset = static_cast<int16_t>((uint16_t(insn_base[1]) << 8) | uint16_t(insn_base[2]));
            size_t pc = insn_pc;
            size_t target = static_cast<size_t>(static_cast<int64_t>(pc) + 3 + offset);
            bool taken = ip() == chunk->functions[func_idx].code.data() + target;
            jit::TraceStep step{ Kind::CondExitIfFalse };
            if (!taken && target > rec.anchor_pc && rec.exit_pc == 0) {
                rec.exit_pc = target;
            } else if (taken) {
                step = jit::TraceStep::with_resume(Kind::SideExitIfTrue, pc + 3);
            } else {
                step = jit::TraceStep::with_resume(Kind::SideExitIfFalse, target);
            }
            rec.steps.push_back(step);
            break;
        }

        case OpCode::OP_LOAD_GLOBAL: {
            uint16_t name_idx = op16();
            const std::string &name = chunk->strings[name_idx];
            if (name == "math") {
                rec.steps.push_back({ Kind::LoadMathGlobal });
                rec.type_vstack.push_back(TraceType::MathObject);
            } else if (rec.pending_inline_func == nullptr) {
                // try to defer as an inline call target for a subsequent OP_CALL,
                // only save if the global is a JIT-compiled function with metadata
                auto git = globals.find(name);
                if (git != globals.end() && git->second.is_function()) {
                    FunctionData &fd = git->second.get_function();
                    if (fd.jit_meta != nullptr) {
                        rec.pending_inline_func = fd.jit_meta;
                        // Don't push to type_vstack; func value is deferred.
                        break;
                    }
                }
                abort_recording();
            } else {
                // second LOAD_GLOBAL while one is pending; too complex to inline.
                abort_recording();
            }
            break;
        }

        // LOAD_GLOBAL + CALL peephole
        // when a prior LOAD_GLOBAL saved a pending_inline_func, analyse the callee's
        // bytecode for a simple 2-arg math pattern and emit the net op
        case OpCode::OP_CALL: {
            // deferred local closure call
            if (rec.pending_closure_capture0 != nullptr) {
                uint8_t argc = insn_base[1];
                Value *capture = static_cast<Value *>(rec.pending_closure_capture0);
                int64_t next = capture->is_int() ? capture->get_int() + rec.pending_closure_imm : 0;
                if (argc == 0 && !rec.type_vstack.empty() && capture->is_int() && next >= Value::INT48_MIN &&
                    next <= Value::INT48_MAX) {
                    rec.type_vstack.pop_back(); // pop the function placeholder
                    Kind kind = (rec.pending_closure_kind == JitInlineKind::ClosureAddConst) ? Kind::ClosureAddConst : Kind::ClosureInc;
                    jit::TraceStep s = jit::TraceStep::with_resume(kind, insn_pc);
                    s.capture_ptr = rec.pending_closure_capture0;
                    s.closure_slot = rec.pending_closure_slot;
                    s.int_val = rec.pending_closure_imm;
                    rec.steps.push_back(s);
                    rec.type_vstack.push_back(TraceType::Int);
                    rec.pending_closure_capture0 = nullptr;
                    rec.pending_closure_slot = 0;
                    rec.pending_closure_kind = JitInlineKind::None;
                    rec.pending_closure_imm = 0;
                    rec.inline_depth = 1; // skip the inlined closure body
                    break;
                }
                rec.pending_closure_capture0 = nullptr;
                rec.pending_closure_slot = 0;
                rec.pending_closure_kind = JitInlineKind::None;
                abort_recording();
                break;
            }
            // --- global function inline (LOAD_GLOBAL + CALL peephole) ---
            if (rec.pending_inline_func != nullptr) {
                FunctionMeta *f = rec.pending_inline_func;
                rec.pending_inline_func = nullptr;
                uint8_t argc = insn_base[1];
                if (argc == 2 && rec.type_vstack.size() >= 2) {
                    TraceType ta = rec.type_vstack[rec.type_vstack.size() - 2]; // first arg
                    TraceType tb = rec.type_vstack[rec.type_vstack.size() - 1]; // second arg
                    const auto &fc = f->code;
                    // Compute the starting offset of the function body, skipping any
                    // strict-mode CHECK_TYPE parameter preamble.
                    // Each preamble entry: LOAD_VAR(3) + CHECK_TYPE(4, ctx=0) +
                    // STORE_VAR(3) + POP(1) = 11 bytes.
                    size_t bo = 0;
                    if (f->strict_mode) {
                        while (bo + 11 <= fc.size()) {
                            if (static_cast<OpCode>(fc[bo]) != OpCode::OP_LOAD_VAR) {
                                break;
                            }
                            if (static_cast<OpCode>(fc[bo + 3]) != OpCode::OP_CHECK_TYPE) {
                                break;
                            }
                            if (fc[bo + 6] != 0) {
                                break; // ctx=0
                            }
                            if (static_cast<OpCode>(fc[bo + 7]) != OpCode::OP_STORE_VAR) {
                                break;
                            }
                            if (static_cast<OpCode>(fc[bo + 10]) != OpCode::OP_POP) {
                                break;
                            }
                            bo += 11;
                        }
                    }
                    // Helper: check if the position ends with [CHECK_TYPE(ctx=1)] RETURN
                    auto is_rt_end = [&](size_t pos) -> bool {
                        if (pos >= fc.size()) {
                            return false;
                        }
                        if (static_cast<OpCode>(fc[pos]) == OpCode::OP_RETURN) {
                            return true;
                        }
                        if (pos + 4 < fc.size() && static_cast<OpCode>(fc[pos]) == OpCode::OP_CHECK_TYPE && fc[pos + 3] == 1 &&
                            static_cast<OpCode>(fc[pos + 4]) == OpCode::OP_RETURN) {
                            return true;
                        }
                        return false;
                    };
                    // Match: LOAD_VAR(0), LOAD_VAR(1), <op>, [CHECK_TYPE] RETURN  (7+ bytes
                    // from body offset)
                    if (bo + 7 <= fc.size() && static_cast<OpCode>(fc[bo + 0]) == OpCode::OP_LOAD_VAR && fc[bo + 1] == 0 &&
                        fc[bo + 2] == 0 && static_cast<OpCode>(fc[bo + 3]) == OpCode::OP_LOAD_VAR && fc[bo + 4] == 0 && fc[bo + 5] == 1 &&
                        is_rt_end(bo + 7)) {
                        auto op2 = static_cast<OpCode>(fc[bo + 6]);
                        Kind kind;
                        TraceType result_type = TraceType::Int;
                        bool ok = false;
                        // Integer calls need their callee restored on overflow;
                        // ordinary integer opcodes below carry a resumable PC.
                        if (ta == TraceType::Float && tb == TraceType::Float) {
                            result_type = TraceType::Float;
                            switch (op2) {
                                case OpCode::OP_ADD:
                                    kind = Kind::FloatAdd;
                                    ok = true;
                                    break;
                                case OpCode::OP_SUB:
                                    kind = Kind::FloatSub;
                                    ok = true;
                                    break;
                                case OpCode::OP_MUL:
                                    kind = Kind::FloatMul;
                                    ok = true;
                                    break;
                                case OpCode::OP_DIV:
                                    kind = Kind::FloatDiv;
                                    ok = true;
                                    break;
                                default:
                                    break;
                            }
                        }
                        if (ok) {
                            rec.type_vstack.pop_back();
                            rec.type_vstack.pop_back();
                            rec.steps.push_back({ kind });
                            rec.type_vstack.push_back(result_type);
                            rec.inline_depth = 1; // skip the inlined function's body
                            break;                // success
                        }
                    }
                }
                abort_recording();
            } else {
                abort_recording(); // CALL without a deferred inline target
            }
            break;
        }

        // method call on an object (e.g. math.sqrt(x)): inline known Math unary methods.
        // Stack: [..., obj, arg0, ..., argN-1], operand: u16 method-name index + u8 argc.
        case OpCode::OP_CALL_METHOD: {
            uint16_t name_idx = op16();
            const std::string &mname = chunk->strings[name_idx];
            uint8_t argc = insn_base[3];
            if (rec.type_vstack.size() < 2) {
                abort_recording();
                break;
            }
            TraceType t_obj = rec.type_vstack[rec.type_vstack.size() - 2];
            rec.type_vstack.pop_back(); // arg
            rec.type_vstack.pop_back(); // obj
            if (t_obj != TraceType::MathObject || argc != 1) {
                abort_recording();
                break;
            }
            Kind kind;
            if (mname == "sqrt") {
                kind = Kind::MathSqrt;
            } else if (mname == "sin") {
                kind = Kind::MathSin;
            } else if (mname == "cos") {
                kind = Kind::MathCos;
            } else if (mname == "tan") {
                kind = Kind::MathTan;
            } else if (mname == "floor") {
                kind = Kind::MathFloor;
            } else if (mname == "ceil") {
                kind = Kind::MathCeil;
            } else {
                abort_recording();
                break;
            }
            rec.steps.push_back({ kind });
            rec.type_vstack.push_back(TraceType::Float);
            break;
        }

        // array element access, same idea as object property access below
        //
        // the ArrayObj was popped before the recorder runs
        // so trace_arr_recordable / trace_arr_ptr / trace_arr_size_bytes are resolved
        // while the array is still live
        case OpCode::OP_GET_INDEX: {
            // stack: [..., Array, Int] -> [..., Int]
            if (rec.type_vstack.size() < 2 || !trace_arr_recordable) {
                abort_recording();
                break;
            }
            TraceType t_idx = rec.type_vstack.back();
            TraceType t_arr = rec.type_vstack[rec.type_vstack.size() - 2];
            if (t_arr != TraceType::Array || t_idx != TraceType::Int) {
                abort_recording();
                break;
            }
            TraceType vt = vtype(peek()); // result on top of stack post-execute
            if (vt != TraceType::Int) {
                abort_recording();
                break;
            }
            rec.type_vstack.pop_back(); // index
            rec.type_vstack.pop_back(); // array
            jit::TraceStep s = jit::TraceStep::with_resume(Kind::ArrayGetIdx, insn_pc);
            s.prop_val_type = TraceType::Int;
            // Resume at this instruction on OOB; the exit re-pushes
            // [array, index] and let the interpreter re-execute OP_GET_INDEX
            rec.steps.push_back(s);
            rec.type_vstack.push_back(TraceType::Int);
            break;
        }

        case OpCode::OP_SET_INDEX: {
            // stack: [..., Array, Int, val] -> [..., val]
            if (rec.type_vstack.size() < 3 || !trace_arr_recordable) {
                abort_recording();
                break;
            }
            TraceType t_val = rec.type_vstack.back();
            TraceType t_idx = rec.type_vstack[rec.type_vstack.size() - 2];
            TraceType t_arr = rec.type_vstack[rec.type_vstack.size() - 3];
            if (t_arr != TraceType::Array || t_idx != TraceType::Int || t_val != TraceType::Int) {
                abort_recording();
                break;
            }
            rec.type_vstack.pop_back(); // val
            rec.type_vstack.pop_back(); // index
            rec.type_vstack.pop_back(); // array
            jit::TraceStep s = jit::TraceStep::with_resume(Kind::ArraySetIdx, insn_pc);
            s.prop_val_type = TraceType::Int;
            // Resume at this instruction on OOB; the exit re-pushes
            // [array, index, val] and let the interpreter re-execute OP_SET_INDEX
            rec.steps.push_back(s);
            rec.type_vstack.push_back(TraceType::Int); // assignment result
            break;
        }

            // object property access (shape-pointer + slot-index discipline)
            //
            // we record the field's slot index within the shape,
            // the trace lowering recomputes &fields[slot] from the live object on every access.
            // A recorded slot can only ever be used against an object with the exact shape it was recorded for.

        case OpCode::OP_GET_PROPERTY: {
            // stack: [..., Obj] -> [..., value]
            if (rec.type_vstack.empty() || rec.type_vstack.back() != TraceType::Obj || !trace_prop_recordable) {
                abort_recording();
                break;
            }
            TraceType vt = vtype(peek()); // result value now on top of stack
            if (vt != TraceType::Int && vt != TraceType::Float) {
                abort_recording();
                break;
            }
            rec.type_vstack.pop_back(); // pop Obj
            jit::TraceStep s = { Kind::ObjGetProp };
            s.prop_slot_index = trace_prop_slot;
            s.prop_val_type = vt;
            rec.steps.push_back(s);
            rec.type_vstack.push_back(vt);
            break;
        }

        case OpCode::OP_SET_PROPERTY: {
            // stack: [..., Obj, value] -> [..., value]
            if (rec.type_vstack.size() < 2 || !trace_prop_recordable) {
                abort_recording();
                break;
            }
            TraceType vt = rec.type_vstack.back();                      // value being stored
            TraceType ot = rec.type_vstack[rec.type_vstack.size() - 2]; // object
            if (ot != TraceType::Obj || (vt != TraceType::Int && vt != TraceType::Float)) {
                abort_recording();
                break;
            }
            rec.type_vstack.pop_back(); // value
            rec.type_vstack.pop_back(); // object

            jit::TraceStep s = { Kind::ObjSetProp };
            s.prop_slot_index = trace_prop_slot;
            s.prop_val_type = vt;
            rec.steps.push_back(s);
            rec.type_vstack.push_back(vt); // value pushed back as result
            break;
        }

        // abort trace if there's an unrecognized opcode
        default:
            abort_recording();
            break;
    }
}
#endif // !DISABLE_JIT

} // namespace bytecode
} // namespace nari
