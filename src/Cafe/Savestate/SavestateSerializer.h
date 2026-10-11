#pragma once
// SavestateSerializer.h
//
// Generic symmetric serializer, modeled after Dolphin's PointerWrap.
// Every DoState(Serializer&) function is written ONCE and works for
// both saving and loading depending on the serializer's mode. This
// eliminates an entire class of save/load-mismatch bugs by construction.
//
// Usage pattern for any subsystem:
//
//   void MyThing::DoState(Serializer& s)
//   {
//       s.DoMarker("MyThing");
//       s.DoPOD(m_someScalar);
//       s.DoVector(m_someVector);
//       s.DoMPTR(m_someGuestPointer);
//   }
//
// Calling this once in MEASURE mode gives the exact byte size needed.
// Calling it again in WRITE mode with a preallocated buffer writes the
// same bytes. Calling it in READ mode restores the same fields, in the
// same order - which is why field order must never differ between the
// save and load code paths (there is only one path).

#include "Cafe/HW/MMU/MMU.h" // MPTR, MEMPTR<T>

enum class SerializerMode
{
	Measure,	// dry run: only accumulates m_size, touches no memory
	Write,		// writes into m_buffer at m_pos
	Read,		// reads from m_buffer at m_pos
};

class Serializer
{
public:
	explicit Serializer(SerializerMode mode) : m_mode(mode) {}

	SerializerMode GetMode() const { return m_mode; }
	bool IsReading() const { return m_mode == SerializerMode::Read; }
	bool IsWriting() const { return m_mode == SerializerMode::Write; }
	bool IsMeasuring() const { return m_mode == SerializerMode::Measure; }

	// Attaches a preallocated buffer for Write or Read mode. Must be
	// called after a Measure pass has determined m_size, for Write.
	void SetBuffer(uint8* buffer, size_t bufferSize)
	{
		cemu_assert_debug(m_mode != SerializerMode::Measure);
		m_buffer = buffer;
		m_bufferSize = bufferSize;
		m_pos = 0;
	}

	size_t GetMeasuredSize() const
	{
		cemu_assert_debug(m_mode == SerializerMode::Measure);
		return m_size;
	}

	size_t GetCurrentPosition() const { return m_pos; }

	// --- raw byte transfer -------------------------------------------------

	// The single primitive everything else is built on.
	void DoBytes(void* data, size_t size)
	{
		if (m_mode == SerializerMode::Measure)
		{
			m_size += size;
			return;
		}
		cemu_assert_debug(m_pos + size <= m_bufferSize);
		if (m_mode == SerializerMode::Write)
			memcpy(m_buffer + m_pos, data, size);
		else // Read
			memcpy(data, m_buffer + m_pos, size);
		m_pos += size;
	}

	// --- typed helpers -------------------------------------------------

	// Any trivially-copyable scalar or fixed-size struct (registers,
	// small fixed arrays, config structs with no pointers inside).
	// NEVER use this on a struct that contains a raw host pointer -
	// use DoMPTR()/DoHostRecomputed() for those fields individually
	// instead of DoPOD()'ing the whole struct.
	template <typename T>
	void DoPOD(T& value)
	{
		static_assert(std::is_trivially_copyable_v<T>,
			"DoPOD<T> requires a trivially copyable type. If T contains "
			"a host pointer, serialize its fields individually instead.");
		DoBytes(&value, sizeof(T));
	}

	template <typename T>
	void DoPODArray(T* arrayPtr, size_t count)
	{
		static_assert(std::is_trivially_copyable_v<T>);
		DoBytes(arrayPtr, sizeof(T) * count);
	}

	// A guest-space pointer (VAddr/MPTR). Portable across processes by
	// construction - this is the ONLY kind of "pointer" that should
	// ever cross the savestate boundary directly.
	void DoMPTR(MPTR& mptr)
	{
		DoPOD(mptr);
	}

	template <typename T>
	void DoMEMPTR(MEMPTR<T>& memPtr)
	{
		// MEMPTR<T> stores its value as a big-endian guest offset
		// internally; treat it as the raw 32-bit value, never as a
		// host pointer.
		uint32 raw = memPtr.GetMPTR();
		DoPOD(raw);
		if (IsReading())
			memPtr.SetMPTR(raw);
	}

