#pragma once

struct RPLModule;

#define RPL_INVALID_HANDLE		0xFFFFFFFF

void RPLLoader_InitState();
void RPLLoader_UnloadAll();

uint8* RPLLoader_AllocateTrampolineCodeSpace(sint32 size);

MPTR RPLLoader_AllocateCodeSpace(uint32 size, uint32 alignment);

uint32 RPLLoader_GetMaxCodeOffset();
uint32 RPLLoader_GetDataAllocatorAddr();

RPLModule* RPLLoader_LoadFromMemory(uint8* rplData, sint32 size, std::string_view name);
uint32 rpl_mapHLEImport(RPLModule* rplLoaderContext, const char* rplName, const char* funcName, bool functionMustExist);
void RPLLoader_Link();

MPTR RPLLoader_FindRPLExport(RPLModule* rplLoaderContext, const char* symbolName, bool isData);
uint32 RPLLoader_GetModuleEntrypoint(RPLModule* rplLoaderContext);

void RPLLoader_SetMainModule(RPLModule* rplLoaderContext);
uint32 RPLLoader_GetMainModuleHandle();

void RPLLoader_CallEntrypoints();
void RPLLoader_CallCoreinitEntrypoint();
void RPLLoader_NotifyControlPassedToApplication();

void RPLLoader_AddDependency(std::string_view name, bool isMainExecutable = false);
void RPLLoader_RemoveDependency(uint32 handle);
bool RPLLoader_HasDependency(std::string_view name);
void RPLLoader_UpdateDependencies();

void RPLLoader_LoadCoreinit();

uint32 RPLLoader_GetHandleByModuleName(const char* name);
const std::string RPLLoader_GetModuleNameByHandle(uint32 handle);
uint32 RPLLoader_GetMaxTLSModuleIndex();
bool RPLLoader_GetTLSDataByTLSIndex(sint16 tlsModuleIndex, uint8** tlsData, sint32* tlsSize);

uint32 RPLLoader_FindModuleOrHLEExport(uint32 moduleHandle, bool isData, const char* exportName);

uint32 RPLLoader_GetSDA1Base();
uint32 RPLLoader_GetSDA2Base();

std::span<RPLModule*> RPLLoader_GetModuleList();
RPLModule* RPLLoader_GetModuleByName(std::string_view name);

// Added to support CEmulador's savestate feature (Cafe/Savestate/SavestateSerializer_RPL.cpp)
struct RPLDependency;
std::span<RPLDependency*> RPLLoader_GetDependencyList();

// Recreates a dependency entry with EXPLICIT identity fields restored
// from a savestate (coreinitHandle, tlsModuleIndex, referenceCount),
// rather than auto-assigning fresh ones the way RPLLoader_AddDependency()
// does. This matters because guest code may hold onto a handle or rely
// on a TLS module index obtained before the savestate was taken - a
// freshly auto-assigned value would silently break that. Mirrors
// RPLLoader_AddDependency()'s HLE-module resolution logic exactly, but
// never advances rplLoader_currentHandleCounter/currentTLSModuleIndex
// (the caller must restore those scalars separately, once, from the
// same savestate - see RPLGlobalsRecord in SavestateSerializer_RPL.cpp).
// Does NOT trigger loading - call RPLLoader_UpdateDependencies() once
// after adding every dependency to actually load them all.
// PPC-callable exports (RPLLoader_MakePPCCallable) are keyed by HOST function
// pointers, which are not portable across processes (ASLR). For savestates
// they are identified by their offset from an anchor function in the same
// binary (constant for a given build regardless of load address).
struct RPLCallableExportSavestateRecord
{
	sint64 functionOffsetFromAnchor;
	uint32 codeAddr; // guest address of the trampoline
	uint32 padding;
};
struct RPLMappedImportSavestateRecord
{
	uint64 hash1;
	uint64 hash2;
	uint32 address;
	uint32 padding;
};
// Everything that ties guest trampolines to host functions.
struct RPLHLESavestateData
{
	// HLE table: slot i -> offset of the function from the anchor (INT64_MIN = empty)
	std::vector<sint64> hleTableOffsets;
	std::vector<RPLCallableExportSavestateRecord> callables;
	std::vector<RPLMappedImportSavestateRecord> mappedImports;
};
RPLHLESavestateData RPLLoader_GetHLEStateForSavestate();
// Replaces the HLE table, remaps cached OS function indices, rebuilds the
// callable export map and the mapped import list.
void RPLLoader_RestoreHLEStateForSavestate(const RPLHLESavestateData& data);
// Fingerprint of the binary layout (distance between two functions); used
// as part of the savestate compatibility hash.
sint64 RPLLoader_GetBinaryLayoutFingerprintForSavestate();

void RPLLoader_RestoreDependencyForSavestate(std::string_view moduleName, bool isMainExecutable,
	sint32 referenceCount, uint32 coreinitHandle, sint16 tlsModuleIndex);

MEMPTR<void> RPLLoader_AllocateCodeCaveMem(uint32 alignment, uint32 size);
void RPLLoader_ReleaseCodeCaveMem(MEMPTR<void> addr);

// exports

uint32 RPLLoader_MakePPCCallable(void(*ppcCallableExport)(struct PPCInterpreter_t* hCPU));

// elf loader

uint32 ELF_LoadFromMemory(uint8* elfData, sint32 size, const char* name);