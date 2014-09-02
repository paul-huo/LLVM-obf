//===-- MCJIT.h - Class definition for the MCJIT ----------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_EXECUTIONENGINE_MCJIT_MCJIT_H
#define LLVM_LIB_EXECUTIONENGINE_MCJIT_MCJIT_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/ObjectCache.h"
#include "llvm/ExecutionEngine/ObjectImage.h"
#include "llvm/ExecutionEngine/RuntimeDyld.h"
#include "llvm/IR/Module.h"

namespace llvm {
class MCJIT;

// This is a helper class that the MCJIT execution engine uses for linking
// functions across modules that it owns.  It aggregates the memory manager
// that is passed in to the MCJIT constructor and defers most functionality
// to that object.
class LinkingMemoryManager : public RTDyldMemoryManager {
public:
  LinkingMemoryManager(MCJIT *Parent, RTDyldMemoryManager *MM)
    : ParentEngine(Parent), ClientMM(MM) {}

  uint64_t getSymbolAddress(const std::string &Name) override;

  // Functions deferred to client memory manager
  uint8_t *allocateCodeSection(uintptr_t Size, unsigned Alignment,
                               unsigned SectionID,
                               StringRef SectionName) override {
    return ClientMM->allocateCodeSection(Size, Alignment, SectionID, SectionName);
  }

  uint8_t *allocateDataSection(uintptr_t Size, unsigned Alignment,
                               unsigned SectionID, StringRef SectionName,
                               bool IsReadOnly) override {
    return ClientMM->allocateDataSection(Size, Alignment,
                                         SectionID, SectionName, IsReadOnly);
  }

  void reserveAllocationSpace(uintptr_t CodeSize, uintptr_t DataSizeRO,
                              uintptr_t DataSizeRW) override {
    return ClientMM->reserveAllocationSpace(CodeSize, DataSizeRO, DataSizeRW);
  }

  bool needsToReserveAllocationSpace() override {
    return ClientMM->needsToReserveAllocationSpace();
  }

  void notifyObjectLoaded(ExecutionEngine *EE,
                          const ObjectImage *Obj) override {
    ClientMM->notifyObjectLoaded(EE, Obj);
  }

  void registerEHFrames(uint8_t *Addr, uint64_t LoadAddr,
                        size_t Size) override {
    ClientMM->registerEHFrames(Addr, LoadAddr, Size);
  }

  void deregisterEHFrames(uint8_t *Addr, uint64_t LoadAddr,
                          size_t Size) override {
    ClientMM->deregisterEHFrames(Addr, LoadAddr, Size);
  }

  bool finalizeMemory(std::string *ErrMsg = nullptr) override {
    return ClientMM->finalizeMemory(ErrMsg);
  }

private:
  MCJIT *ParentEngine;
  std::unique_ptr<RTDyldMemoryManager> ClientMM;
};

// About Module states: added->loaded->finalized.
//
// The purpose of the "added" state is having modules in standby. (added=known
// but not compiled). The idea is that you can add a module to provide function
// definitions but if nothing in that module is referenced by a module in which
// a function is executed (note the wording here because it's not exactly the
// ideal case) then the module never gets compiled. This is sort of lazy
// compilation.
//
// The purpose of the "loaded" state (loaded=compiled and required sections
// copied into local memory but not yet ready for execution) is to have an
// intermediate state wherein clients can remap the addresses of sections, using
// MCJIT::mapSectionAddress, (in preparation for later copying to a new location
// or an external process) before relocations and page permissions are applied.
//
// It might not be obvious at first glance, but the "remote-mcjit" case in the
// lli tool does this.  In that case, the intermediate action is taken by the
// RemoteMemoryManager in response to the notifyObjectLoaded function being
// called.

class MCJIT : public ExecutionEngine {
  MCJIT(std::unique_ptr<Module> M, TargetMachine *tm,
        RTDyldMemoryManager *MemMgr);

  typedef llvm::SmallPtrSet<Module *, 4> ModulePtrSet;

  class OwningModuleContainer {
  public:
    OwningModuleContainer() {
    }
    ~OwningModuleContainer() {
      freeModulePtrSet(AddedModules);
      freeModulePtrSet(LoadedModules);
      freeModulePtrSet(FinalizedModules);
    }

