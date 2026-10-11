#include "Cafe/Savestate/SavestateSerializer_RPL.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/GamePatch.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"

// This file assumes the PATCH described in PATCH_ChunkedHeap.h.txt has
// been applied (GetAllocatedRangesForSavestate() / RestoreAllocatedRangesFromSavestate()
// on VHeap).
//
// -------------------------------------------------------------------
// WHY LOADING IS NOT A MIRROR OF SAVING FOR THIS SECTION
// -------------------------------------------------------------------
// RPLModule::RPLRawData is a non-owning std::span<uint8> - the actual
// file bytes are owned elsewhere (most likely by whatever FSC read call
// produced them in RPLLoader_LoadFromMemory(), which was not shown to
// me in full). Embedding the entire .rpx/.rpl file content in every
// savestate would be wasteful (these are static files already on disk)
// and the pointer fields derived from it (exportDDataPtr/exportFDataPtr,
// sectionAddressTable) are not portable host pointers anyway.
//
// So instead of serializing a module's raw bytes and pointers, saving
// only records "this module, under this name, was loaded and linked",
// plus the small scalar/guest-address fields that could differ from a
// fresh load (trampolineMap, fileInfo-derived fields already come from
// the file deterministically, but are stored anyway as a cheap
// consistency check - see VerifyReloadedModuleMatches() below).
//
// Loading:
//   1. Unload whatever is currently loaded (RPLLoader_UnloadAll()).
//   2. For each module recorded in the savestate, call
//      RPLLoader_LoadFromMemory() again on the SAME underlying file
//      (found by name via whatever path the title's content mount
//      uses - this glue was not available to me and needs writing
//      against your actual FSC/title-mount code).
//   3. RPLLoader_Link() to re-derive every pointer field exactly as
//      at boot.
//   4. Only THEN restore the allocator bookkeeping (heapTrampolineArea,
//      the three VHeap-backed heaps) and the handful of fields that a
//      fresh reload cannot reproduce (trampolineMap entries created
//      dynamically after linking, if any).
//   5. PPCRecompiler_invalidateRange() over the entire code area,
//      unconditionally - the MMU section has already overwritten
//      guest memory contents, so any JIT-compiled code from before
//      the load is invalid regardless of what RPL step reproduced.

// defined in rpl.cpp (external linkage)
extern ChunkedFlatAllocator<64 * 1024> g_heapTrampolineArea;

namespace SavestateSerializer_RPL
{
	// Mirrors the globals declared in rpl.cpp (rplLoader_maxCodeAddress,
	// etc.) - kept as one record for a single DoPOD() call.
	struct RPLGlobalsRecord
	{
		bool applicationHasMemoryControl;
		uint32 maxCodeAddress;
		uint32 currentTLSModuleIndex;
		uint32 currentHandleCounter;
		uint32 sdataAddr;
		uint32 sdata2Addr;
		uint32 currentDataAllocatorAddr;
		// g_map_callableExports is intentionally excluded - see the
		// project spec doc section 7 / earlier analysis: it is
		// repopulated identically at HLE bootstrap, before any title
		// loads, and is independent of which game is running.
	};

	struct ModuleRecord
	{
		char moduleName[RPL_MODULE_PATH_LENGTH];
		bool isMainExecutable;
		bool isLinked;
		bool entrypointCalled;
		uint32 entrypoint;
		MPTR regionMappingBase_text;
		MPTR regionMappingBase_data;
		MPTR regionMappingBase_loaderInfo;
		uint32 tlsStartAddress;
		uint32 tlsEndAddress;
		sint16 tlsModuleIndex;
		uint32 patchCRC;
		MPTR funcAlloc;
		MPTR funcFree;
		// trampolineMap and heapTrampolineArea state are appended
		// after this fixed record by DoState (variable-length).
	};

	// Deliberately minimal: loadAttempted/isCafeOSModule/
	// rplHLEModule are NOT saved - RPLLoader_RestoreDependencyForSavestate()
	// + one RPLLoader_UpdateDependencies() call regenerate all of that
	// exactly as a normal boot would, for every dependency at once.
	// coreinitHandle/tlsModuleIndex ARE saved and restored explicitly
	// (not auto-reassigned) since guest code may already hold onto
	// these values - see rpl.h for the full reasoning.
	struct DependencyRecord
	{
		char moduleName[RPL_MODULE_PATH_LENGTH];
		bool isMainExecutable;
		// MUST be restored: a freshly created dependency starts with
		// hleEntrypointCalled == false, and a later RPLLoader_CallEntrypoints()
		// would then re-run the entry point of every HLE module (coreinit,
		// gx2, ...) in the middle of a running game.
		bool hleEntrypointCalled;
		sint32 referenceCount;
		uint32 coreinitHandle;
		sint16 tlsModuleIndex;
	};

