/*
 * Copyright (c) 2021-23 CHIP-SPV developers
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
/**
 * @file CHIPBackend.hh
 * @author Paulius Velesko (pvelesko@pglc.io)
 * @brief CHIPBackend class definition. CHIP backends are to inherit from this
 * base class and override desired virtual functions. Overrides for this class
 * are expected to be minimal with primary overrides being done on lower-level
 * classes such as CHIPContext consturctors, etc.
 * @version 0.1
 * @date 2021-08-19
 *
 * @copyright Copyright (c) 2021
 *
 */
#ifndef CHIP_BACKEND_H
#define CHIP_BACKEND_H

#include "spirv.hh"
#include "common.hh"
#include "hip/hip_runtime_api.h"
#include "hip/spirv_hip.hh"

#include "CHIPDriver.hh"
#include "logging.hh"
#include "macros.hh"
#include "CHIPException.hh"
#include "CHIPGraph.hh"
#include "SPVRegister.hh"

#define DEFAULT_QUEUE_PRIORITY 1

inline CHIPContext *PrimaryContext = nullptr;
inline thread_local std::stack<CHIPExecItem *> ChipExecStack;
inline thread_local std::stack<CHIPContext *> ChipCtxStack;

inline std::string hipMemcpyKindToString(hipMemcpyKind Kind) {
  switch (Kind) {
  case hipMemcpyHostToHost:
    return "hipMemcpyHostToHost";
  case hipMemcpyHostToDevice:
    return "hipMemcpyHostToDevice";
  case hipMemcpyDeviceToHost:
    return "hipMemcpyDeviceToHost";
  case hipMemcpyDeviceToDevice:
    return "hipMemcpyDeviceToDevice";
  case hipMemcpyDefault:
    return "hipMemcpyDefault";
  default:
    return "hipMemcpyUnknown";
  }
}

static inline size_t getChannelByteSize(hipChannelFormatDesc Desc) {
  unsigned TotalNumBits = Desc.x + Desc.y + Desc.z + Desc.w;
  return ((TotalNumBits + 7u) / 8u); // Round upwards.
}

/// Describes a memory region to copy from/to.
class CHIPRegionDesc {
public:
  static constexpr unsigned MaxNumDims = 3;
  // The number of dimensions.
  unsigned char NumDims = 1;
  // The size of element measured in bytes.
  size_t ElementSize = 0;
  // Measured in elements. The minimum size allowed is one for the first
  // NumDim elements.
  size_t Size[MaxNumDims] = {1, 1, 1};
  // Measured in elements.
  size_t Offset[MaxNumDims] = {0, 0, 0};
  // Row and slice pitch. Measured in bytes.
  size_t Pitch[MaxNumDims - 1] = {1, 1};

  std::string dumpAsString() const {
    std::string Result =
        "Size=(" + std::to_string(Size[0]) + ", " + std::to_string(Size[1]) +
        ", " + std::to_string(Size[2]) + "), Offset=(" +
        std::to_string(Offset[0]) + ", " + std::to_string(Offset[1]) + ", " +
        std::to_string(Offset[2]) + "), Pitch=(" + std::to_string(Pitch[0]) +
        "," + std::to_string(Pitch[1]) + ")";
    return Result;
  }

  unsigned getNumDims() const {
    CHIPASSERT(NumDims > 0 && NumDims <= MaxNumDims);
    return NumDims;
  }

  bool isPitched() const {
    switch (getNumDims()) {
    default:
      CHIPASSERT(false && "Unexpected dimension count.");
      return false;
    case 1:
      return false;
    case 2:
      return Pitch[0] > Size[0] * ElementSize;
    case 3:
      return Pitch[0] > Size[0] * ElementSize &&
             Pitch[1] > Size[1] * Size[0] * ElementSize;
    }
  }

  static CHIPRegionDesc get3DRegion(size_t TheWidth, size_t TheHeight,
                                    size_t TheDepth,
                                    size_t ElementByteSize = 1) {
    CHIPRegionDesc Result;
    Result.NumDims = 3;
    Result.ElementSize = ElementByteSize;
    Result.Size[0] = TheWidth;
    Result.Size[1] = TheHeight;
    Result.Size[2] = TheDepth;
    Result.Pitch[0] = TheWidth * ElementByteSize;
    Result.Pitch[1] = TheWidth * TheHeight * ElementByteSize;
    return Result;
  }

  static CHIPRegionDesc get2DRegion(size_t TheWidth, size_t TheHeight,
                                    size_t ElementByteSize = 1) {
    auto R = get3DRegion(TheWidth, TheHeight, 1, ElementByteSize);
    R.NumDims = 2;
    return R;
  }

  static CHIPRegionDesc get1DRegion(size_t TheWidth, size_t TheHeight,
                                    size_t ElementByteSize = 1) {
    auto R = get2DRegion(TheWidth, 1, ElementByteSize);
    R.NumDims = 1;
    return R;
  }

  static CHIPRegionDesc from(const hipArray &Array) {
    auto TexelByteSize = getChannelByteSize(Array.desc);
    switch (Array.textureType) {
    default:
      assert(false && "Unkown texture type.");
      return CHIPRegionDesc();
    case hipTextureType1D:
      return CHIPRegionDesc::get1DRegion(Array.width, TexelByteSize);
    case hipTextureType2D:
      return CHIPRegionDesc::get2DRegion(Array.width, Array.height,
                                         TexelByteSize);
    case hipTextureType3D:
      return CHIPRegionDesc::get3DRegion(Array.width, Array.height, Array.depth,
                                         TexelByteSize);
    }
  }

  static CHIPRegionDesc from(const hipResourceDesc &ResDesc) {
    switch (ResDesc.resType) {
    default:
      CHIPASSERT(false && "Unknown resource type");
      return CHIPRegionDesc();
    case hipResourceTypePitch2D: {
      auto &Res = ResDesc.res.pitch2D;
      auto R = get2DRegion(Res.width, Res.height, getChannelByteSize(Res.desc));
      R.Pitch[0] = Res.pitchInBytes;
      CHIPASSERT(Res.pitchInBytes >= Res.width * getChannelByteSize(Res.desc) &&
                 "Invalid pitch.");
      return R;
    }
    case hipResourceTypeArray: {
      const auto *Array = ResDesc.res.array.array;
      assert(Array);
      return from(*Array);
    }
    } // switch
  }
};

class CHIPEventMonitor;

class CHIPQueueFlags {
  unsigned int FlagsRaw_;
  bool Default_ = true;
  bool NonBlocking_ = false;

public:
  CHIPQueueFlags() : CHIPQueueFlags(hipStreamDefault) {}
  CHIPQueueFlags(unsigned int FlagsRaw) : FlagsRaw_(FlagsRaw) {

    if (FlagsRaw & hipStreamDefault) {
      Default_ = true;
      FlagsRaw = FlagsRaw & (~hipStreamDefault);
    }

    if (FlagsRaw & hipStreamNonBlocking) {
      NonBlocking_ = true;
      FlagsRaw = FlagsRaw & (~hipStreamNonBlocking);
    }

    if (FlagsRaw > 0)
      CHIPERR_LOG_AND_THROW("Invalid CHIPQueueFlags", hipErrorInvalidValue);
  }

  bool isDefault() { return Default_; }
  bool isNonBlocking() { return NonBlocking_; }
  bool isBlocking() { return !NonBlocking_; }
  unsigned int getRaw() { return FlagsRaw_; }
};

enum class CHIPManagedMemFlags : unsigned int {
  AttachHost = hipMemAttachHost,
  AttachGlobal = hipMemAttachGlobal
};

