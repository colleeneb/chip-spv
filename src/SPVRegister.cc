/*
Copyright (c) 2023 CHIP-SPV developers.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

// Hosts a global register for managing device SPIR-V binary source modules.
//
// Register provides methods for registering the SPIR-V modules and
// associating host-pointers to their functions and variables (and the
// modules themselves).
//
// The registered sources are post-processed (IOW, finalized) lazily
// by the getSource() functions. The registerFunction/Variable()
// function may not be called on Handles assosiated with modules
// provided by the getSource() calls.

#include "SPVRegister.hh"

#include "common.hh"
#include "logging.hh"
#include "macros.hh"

#include <mutex>
#include <utility>
#include <cassert>

/// Register a source module. The 'Source' must be non-empty and its
/// lifetime must last until the unregistration.
SPVRegister::Handle SPVRegister::registerSource(std::string_view SourceModule) {
  assert(SourceModule.size() && "Source module must be non-empty.");
  LOCK(Mtx_); // SPVRegister::Sources_
  auto Ins = Sources_.emplace(std::make_unique<SPVModule>());
  SPVModule *SrcMod = Ins.first->get();
  SrcMod->OriginalBinary_ = SourceModule;
  return Handle{reinterpret_cast<void *>(SrcMod)};
}

/// Associates the given host-pointer with a function by name in the
/// source module (Handle).
void SPVRegister::bindFunction(SPVRegister::Handle Handle, HostPtr Ptr,
                               std::string_view Name) {
  LOCK(Mtx_); // SPVRegister::Sources_
  auto *SrcMod = reinterpret_cast<SPVModule *>(Handle.Module);
  assert(Sources_.count(SrcMod) && "Not a member of the register.");

  // Host pointer should be associated with one source module and function.
  // assert(!HostPtrLookup_.count(Ptr) && "Host-pointer is already mapped.");
  if (HostPtrLookup_.count(Ptr) && HostPtrLookup_.at(Ptr)->Parent != SrcMod) {
    // Apparently templated kernels, by the same signature defined in
    // different TUs, causes these cases - which probably should not
    // happen in the whole [device] program compilation mode because
    // it means the some kernel invocations will launch kernels from
    // wrong modules! A bug in Clang?
    logWarn("A device function is already registered and mapped to a different "
            "module.");
    return;
  }

  SrcMod->Kernels.emplace_back(SPVFunction{SrcMod, Ptr, Name});
  HostPtrLookup_.emplace(std::make_pair(Ptr, &SrcMod->Kernels.back()));
}

/// Associates the given host-pointer with a variable by name in the
/// source module (Handle).
void SPVRegister::bindVariable(SPVRegister::Handle Handle, HostPtr Ptr,
                               std::string_view Name, size_t Size) {
  LOCK(Mtx_); // SPVRegister::Sources_
  auto *SrcMod = reinterpret_cast<SPVModule *>(Handle.Module);
  assert(Sources_.count(SrcMod) && "Not a member of the register.");
  assert(
      // Host pointer should be associated with one source module and variable
      // at most.
      (!HostPtrLookup_.count(Ptr)) ||
      // A variable made for abort() implementation is an exception to this due
      // to the way it's modeled.
      (Name == ChipDeviceAbortFlagName && HostPtrLookup_[Ptr]->Name == Name) &&
          "Host-pointer is already mapped.");

  SrcMod->Variables.emplace_back(SPVVariable{{SrcMod, Ptr, Name}, Size});
  HostPtrLookup_.emplace(std::make_pair(Ptr, &SrcMod->Variables.back()));
}

/// Unregisters the given source module. References to it and the
/// associated SPVModule and its SPV* objects are invalid after the
/// call.
void SPVRegister::unregisterSource(SPVRegister::Handle Handle) {
  unregisterSource(reinterpret_cast<SPVModule *>(Handle.Module));
}

/// Same as unregisterSource(SPVRegister::Handle)
void SPVRegister::unregisterSource(const SPVModule *ConstSrcMod) {
  LOCK(Mtx_); // SPVRegister::Sources_
  auto *SrcMod = const_cast<SPVModule *>(ConstSrcMod);

  auto SrcIt = Sources_.find(SrcMod);
  assert(SrcIt != Sources_.end() &&
         "Source module is not a member of the source register!");

  for (auto &k : SrcMod->Kernels)
    HostPtrLookup_.erase(k.Ptr);
  for (auto &V : SrcMod->Variables)
    HostPtrLookup_.erase(V.Ptr);

  Sources_.erase(SrcIt);
}

/// Get finalized source module associated with the given host pointer.
const SPVModule *SPVRegister::getSource(HostPtr Ptr) {
  LOCK(Mtx_); // SPVRegister::HostPtrLookup_
  auto IT = HostPtrLookup_.find(Ptr);
  if (IT == HostPtrLookup_.end())
    return nullptr;
  return getFinalizedSource(IT->second->Parent);
}

/// Get finalized source module associated with the given Handle.
const SPVModule *SPVRegister::getSource(SPVRegister::Handle Handle) {
  return getFinalizedSource(reinterpret_cast<SPVModule *>(Handle.Module));
}

/// Get Finalized source for 'SrcMod'.
SPVModule *SPVRegister::getFinalizedSource(SPVModule *SrcMod) {
  logDebug("Finalize module {}", static_cast<void *>(SrcMod));

  if (SrcMod->FinalizedBinary_.size()) // Already processed source. Return it.
    return SrcMod;

  // TODO: Optimization: Try to split the original large source module
  //       into smaller independent ones (is possible) for reducing
  //       compilation time in the backend.

  bool Success =
      filterSPIRV(SrcMod->OriginalBinary_.data(),
                  SrcMod->OriginalBinary_.size(), SrcMod->FinalizedBinary_);
  assert(Success && "SPIRV post processing failed!");
  // Can't be empty. There should be at least a SPIR-V header.
  assert(SrcMod->FinalizedBinary_.size() && "Empty finalized source");
  return SrcMod;
}

static std::once_flag Constructed;
static SPVRegister *GlobalSPVRegister = nullptr;

/// Give the global source module register.
SPVRegister &getSPVRegister() {
  std::call_once(Constructed, []() { GlobalSPVRegister = new SPVRegister(); });
  return *GlobalSPVRegister;
}
