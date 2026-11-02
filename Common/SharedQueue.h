#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace MessageQueue
{
    constexpr char kFileMagic[8] = { 'M', 'Q', 'U', 'E', 'U', 'E', '1', '\0' };
    constexpr std::uint32_t kFileVersion = 1;
    constexpr std::size_t kMessageLength = 20; // Буфер на 20 символов, включая '\0'.

    struct QueueHeader
    {
        char magic[8];
        std::uint32_t version;
        std::uint32_t capacity;
        std::uint32_t head;
        std::uint32_t tail;
        std::uint32_t count;
    };

    struct MessageRecord
    {
        wchar_t text[kMessageLength];
        DWORD senderProcessId;
    };

    struct ObjectNames
    {
        std::string mutex;
        std::string freeSlots;
        std::string usedSlots;
        std::string ready;
        std::string stop;
    };

    inline QueueHeader MakeInitialHeader(std::uint32_t capacity)
    {
        QueueHeader header{};
        std::memcpy(header.magic, kFileMagic, sizeof(kFileMagic));
        header.version = kFileVersion;
        header.capacity = capacity;
        header.head = 0;
        header.tail = 0;
        header.count = 0;
        return header;
    }

    inline bool IsValidHeader(const QueueHeader& header)
    {
        return std::memcmp(header.magic, kFileMagic, sizeof(kFileMagic)) == 0
            && header.version == kFileVersion
            && header.capacity > 0
            && header.head < header.capacity
            && header.tail < header.capacity
            && header.count <= header.capacity;
    }

    inline bool IsValidMessage(const std::wstring& message)
    {
        return !message.empty()
            && message.size() < kMessageLength
            && message.find(L'\0') == std::wstring::npos;
    }

    inline std::uint32_t NextIndex(std::uint32_t index, std::uint32_t capacity)
    {
        if (capacity == 0 || index >= capacity - 1)
        {
            return 0;
        }
        return index + 1;
    }

    inline ObjectNames MakeObjectNames(const std::string& prefix)
    {
        ObjectNames names;
        names.mutex = prefix + "_mutex";
        names.freeSlots = prefix + "_free";
        names.usedSlots = prefix + "_used";
        names.ready = prefix + "_ready";
        names.stop = prefix + "_stop";
        return names;
    }

    inline bool ReadAt(HANDLE file, std::uint64_t offset, void* data, DWORD size)
    {
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);

        if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
        {
            return false;
        }

        DWORD bytesRead = 0;
        return ReadFile(file, data, size, &bytesRead, nullptr) != FALSE
            && bytesRead == size;
    }

    inline bool WriteAt(HANDLE file, std::uint64_t offset, const void* data, DWORD size)
    {
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);

        if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
        {
            return false;
        }

        DWORD bytesWritten = 0;
        return WriteFile(file, data, size, &bytesWritten, nullptr) != FALSE
            && bytesWritten == size;
    }

    inline bool ReadHeader(HANDLE file, QueueHeader& header)
    {
        return ReadAt(file, 0, &header, static_cast<DWORD>(sizeof(header)))
            && IsValidHeader(header);
    }

    inline bool WriteHeader(HANDLE file, const QueueHeader& header)
    {
        return WriteAt(file, 0, &header, static_cast<DWORD>(sizeof(header)));
    }

    inline std::uint64_t RecordOffset(std::uint32_t index)
    {
        return sizeof(QueueHeader)
            + static_cast<std::uint64_t>(index) * sizeof(MessageRecord);
    }

    inline bool ReadRecord(HANDLE file, std::uint32_t index, MessageRecord& record)
    {
        return ReadAt(file, RecordOffset(index), &record,
            static_cast<DWORD>(sizeof(record)));
    }

    inline bool WriteRecord(HANDLE file, std::uint32_t index, const MessageRecord& record)
    {
        return WriteAt(file, RecordOffset(index), &record,
            static_cast<DWORD>(sizeof(record)));
    }

    inline bool IsMutexAcquired(DWORD waitResult)
    {
        return waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED;
    }
}
