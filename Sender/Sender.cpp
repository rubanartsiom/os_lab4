#include "../Common/SharedQueue.h"

#include <Windows.h>

#include <cstring>
#include <cwchar>
#include <iostream>
#include <string>

namespace
{
    struct SenderHandles
    {
        HANDLE file = INVALID_HANDLE_VALUE;
        HANDLE mutex = nullptr;
        HANDLE freeSlots = nullptr;
        HANDLE usedSlots = nullptr;
        HANDLE readySemaphore = nullptr;
        HANDLE stopEvent = nullptr;
    };

    bool OpenSenderObjects(const std::string& fileName,
        const MessageQueue::ObjectNames& names, SenderHandles& handles)
    {
        handles.file = CreateFileA(fileName.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (handles.file == INVALID_HANDLE_VALUE)
        {
            std::wcout << L"Sender: не удалось открыть файл. Код ошибки: "
                << GetLastError() << L"\n";
            return false;
        }

        MessageQueue::QueueHeader header{};
        LARGE_INTEGER fileSize{};
        if (!MessageQueue::ReadHeader(handles.file, header)
            || !GetFileSizeEx(handles.file, &fileSize))
        {
            std::wcout << L"Sender: файл имеет неверный формат.\n";
            return false;
        }

        const std::uint64_t expectedSize = sizeof(MessageQueue::QueueHeader)
            + static_cast<std::uint64_t>(header.capacity) * sizeof(MessageQueue::MessageRecord);
        if (static_cast<std::uint64_t>(fileSize.QuadPart) < expectedSize)
        {
            std::wcout << L"Sender: размер файла меньше ожидаемого.\n";
            return false;
        }

        handles.mutex = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
            names.mutex.c_str());
        handles.freeSlots = OpenSemaphoreA(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE,
            FALSE, names.freeSlots.c_str());
        handles.usedSlots = OpenSemaphoreA(SEMAPHORE_MODIFY_STATE,
            FALSE, names.usedSlots.c_str());
        handles.readySemaphore = OpenSemaphoreA(SEMAPHORE_MODIFY_STATE,
            FALSE, names.ready.c_str());
        handles.stopEvent = OpenEventA(SYNCHRONIZE, FALSE, names.stop.c_str());

        if (handles.mutex == nullptr || handles.freeSlots == nullptr
            || handles.usedSlots == nullptr || handles.readySemaphore == nullptr
            || handles.stopEvent == nullptr)
        {
            std::wcout << L"Sender: не удалось открыть объекты синхронизации. Код ошибки: "
                << GetLastError() << L"\n";
            return false;
        }

        return true;
    }

    void CloseSenderObjects(SenderHandles& handles)
    {
        if (handles.file != INVALID_HANDLE_VALUE) CloseHandle(handles.file);
        if (handles.mutex) CloseHandle(handles.mutex);
        if (handles.freeSlots) CloseHandle(handles.freeSlots);
        if (handles.usedSlots) CloseHandle(handles.usedSlots);
        if (handles.readySemaphore) CloseHandle(handles.readySemaphore);
        if (handles.stopEvent) CloseHandle(handles.stopEvent);
    }