class CHIPHostAllocFlags {
  bool Default_ = true;
  bool Portable_ = false;
  bool Mapped_ = false;
  bool WriteCombined_ = false;
  bool NumaUser_ = false;
  bool Coherent_ = false;
  bool NonCoherent_ = false;
  unsigned int FlagsRaw_;

public:
  CHIPHostAllocFlags() : FlagsRaw_(hipHostMallocDefault){};
  CHIPHostAllocFlags(unsigned int FlagsRaw) : FlagsRaw_(FlagsRaw) {
    if (FlagsRaw & hipHostMallocDefault) {
      Default_ = true;
      FlagsRaw = FlagsRaw & (~hipHostMallocDefault);
    }

    if (FlagsRaw & hipHostMallocPortable) {
      Portable_ = true;
      Default_ = false;
      FlagsRaw = FlagsRaw & (~hipHostMallocPortable);
    }

    if (FlagsRaw & hipHostMallocMapped) {
      Mapped_ = true;
      Default_ = false;
      FlagsRaw = FlagsRaw & (~hipHostMallocMapped);
    }

    if (FlagsRaw & hipHostMallocWriteCombined) {
      WriteCombined_ = true;
      Default_ = false;
      FlagsRaw = FlagsRaw & (~hipHostMallocWriteCombined);
    }

    if (FlagsRaw & hipHostMallocNumaUser) {
      NumaUser_ = true;
      Default_ = false;
      FlagsRaw = FlagsRaw & (~hipHostMallocNumaUser);
    }

    if (FlagsRaw & hipHostMallocCoherent) {
      Coherent_ = true;
      Default_ = false;
      FlagsRaw = FlagsRaw & (~hipHostMallocCoherent);
    }

    if (FlagsRaw & hipHostMallocNonCoherent) {
      NonCoherent_ = true;
      Default_ = false;
      FlagsRaw = FlagsRaw & (~hipHostMallocNonCoherent);
    }

    if (FlagsRaw > 0)
      CHIPERR_LOG_AND_THROW("Invalid CHIPHostAllocFlag", hipErrorInvalidValue);

    if (Coherent_ && NonCoherent_)
      CHIPERR_LOG_AND_THROW("Invalid CHIPHostAllocFlag", hipErrorInvalidValue);
  }
  unsigned int getRaw() { return FlagsRaw_; }
  bool isDefault() { return Default_; }
  bool isPortable() { return Portable_; }
  bool isMapped() { return Mapped_; }
  bool isWriteCombined() { return WriteCombined_; }
  bool isNumaUser() { return NumaUser_; }
  bool isCoherent() { return Coherent_; }
  bool isNonCoherent() { return NonCoherent_; }
};

/**
 * @brief This object gets created when a callback is requested. Once created,
 * it gets placed on the CHIPBackend callback queue. A Callback monitor thread
 * gets created and executes these callback objects. This object stores all the
 * necessary data to execute a callback function:
 * - Events for synching
 * - Callback function
 * - Arguments for the callback function
 */
class CHIPCallbackData {
protected:
  virtual ~CHIPCallbackData() = default;

public:
  CHIPQueue *ChipQueue;
  CHIPEvent *GpuReady;
  CHIPEvent *CpuCallbackComplete;
  CHIPEvent *GpuAck;

  hipError_t Status;
  void *CallbackArgs;
  hipStreamCallback_t CallbackF;

  CHIPCallbackData(hipStreamCallback_t CallbackF, void *CallbackArgs,
                   CHIPQueue *ChipQueue);

  void execute(hipError_t ResultFromDependency);
};

class CHIPEventMonitor {
  typedef void *(*THREADFUNCPTR)(void *);

protected:
  CHIPEventMonitor() = default;
  virtual ~CHIPEventMonitor() = default;
  pthread_t Thread_;

public:
  std::mutex EventMonitorMtx;
  volatile bool Stop = false;

  void join() { pthread_join(Thread_, nullptr); }
  static void *monitorWrapper(void *Arg) {
    auto Monitor = (CHIPEventMonitor *)Arg;
    Monitor->monitor();
    return 0;
  }
  virtual void monitor(){};

  void start() {
    logDebug("Starting Event Monitor Thread");
    auto Res = pthread_create(&Thread_, 0, monitorWrapper, (void *)this);
    if (Res)
      CHIPERR_LOG_AND_THROW("Failed to create thread", hipErrorTbd);
    logDebug("Thread Created with ID : {}", Thread_);
  }

  void stop() {
    LOCK(EventMonitorMtx) // Lock the mutex to ensure that the thread is not
                          // executing the monitor function
    logDebug("Stopping Event Monitor Thread");
    Stop = true;
    join();
  }
};

class CHIPTexture {
  /// Resource description used to create this texture.
  hipResourceDesc ResourceDesc;

public:
  CHIPTexture() = delete;
  CHIPTexture(const hipResourceDesc &ResDesc) : ResourceDesc(ResDesc) {}
  virtual ~CHIPTexture() {}

  const hipResourceDesc &getResourceDesc() const { return ResourceDesc; }
};

template <class T> std::string resultToString(T Err);

class CHIPEventFlags {
  bool BlockingSync_ = false;
  bool DisableTiming_ = false;
  bool Interprocess_ = false;
  bool ReleaseToDevice_ = false;
  bool ReleaseToSystem_ = false;

public:
  CHIPEventFlags() = default;
  CHIPEventFlags(unsigned Flags) {

    if (Flags & hipEventBlockingSync) {
      Flags = Flags & (~hipEventBlockingSync);
      BlockingSync_ = true;
    }
    if (Flags & hipEventDisableTiming) {
      Flags = Flags & (~hipEventDisableTiming);
      DisableTiming_ = true;
    }
    if (Flags & hipEventInterprocess) {
      Flags = Flags & (~hipEventInterprocess);
      logWarn("hipEventInterprocess is not supported on CHIP-SPV");
      Interprocess_ = true;
    }
    if (Flags & hipEventReleaseToDevice) {
      Flags = Flags & (~hipEventReleaseToDevice);
      logWarn("hipEventReleaseToDevice is not supported on CHIP-SPV");
      ReleaseToDevice_ = true;
    }
    if (Flags & hipEventReleaseToSystem) {
      Flags = Flags & (~hipEventReleaseToSystem);
      logWarn("hipEventReleaseToSystem is not supported on CHIP-SPV");
      ReleaseToSystem_ = true;
    }

    if (Interprocess_ && !DisableTiming_) {
      CHIPERR_LOG_AND_THROW(
          "hipEventInterprocess requires hipEventDisableTiming",
          hipErrorInvalidValue);
    }
    if (Flags > 0)
      CHIPERR_LOG_AND_THROW("Invalid hipEvent flag combination",
                            hipErrorInvalidValue);
  }

  bool isDefault() {
    return !BlockingSync_ && !DisableTiming_ && !Interprocess_;
  };
  bool isBlockingSync() { return BlockingSync_; };
  bool isDisableTiming() { return DisableTiming_; };
  bool isInterprocess() { return Interprocess_; };
};

/**
 * @brief  Structure describing an allocation
 *
 */
struct AllocationInfo {
  // TODO make this into a class
  void *DevPtr;
  void *HostPtr;
  size_t Size;
  CHIPHostAllocFlags Flags;
  hipDevice_t Device;
  bool Managed = false;
  enum hipMemoryType MemoryType;
  bool RequiresMapUnmap = false;
};

/**
 * @brief Class for keeping track of device allocations.
 *
 */
class CHIPAllocationTracker {
private:
  std::string Name_;

  std::unordered_set<AllocationInfo *> AllocInfos_;
  std::unordered_map<void *, AllocationInfo *> PtrToAllocInfo_;

public:
  mutable std::mutex AllocationTrackerMtx;

  /**
   * @brief Associate a host pointer with a device pointer. @see hipHostRegister
   *
   * @param HostPtr
   */
  void registerHostPointer(void *HostPtr, void *DevPtr) {
    CHIPASSERT(HostPtr && "HostPtr is null");
    CHIPASSERT(DevPtr && "DevPtr is null");
    auto AllocInfo = this->getAllocInfo(DevPtr);
    AllocInfo->HostPtr = HostPtr;
    this->PtrToAllocInfo_[HostPtr] = AllocInfo;
    AllocInfo->MemoryType = hipMemoryTypeManaged;
  }

  size_t GlobalMemSize, TotalMemSize, MaxMemUsed;
  /**
   * @brief Construct a new CHIPAllocationTracker object
   *
   * @param GlobalMemSize Total available global memory on the device
   * @param Name name for this allocation tracker for logging. Normally device
   * name
   */
  CHIPAllocationTracker(size_t GlobalMemSize, std::string Name);

  /**
   * @brief Destroy the CHIPAllocationTracker object
   *
   */
  ~CHIPAllocationTracker();

  /**
   * @brief Get the Name object
   *
   * @return std::string
   */
  std::string getName();

  /**
   * @brief Get Allocation Info associated with this pointer
   *
   * @return AllocationInfo contains the base pointer and allocation size;
   */
  AllocationInfo *getAllocInfo(const void *);

  /**
   * @brief Reserve memory for an allocation.
   * This method is run prior to allocations to keep track of how much memory is
   * available on the device
   *
   * @param bytes
   * @return true Reservation successful
   * @return false Not enough available memory for reservation of this size.
   */
  bool reserveMem(size_t Bytes);

  /**
   * @brief Release some of the reserved memory. Called by free()
   *
   * @param bytes
   * @return true
   * @return false
   */
  bool releaseMemReservation(size_t Bytes);

  /**
   * @brief Record the pointer received from CHIPContext::allocate_()
   *
   * @param dev_ptr
   */
  void recordAllocation(void *DevPtr, void *HostPtr, hipDevice_t Device,
                        size_t Size, CHIPHostAllocFlags Flags,
                        hipMemoryType MemoryType);

