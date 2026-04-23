#pragma once

#include "PCH.h"

#include <cstring>
#include <memory>
#include <vector>

namespace BehaviorSwitchboard {

// =============================================================================
// BSBMemoryStream — read-only in-memory BSResource::Stream
//
// Wraps a shared immutable byte buffer so that DoClone() creates an
// independent cursor over the same data without copying the bytes.
//
// DoRead/DoSeek are const (matching the Stream vtable) — position is
// tracked via a mutable cursor.  Thread safety: each stream/clone owns its
// own cursor, so concurrent reads on separate instances are safe.  Calling
// DoRead on the same instance from multiple threads is not supported (Havok
// doesn't do this for packfile loading).
//
// Memory: inherits TES_HEAP_REDEFINE_NEW() from StreamBase so operator new
// goes through the game heap — symmetric with the delete in DecRef().
// =============================================================================

class BSBMemoryStream : public RE::BSResource::Stream
{
public:
    /// Construct from an rvalue buffer.  Moves the bytes into a shared_ptr;
    /// subsequent DoClone() calls share the same allocation with no copy.
    explicit BSBMemoryStream(std::vector<std::uint8_t> a_data)
        : RE::BSResource::Stream(static_cast<std::uint32_t>(a_data.size()))
        , m_data(std::make_shared<const std::vector<std::uint8_t>>(std::move(a_data)))
        , m_pos(0)
    {}

    ~BSBMemoryStream() override = default;

    // ── StreamBase virtuals ─────────────────────────────────────────────────

    RE::BSResource::ErrorCode DoOpen() override
    {
        m_pos = 0;
        return RE::BSResource::ErrorCode::kNone;
    }

    void DoClose() override
    {
        m_pos = 0;
    }

    // ── Stream virtuals ─────────────────────────────────────────────────────

    /// Clone shares the buffer; the new instance starts at position 0.
    void DoClone(RE::BSTSmartPointer<RE::BSResource::Stream>& a_out) const override
    {
        a_out = RE::BSTSmartPointer<RE::BSResource::Stream>{ new BSBMemoryStream(m_data) };
    }

    RE::BSResource::ErrorCode DoRead(
        void*          a_buf,
        std::uint64_t  a_toRead,
        std::uint64_t& a_read) const override
    {
        const auto& data  = *m_data;
        const auto  avail = static_cast<std::uint64_t>(data.size()) - m_pos;
        a_read            = (a_toRead < avail) ? a_toRead : avail;
        if (a_read > 0) {
            std::memcpy(a_buf, data.data() + static_cast<std::size_t>(m_pos),
                        static_cast<std::size_t>(a_read));
            m_pos += a_read;
        }
        return RE::BSResource::ErrorCode::kNone;
    }

    RE::BSResource::ErrorCode DoWrite(
        const void*, std::uint64_t, std::uint64_t& a_written) const override
    {
        a_written = 0;
        return RE::BSResource::ErrorCode::kUnsupported;
    }

    RE::BSResource::ErrorCode DoSeek(
        std::uint64_t            a_offset,
        RE::BSResource::SeekMode a_mode,
        std::uint64_t&           a_sought) const override
    {
        const auto size = static_cast<std::uint64_t>(m_data->size());
        std::uint64_t newPos;
        switch (a_mode) {
        case RE::BSResource::SeekMode::kSet: newPos = a_offset;         break;
        case RE::BSResource::SeekMode::kCur: newPos = m_pos + a_offset; break;
        case RE::BSResource::SeekMode::kEnd: newPos = size + a_offset;  break;
        default:
            a_sought = m_pos;
            return RE::BSResource::ErrorCode::kUnsupported;
        }
        m_pos    = (newPos < size) ? newPos : size;
        a_sought = m_pos;
        return RE::BSResource::ErrorCode::kNone;
    }

private:
    /// Clone constructor — shares an existing buffer, fresh cursor at 0.
    explicit BSBMemoryStream(std::shared_ptr<const std::vector<std::uint8_t>> a_data)
        : RE::BSResource::Stream(static_cast<std::uint32_t>(a_data->size()))
        , m_data(std::move(a_data))
        , m_pos(0)
    {}

    std::shared_ptr<const std::vector<std::uint8_t>> m_data;
    mutable std::uint64_t                            m_pos;
};

}  // namespace BehaviorSwitchboard