    ModulePtrSet::iterator begin_added() { return AddedModules.begin(); }
    ModulePtrSet::iterator end_added() { return AddedModules.end(); }

    ModulePtrSet::iterator begin_loaded() { return LoadedModules.begin(); }
    ModulePtrSet::iterator end_loaded() { return LoadedModules.end(); }

    ModulePtrSet::iterator begin_finalized() { return FinalizedModules.begin(); }
    ModulePtrSet::iterator end_finalized() { return FinalizedModules.end(); }

    void addModule(std::unique_ptr<Module> M) {
      AddedModules.insert(M.release());
    }

    bool removeModule(Module *M) {
      return AddedModules.erase(M) || LoadedModules.erase(M) ||
             FinalizedModules.erase(M);
    }

    bool hasModuleBeenAddedButNotLoaded(Module *M) {
      return AddedModules.count(M) != 0;
    }

    bool hasModuleBeenLoaded(Module *M) {
      // If the module is in either the "loaded" or "finalized" sections it
      // has been loaded.
      return (LoadedModules.count(M) != 0 ) || (FinalizedModules.count(M) != 0);
    }

    bool hasModuleBeenFinalized(Module *M) {
      return FinalizedModules.count(M) != 0;
    }

    bool ownsModule(Module* M) {
      return (AddedModules.count(M) != 0) || (LoadedModules.count(M) != 0) ||
             (FinalizedModules.count(M) != 0);
    }

    void markModuleAsLoaded(Module *M) {
      // This checks against logic errors in the MCJIT implementation.
      // This function should never be called with either a Module that MCJIT
      // does not own or a Module that has already been loaded and/or finalized.
      assert(AddedModules.count(M) &&
             "markModuleAsLoaded: Module not found in AddedModules");

      // Remove the module from the "Added" set.
      AddedModules.erase(M);

      // Add the Module to the "Loaded" set.
      LoadedModules.insert(M);
    }

    void markModuleAsFinalized(Module *M) {
      // This checks against logic errors in the MCJIT implementation.
      // This function should never be called with either a Module that MCJIT
      // does not own, a Module that has not been loaded or a Module that has
      // already been finalized.
      assert(LoadedModules.count(M) &&
             "markModuleAsFinalized: Module not found in LoadedModules");

      // Remove the module from the "Loaded" section of the list.
      LoadedModules.erase(M);

      // Add the Module to the "Finalized" section of the list by inserting it
      // before the 'end' iterator.
      FinalizedModules.insert(M);
    }

    void markAllLoadedModulesAsFinalized() {
      for (ModulePtrSet::iterator I = LoadedModules.begin(),
                                  E = LoadedModules.end();
           I != E; ++I) {
        Module *M = *I;
        FinalizedModules.insert(M);
      }
      LoadedModules.clear();
    }

  private:
    ModulePtrSet AddedModules;
    ModulePtrSet LoadedModules;
    ModulePtrSet FinalizedModules;

    void freeModulePtrSet(ModulePtrSet& MPS) {
      // Go through the module set and delete everything.
      for (ModulePtrSet::iterator I = MPS.begin(), E = MPS.end(); I != E; ++I) {
        Module *M = *I;
        delete M;
      }
      MPS.clear();
    }
  };

  TargetMachine *TM;
  MCContext *Ctx;
  LinkingMemoryManager MemMgr;
  RuntimeDyld Dyld;
  std::vector<JITEventListener*> EventListeners;

  OwningModuleContainer OwnedModules;

  SmallVector<object::OwningBinary<object::Archive>, 2> Archives;
  SmallVector<std::unique_ptr<MemoryBuffer>, 2> Buffers;

  typedef SmallVector<ObjectImage *, 2> LoadedObjectList;
  LoadedObjectList  LoadedObjects;

  // An optional ObjectCache to be notified of compiled objects and used to
  // perform lookup of pre-compiled code to avoid re-compilation.
  ObjectCache *ObjCache;

  Function *FindFunctionNamedInModulePtrSet(const char *FnName,
                                            ModulePtrSet::iterator I,
                                            ModulePtrSet::iterator E);

  void runStaticConstructorsDestructorsInModulePtrSet(bool isDtors,
                                                      ModulePtrSet::iterator I,
                                                      ModulePtrSet::iterator E);

public:
  ~MCJIT();

