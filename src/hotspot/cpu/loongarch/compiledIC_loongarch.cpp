/*
 * Copyright (c) 1997, 2014, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2015, 2022, Loongson Technology. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "code/compiledIC.hpp"
#include "code/icBuffer.hpp"
#include "code/nmethod.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/safepoint.hpp"

// ----------------------------------------------------------------------------

#define __ _masm.
address CompiledStaticCall::emit_to_interp_stub(CodeBuffer &cbuf, address mark) {
  precond(cbuf.stubs()->start() != badAddress);
  precond(cbuf.stubs()->end() != badAddress);

  if (mark == NULL) {
    mark = cbuf.insts_mark();  // get mark within main instrs section
  }

  // Note that the code buffer's insts_mark is always relative to insts.
  // That's why we must use the macroassembler to generate a stub.
  MacroAssembler _masm(&cbuf);

  address base = __ start_a_stub(CompiledStaticCall::to_interp_stub_size());
  if (base == NULL)  return NULL;  // CodeBuffer::expand failed
  // static stub relocation stores the instruction address of the call

  __ relocate(static_stub_Relocation::spec(mark), 0);

  // Code stream for loading method may be changed.
  __ ibar(0);

  // Rmethod contains methodOop, it should be relocated for GC
  // static stub relocation also tags the methodOop in the code-stream.
  __ mov_metadata(Rmethod, NULL);
  // This is recognized as unresolved by relocs/nativeInst/ic code

  cbuf.set_insts_mark();
  __ patchable_jump(__ pc());
  // Update current stubs pointer and restore code_end.
  __ end_a_stub();
  return base;
}
#undef __

int CompiledStaticCall::to_interp_stub_size() {
  return NativeInstruction::nop_instruction_size + NativeMovConstReg::instruction_size + NativeGeneralJump::instruction_size;
}

int CompiledStaticCall::to_trampoline_stub_size() {
  return  NativeInstruction::nop_instruction_size + NativeCallTrampolineStub::instruction_size;
}

// Relocation entries for call stub, compiled java to interpreter.
int CompiledStaticCall::reloc_to_interp_stub() {
  return 16;
}

void CompiledDirectStaticCall::set_to_interpreted(const methodHandle& callee, address entry) {
  address stub = find_stub(false /* is_aot */);
  guarantee(stub != NULL, "stub not found");

  if (TraceICs) {
    ResourceMark rm;
    tty->print_cr("CompiledDirectStaticCall@" INTPTR_FORMAT ": set_to_interpreted %s",
                  p2i(instruction_address()),
                  callee->name_and_sig_as_C_string());
  }

  // Creation also verifies the object.
  NativeMovConstReg* method_holder = nativeMovConstReg_at(stub + NativeInstruction::nop_instruction_size);
  NativeGeneralJump*        jump          = nativeGeneralJump_at(method_holder->next_instruction_address());

  assert(method_holder->data() == 0 || method_holder->data() == (intptr_t)callee(),
         "a) MT-unsafe modification of inline cache");
  assert(jump->jump_destination() == (address)-1 || jump->jump_destination() == entry,
         "b) MT-unsafe modification of inline cache");

  // Update stub.
  method_holder->set_data((intptr_t)callee());
  jump->set_jump_destination(entry);

  // Update jump to call.
  set_destination_mt_safe(stub);
}

void CompiledDirectStaticCall::set_stub_to_clean(static_stub_Relocation* static_stub) {
  assert (CompiledIC_lock->is_locked() || SafepointSynchronize::is_at_safepoint(), "mt unsafe call");
  // Reset stub.
  address stub = static_stub->addr();
  assert(stub != NULL, "stub not found");
  // Creation also verifies the object.
  NativeMovConstReg* method_holder = nativeMovConstReg_at(stub + NativeInstruction::nop_instruction_size);
  NativeGeneralJump* jump          = nativeGeneralJump_at(method_holder->next_instruction_address());
  method_holder->set_data(0);
  jump->set_jump_destination(jump->instruction_address());
}

//-----------------------------------------------------------------------------
// Non-product mode code
#ifndef PRODUCT

void CompiledDirectStaticCall::verify() {
  // Verify call.
  _call->verify();
  if (os::is_MP()) {
    _call->verify_alignment();
  }

  // Verify stub.
  address stub = find_stub(false /* is_aot */);
  assert(stub != NULL, "no stub found for static call");
  // Creation also verifies the object.
  NativeMovConstReg* method_holder = nativeMovConstReg_at(stub + NativeInstruction::nop_instruction_size);
  NativeGeneralJump* jump          = nativeGeneralJump_at(method_holder->next_instruction_address());


  // Verify state.
  assert(is_clean() || is_call_to_compiled() || is_call_to_interpreted(), "sanity check");
}

#endif // !PRODUCT