  /**
   * @brief Check if a given pointer belongs to any of the existing allocations
   *
   * @param DevPtr device side pointer
   * @return AllocationInfo* pointer to allocation info. Nullptr if this pointer
   * does not belong to any existing allocations
   */
  AllocationInfo *getAllocInfoCheckPtrRanges(void *DevPtr);

  /**
   * @brief Delete an AllocationInfo item
   *
   * @param AllocInfo
   */
  void eraseRecord(AllocationInfo *AllocInfo) {
    assert(AllocInfos_.count(AllocInfo) &&
           "Not a member of the allocation tracker!");
    LOCK(AllocationTrackerMtx); // CHIPAllocationTracker::PtrToAllocInfo_
                                // CHIPAllocationTracker::AllocInfos_
    PtrToAllocInfo_.erase(AllocInfo->DevPtr);
    if (AllocInfo->HostPtr)
      PtrToAllocInfo_.erase(AllocInfo->HostPtr);
    AllocInfos_.erase(AllocInfo);
    delete AllocInfo;
  }

  /**
   * @brief Visit tracked allocations.
   *
   * The visitor is called with 'const AllocationInfo&' argument.
   */
  template <typename VisitorT> void visitAllocations(VisitorT Visitor) const {
    LOCK(AllocationTrackerMtx); // CHIPAllocationTracker::AllocInfos_
    for (const auto *Info : AllocInfos_)
      Visitor(*Info);
  }

  size_t getNumAllocations() const { return AllocInfos_.size(); }
};

class CHIPDeviceVar {
private:
  const SPVVariable *SrcVar_ = nullptr;
  void *DevAddr_ = nullptr;
  /// The alignment requirement of the variable.
  // NOTE: The alignment infromation is not carried in __hipRegisterVar() calls
  // It have to be queried via shadow kernels.
  size_t Alignment_ = 0;
  /// Tells if the variable has an initializer. NOTE: Variables are
  /// initialized via a shadow kernel.
  bool HasInitializer_ = false;

public:
  CHIPDeviceVar(const SPVVariable *SrcVar) : SrcVar_(SrcVar) {}
  ~CHIPDeviceVar();

  void *getDevAddr() const { return DevAddr_; }
  void setDevAddr(void *Addr) { DevAddr_ = Addr; }
  std::string_view getName() const { return SrcVar_->Name; }
  size_t getSize() const { return SrcVar_->Size; }
  size_t getAlignment() const { return Alignment_; }
  void setAlignment(size_t TheAlignment) {
    assert(Alignment_ && "Invalid alignment");
    Alignment_ = TheAlignment;
  }
  bool hasInitializer() const { return HasInitializer_; }
  void markHasInitializer(bool State = true) { HasInitializer_ = State; }
};

class CHIPEvent : public ihipEvent_t {
protected:
  bool UserEvent_ = false;
  bool TrackCalled_ = false;
  event_status_e EventStatus_;
  CHIPEventFlags Flags_;
  std::vector<CHIPEvent *> DependsOnList;

#ifndef NDEBUG
  // A debug flag for cathing use-after-delete.
  bool Deleted_ = false;
#endif

  // reference count
  size_t *Refc_;

  /**
   * @brief Events are always created with a context
   *
   */
  CHIPContext *ChipContext_;

  /**
   * @brief hidden default constructor for CHIPEvent. Only derived class
   * constructor should be called.
   *
   */
  CHIPEvent() = default;

public:
  bool isUserEvent() { return UserEvent_; }
  void addDependency(CHIPEvent *Event) {
    assert(!Deleted_ && "Event use after delete!");
    DependsOnList.push_back(Event);
  }
  void releaseDependencies();
  void track();
  CHIPEventFlags getFlags() { return Flags_; }
  std::mutex EventMtx;
  std::string Msg;
  size_t getCHIPRefc();
  virtual void decreaseRefCount(std::string Reason);
  virtual void increaseRefCount(std::string Reason);
  virtual ~CHIPEvent() = default;
  // Optionally provide a field for origin of this event
  /**
   * @brief CHIPEvent constructor. Must always be created with some context.
   *
   */
  CHIPEvent(CHIPContext *Ctx, CHIPEventFlags Flags = CHIPEventFlags());
  /**
   * @brief Get the Context object
   *
   * @return CHIPContext* pointer to context on which this event was created
   */
  CHIPContext *getContext() {
    assert(!Deleted_ && "Event use after delete!");
    return ChipContext_;
  }

  /**
   * @brief Query the state of this event and update it's status
   * Each backend must override this method with implementation specific calls
   * e.x. clGetEventInfo()
   *
   * @return true event was in recording state, state might have changed
   * @return false event was not in recording state
   */
  virtual bool updateFinishStatus(bool ThrowErrorIfNotReady = true) = 0;

  /**
   * @brief Check if this event is recording or already recorded
   *
   * @return true event is recording/recorded
   * @return false event is in init or invalid state
   */
  bool isRecordingOrRecorded() {
    assert(!Deleted_ && "Event use after delete!");
    return EventStatus_ >= EVENT_STATUS_RECORDING;
  }

  /**
   * @brief check if this event is done recording
   *
   * @return true recoded
   * @return false not recorded
   */
  bool isFinished() {
    assert(!Deleted_ && "Event use after delete!");
    return (EventStatus_ == EVENT_STATUS_RECORDED);
  }

  /**
   * @brief Get the Event Status object
   *
   * @return event_status_e current event status
   */
  event_status_e getEventStatus() { return EventStatus_; }

  std::string getEventStatusStr() {
    switch (EventStatus_) {
    case EVENT_STATUS_INIT:
      return "EVENT_STATUS_INIT";
    case EVENT_STATUS_RECORDING:
      return "EVENT_STATUS_RECORDING";
    case EVENT_STATUS_RECORDED:
      return "EVENT_STATUS_RECORDED";
    default:
      return "INVALID_EVENT_STATUS";
    };
  }

  /**
   * @brief Enqueue this event in a given CHIPQueue
   *
   * @param chip_queue_ CHIPQueue in which to enque this event
   * @return true
   * @return false
   */
  virtual void recordStream(CHIPQueue *ChipQueue) = 0;
  /**
   * @brief Wait for this event to complete
   *
   * @return true
   * @return false
   */
  virtual bool wait() = 0;

  /**
   * @brief Calculate absolute difference between completion timestamps of this
   * event and other
   *
   * @param other
   * @return float
   */
  virtual float getElapsedTime(CHIPEvent *Other) = 0;

  /**
   * @brief Toggle this event from the host.
   *
   */
  virtual void hostSignal() = 0;

#ifndef NDEBUG
  void markDeleted(bool State = true) { Deleted_ = State; }
#endif
};

class CHIPProgram {
  std::string ProgramName_;   ///< Program name.
  std::string ProgramSource_; ///< Program source code.

  /// Include headers.
  std::map<std::string, std::string> Headers_;

  /// Name expressions added before compilation as key to the
  /// map. After compilation they point to their lowered/mangled
  /// names. The map value may also be empty meaning the lowered name
  /// is unknown.
  std::map<std::string, std::string> NameExpressions_;

  std::string ProgramLog_; ///< Captured compilation log.
  std::string Code_;       ///< Compiled program.

public:
  CHIPProgram() = delete;
  CHIPProgram(const std::string &ProgramName) : ProgramName_(ProgramName) {}

  void setSource(const char *Source) {
    if (Source)
      ProgramSource_.assign(Source);
  }

  void addHeader(std::string_view IncludeName, std::string_view Contents) {
    assert(!IncludeName.empty() && "Nameless include header!");
    Headers_[std::string(IncludeName)].assign(Contents);
  }

  void appendToLog(const std::string &Log) { ProgramLog_.append(Log); }

  void addCode(std::string_view Code) { Code_.append(Code); }

  void addNameExpression(std::string_view NameExpr) {
    assert(!isAfterCompilation() &&
           "Must not add name expressions after compilation!");
    NameExpressions_.emplace(std::make_pair(NameExpr, ""));
  }

  const std::map<std::string, std::string> &getHeaders() const {
    return Headers_;
  }
  const std::string &getSource() const { return ProgramSource_; }
  const std::string &getProgramLog() const { return ProgramLog_; }
  const std::string &getCode() const { return Code_; }

  const std::map<std::string, std::string> &getNameExpressionMap() const {
    return NameExpressions_;
  }
  std::map<std::string, std::string> &getNameExpressionMap() {
    return NameExpressions_;
  }

  /// Return true if the program has been compiled.
  bool isAfterCompilation() const { return !Code_.empty(); }
};

/**
 * @brief Module abstraction. Contains global variables and kernels. Can be
 * extracted from FatBinary or loaded at runtime.
 * OpenCL - ClProgram
 * Level Zero - zeModule
 * ROCclr - amd::Program
 * CUDA - CUmodule
 */
