#include "../Common/SharedQueue.h"

#include <Windows.h>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{
    // ЗАМЕНИТЕ YOUR_SURNAME на свою фамилию латиницей.
    constexpr const char* kEnvironmentVariableName = "YOUR_SURNAME";
    constexpr std::uint32_t kMaxQueueSize = 10000;
    constexpr int kMaxSenderCount = 60;

    std::string WideToUtf8(const std::wstring& value)
    {
        if (value.empty()) return {};
        const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1,
            nullptr, 0, nullptr, nullptr);
        if (required <= 1) return {};
        std::string result(static_cast<std::size_t>(required), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1,
            result.data(), required, nullptr, nullptr);
        result.pop_back();
        return result;
    }

    bool ReadNumber(const std::string& prompt, int minValue, int maxValue, int& result)
    {
        while (true)
        {
            std::cout << prompt;
            std::string line;
            if (!std::getline(std::cin, line))
            {
                return false;
            }

            try
            {
                std::size_t used = 0;
                const int value = std::stoi(line, &used);
                if (used == line.size() && value >= minValue && value <= maxValue)
                {
                    result = value;
                    return true;
                }
            }
            catch (...)
            {
                // Ниже будет показано обычное сообщение об ошибке ввода.
            }

            std::cout << "Введите целое число от " << minValue
                << " до " << maxValue << ".\n";
        }
    }

    bool ReadFileName(std::string& fileName)
    {
        int choice = 0;
        std::cout << "Как получить имя бинарного файла?\n"
            << "1 - ввести имя с клавиатуры\n"
            << "2 - получить имя из переменной среды\n";

        if (!ReadNumber("Ваш выбор: ", 1, 2, choice))
        {
            return false;
        }

        if (choice == 1)
        {
            std::cout << "Введите имя файла (например, messages.bin): ";
            return static_cast<bool>(std::getline(std::cin, fileName)) && !fileName.empty();
        }

        char* environmentValue = nullptr;
        std::size_t valueSize = 0;
        const errno_t error = _dupenv_s(
            &environmentValue,
            &valueSize,
            kEnvironmentVariableName);
        (void)valueSize;

        if (error != 0 || environmentValue == nullptr || environmentValue[0] == '\0')
        {
            std::cout << "Переменная среды '" << kEnvironmentVariableName
                << "' не найдена или не содержит имя файла.\n";
            std::free(environmentValue);
            return false;
        }

        fileName = environmentValue;
        std::free(environmentValue); // Память, выделенную _dupenv_s, обязательно освобождаем.

        std::cout << "Имя файла получено из переменной среды: " << fileName << "\n";
        return true;
    }

    bool MakeFullPath(std::string& fileName)
    {
        char fullPath[MAX_PATH]{};
        const DWORD length = GetFullPathNameA(
            fileName.c_str(), MAX_PATH, fullPath, nullptr);

        if (length == 0 || length >= MAX_PATH)
        {
            std::cout << "Не удалось получить полный путь к файлу. Код ошибки: "
                << GetLastError() << "\n";
            return false;
        }

        fileName = fullPath;
        return true;
    }

    bool CreateMessageFile(const std::string& fileName, std::uint32_t capacity)
    {
        HANDLE file = CreateFileA(
            fileName.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (file == INVALID_HANDLE_VALUE)
        {
            std::cout << "Не удалось создать файл. Код ошибки: " << GetLastError() << "\n";
            return false;
        }

        const MessageQueue::QueueHeader header = MessageQueue::MakeInitialHeader(capacity);
        bool success = MessageQueue::WriteHeader(file, header);

        if (success)
        {
            const std::uint64_t fileSize = sizeof(MessageQueue::QueueHeader)
                + static_cast<std::uint64_t>(capacity) * sizeof(MessageQueue::MessageRecord);

            LARGE_INTEGER endPosition{};
            endPosition.QuadPart = static_cast<LONGLONG>(fileSize);
            success = SetFilePointerEx(file, endPosition, nullptr, FILE_BEGIN) != FALSE
                && SetEndOfFile(file) != FALSE
                && FlushFileBuffers(file) != FALSE;
        }

        CloseHandle(file);

        if (!success)
        {
            std::cout << "Не удалось подготовить бинарный файл. Код ошибки: "
                << GetLastError() << "\n";
        }
        return success;
    }

    std::string GetSenderExePath()
    {
        char receiverPath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameA(nullptr, receiverPath, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
        {
            return {};
        }

        std::string path(receiverPath);
        const std::size_t separator = path.find_last_of("\\/");
        if (separator == std::string::npos)
        {
            return "Sender.exe";
        }
        return path.substr(0, separator + 1) + "Sender.exe";
    }

    HANDLE CreateNamedObjects(
        const MessageQueue::ObjectNames& names,
        std::uint32_t capacity,
        int senderCount,
        HANDLE& freeSlots,
        HANDLE& usedSlots,
        HANDLE& readySemaphore,
        HANDLE& stopEvent)
    {
        HANDLE mutex = CreateMutexA(nullptr, FALSE, names.mutex.c_str());
        freeSlots = CreateSemaphoreA(nullptr, static_cast<LONG>(capacity),
            static_cast<LONG>(capacity), names.freeSlots.c_str());
        usedSlots = CreateSemaphoreA(nullptr, 0, static_cast<LONG>(capacity),
            names.usedSlots.c_str());
        readySemaphore = CreateSemaphoreA(nullptr, 0, senderCount, names.ready.c_str());
        stopEvent = CreateEventA(nullptr, TRUE, FALSE, names.stop.c_str());

        if (mutex == nullptr || freeSlots == nullptr || usedSlots == nullptr
            || readySemaphore == nullptr || stopEvent == nullptr)
        {
            std::cout << "Не удалось создать объекты синхронизации. Код ошибки: "
                << GetLastError() << "\n";
            if (mutex) CloseHandle(mutex);
            if (freeSlots) CloseHandle(freeSlots);
            if (usedSlots) CloseHandle(usedSlots);
            if (readySemaphore) CloseHandle(readySemaphore);
            if (stopEvent) CloseHandle(stopEvent);
            freeSlots = usedSlots = readySemaphore = stopEvent = nullptr;
            return nullptr;
        }

        return mutex;
    }

    bool StartSenders(
        const std::string& senderExe,
        const std::string& fileName,
        const std::string& prefix,
        int senderCount,
        std::vector<PROCESS_INFORMATION>& processes)
    {
        for (int i = 0; i < senderCount; ++i)
        {
            STARTUPINFOA startupInfo{};
            startupInfo.cb = sizeof(startupInfo);
            PROCESS_INFORMATION processInfo{};

            std::string commandLine = "\"" + senderExe + "\" \""
                + fileName + "\" \"" + prefix + "\"";
            std::vector<char> writableCommand(commandLine.begin(), commandLine.end());
            writableCommand.push_back('\0');

            const BOOL created = CreateProcessA(
                senderExe.c_str(),
                writableCommand.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NEW_CONSOLE,
                nullptr,
                nullptr,
                &startupInfo,
                &processInfo);

            if (!created)
            {
                std::cout << "Не удалось запустить Sender.exe. Код ошибки: "
                    << GetLastError() << "\n";
                return false;
            }

            CloseHandle(processInfo.hThread);
            processes.push_back(processInfo);
        }
        return true;
    }

    void StopAndCloseProcesses(
        std::vector<PROCESS_INFORMATION>& processes,
        bool terminate)
    {
        for (PROCESS_INFORMATION& process : processes)
        {
            if (terminate && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT)
            {
                TerminateProcess(process.hProcess, 1);
            }
            WaitForSingleObject(process.hProcess, INFINITE);
            CloseHandle(process.hProcess);
        }
        processes.clear();
    }

    bool ReadMessage(
        HANDLE file,
        HANDLE mutex,
        HANDLE freeSlots,
        HANDLE usedSlots,
        MessageQueue::MessageRecord& result)
    {
        // Если очередь пуста, Receiver будет ждать, пока Sender не положит сообщение.
        const DWORD usedWait = WaitForSingleObject(usedSlots, INFINITE);
        if (usedWait != WAIT_OBJECT_0)
        {
            std::cout << "Ошибка ожидания сообщения. Код: " << GetLastError() << "\n";
            return false;
        }

        const DWORD mutexWait = WaitForSingleObject(mutex, INFINITE);
        if (!MessageQueue::IsMutexAcquired(mutexWait))
        {
            ReleaseSemaphore(usedSlots, 1, nullptr);
            std::cout << "Не удалось захватить mutex. Код: " << GetLastError() << "\n";
            return false;
        }

        MessageQueue::QueueHeader header{};
        bool success = MessageQueue::ReadHeader(file, header) && header.count > 0;

        if (success)
        {
            success = MessageQueue::ReadRecord(file, header.head, result);
            if (success)
            {
                result.text[MessageQueue::kMessageLength - 1] = L'\0';
                header.head = MessageQueue::NextIndex(header.head, header.capacity);
                --header.count;
                success = MessageQueue::WriteHeader(file, header);
            }
        }

        ReleaseMutex(mutex);

        if (success)
        {
            ReleaseSemaphore(freeSlots, 1, nullptr);
        }
        else
        {
            // Возвращаем занятое место в семафор сообщений, чтобы не потерять сигнал.
            ReleaseSemaphore(usedSlots, 1, nullptr);
            std::cout << "Ошибка чтения очереди из бинарного файла.\n";
        }
        return success;
    }
}

int main()
{
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    std::cout << "=== Receiver: прием сообщений из общего бинарного файла ===\n";

    std::string fileName;
    if (!ReadFileName(fileName) || !MakeFullPath(fileName))
    {
        return 1;
    }

    int capacityInput = 0;
    if (!ReadNumber("Введите количество записей в бинарном файле: ",
        1, static_cast<int>(kMaxQueueSize), capacityInput))
    {
        return 1;
    }
    const auto capacity = static_cast<std::uint32_t>(capacityInput);

    if (!CreateMessageFile(fileName, capacity))
    {
        return 1;
    }
    HANDLE queueFile = CreateFileA(fileName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (queueFile == INVALID_HANDLE_VALUE)
    {
        std::cout << "Не удалось повторно открыть файл очереди. Код ошибки: "
            << GetLastError() << "\n";
        return 1;
    }
    std::cout << "Файл очереди создан: " << fileName << "\n";

    int senderCount = 0;
    if (!ReadNumber("Сколько процессов Sender запустить (1-60): ",
        1, kMaxSenderCount, senderCount))
    {
        CloseHandle(queueFile);
        return 1;
    }

    const std::string prefix = "Local\\Lab4Queue_"
        + std::to_string(GetCurrentProcessId()) + "_"
        + std::to_string(GetTickCount64());
    const MessageQueue::ObjectNames names = MessageQueue::MakeObjectNames(prefix);

    HANDLE freeSlots = nullptr;
    HANDLE usedSlots = nullptr;
    HANDLE readySemaphore = nullptr;
    HANDLE stopEvent = nullptr;
    HANDLE mutex = CreateNamedObjects(names, capacity, senderCount,
        freeSlots, usedSlots, readySemaphore, stopEvent);

    if (mutex == nullptr)
    {
        CloseHandle(queueFile);
        return 1;
    }

    const std::string senderExe = GetSenderExePath();
    if (senderExe.empty())
    {
        std::cout << "Не удалось определить путь к Sender.exe.\n";
        SetEvent(stopEvent);
        CloseHandle(mutex);
        CloseHandle(freeSlots);
        CloseHandle(usedSlots);
        CloseHandle(readySemaphore);
        CloseHandle(stopEvent);
        CloseHandle(queueFile);
        return 1;
    }

    std::vector<PROCESS_INFORMATION> processes;
    if (!StartSenders(senderExe, fileName, prefix, senderCount, processes))
    {
        SetEvent(stopEvent);
        StopAndCloseProcesses(processes, true);
        CloseHandle(mutex);
        CloseHandle(freeSlots);
        CloseHandle(usedSlots);
        CloseHandle(readySemaphore);
        CloseHandle(stopEvent);
        CloseHandle(queueFile);
        return 1;
    }

    std::cout << "Ожидание готовности всех Sender...\n";
    bool allReady = true;
    for (int i = 0; i < senderCount; ++i)
    {
        const DWORD waitResult = WaitForSingleObject(readySemaphore, 30000);
        if (waitResult != WAIT_OBJECT_0)
        {
            allReady = false;
            break;
        }
    }

    if (!allReady)
    {
        std::cout << "Не все Sender сообщили о готовности. Проверьте запуск Sender.exe.\n";
        SetEvent(stopEvent);
        StopAndCloseProcesses(processes, true);
        CloseHandle(mutex);
        CloseHandle(freeSlots);
        CloseHandle(usedSlots);
        CloseHandle(readySemaphore);
        CloseHandle(stopEvent);
        CloseHandle(queueFile);
        return 1;
    }

    std::cout << "Все процессы Sender готовы.\n";

    bool receiverRunning = true;
    while (receiverRunning)
    {
        std::cout << "\n1 - прочитать сообщение\n0 - завершить Receiver\n";
        std::string command;
        std::cout << "Команда: ";
        if (!std::getline(std::cin, command))
        {
            break;
        }

        if (command == "0")
        {
            receiverRunning = false;
        }
        else if (command == "1")
        {
            MessageQueue::MessageRecord message{};
            if (ReadMessage(queueFile, mutex, freeSlots, usedSlots, message))
            {
                std::cout << "Получено сообщение от процесса PID "
                    << message.senderProcessId << ": "
                    << WideToUtf8(message.text) << "\n";
            }
        }
        else
        {
            std::cout << "Неизвестная команда. Введите 1 или 0.\n";
        }
    }

    // Сообщаем Sender, что Receiver завершает работу.
    SetEvent(stopEvent);
    std::cout << "\nReceiver завершает работу. В окнах Sender, которые ждут ввода, "
        << "нажмите 0 (или закройте окно). Ожидаю завершения процессов...\n";
    StopAndCloseProcesses(processes, false);

    CloseHandle(mutex);
    CloseHandle(freeSlots);
    CloseHandle(usedSlots);
    CloseHandle(readySemaphore);
    CloseHandle(stopEvent);
    CloseHandle(queueFile);

    std::cout << "Все процессы Sender завершены. Receiver закончил работу.\n";
    return 0;
}