	// std::vector of POD elements: length-prefixed.
	template <typename T>
	void DoVector(std::vector<T>& vec)
	{
		static_assert(std::is_trivially_copyable_v<T>);
		uint32 count = (uint32)vec.size();
		DoPOD(count);
		if (IsReading())
			vec.resize(count);
		if (count > 0)
			DoBytes(vec.data(), sizeof(T) * count);
	}

	// std::string: length-prefixed, no null terminator stored.
	void DoString(std::string& str)
	{
		uint32 length = (uint32)str.size();
		DoPOD(length);
		if (IsReading())
			str.resize(length);
		if (length > 0)
			DoBytes(str.data(), length);
	}

	// Fixed-size char buffer (e.g. a char[64] module name field).
	void DoFixedString(char* buf, size_t bufSize)
	{
		DoBytes(buf, bufSize);
	}

	// A boundary marker written between logical sections. On read,
	// mismatches immediately (rather than silently misinterpreting
	// whatever comes next) if a prior field desynced the stream -
	// this is the single most useful debugging aid for this kind of
	// serializer and should be used liberally, once per DoState().
	void DoMarker(const char* name)
	{
		char marker[32] = {};
		strncpy(marker, name, sizeof(marker) - 1);
		if (IsWriting() || IsMeasuring())
		{
			DoBytes(marker, sizeof(marker));
			return;
		}
		// Reading: compare against what's actually in the stream.
		char markerFromFile[32];
		DoBytes(markerFromFile, sizeof(markerFromFile));
		if (memcmp(marker, markerFromFile, sizeof(marker)) != 0)
		{
			throw SavestateDesyncException(name, markerFromFile);
		}
	}

	// --- host-recomputed fields -------------------------------------------------
	//
	// For fields that are host pointers or host-only values which must
	// NOT be carried across a savestate (GPU vendor, allocator internal
	// pointers, etc.) - these are intentionally NOT serialized. This
	// helper exists purely as documentation/marker at the call site so
	// a reviewer can see the field was consciously skipped, not
	// forgotten.
	template <typename T>
	void SkipHostRecomputed(const char* fieldName, T& /*value*/)
	{
		// no-op by design
		(void)fieldName;
	}

private:
	SerializerMode m_mode;
	uint8* m_buffer = nullptr;
	size_t m_bufferSize = 0;
	size_t m_pos = 0;
	size_t m_size = 0; // only meaningful in Measure mode
};

// Thrown when DoMarker() detects the read stream has desynced from
// what the writer produced - almost always means a DoState() function
// was changed on one side without a matching format version bump, or
// a field was added/removed/reordered inconsistently.
class SavestateDesyncException : public std::exception
{
public:
	SavestateDesyncException(const char* expectedMarker, const char* actualMarker)
	{
		m_message = fmt::format("Savestate desync at marker '{}' (found '{}' instead). "
			"The savestate format or a DoState() implementation is out of sync.",
			expectedMarker, actualMarker);
	}
	const char* what() const noexcept override { return m_message.c_str(); }
private:
	std::string m_message;
};

// Helper: runs a DoState-style function twice (measure, then write)
// into a freshly allocated buffer. Returns the buffer plus its size.
// This is the standard entry point every SavestateSerializer_*.cpp
// section should be driven through.
template <typename TDoStateFunc>
std::vector<uint8> Savestate_SerializeSection(TDoStateFunc&& doStateFunc)
{
	Serializer measurer(SerializerMode::Measure);
	doStateFunc(measurer);
	std::vector<uint8> buffer(measurer.GetMeasuredSize());

	Serializer writer(SerializerMode::Write);
	writer.SetBuffer(buffer.data(), buffer.size());
	doStateFunc(writer);
	cemu_assert_debug(writer.GetCurrentPosition() == buffer.size());
	return buffer;
}

template <typename TDoStateFunc>
void Savestate_DeserializeSection(const std::vector<uint8>& buffer, TDoStateFunc&& doStateFunc)
{
	Serializer reader(SerializerMode::Read);
	reader.SetBuffer(const_cast<uint8*>(buffer.data()), buffer.size());
	doStateFunc(reader);
}