class CHIPModule : public ihipModule_t {
  /// Flag for the allocation state of the device variables. True if
  /// all variables have space allocated for this module for the
  /// device this module is attached to. False implies that
  /// DeviceVariablesAllocated false.
  bool DeviceVariablesAllocated_ = false;
  /// Flag for the initialization state of the device variables. True
  /// if all variables are initialized for this module for the device
  /// this module is attached to.
  bool DeviceVariablesInitialized_ = false;

  OpenCLFunctionInfoMap FuncInfos_;

protected:
  uint8_t *FuncIL_;
  size_t IlSize_;
  std::mutex Mtx_;
  // Global variables
  std::vector<CHIPDeviceVar *> ChipVars_;
  // Kernels
  std::vector<CHIPKernel *> ChipKernels_;
  /// Binary representation extracted from FatBinary.
  const SPVModule *Src_;
  // Kernel JIT compilation can be lazy
  std::once_flag Compiled_;

  uint32_t *BinaryData_;

  /**
   * @brief hidden default constuctor. Only derived type constructor should be
   * called.
   *
   */
  CHIPModule() = default;

public:
  /**
   * @brief Destroy the CHIPModule object
   *
   */
  virtual ~CHIPModule();
  /**
   * @brief Construct a new CHIPModule object.
   * This constructor should be implemented by the derived class (specific
   * backend implementation). Call to this constructor should result in a
   * populated chip_kernels vector.
   *
   * @param module_str string prepresenting the binary extracted from FatBinary
   */
  CHIPModule(const SPVModule &Src) : Src_(&Src) {}

  /**
   * @brief Add a CHIPKernel to this module.
   * During initialization when the FatBinary is consumed, a CHIPModule is
   * constructed for every device. SPIR-V kernels reside in this module. This
   * method is called called via the constructor during this initialization
   * phase. Modules can also be loaded from a file during runtime, however.
   *
   * @param kernel CHIPKernel to be added to this module.
   */
  void addKernel(CHIPKernel *Kernel);

  /**
   * @brief Wrapper around compile() called via std::call_once
   *
   * @param chip_dev device for which to compile the kernels
   */
  void compileOnce(CHIPDevice *ChipDev);
  /**
   * @brief Kernel JIT compilation can be lazy. This is configured via Cmake
   * LAZY_JIT option. If LAZY_JIT is set to true then this module won't be
   * compiled until the first call to one of its kernels. If LAZY_JIT is set to
   * false(default) then this method should be called in the constructor;
   *
   * This method should populate this modules chip_kernels vector. These
   * kernels would have a name extracted from the kernel but no associated host
   * function pointers.
   *
   */
  virtual void compile(CHIPDevice *ChipDev) = 0;
  /**
   * @brief Get the Global Var object
   * A module, along with device kernels, can also contain global variables.
   *
   * @param name global variable name
   * @return CHIPDeviceVar*
   */
  virtual CHIPDeviceVar *getGlobalVar(const char *VarName);

  /**
   * @brief Get the Kernel object
   *
   * @param name name of the corresponding host function
   * @return CHIPKernel* if found and nullptr otherwise.
   */
  CHIPKernel *findKernel(const std::string &Name);

  /**
   * @brief Get the Kernel object
   *
   * @param name name of the corresponding host function
   * @return CHIPKernel*
   */
  CHIPKernel *getKernelByName(const std::string &Name);

  /**
   * @brief Checks if the module has a kernel with the given name.
   *
   * @param name the name of the kernel
   * @return true in case the kernels is found
   */
  bool hasKernel(std::string Name);

  /**
   * @brief Get the Kernels object
   *
   * @return std::vector<CHIPKernel*>&
   */
  std::vector<CHIPKernel *> &getKernels();

  /**
   * @brief Get the Kernel object
   *
   * @param host_f_ptr host-side function pointer
   * @return CHIPKernel*
   */
  CHIPKernel *getKernel(const void *HostFPtr);

  /**
   * @brief consume SPIRV and fill in SPVFuncINFO
   *
   */
  void consumeSPIRV();

  /**
   * @brief Record a device variable
   *
   * Takes ownership of the variable.
   */
  void addDeviceVariable(CHIPDeviceVar *DevVar) { ChipVars_.push_back(DevVar); }

  std::vector<CHIPDeviceVar *> &getDeviceVariables() { return ChipVars_; }

  hipError_t allocateDeviceVariablesNoLock(CHIPDevice *Device,
                                           CHIPQueue *Queue);
  void prepareDeviceVariablesNoLock(CHIPDevice *Device, CHIPQueue *Queue);
  void invalidateDeviceVariablesNoLock();
  void deallocateDeviceVariablesNoLock(CHIPDevice *Device);

  SPVFuncInfo *findFunctionInfo(const std::string &FName);

  const SPVModule &getSourceModule() const { return *Src_; }
};

/**
 * @brief Contains information about the function on the host and device
 */
class CHIPKernel : public ihipModuleSymbol_t {
protected:
  /**
   * @brief hidden default constructor. Only derived type constructor should be
   * called.
   *
   */
  CHIPKernel(std::string HostFName, SPVFuncInfo *FuncInfo);
  /// Name of the function
  std::string HostFName_;
  /// Pointer to the host function
  const void *HostFPtr_ = nullptr;
  /// Pointer to the device function
  const void *DevFPtr_;

  SPVFuncInfo *FuncInfo_;

public:
  virtual ~CHIPKernel();

  /**
   * @brief Get the Name object
   *
   * @return std::string
   */
  std::string getName();

  /**
   * @brief Get the Func Info object
   *
   * @return SPVFuncInfo&
   */
  SPVFuncInfo *getFuncInfo();
  /**
   * @brief Get the associated host pointer to a host function
   *
   * @return const void*
   */
  const void *getHostPtr();
  /**
   * @brief Get the associated funciton pointer on the device
   *
   * @return const void*
   */
  const void *getDevPtr();

  /**
   * @brief Get the Name object
   *
   * @return std::string
   */
  void setName(std::string HostFName);
  /**
   * @brief Get the associated host pointer to a host function
   *
   * @return const void*
   */
  void setHostPtr(const void *HostFPtr);
  /**
   * @brief Get the associated funciton pointer on the device
   *
   * @return const void*
   */
  void setDevPtr(const void *DevFPtr);

  /**
   * @brief Get the associated kernel information (max threads and so on)
   *
   * @return hipError_t
   * @return hipFuncAttributes
   */
  virtual hipError_t getAttributes(hipFuncAttributes *Attr) = 0;

  /**
   * @brief Return the parent module of the kernel.
   */
  virtual CHIPModule *getModule() = 0;
  virtual const CHIPModule *getModule() const = 0;
};

class CHIPArgSpillBuffer {
  CHIPContext *Ctx_; ///< A context to allocate device space from.
  std::unique_ptr<char[]> HostBuffer_;
  char *DeviceBuffer_ = nullptr;
  std::map<size_t, size_t> ArgIndexToOffset_;
  size_t Size_ = 0;

public:
  CHIPArgSpillBuffer() = delete;
  CHIPArgSpillBuffer(CHIPContext *Ctx) : Ctx_(Ctx) {}
  ~CHIPArgSpillBuffer();
  void computeAndReserveSpace(const SPVFuncInfo &KernelInfo);
  void *allocate(const SPVFuncInfo::Arg &Arg);
  size_t getSize() const { return Size_; }
  const void *getHostBuffer() const {
    assert(HostBuffer_.get());
    return HostBuffer_.get();
  }
  void *getDeviceBuffer() {
    assert(DeviceBuffer_);
    return DeviceBuffer_;
  }
};

/**
 * @brief Contains kernel arguments and a queue on which to execute.
 * Prior to kernel launch, the arguments are setup via
 * CHIPBackend::configureCall(). Because of this, we get the kernel last so the
 * kernel so the launch() takes a kernel argument as opposed to queue receiving
 * a CHIPExecItem containing the kernel and arguments
 *
 */
class CHIPExecItem {
protected:
  bool ArgsSetup = false;
  size_t SharedMem_;

  dim3 GridDim_;
  dim3 BlockDim_;

  CHIPQueue *ChipQueue_;

  std::vector<void *> Args_;

  std::shared_ptr<CHIPArgSpillBuffer> ArgSpillBuffer_;

public:
  void copyArgs(void **Args);
  void setQueue(CHIPQueue *Queue) { ChipQueue_ = Queue; }
  std::mutex ExecItemMtx;
  size_t getNumArgs() {
    assert(getKernel() && "Kernel was not set! (call setKernel() first)");
    return getKernel()->getFuncInfo()->getNumClientArgs();
  }

  /**
   * @brief Return argument list.
   */
  const std::vector<void *> &getArgs() const { return Args_; }