  /// @name ExecutionEngine interface implementation
  /// @{
  void addModule(std::unique_ptr<Module> M) override;
  void addObjectFile(std::unique_ptr<object::ObjectFile> O) override;
  void addObjectFile(object::OwningBinary<object::ObjectFile> O) override;
  void addArchive(object::OwningBinary<object::Archive> O) override;
  bool removeModule(Module *M) override;

  /// FindFunctionNamed - Search all of the active modules to find the one that
  /// defines FnName.  This is very slow operation and shouldn't be used for
  /// general code.
  Function *FindFunctionNamed(const char *FnName) override;

  /// Sets the object manager that MCJIT should use to avoid compilation.
  void setObjectCache(ObjectCache *manager) override;

  void setProcessAllSections(bool ProcessAllSections) override {
    Dyld.setProcessAllSections(ProcessAllSections);
  }

  void generateCodeForModule(Module *M) override;

  /// finalizeObject - ensure the module is fully processed and is usable.
  ///
  /// It is the user-level function for completing the process of making the
  /// object usable for execution. It should be called after sections within an
  /// object have been relocated using mapSectionAddress.  When this method is
  /// called the MCJIT execution engine will reapply relocations for a loaded
  /// object.
  /// Is it OK to finalize a set of modules, add modules and finalize again.
  // FIXME: Do we really need both of these?
  void finalizeObject() override;
  virtual void finalizeModule(Module *);
  void finalizeLoadedModules();

  /// runStaticConstructorsDestructors - This method is used to execute all of
  /// the static constructors or destructors for a program.
  ///
  /// \param isDtors - Run the destructors instead of constructors.
  void runStaticConstructorsDestructors(bool isDtors) override;

  void *getPointerToFunction(Function *F) override;

  GenericValue runFunction(Function *F,
                           const std::vector<GenericValue> &ArgValues) override;

  /// getPointerToNamedFunction - This method returns the address of the
  /// specified function by using the dlsym function call.  As such it is only
  /// useful for resolving library symbols, not code generated symbols.
  ///
  /// If AbortOnFailure is false and no function with the given name is
  /// found, this function silently returns a null pointer. Otherwise,
  /// it prints a message to stderr and aborts.
  ///
  void *getPointerToNamedFunction(const std::string &Name,
                                  bool AbortOnFailure = true) override;

  /// mapSectionAddress - map a section to its target address space value.
  /// Map the address of a JIT section as returned from the memory manager
  /// to the address in the target process as the running code will see it.
  /// This is the address which will be used for relocation resolution.
  void mapSectionAddress(const void *LocalAddress,
                         uint64_t TargetAddress) override {
    Dyld.mapSectionAddress(LocalAddress, TargetAddress);
  }
  void RegisterJITEventListener(JITEventListener *L) override;
  void UnregisterJITEventListener(JITEventListener *L) override;

  // If successful, these function will implicitly finalize all loaded objects.
  // To get a function address within MCJIT without causing a finalize, use
  // getSymbolAddress.
  uint64_t getGlobalValueAddress(const std::string &Name) override;
  uint64_t getFunctionAddress(const std::string &Name) override;

  TargetMachine *getTargetMachine() override { return TM; }

  /// @}
  /// @name (Private) Registration Interfaces
  /// @{

  static void Register() {
    MCJITCtor = createJIT;
  }

  static ExecutionEngine *createJIT(std::unique_ptr<Module> M,
                                    std::string *ErrorStr,
                                    RTDyldMemoryManager *MemMgr,
                                    TargetMachine *TM);

  // @}

  // This is not directly exposed via the ExecutionEngine API, but it is
  // used by the LinkingMemoryManager.
  uint64_t getSymbolAddress(const std::string &Name,
                          bool CheckFunctionsOnly);

protected:
  /// emitObject -- Generate a JITed object in memory from the specified module
  /// Currently, MCJIT only supports a single module and the module passed to
  /// this function call is expected to be the contained module.  The module
  /// is passed as a parameter here to prepare for multiple module support in
  /// the future.
  ObjectBufferStream* emitObject(Module *M);

  void NotifyObjectEmitted(const ObjectImage& Obj);
  void NotifyFreeingObject(const ObjectImage& Obj);

  uint64_t getExistingSymbolAddress(const std::string &Name);
  Module *findModuleForSymbol(const std::string &Name,
                              bool CheckFunctionsOnly);
};

} // End llvm namespace

#endif
