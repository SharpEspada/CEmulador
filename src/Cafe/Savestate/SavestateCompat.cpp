#include "Cafe/Savestate/SavestateCompat.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/OS/RPL/rpl.h"
#include "config/ActiveSettings.h"
#include "util/crypto/crc32.h"

std::string SavestateCompat_GetBuildIdString()
{
	// Purely informational (shown to the user / logged on refusal),
	// does not gate compatibility by itself.
#if defined(CEMU_VERSION_STRING)
	return CEMU_VERSION_STRING;
#else
	return "unknown-build";
#endif
}

uint64 SavestateCompat_ComputeCurrentHash()
{
	std::string compatString;

	// 1. Build identity. A savestate from a different build must never
	//    silently load - RPLModule layout, GPU register layout, or
	//    struct packing could differ even between builds that "should"
	//    be compatible. Strict for V1; revisit only with an explicit,
	//    tested migration path.
	compatString += SavestateCompat_GetBuildIdString();
	compatString += '|';

	// 1b. Binary layout fingerprint. Savestates store PPC-callable HLE
	//     functions as offsets from an anchor function inside this
	//     executable (see RPLLoader_GetHLEStateForSavestate), which
	//     is only valid for the exact same compiled binary. The version
	//     string alone cannot guarantee that (two local builds can share
	//     it), so the distance between two functions is folded in too.
	compatString += fmt::format("layout={}|", RPLLoader_GetBinaryLayoutFingerprintForSavestate());

	// 2. CPU mode - determines how many host scheduler threads exist
	//    (see coreinit_Thread.cpp's single-core vs sSchedulerThreads
	//    model) and therefore how SavestateSerializer_Thread.cpp must
	//    reconstruct the barrier on load.
	compatString += fmt::format("cpumode={}|", (int)ActiveSettings::GetCPUMode());

	// 3. Active graphic packs. These can change MMURange sizes at
	//    runtime (see MMU.cpp memory_mapForCurrentTitle(), which reads
	//    GraphicPack2::GetActiveRAMMappings() and calls mmuRange->setEnd()).
	//    A savestate made with a different set of active packs may
	//    have differently-sized memory regions and MUST be refused
	//    rather than partially applied.
	auto activeMappings = GraphicPack2::GetActiveRAMMappings();
	for (auto& mapping : activeMappings)
		compatString += fmt::format("gfxpack_range={:08x}-{:08x}|", mapping.first, mapping.second);

	// NOTE: if GraphicPack2 exposes a per-pack name+version list
	// elsewhere (I have only seen GetActiveRAMMappings() in MMU.cpp),
	// fold that in here too - two different packs could theoretically
	// produce the same RAM mapping footprint while still changing
	// emulated behavior in other ways (shader patches, etc.).

	uint32 lo = crc32(0, (const uint8*)compatString.data(), (uint32)compatString.size());
	uint32 hi = crc32(0x9E3779B9u, (const uint8*)compatString.data(), (uint32)compatString.size());
	return ((uint64)hi << 32) | (uint64)lo;
}