  /**
   * @brief Deleted default constructor
   * Doesn't make sense for CHIPExecItem to exist without arguments
   *
   */
  CHIPExecItem() = delete;

  /**
   * @brief Deleted copy constructor
   * Since this is an abstract class and derived classes might add their own
   * members and overrides, we must request an implementation for clone()
   *
   * @param Other
   */
  CHIPExecItem(const CHIPExecItem &Other) = delete;

  /**
   * @brief Destroy the CHIPExecItem object
   *
   */
  virtual ~CHIPExecItem() {}

  virtual CHIPExecItem *clone() const = 0;

  /**
   * @brief Construct a new CHIPExecItem object
   *
   * @param grid_dim_
   * @param block_dim_
   * @param shared_mem_
   * @param chip_queue_
   */
  CHIPExecItem(dim3 GirdDim, dim3 BlockDim, size_t SharedMem,
               hipStream_t ChipQueue);

  /**
   * @brief Set the Kernel object
   *
   * @return CHIPKernel* Kernel to be executed
   */
  virtual void setKernel(CHIPKernel *Kernel) = 0;

  /**
   * @brief Get the Kernel object
   *
   * @return CHIPKernel* Kernel to be executed
   */
  virtual CHIPKernel *getKernel() = 0;

  /**
   * @brief Get the Queue object
   *
   * @return CHIPQueue*
   */

  CHIPQueue *getQueue();

  /**
   * @brief Get the Grid object
   *
   * @return dim3
   */
  dim3 getGrid();

  /**
   * @brief Get the Block object
   *
   * @return dim3
   */
  dim3 getBlock();

  /**
   * @brief Get the SharedMem
   *
   * @return size_t
   */
  size_t getSharedMem();

  /**
   * @brief Sets up the kernel arguments via backend API calls.
   * Called after all the arugments are setup either via hipSetupArg() (old HIP
   * kernel launch API)
   * Or after hipLaunchKernel (new HIP kernel launch API)
   *
   */
  virtual void setupAllArgs() = 0;

  std::shared_ptr<CHIPArgSpillBuffer> getArgSpillBuffer() const {
    return ArgSpillBuffer_;
  };
};

/**
 * @brief Compute device class
 */
class CHIPDevice {

  // A bundle for CHIPReinitialize.
  class ModuleState {
    friend class CHIPDevice;
    std::unordered_map<const SPVModule *, CHIPModule *> SrcModToCompiledMod_;
    std::unordered_map<const void *, CHIPModule *> HostPtrToCompiledMod_;
  };

  /// Modules compiled so far.
  std::unordered_map<const SPVModule *, CHIPModule *> SrcModToCompiledMod_;
  /// Host pointer mapping to modules.
  std::unordered_map<const void *, CHIPModule *> HostPtrToCompiledMod_;

protected:
  std::string DeviceName_;
  CHIPContext *Ctx_;
  std::vector<CHIPQueue *> ChipQueues_;
  std::once_flag PropsPopulated_;

  hipDeviceAttribute_t Attrs_;
  hipDeviceProp_t HipDeviceProps_;

  size_t TotalUsedMem_;
  size_t MaxUsedMem_;
  size_t MaxMallocSize_ = 0;

  /// Maps host-side shadow variables to the corresponding device variables.
  std::unordered_map<const void *, CHIPDeviceVar *> DeviceVarLookup_;

  int Idx_ = -1; // Initialized with a value indicating unset ID.

  // only callable from derived classes, because we need to call also init()
  CHIPDevice(CHIPContext *Ctx, int DeviceIdx);
  // initializer. may call virtual methods
  void init();
  bool PerThreadStreamUsed_ = false;

public:
  hipDeviceProp_t getDeviceProps() { return HipDeviceProps_; }
  std::mutex DeviceVarMtx;
  std::mutex DeviceMtx;

  std::vector<CHIPQueue *> getQueuesNoLock() { return ChipQueues_; }

  CHIPQueue *LegacyDefaultQueue;
  inline static thread_local std::unique_ptr<CHIPQueue> PerThreadDefaultQueue;

  /**
   * @brief Get the Legacy Default Queue object.
   *
   * @return CHIPQueue* default legacy queue
   */
  CHIPQueue *getLegacyDefaultQueue();
  /**
   * @brief Get the Per Thread Default Queue object. If it was not initialized,
   * initialize it and set PerThreadStreamUsed to true
   * @see CHIPDevice::PerThreadStreamUsed
   *
   * @return CHIPQueue*
   */
  CHIPQueue *getPerThreadDefaultQueue();
  CHIPQueue *getPerThreadDefaultQueueNoLock();

  bool isPerThreadStreamUsed();
  bool isPerThreadStreamUsedNoLock();
  void setPerThreadStreamUsed(bool Status);

  /**
   * @brief Get the Default Queue object. If HIP_API_PER_THREAD_DEFAULT_STREAM
   * was set during compilation, return PerThreadStream, otherwise return legacy
   * stream
   *
   * @return CHIPQueue*
   */
  CHIPQueue *getDefaultQueue();

  /**
   * @brief Create a Queue object
   *
   * @param Flags
   * @param Priority
   * @return CHIPQueue*
   */
  CHIPQueue *createQueueAndRegister(CHIPQueueFlags Flags = CHIPQueueFlags(),
                                    int Priority = DEFAULT_QUEUE_PRIORITY);

  CHIPQueue *createQueueAndRegister(const uintptr_t *NativeHandles,
                                    const size_t NumHandles);

  void removeContext(CHIPContext *Ctx);
  virtual CHIPContext *createContext() = 0;
  CHIPContext *createContextAndRegister() {
    Ctx_ = createContext();
    return Ctx_;
  }

  size_t getMaxMallocSize() {
    if (MaxMallocSize_ < 1)
      CHIPERR_LOG_AND_THROW("MaxMallocSize was not set", hipErrorTbd);
    return MaxMallocSize_;
  }

  CHIPAllocationTracker *AllocationTracker = nullptr;

  virtual ~CHIPDevice();

  /// Return kernel the host-pointer 'Ptr' is associated with, if
  /// found. Otherwise return nullptr.
  CHIPKernel *findKernel(HostPtr Ptr) {
    if (auto *Mod = getOrCreateModule(Ptr))
      return Mod->getKernel(Ptr);
    return nullptr;
  }

  CHIPModule *getOrCreateModule(HostPtr Ptr);
  CHIPModule *getOrCreateModule(const SPVModule &SrcMod);

  /// Return the number of currently compiled modules on this device.
  size_t getNumCompiledModules() const { return SrcModToCompiledMod_.size(); }

  /**
   * @brief Get the Kernels object
   *
   * @return std::vector<CHIPKernel*>&
   */
  std::vector<CHIPKernel *> getKernels();

  ModuleState getModuleState() const {
    ModuleState State;
    State.SrcModToCompiledMod_ = SrcModToCompiledMod_;
    State.HostPtrToCompiledMod_ = HostPtrToCompiledMod_;
    return State;
  }

  void addFromModuleState(ModuleState &State) {
    for (auto &kv : State.SrcModToCompiledMod_)
      SrcModToCompiledMod_.insert(kv);
    for (auto &kv : State.HostPtrToCompiledMod_)
      HostPtrToCompiledMod_.insert(kv);
  }

  /**
   * @brief Use a backend to populate device properties such as memory
   * available, frequencies, etc.
   */
  virtual void populateDevicePropertiesImpl() = 0;

  /**
   * @brief Query the device for properties
   *
   * @param prop
   */
  void copyDeviceProperties(hipDeviceProp_t *Prop);

  /**
   * @brief Get the context object
   *
   * @return CHIPContext* pointer to the CHIPContext object this CHIPDevice
   * was created with
   */
  CHIPContext *getContext();

  /**
   * @brief Construct an additional queue for this device
   *
   * @param flags
   * @param priority
   * @return CHIPQueue* pointer to the newly created queue (can also be found
   * in chip_queues vector)
   */
  virtual CHIPQueue *createQueue(CHIPQueueFlags Flags, int Priority) = 0;
  virtual CHIPQueue *createQueue(const uintptr_t *NativeHandles,
                                 int NumHandles) = 0;

  /**
   * @brief Add a queue to this device and the backend
   *
   * @param chip_queue_  CHIPQueue to be added
   */
  void addQueue(CHIPQueue *ChipQueue);
  /**
   * @brief Get the Queues object
   *
   * @return std::vector<CHIPQueue*>
   */
  std::vector<CHIPQueue *> &getQueues();

  /**
   * @brief Remove a queue from this device's queue vector
   *
   * @param q
   * @return true
   * @return false
   */
  bool removeQueue(CHIPQueue *ChipQueue);