	// Common helper for the three VHeap-backed allocators (see
	// PATCH_ChunkedHeap.h.txt). Writes a length-prefixed list of
	// (offset, size) pairs.
	template <typename THeap>
	static void DoStateHeapAllocations(Serializer& s, THeap& heap)
	{
		if (s.IsWriting() || s.IsMeasuring())
		{
			auto ranges = heap.GetAllocatedRangesForSavestate();
			uint32 count = (uint32)ranges.size();
			s.DoPOD(count);
			for (auto& [offset, size] : ranges)
			{
				s.DoPOD(offset);
				s.DoPOD(size);
			}
			return;
		}
		uint32 count = 0;
		s.DoPOD(count);
		std::vector<std::pair<uint32, uint32>> ranges(count);
		for (auto& [offset, size] : ranges)
		{
			s.DoPOD(offset);
			s.DoPOD(size);
		}
		heap.RestoreAllocatedRangesFromSavestate(ranges);
	}

	// ChunkedFlatAllocator<T> (g_heapTrampolineArea, and each
	// RPLModule::heapTrampolineArea) only ever grows via releaseAll(),
	// never frees individual allocations - its state is the ordered
	// list of chunk-sized blocks it owns (each already accounted for as
	// an ordinary allocation of its BASE heap - see
	// DoStateHeapAllocations above) plus the bump cursor within the
	// most recent one. Requires PATCH_ChunkedHeap.h.txt part 2
	// (GetAllocatedBlocksForSavestate/RestoreForSavestate).
	template <uint32 TChunkSize>
	static void DoStateChunkedFlatAllocator(Serializer& s, ChunkedFlatAllocator<TChunkSize>& allocator)
	{
		if (s.IsWriting() || s.IsMeasuring())
		{
			const std::vector<void*>& blocks = allocator.GetAllocatedBlocksForSavestate();
			uint32 blockCount = (uint32)blocks.size();
			s.DoPOD(blockCount);
			for (void* blockPtr : blocks)
			{
				MPTR blockAddr = memory_getVirtualOffsetFromPointer(blockPtr);
				s.DoMPTR(blockAddr);
			}
			uint32 currentOffset = allocator.getCurrentBlockOffset();
			s.DoPOD(currentOffset);
			return;
		}

		uint32 blockCount = 0;
		s.DoPOD(blockCount);
		std::vector<void*> blocks(blockCount);
		for (auto& blockPtr : blocks)
		{
			MPTR blockAddr = 0;
			s.DoMPTR(blockAddr);
			blockPtr = memory_getPointerFromVirtualOffset(blockAddr);
		}
		uint32 currentOffset = 0;
		s.DoPOD(currentOffset);

		// Must run AFTER this allocator's base heap (rplLoaderHeap_workarea,
		// _lowerAreaCodeMem2 or _codeArea2) has already restored its own
		// allocation list via RestoreAllocatedRangesFromSavestate() -
		// these block addresses must already be marked allocated there,
		// or later allocations from either side could collide.
		allocator.RestoreForSavestate(blocks, currentOffset);
	}

	bool CanSafelySave()
	{
		for (RPLModule* module : RPLLoader_GetModuleList())
		{
			if (module->tempRegionPtr != nullptr)
				return false; // mid-load, unsupported savestate point
		}
		return true;
	}