    bool SendMessage(SenderHandles& handles, const std::wstring& message)
    {
        HANDLE waitObjects[2] = { handles.freeSlots, handles.stopEvent };
        const DWORD waitResult = WaitForMultipleObjects(2, waitObjects, FALSE, INFINITE);

        if (waitResult == WAIT_OBJECT_0 + 1)
        {
            std::wcout << L"Receiver завершает работу. Сообщение не отправлено.\n";
            return false;
        }
        if (waitResult != WAIT_OBJECT_0)
        {
            std::wcout << L"Sender: ошибка ожидания свободной записи. Код: "
                << GetLastError() << L"\n";
            return false;
        }

        // Receiver мог завершиться сразу после того, как свободная запись была получена.
        if (WaitForSingleObject(handles.stopEvent, 0) == WAIT_OBJECT_0)
        {
            ReleaseSemaphore(handles.freeSlots, 1, nullptr);
            return false;
        }

        const DWORD mutexWait = WaitForSingleObject(handles.mutex, INFINITE);
        if (!MessageQueue::IsMutexAcquired(mutexWait))
        {
            ReleaseSemaphore(handles.freeSlots, 1, nullptr);
            std::wcout << L"Sender: не удалось захватить mutex. Код: " << GetLastError() << L"\n";
            return false;
        }

        MessageQueue::QueueHeader header{};
        bool success = MessageQueue::ReadHeader(handles.file, header)
            && header.count < header.capacity;

        if (success)
        {
            MessageQueue::MessageRecord record{};
            std::wmemcpy(record.text, message.c_str(), message.size());
            record.text[message.size()] = L'\0';
            record.senderProcessId = GetCurrentProcessId();

            success = MessageQueue::WriteRecord(handles.file, header.tail, record);
            if (success)
            {
                header.tail = MessageQueue::NextIndex(header.tail, header.capacity);
                ++header.count;
                success = MessageQueue::WriteHeader(handles.file, header);
            }
        }

        ReleaseMutex(handles.mutex);

        if (success)
        {
            if (!ReleaseSemaphore(handles.usedSlots, 1, nullptr))
            {
                std::wcout << L"Sender: не удалось отправить сигнал о новом сообщении.\n";
                return false;
            }
            std::wcout << L"Сообщение отправлено.\n";
        }
        else
        {
            ReleaseSemaphore(handles.freeSlots, 1, nullptr);
            std::wcout << L"Sender: не удалось записать сообщение в файл.\n";
        }
        return success;
    }
}

int main(int argc, char* argv[])
{
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    if (argc < 3)
    {
        std::wcout << L"Запуск: Sender.exe <имя_файла> <имя_группы_объектов>\n";
        return 1;
    }

    const std::string fileName = argv[1];
    const std::string prefix = argv[2];
    const MessageQueue::ObjectNames names = MessageQueue::MakeObjectNames(prefix);

    SenderHandles handles;
    if (!OpenSenderObjects(fileName, names, handles))
    {
        CloseSenderObjects(handles);
        return 1;
    }

    if (!ReleaseSemaphore(handles.readySemaphore, 1, nullptr))
    {
        std::wcout << L"Sender: не удалось сообщить Receiver о готовности.\n";
        CloseSenderObjects(handles);
        return 1;
    }

    std::wcout << L"=== Sender (PID " << GetCurrentProcessId() << L") ===\n"
        << L"Готов к работе. Сообщения короче 20 символов.\n";

    while (true)
    {
        if (WaitForSingleObject(handles.stopEvent, 0) == WAIT_OBJECT_0)
        {
            std::wcout << L"Получен сигнал завершения от Receiver.\n";
            break;
        }

        std::wcout << L"\n1 - отправить сообщение\n0 - завершить Sender\nКоманда: ";
        std::wstring command;
        if (!std::getline(std::wcin, command))
        {
            break;
        }

        if (WaitForSingleObject(handles.stopEvent, 0) == WAIT_OBJECT_0)
        {
            std::wcout << L"Receiver завершает работу. Sender закрывается.\n";
            break;
        }

        if (command == L"0")
        {
            break;
        }
        if (command != L"1")
        {
            std::wcout << L"Введите 1 или 0.\n";
            continue;
        }

        std::wcout << L"Введите сообщение (от 1 до 19 символов): ";
        std::wstring message;
        if (!std::getline(std::wcin, message))
        {
            break;
        }

        if (!MessageQueue::IsValidMessage(message))
        {
            std::wcout << L"Ошибка: сообщение должно содержать от 1 до 19 символов.\n";
            continue;
        }

        SendMessage(handles, message);
    }

    CloseSenderObjects(handles);
    std::wcout << L"Sender завершил работу.\n";
    return 0;
}