  /**
   * @brief Get the integer ID of this device as it appears in the Backend's
   * chip_devices list
   *
   * @return int
   */
  int getDeviceId();
  /**
   * @brief Get the device name
   *
   * @return std::string
   */
  std::string getName();

  /**
   * @brief Destroy all allocations and reset all state on the current device in
   the current process.
   *
   */
  void reset() {
    invalidateDeviceVariables();
    // resetImpl();
  }

  /**
   * @brief Query for a specific device attribute. Implementation copied from
   * HIPAMD.
   *
   * @param attr attribute to query
   * @return int attribute value. In case invalid query returns -1;
   */
  int getAttr(hipDeviceAttribute_t Attr);

  /**
   * @brief Get the total global memory available for this device.
   *
   * @return size_t
   */
  size_t getGlobalMemSize();

  /**
   * @brief Set the Cache Config object
   *
   * @param cfg configuration
   */
  virtual void setCacheConfig(hipFuncCache_t Cfg);

  /**
   * @brief Get the cache configuration for this device
   *
   * @return hipFuncCache_t
   */
  virtual hipFuncCache_t getCacheConfig();

  /**
   * @brief Configure shared memory for this device
   *
   * @param config
   */
  virtual void setSharedMemConfig(hipSharedMemConfig Cfg);

  /**
   * @brief Get the shared memory configuration for this device
   *
   * @return hipSharedMemConfig
   */
  virtual hipSharedMemConfig getSharedMemConfig();

  /**
   * @brief Setup the cache configuration for the device to use when executing
   * this function
   *
   * @param func
   * @param config
   */
  virtual void setFuncCacheConfig(const void *Func, hipFuncCache_t Cfg);

  /**
   * @brief Check if the current device has same PCI bus ID as the one given by
   * input
   *
   * @param pciDomainID
   * @param pciBusID
   * @param pciDeviceID
   * @return true
   * @return false
   */
  bool hasPCIBusId(int PciDomainID, int PciBusID, int PciDeviceID);

  /**
   * @brief Get peer-accesability between this and another device
   *
   * @param peerDevice
   * @return int
   */
  int getPeerAccess(CHIPDevice *PeerDevice);

  /**
   * @brief Set access between this and another device
   *
   * @param peer
   * @param flags
   * @param canAccessPeer
   * @return hipError_t
   */
  hipError_t setPeerAccess(CHIPDevice *Peer, int Flags, bool CanAccessPeer);

  /**
   * @brief Get the total used global memory
   *
   * @return size_t
   */
  size_t getUsedGlobalMem();

  /**
   * @brief Get the global variable that came from a FatBinary module
   *
   * @param var host pointer to the variable
   * @return CHIPDeviceVar* if not found returns nullptr
   */
  CHIPDeviceVar *getDynGlobalVar(const void *Var) { return nullptr; }

  /**
   * @brief Get the global variable that came from a FatBinary module
   *
   * @param var Pointer to host side shadow variable.
   * @return CHIPDeviceVar* if not found returns nullptr
   */
  CHIPDeviceVar *getStatGlobalVar(const void *Var);

  /**
   * @brief Get the global variable
   *
   * @param var Pointer to host side shadow variable.
   * @return CHIPDeviceVar* if not found returns nullptr
   */
  CHIPDeviceVar *getGlobalVar(const void *Var);

  void eraseModule(CHIPModule *Module);

  virtual CHIPTexture *
  createTexture(const hipResourceDesc *ResDesc, const hipTextureDesc *TexDesc,
                const struct hipResourceViewDesc *ResViewDesc) = 0;

  virtual void destroyTexture(CHIPTexture *TextureObject) = 0;

  void prepareDeviceVariables(HostPtr Ptr);
  void invalidateDeviceVariables();
  void deallocateDeviceVariables();

protected:
  /**
   * @brief The backend hook for reset().
   */
  virtual void resetImpl() = 0;

  /// Compile the source (SPIR-V) to native/backend code.
  virtual CHIPModule *compile(const SPVModule &Src) = 0;
};

/**
 * @brief Context class
 * Contexts contain execution queues and are created on top of a single or
 * multiple devices. Provides for creation of additional queues, events, and
 * interaction with devices.
 */
class CHIPContext : public ihipCtx_t {
protected:
  int RefCount_;
  CHIPDevice *ChipDevice_;
  std::vector<void *> AllocatedPtrs_;

  unsigned int Flags_;

  /**
   * @brief Construct a new CHIPContext object
   *
   */
  CHIPContext();

public:
  std::vector<CHIPEvent *> Events;
  mutable std::mutex ContextMtx;

  /**
   * @brief Destroy the CHIPContext object
   *
   */
  virtual ~CHIPContext();

  virtual void syncQueues(CHIPQueue *TargetQueue);

  void setDevice(CHIPDevice *Device) { ChipDevice_ = Device; }

  /**
   * @brief Get this context's CHIPDevices
   *
   * @return CHIPDevice *
   */
  CHIPDevice *getDevice();

  /**
   * @brief Allocate data.
   * Calls reserveMem() to keep track memory used on the device.
   * Calls CHIPContext::allocate_(size_t size, size_t alignment,
   * hipMemoryType mem_type) with allignment = 0
   *
   * @param size size of the allocation
   * @param mem_type type of the allocation: Host, Device, Shared
   * @return void* pointer to allocated memory
   */
  void *allocate(size_t Size, hipMemoryType MemType);

  /**
   * @brief Allocate data.
   * Calls reserveMem() to keep track memory used on the device.
   * Calls CHIPContext::allocate_(size_t size, size_t alignment,
   * hipMemoryType mem_type)
   *
   * @param size size of the allocation
   * @param alignment allocation alignment in bytes
   * @param mem_type type of the allocation: Host, Device, Shared
   * @return void* pointer to allocated memory
   */
  void *allocate(size_t Size, size_t Alignment, hipMemoryType MemType);

  /**
   * @brief Allocate data.
   * Calls reserveMem() to keep track memory used on the device.
   * Calls CHIPContext::allocate_(size_t size, size_t alignment,
   * hipMemoryType mem_type)
   *
   * @param size size of the allocation
   * @param alignment allocation alignment in bytes
   * @param mem_type type of the allocation: Host, Device, Shared
   * @param Flags flags
   * @return void* pointer to allocated memory
   */
  void *allocate(size_t Size, size_t Alignment, hipMemoryType MemType,
                 CHIPHostAllocFlags Flags);

  /**
   * @brief Allocate data. Pure virtual function - to be overriden by each
   * backend. This member function is the one that's called by all the
   * publically visible CHIPContext::allocate() variants
   *
   * @param size size of the allocation.
   * @param alignment allocation alignment in bytes
   * @param mem_type type of the allocation: Host, Device, Shared
   * @return void*
   */
  virtual void *
  allocateImpl(size_t Size, size_t Alignment, hipMemoryType MemType,
               CHIPHostAllocFlags Flags = CHIPHostAllocFlags()) = 0;

  /**
   * @brief Returns true if the pointer is mapped to virtual memory with
   * updates synchronized to it automatically at synchronization points.
   *
   * @param Ptr pointer to memory allocated by allocate().
   * @return true/false
   */

  virtual bool isAllocatedPtrMappedToVM(void *Ptr) = 0;

  /**
   * @brief Free memory
   *
   * @param ptr pointer to the memory location to be deallocated. Internally
   * calls CHIPContext::free_()
   * @return true Success
   * @return false Failure
   */
  hipError_t free(void *Ptr);

  /**
   * @brief Free memory
   * To be overriden by the backend
   *
   * @param ptr
   * @return true
   * @return false
   */
  virtual void freeImpl(void *Ptr) = 0;

  /**
   * @brief Get the flags set on this context
   *
   * @return unsigned int context flags
   */
  unsigned int getFlags();

  /**
   * @brief Set the flags for this context
   *
   * @param flags flags to set on this context
   */
  void setFlags(unsigned int Flags);

  /**
   * @brief Reset this context.
   *
   */
  void reset();

  /**
   * @brief Retain this context.
   * TODO: What does it mean to retain a context?
   *
   * @return CHIPContext*
   */
  void retain() { ++RefCount_; }

  void release() {
    --RefCount_;
    if (RefCount_ == 0) {
      // TODO hipCtx - shuold call overlaoded destructor
    }
  }
};

/**
 * @brief Primary object to interact with the backend
 */
class CHIPBackend {
protected:
  CHIPEventMonitor *CallbackEventMonitor_ = nullptr;
  CHIPEventMonitor *StaleEventMonitor_ = nullptr;

  int MinQueuePriority_;
  int MaxQueuePriority_ = 0;

  CHIPContext *ActiveCtx_;
  CHIPDevice *ActiveDev_;