	void DoState_Save(Serializer& s)
	{
		s.DoMarker("RPL");

		if (!CanSafelySave())
			throw std::runtime_error("Cannot save: a module is currently being loaded/linked.");

		RPLGlobalsRecord globals{};
		globals.applicationHasMemoryControl = rplLoader_applicationHasMemoryControl;
		globals.maxCodeAddress = rplLoader_maxCodeAddress;
		globals.currentTLSModuleIndex = rplLoader_currentTLSModuleIndex;
		globals.currentHandleCounter = rplLoader_currentHandleCounter;
		globals.sdataAddr = rplLoader_sdataAddr;
		globals.sdata2Addr = rplLoader_sdata2Addr;
		globals.currentDataAllocatorAddr = rplLoader_currentDataAllocatorAddr;
		s.DoPOD(globals);

		std::span<RPLModule*> modules = RPLLoader_GetModuleList();
		uint32 moduleCount = (uint32)modules.size();
		s.DoPOD(moduleCount);
		for (RPLModule* module : modules)
		{
			ModuleRecord record{};
			strncpy(record.moduleName, module->moduleName.c_str(), sizeof(record.moduleName) - 1);
			record.isMainExecutable = (module == rplLoader_mainModule);
			record.isLinked = module->isLinked;
			record.entrypointCalled = module->entrypointCalled;
			record.entrypoint = module->entrypoint;
			record.regionMappingBase_text = memory_getVirtualOffsetFromPointer(module->regionMappingBase_text.GetPtr());
			record.regionMappingBase_data = module->regionMappingBase_data;
			record.regionMappingBase_loaderInfo = module->regionMappingBase_loaderInfo;
			record.tlsStartAddress = module->tlsStartAddress;
			record.tlsEndAddress = module->tlsEndAddress;
			record.tlsModuleIndex = module->fileInfo.tlsModuleIndex;
			record.patchCRC = module->patchCRC;
			record.funcAlloc = module->funcAlloc.value();
			record.funcFree = module->funcFree.value();
			s.DoPOD(record);

			// per-module trampoline map (guest MPTR -> guest MPTR, fully portable)
			uint32 trampolineCount = (uint32)module->trampolineMap.size();
			s.DoPOD(trampolineCount);
			for (auto& [from, to] : module->trampolineMap)
			{
				MPTR f = from, t = to;
				s.DoMPTR(f);
				s.DoMPTR(t);
			}

			// per-module trampoline heap bump-allocator state
			DoStateChunkedFlatAllocator(s, module->heapTrampolineArea);
		}

		std::span<RPLDependency*> dependencies = RPLLoader_GetDependencyList();
		uint32 depCount = (uint32)dependencies.size();
		s.DoPOD(depCount);
		for (RPLDependency* dep : dependencies)
		{
			DependencyRecord record{};
			strncpy(record.moduleName, dep->moduleName.c_str(), sizeof(record.moduleName) - 1);
			record.isMainExecutable = dep->isMainExecutable;
			record.hleEntrypointCalled = dep->hleEntrypointCalled;
			record.referenceCount = dep->referenceCount;
			record.coreinitHandle = dep->coreinitHandle;
			record.tlsModuleIndex = dep->tlsModuleIndex;
			s.DoPOD(record);
		}

		// HLE table + PPC-callable exports + mapped imports: HLE call indices
		// embedded in guest trampoline opcodes depend on registration order
		// (execution history), so the whole table is saved - see rpl.h.
		{
			RPLHLESavestateData hle = RPLLoader_GetHLEStateForSavestate();
			s.DoVector(hle.hleTableOffsets);
			s.DoVector(hle.callables);
			s.DoVector(hle.mappedImports);
		}
		// The global (not per-module) trampoline allocator was previously
		// not saved; its blocks/bump cursor must match the restored heap.
		DoStateChunkedFlatAllocator(s, g_heapTrampolineArea);

		// Allocator bookkeeping for the three fixed heaps. Names match
		// the globals declared at the top of rpl.cpp.
		DoStateHeapAllocations(s, rplLoaderHeap_workarea);
		DoStateHeapAllocations(s, rplLoaderHeap_lowerAreaCodeMem2);
		DoStateHeapAllocations(s, rplLoaderHeap_codeArea2);
	}

	void DoState_Load(Serializer& s)
	{
		s.DoMarker("RPL");

		RPLGlobalsRecord globals{};
		s.DoPOD(globals);

		uint32 moduleCount = 0;
		s.DoPOD(moduleCount);

		// Everything read per module is held here until the three shared
		// heaps are restored further down - DoStateChunkedFlatAllocator()
		// must not run until rplLoaderHeap_workarea/_lowerAreaCodeMem2/
		// _codeArea2 have already restored their own allocation lists,
		// since a module's heapTrampolineArea blocks are themselves
		// entries in one of those three base heaps' saved allocation
		// lists (see PATCH_ChunkedHeap.h.txt part 2).
		struct PendingModuleState
		{
			ModuleRecord record;
			std::vector<std::pair<MPTR, MPTR>> trampolineEntries;
			std::vector<MPTR> flatAllocatorBlocks;
			uint32 flatAllocatorCurrentOffset;
		};
		std::vector<PendingModuleState> pending(moduleCount);

		// (Unloading, if needed at all, happens after the whole section was
		// read - see "module reuse" below. Nothing is modified before that.)

		// Step 2: read every module's saved record fully (no engine calls
		// yet) so the stream order below matches DoState_Save() exactly.
		for (uint32 i = 0; i < moduleCount; i++)
		{
			PendingModuleState& p = pending[i];
			s.DoPOD(p.record);

			uint32 trampolineCount = 0;
			s.DoPOD(trampolineCount);
			p.trampolineEntries.resize(trampolineCount);
			for (auto& [from, to] : p.trampolineEntries)
			{
				s.DoMPTR(from);
				s.DoMPTR(to);
			}

			// Raw read only - matches DoStateChunkedFlatAllocator()'s
			// on-disk layout (blockCount, then that many MPTRs, then
			// currentOffset) without calling it, since calling it here
			// would try to resolve block addresses to pointers and mark
			// them in the allocator before the base heap is ready.
			uint32 blockCount = 0;
			s.DoPOD(blockCount);
			p.flatAllocatorBlocks.resize(blockCount);
			for (auto& blockAddr : p.flatAllocatorBlocks)
				s.DoMPTR(blockAddr);
			s.DoPOD(p.flatAllocatorCurrentOffset);

		}

		// Step 2.5: read the dependency list the same way (see
		// DoState_Save - dependencies are written right after modules).
		uint32 depCount = 0;
		s.DoPOD(depCount);
		std::vector<DependencyRecord> depRecords(depCount);
		for (auto& depRecord : depRecords)
			s.DoPOD(depRecord);

		// Step 2.6: read callable exports + global trampoline allocator state
		// (written after the dependency list in DoState_Save).
		RPLHLESavestateData hleState;
		s.DoVector(hleState.hleTableOffsets);
		s.DoVector(hleState.callables);
		s.DoVector(hleState.mappedImports);
		std::vector<MPTR> globalTrampolineBlocks;
		uint32 globalTrampolineCurrentOffset = 0;
		{
			uint32 blockCount = 0;
			s.DoPOD(blockCount);
			globalTrampolineBlocks.resize(blockCount);
			for (auto& blockAddr : globalTrampolineBlocks)
				s.DoMPTR(blockAddr);
			s.DoPOD(globalTrampolineCurrentOffset);
		}

		// ---- Module reuse (fast path) -------------------------------------
		// Re-running RPLLoader_UnloadAll() + RPLLoader_UpdateDependencies()
		// re-executes every HLE module's RPLMapped() (coreinit re-initializes
		// its globals and re-allocates from the host-side system-area bump
		// allocator, for example). That is not guaranteed to reproduce the
		// addresses/host state the saved game was running with. So when the
		// game that is running right now already has exactly the modules the
		// savestate was taken with (same names, same addresses, same handles)
		// nothing is unloaded or reloaded at all: only the bookkeeping that
		// changed since is restored. This is the normal case (same game, same
		// session or freshly started game).
		auto currentModulesMatch = [&]() -> bool
		{
			std::span<RPLModule*> curModules = RPLLoader_GetModuleList();
			std::span<RPLDependency*> curDeps = RPLLoader_GetDependencyList();
			if (curModules.size() != pending.size() || curDeps.size() != depRecords.size())
				return false;
			for (auto& p : pending)
			{
				RPLModule* m = RPLLoader_GetModuleByName(p.record.moduleName);
				if (!m)
					return false;
				if (memory_getVirtualOffsetFromPointer(m->regionMappingBase_text.GetPtr()) != p.record.regionMappingBase_text ||
					m->regionMappingBase_data != p.record.regionMappingBase_data ||
					m->regionMappingBase_loaderInfo != p.record.regionMappingBase_loaderInfo)
					return false;
			}
			for (auto& d : depRecords)
			{
				bool found = false;
				for (RPLDependency* cur : curDeps)
				{
					if (cur->moduleName == d.moduleName && (bool)cur->isMainExecutable == (bool)d.isMainExecutable &&
						cur->coreinitHandle == d.coreinitHandle && cur->tlsModuleIndex == d.tlsModuleIndex)
					{
						found = true;
						break;
					}
				}
				if (!found)
					return false;
			}
			return true;
		};
		const bool modulesAlreadyMatch = currentModulesMatch();
		if (!modulesAlreadyMatch)
		{
			if constexpr (!SAVESTATE_ALLOW_MODULE_RELOAD)
			{
				throw std::runtime_error(
					"This savestate was made with a different set of loaded game modules than the game currently running "
					"(for example it was made after the game loaded an extra library). Close the game, start it again, "
					"play until roughly the same point, then load the savestate.");
			}
			RPLLoader_UnloadAll();
		}

		// Step 3a: RPLLoader_UnloadAll() (step 1, above) calls
		// RPLLoader_UnloadModule(dep, /*skipPPCCalls*/ true) for every
		// module - and critically, with skipPPCCalls == true, the DATA and
		// LOADER-INFO regions (rplLoaderHeap_workarea) are deliberately
		// NEVER FREED (see the comment "for some reason freeing the data
		// allocations causes a crash in MP10 on boot" right above that
		// call in rpl.cpp). Only the TEXT region is freed unconditionally.
		// Left as-is, the reload below would see a heap still full of
		// leaked "ghost" allocations from the old module and place the new
		// module's data/loaderInfo at DIFFERENT addresses than originally -
		// silently invalidating every pointer already baked into the MMU
		// section (guest data structures, CPU registers) that assumed the
		// old addresses. Explicitly wipe all three heaps' bookkeeping back
		// to a single free range first, so the reload starts from a
		// genuinely empty state - matching the condition the ORIGINAL load
		// happened under - and reproduces identical addresses
		// deterministically (same load order, same file sizes => same
		// first-fit allocation result).
		rplLoaderHeap_workarea.ResetForSavestate();
		rplLoaderHeap_lowerAreaCodeMem2.ResetForSavestate();
		rplLoaderHeap_codeArea2.ResetForSavestate();

		if (!modulesAlreadyMatch)
		{
			// Step 3b: recreate every dependency with its EXACT saved identity
			// (coreinitHandle, tlsModuleIndex - see rpl.h for why these must
			// not be freshly reassigned), then trigger one single
			// RPLLoader_UpdateDependencies() call - this reuses the real,
			// already-tested engine logic (cafeLibs override, HLE module
			// resolution, code-directory fallback via fsc_extractFile) for
			// every dependency at once, exactly as a normal boot does, into the
			// now-empty heaps from step 3a. It also calls RPLLoader_Link()
			// internally, so no separate call is needed.
			for (auto& depRecord : depRecords)
			{
				RPLLoader_RestoreDependencyForSavestate(depRecord.moduleName, depRecord.isMainExecutable,
					depRecord.referenceCount, depRecord.coreinitHandle, depRecord.tlsModuleIndex);
			}
			RPLLoader_UpdateDependencies();

			// Identify and (re-)register the main module now that loading is done.
			for (RPLDependency* dep : RPLLoader_GetDependencyList())
			{
				if (dep->isMainExecutable && dep->rplLoaderContext)
				{
					RPLLoader_SetMainModule(dep->rplLoaderContext);
					break;
				}
			}

			// Re-apply the "entrypoint already called" state (see DependencyRecord).
			for (RPLDependency* dep : RPLLoader_GetDependencyList())
			{
				for (auto& depRecord : depRecords)
				{
					if (dep->moduleName == depRecord.moduleName)
					{
						dep->hleEntrypointCalled = depRecord.hleEntrypointCalled;
						break;
					}
				}
			}

			// Step 3c: VERIFY the determinism assumption above actually held,
			// rather than silently trusting it. If a reloaded module ended up
			// at a different address than it was saved at, every pointer the
			// MMU section is about to restore into guest memory referencing
			// this module's old addresses would silently point at garbage -
			// refuse the load outright instead of continuing with corrupted
			// state.
			for (auto& p : pending)
			{
				RPLModule* module = RPLLoader_GetModuleByName(p.record.moduleName);
				if (!module)
					continue; // reported again, more specifically, in step 5 below
				MPTR actualText = memory_getVirtualOffsetFromPointer(module->regionMappingBase_text.GetPtr());
				if (actualText != p.record.regionMappingBase_text ||
					module->regionMappingBase_data != p.record.regionMappingBase_data ||
					module->regionMappingBase_loaderInfo != p.record.regionMappingBase_loaderInfo)
				{
					throw std::runtime_error(fmt::format(
						"Savestate load failed: module '{}' reloaded at a different address than it was "
						"saved at (text {:08x}->{:08x}, data {:08x}->{:08x}, loaderInfo {:08x}->{:08x}). "
						"Continuing would corrupt pointers already present in restored guest memory.",
						p.record.moduleName,
						p.record.regionMappingBase_text, actualText,
						p.record.regionMappingBase_data, module->regionMappingBase_data,
						p.record.regionMappingBase_loaderInfo, module->regionMappingBase_loaderInfo));
				}
			}

		}
		else
		{
			// modules stay as they are; only restore the saved identity data
			for (RPLDependency* dep : RPLLoader_GetDependencyList())
			{
				for (auto& depRecord : depRecords)
				{
					if (dep->moduleName == depRecord.moduleName)
					{
						dep->referenceCount = depRecord.referenceCount;
						dep->hleEntrypointCalled = depRecord.hleEntrypointCalled;
						break;
					}
				}
			}
		}

		// Step 4: NOW restore the three shared heaps' bookkeeping to the
		// exact saved allocation list (step 3c already confirmed the
		// actual addresses match, so this is authoritative bookkeeping
		// cleanup/confirmation, not a source of new addresses).
		DoStateHeapAllocations(s, rplLoaderHeap_workarea);
		DoStateHeapAllocations(s, rplLoaderHeap_lowerAreaCodeMem2);
		DoStateHeapAllocations(s, rplLoaderHeap_codeArea2);

		// Step 4b: restore the global trampoline allocator (its blocks are
		// entries of rplLoaderHeap_lowerAreaCodeMem2, restored just above)
		// and the callable export map, forcing HLE table slots so every
		// trampoline already present in restored guest memory keeps
		// resolving to the same host function. Overwrites whatever the
		// reload's own RPLLoader_Link() registered, which is correct: the
		// saved map is the authoritative superset.
		{
			std::vector<void*> blockPtrs(globalTrampolineBlocks.size());
			for (size_t i = 0; i < blockPtrs.size(); i++)
				blockPtrs[i] = memory_getPointerFromVirtualOffset(globalTrampolineBlocks[i]);
			g_heapTrampolineArea.RestoreForSavestate(blockPtrs, globalTrampolineCurrentOffset);
		}
		RPLLoader_RestoreHLEStateForSavestate(hleState);
		GamePatch_RefreshHLEIndicesForSavestate();

		// Step 5: only now apply each module's trampolineMap and
		// heapTrampolineArea state - safe, since step 4 already restored
		// the base heap allocations these blocks belong to.
		for (auto& p : pending)
		{
			RPLModule* module = RPLLoader_GetModuleByName(p.record.moduleName);
			if (!module)
			{
				// RPLLoader_UpdateDependencies() above failed to (re)load this
				// particular module - e.g. its file is genuinely missing on
				// disk. Skip rather than dereference null; the savestate load
				// is incomplete for this module in that case.
				cemuLog_log(LogType::Force, "Savestate: module '{}' was not reloaded, skipping trampoline restore", p.record.moduleName);
				continue;
			}

			module->entrypointCalled = p.record.entrypointCalled;
			module->trampolineMap.clear();
			for (auto& [from, to] : p.trampolineEntries)
				module->trampolineMap.emplace(from, to);

			std::vector<void*> blockPtrs(p.flatAllocatorBlocks.size());
			for (size_t i = 0; i < blockPtrs.size(); i++)
				blockPtrs[i] = memory_getPointerFromVirtualOffset(p.flatAllocatorBlocks[i]);
			module->heapTrampolineArea.RestoreForSavestate(blockPtrs, p.flatAllocatorCurrentOffset);
		}

		rplLoader_applicationHasMemoryControl = globals.applicationHasMemoryControl;
		rplLoader_maxCodeAddress = globals.maxCodeAddress;
		rplLoader_currentTLSModuleIndex = globals.currentTLSModuleIndex;
		rplLoader_currentHandleCounter = globals.currentHandleCounter;
		rplLoader_sdataAddr = globals.sdataAddr;
		rplLoader_sdata2Addr = globals.sdata2Addr;
		rplLoader_currentDataAllocatorAddr = globals.currentDataAllocatorAddr;

		// Mandatory: guest memory contents just changed underneath any
		// previously compiled code, regardless of how much of the reload
		// above matched the old layout exactly.
		PPCRecompiler_invalidateRange(MEMORY_CODEAREA_ADDR, MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE);
		PPCRecompiler_invalidateRange(MEMORY_CODE_TRAMPOLINE_AREA_ADDR, MEMORY_CODE_TRAMPOLINE_AREA_ADDR + MEMORY_CODE_TRAMPOLINE_AREA_SIZE);
	}

}