  // Keep hold on the default logger instance to make sure that it is
  // not destructed before the backend finishes uninitialization.
  std::shared_ptr<spdlog::logger> Logger;

public:
#ifdef DUBIOUS_LOCKS
  std::mutex DubiousLockOpenCL;
  std::mutex DubiousLockLevel0;
#endif

  virtual CHIPExecItem *createCHIPExecItem(dim3 GirdDim, dim3 BlockDim,
                                           size_t SharedMem,
                                           hipStream_t ChipQueue) = 0;

  int getPerThreadQueuesActive();
  std::mutex SetActiveMtx;
  std::mutex QueueCreateDestroyMtx;
  mutable std::mutex BackendMtx;
  std::mutex CallbackQueueMtx;
  std::vector<CHIPEvent *> Events;
  std::mutex EventsMtx;

  std::queue<CHIPCallbackData *> CallbackQueue;

  // Adds -std=c++17 requirement
  inline static thread_local hipError_t TlsLastError;

  std::vector<CHIPContext *> ChipContexts;

  /**
   * @brief User defined compiler options to pass to the JIT compiler
   *
   */
  std::string CustomJitFlags;

  int getQueuePriorityRange();

  /**
   * @brief Get the default compiler flags for the JIT compiler
   *
   * @return std::string flags to pass to JIT compiler
   */
  virtual std::string getDefaultJitFlags() = 0;

  virtual int ReqNumHandles() = 0;
  /**
   * @brief Get the jit options object
   * return CHIP_JIT_FLAGS if it is set, otherwise return default options as
   * defined by CHIPBackend<implementation>::getDefaultJitFlags()
   *
   * @return std::string flags to pass to JIT compiler
   */
  std::string getJitFlags();

  // TODO
  // key for caching compiled modules. To get a cached compiled module on a
  // particular device you must make sure that you have a module which matches
  // the host funciton pointer and also that this module was compiled for the
  // same device model.
  // typedef  std::pair<const void*, std::string> ptr_dev;
  // /**
  //  * @brief
  //  *
  //  */
  // std::unordered_map<ptr_dev, CHIPModule*> host_f_ptr_to_chipmodule_map;

  /**
   * @brief Construct a new CHIPBackend object
   *
   */
  CHIPBackend();
  /**
   * @brief Destroy the CHIPBackend objectk
   *
   */
  virtual ~CHIPBackend();

  /**
   * @brief Initialize this backend with given environment flags
   *
   * @param platform_str
   * @param device_type_str
   * @param device_ids_str
   */
  void initialize(std::string PlatformStr, std::string DeviceTypeStr,
                  std::string DeviceIdStr);

  /**
   * @brief Initialize this backend with given environment flags
   *
   * @param platform_str
   * @param device_type_str
   * @param device_ids_str
   */
  virtual void initializeImpl(std::string PlatformStr,
                              std::string DeviceTypeStr,
                              std::string DeviceIdStr) = 0;

  /**
   * @brief Initialize this backend with given Native handles
   *
   * @param platform_str
   * @param device_type_str
   * @param device_ids_str
   */
  virtual void initializeFromNative(const uintptr_t *NativeHandles,
                                    int NumHandles) = 0;

  /**
   * @brief
   *
   */
  virtual void uninitialize() = 0;

  /**
   * @brief Wait for all per-thread queues to finish
   *
   */
  void waitForThreadExit();
  /**
   * @brief Get the Active Context object. Returns the context of the active
   * queue.
   *
   * @return CHIPContext*
   */
  CHIPContext *getActiveContext();
  /**
   * @brief Get the Active Device object. Returns the device of the active
   * queue.
   *
   * @return CHIPDevice*
   */
  CHIPDevice *getActiveDevice();
  /**
   * @brief Set the active device. Sets the active queue to this device's
   * first/default/primary queue.
   *
   * @param chip_dev
   */
  void setActiveContext(CHIPContext *ChipContext);
  void setActiveDevice(CHIPDevice *ChipDevice);

  std::vector<CHIPDevice *> getDevices();
  /**
   * @brief Get the Num Devices object
   *
   * @return size_t
   */
  size_t getNumDevices();
  /**
   * @brief Add a context to this backend.
   *
   * @param ctx_in
   */
  void addContext(CHIPContext *ChipContext);
  void removeContext(CHIPContext *ChipContext);

  /**
   * @brief Configure an upcoming kernel call
   *
   * @param grid
   * @param block
   * @param shared
   * @param q
   * @return hipError_t
   */
  hipError_t configureCall(dim3 GridDim, dim3 BlockDim, size_t SharedMem,
                           hipStream_t ChipQueue);

  /**
   * @brief Return a device which meets or exceeds the requirements
   *
   * @param props
   * @return CHIPDevice*
   */
  CHIPDevice *findDeviceMatchingProps(const hipDeviceProp_t *Props);

  /**
   * @brief Find a given queue in this backend.
   *
   * @param q queue to find
   * @return CHIPQueue* return queue or nullptr if not found
   */
  CHIPQueue *findQueue(CHIPQueue *ChipQueue);

  /**
   * @brief Add a CHIPModule to every initialized device
   *
   * @param chip_module pointer to CHIPModule object
   * @return hipError_t
   */
  // CHIPModule* addModule(std::string* module_src);
  /**
   * @brief Remove this module from every device
   *
   * @param chip_module pointer to the module which is to be removed
   * @return hipError_t
   */
  // hipError_t removeModule(CHIPModule* chip_module);

  /************Factories***************/

  virtual CHIPQueue *createCHIPQueue(CHIPDevice *ChipDev) = 0;

  /**
   * @brief Create an Event, adding it to the Backend Event list.
   *
   * @param ChipCtx Context in which to create the event in
   * @param Flags Events falgs
   * @param UserEvent Is this a user event? If so, increase refcount to 2 to
   * prevent it from being garbage collected.
   * @return CHIPEvent* Event
   */
  virtual CHIPEvent *createCHIPEvent(CHIPContext *ChipCtx,
                                     CHIPEventFlags Flags = CHIPEventFlags(),
                                     bool UserEvent = false) = 0;

  // CHIPEvent *createCHIPEvent(CHIPContext *ChipCtx, std::string MsgIn,
  //                            CHIPEventFlags Flags = CHIPEventFlags(),
  //                            bool UserEvent = false) {
  //   auto NewEvent = createCHIPEvent(ChipCtx, MsgIn, Flags, UserEvent);
  //   NewEvent->Msg = MsgIn;
  //   return NewEvent;
  // }

  /**
   * @brief Create a Callback Obj object
   * Each backend must implement this function which calls a derived
   * CHIPCallbackData constructor.
   * @return CHIPCallbackData* pointer to newly allocated CHIPCallbackData
   * object.
   */
  virtual CHIPCallbackData *createCallbackData(hipStreamCallback_t Callback,
                                               void *UserData,
                                               CHIPQueue *ChipQ) = 0;

  virtual CHIPEventMonitor *createCallbackEventMonitor_() = 0;
  virtual CHIPEventMonitor *createStaleEventMonitor_() = 0;

  /* event interop */
  virtual hipEvent_t getHipEvent(void *NativeEvent) = 0;
  virtual void *getNativeEvent(hipEvent_t HipEvent) = 0;
};

/**
 * @brief Queue class for submitting kernels to for execution
 */
class CHIPQueue : public ihipStream_t {
protected:
  hipStreamCaptureStatus CaptureStatus_ = hipStreamCaptureStatusNone;
  hipStreamCaptureMode CaptureMode_ = hipStreamCaptureModeGlobal;
  hipGraph_t CaptureGraph_;
  std::mutex LastEventMtx;
  /// @brief  node for creating a dependency chain between subsequent record
  /// events when in graph capture mode
  CHIPGraphNode *LastNode_ = nullptr;
  int Priority_;
  /**
   * @brief Maximum priority that can be had by a queue is 0; Priority range is
   * defined to be [0, MinPriority]
   */
  const int MaxPriority = 0;
  int MinPriority;
  CHIPQueueFlags QueueFlags_;
  /// Device on which this queue will execute
  CHIPDevice *ChipDevice_;
  /// Context to which device belongs to
  CHIPContext *ChipContext_;

  /** Keep track of what was the last event submitted to this queue. Required
   * for enforcing proper queue syncronization as per HIP/CUDA API. */
  CHIPEvent *LastEvent_ = nullptr;

  enum class MANAGED_MEM_STATE { PRE_KERNEL, POST_KERNEL };

  CHIPEvent *RegisteredVarCopy(CHIPExecItem *ExecItem,
                               MANAGED_MEM_STATE ExecState);

public:
  enum MEM_MAP_TYPE { HOST_READ, HOST_WRITE, HOST_READ_WRITE };
  virtual void MemMap(const AllocationInfo *AllocInfo, MEM_MAP_TYPE MapType) {}
  virtual void MemUnmap(const AllocationInfo *AllocInfo) {}

  /**
   * @brief Check the stream to see if it's in capture mode and if so, capture.
   *
   * @tparam GraphNodeType the type of graph node to create
   * @tparam ArgTypes variadic template parameter
   * @param ArgsPack graph node type constructor arguments
   * @return true stream was in capture mode and a graph node was created -
   * caller should return from whatever HIP API function was invoking this
   * @return false stream was not in capture mode, proceed with executing the
   * HIP API call.
   */
  template <class GraphNodeType, class... ArgTypes>
  bool captureIntoGraph(ArgTypes... ArgsPack) {
    if (getCaptureStatus() == hipStreamCaptureStatusActive) {
      auto Graph = getCaptureGraph();
      auto Node = new GraphNodeType(ArgsPack...);
      updateLastNode(Node);
      Graph->addNode(Node);
      return true;
    }
    return false;
  }

  void updateLastNode(CHIPGraphNode *NewNode);
  void initCaptureGraph();

  hipStreamCaptureStatus getCaptureStatus() const { return CaptureStatus_; }
  void setCaptureStatus(hipStreamCaptureStatus CaptureMode) {
    CaptureStatus_ = CaptureMode;
  }
  hipStreamCaptureMode getCaptureMode() const { return CaptureMode_; }
  void setCaptureMode(hipStreamCaptureMode CaptureMode) {
    CaptureMode_ = CaptureMode;
  }
  CHIPGraph *getCaptureGraph() const {
    return static_cast<CHIPGraph *>(CaptureGraph_);
  }

  CHIPDevice *PerThreadQueueForDevice = nullptr;

  // I want others to be able to lock this queue?
  std::mutex QueueMtx;

  virtual CHIPEvent *getLastEvent() = 0;

  /**
   * @brief Construct a new CHIPQueue object
   *
   * @param chip_dev
   * @param flags
   */
  CHIPQueue(CHIPDevice *ChipDev, CHIPQueueFlags Flags);
  /**
   * @brief Construct a new CHIPQueue object
   *
   * @param chip_dev
   * @param flags
   * @param priority
   */
  CHIPQueue(CHIPDevice *ChipDev, CHIPQueueFlags Flags, int Priority);
  /**
   * @brief Destroy the CHIPQueue object
   *
   */
  virtual ~CHIPQueue();

  CHIPQueueFlags getQueueFlags() { return QueueFlags_; }
  virtual void updateLastEvent(CHIPEvent *NewEvent) {
    LOCK(LastEventMtx); // CHIPQueue::LastEvent_
    logDebug("Setting LastEvent for {} {} -> {}", (void *)this,
             (void *)LastEvent_, (void *)NewEvent);
    if (NewEvent == LastEvent_)
      return;

    if (LastEvent_ != nullptr) {
      LastEvent_->decreaseRefCount("updateLastEvent - old event");
    }

    if (NewEvent != nullptr) {
      NewEvent->increaseRefCount("updateLastEvent - new event");
    }
    LastEvent_ = NewEvent;
  }

  /**
   * @brief Blocking memory copy
   *
   * @param dst Destination
   * @param src Source
   * @param size Transfer size
   * @return hipError_t
   */
  hipError_t memCopy(void *Dst, const void *Src, size_t Size);

  /**
   * @brief Non-blocking memory copy
   *
   * @param dst Destination
   * @param src Source
   * @param size Transfer size
   * @return hipError_t
   */
  virtual CHIPEvent *memCopyAsyncImpl(void *Dst, const void *Src,
                                      size_t Size) = 0;
  void memCopyAsync(void *Dst, const void *Src, size_t Size);

  /**
   * @brief Blocking memset
   *
   * @param dst
   * @param size
   * @param pattern
   * @param pattern_size
   */
  virtual void memFill(void *Dst, size_t Size, const void *Pattern,
                       size_t PatternSize);

  /**
   * @brief Non-blocking mem set
   *
   * @param dst
   * @param size
   * @param pattern
   * @param pattern_size
   */
  virtual CHIPEvent *memFillAsyncImpl(void *Dst, size_t Size,
                                      const void *Pattern,
                                      size_t PatternSize) = 0;
  virtual void memFillAsync(void *Dst, size_t Size, const void *Pattern,
                            size_t PatternSize);

  // The memory copy 2D support
  virtual void memCopy2D(void *Dst, size_t DPitch, const void *Src,
                         size_t SPitch, size_t Width, size_t Height);

  virtual CHIPEvent *memCopy2DAsyncImpl(void *Dst, size_t DPitch,
                                        const void *Src, size_t SPitch,
                                        size_t Width, size_t Height) = 0;
  virtual void memCopy2DAsync(void *Dst, size_t DPitch, const void *Src,
                              size_t SPitch, size_t Width, size_t Height);

  // The memory copy 3D support
  virtual void memCopy3D(void *Dst, size_t DPitch, size_t DSPitch,
                         const void *Src, size_t SPitch, size_t SSPitch,
                         size_t Width, size_t Height, size_t Depth);

  virtual CHIPEvent *memCopy3DAsyncImpl(void *Dst, size_t DPitch,
                                        size_t DSPitch, const void *Src,
                                        size_t SPitch, size_t SSPitch,
                                        size_t Width, size_t Height,
                                        size_t Depth) = 0;
  virtual void memCopy3DAsync(void *Dst, size_t DPitch, size_t DSPitch,
                              const void *Src, size_t SPitch, size_t SSPitch,
                              size_t Width, size_t Height, size_t Depth);

  /**
   * @brief Submit a CHIPExecItem to this queue for execution. CHIPExecItem
   * needs to be complete - contain the kernel and arguments
   *
   * @param exec_item
   * @return hipError_t
   */
  virtual CHIPEvent *launchImpl(CHIPExecItem *ExecItem) = 0;
  virtual void launch(CHIPExecItem *ExecItem);

  /**
   * @brief Get the Device obj
   *
   * @return CHIPDevice*
   */

  CHIPDevice *getDevice();
  /**
   * @brief Wait for this queue to finish.
   *
   */

  virtual void finish() = 0;
  /**
   * @brief Check if the queue is still actively executing
   *
   * @return true
   * @return false
   */

  bool query() {
    if (!LastEvent_)
      return true;

    if (LastEvent_->updateFinishStatus(false))
      // LastEvent_->decreaseRefCount("query(): event became ready");
      if (LastEvent_->isFinished())
        return true;

    return false;
  };

  /**
   * @brief Insert an event into this queue
   *
   * @param e
   * @return true
   * @return false
   */
  virtual CHIPEvent *
  enqueueBarrierImpl(std::vector<CHIPEvent *> *EventsToWaitFor) = 0;
  virtual CHIPEvent *enqueueBarrier(std::vector<CHIPEvent *> *EventsToWaitFor);

  virtual CHIPEvent *enqueueMarkerImpl() = 0;
  CHIPEvent *enqueueMarker();

  /**
   * @brief Get the Flags object with which this queue was created.
   *
   * @return unsigned int
   */

  CHIPQueueFlags getFlags();
  /**
   * @brief Get the Priority object with which this queue was created.
   *
   * @return int
   */

  int getPriority();
  /**
   * @brief Add a callback funciton to be called on the host after the specified
   * stream is done
   *
   * @param callback function pointer for a ballback function
   * @param userData
   * @return true
   * @return false
   */

  virtual void addCallback(hipStreamCallback_t Callback, void *UserData);
  /**
   * @brief Insert a memory prefetch
   *
   * @param ptr
   * @param count
   * @return true
   * @return false
   */

  virtual CHIPEvent *memPrefetchImpl(const void *Ptr, size_t Count) = 0;
  void memPrefetch(const void *Ptr, size_t Count);

  /**
   * @brief Launch a kernel on this queue given a host pointer and arguments
   *
   * @param hostFunction
   * @param numBlocks
   * @param dimBlocks
   * @param args
   * @param sharedMemBytes
   */
  void launchKernel(CHIPKernel *ChipKernel, dim3 NumBlocks, dim3 DimBlocks,
                    void **Args, size_t SharedMemBytes);

  /**
   * @brief returns Native backend handles for a stream
   *
   * @param NativeHandles storage for handles
   * @param NumHandles variable to hold number of returned handles
   * @return for Level0 backend, returns { ze_driver_handle_t,
   * ze_device_handle_t, ze_context_handle_t, ze_command_queue_handle_t }
   * @return for OpenCL backend, returns { cl_platform_id, cl_device_id,
   * cl_context, cl_command_queue }
   */
  virtual hipError_t getBackendHandles(uintptr_t *NativeHandles,
                                       int *NumHandles) = 0;

  CHIPContext *getContext() { return ChipContext_; }
  void setFlags(CHIPQueueFlags TheFlags) { QueueFlags_ = TheFlags; }
};

#endif
