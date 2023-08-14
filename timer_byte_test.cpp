
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dirent.h>
#include <cstring>
#include <cstdio>
#include <errno.h>
#include <string>
#include <atomic>
#include <stdexcept>

bool findPid(pid_t& pid, std::string& pidString, const char** gameNames, size_t howManyGameNames)
{
    char pathBuffer[64]{};
    char fileTextBuffer[256]{};
    FILE* f = nullptr;
    
    struct dirent *directoryEntry;
    DIR* procDirectory = opendir("/proc"); // make sure this gets closed
    if (procDirectory == nullptr)
    {
        printf("error opening proc directory: %d", errno);
        return false;
    }
    
    bool foundPID = false;
    while ((directoryEntry = readdir(procDirectory)) && !foundPID)
    {
        size_t truncationCheck = snprintf(pathBuffer, sizeof(pathBuffer), "/proc/%s/cmdline", directoryEntry->d_name);
        if (truncationCheck >= sizeof(pathBuffer))
        {
            continue;
        }
        
        f = fopen(pathBuffer, "r"); // make sure this also gets closed
        if (!f)
        {
            errno = 0;
            continue;
        }
        
        size_t bytesRead = fread(fileTextBuffer, sizeof(char), sizeof(fileTextBuffer), f);
        size_t filenameIndex = 0;
        size_t lastSlashIndex = 0;
        for (; fileTextBuffer[filenameIndex] != '\0' && filenameIndex < bytesRead; filenameIndex++)
        {
            if (fileTextBuffer[filenameIndex] == '/')
            {
                lastSlashIndex = filenameIndex;
            }
        }
        if (filenameIndex != bytesRead && lastSlashIndex != 0)
        {
            for (int i = 0; i < howManyGameNames; i++)
            {
                if (strcmp(gameNames[i], fileTextBuffer + lastSlashIndex) == 0)
                {
                    pidString = directoryEntry->d_name;
                    pid = stol(pidString);
                    foundPID = true;
                    break;
                }
            }
        }
        
        fclose(f); // file closed here
    }
    
    closedir(procDirectory); // directory closed here
    
    if (!foundPID)
    {
        printf("Couldn't find game PID\n");
        return false;
    }
    
    return true;
}

static bool getMemfdName(char* memfdName, const size_t memfdNameBufferSize, size_t& linkFirstPartSize)
{
    const int memfdNameMaxSize = 249; // "The limit is 249 bytes, excluding the terminating null byte."
    const char fileWithMemfdName[] = "shared_memory_name.txt";
    
    FILE* f = fopen(fileWithMemfdName, "rb"); // make sure this gets closed
    if (!f)
    {
        printf("fopen error when opening %s: %d\n", fileWithMemfdName, errno);
        return false;
    }
    
    int charactersRead = fread(&memfdName[0], 1, memfdNameBufferSize, f);
    fclose(f); // file closed here
    f = nullptr;
    if (charactersRead == memfdNameBufferSize)
    {
        printf("%s should be shorter than %zu bytes\n", fileWithMemfdName, memfdNameBufferSize);
        return false;
    }
    
    // removing non-alphanumeric characters and characters which aren't dashes or underscores
    int write_idx = 0;
    for (int i = 0; memfdName[i] != '\0'; i++)
    {
        memfdName[write_idx] = memfdName[i];
        write_idx += (
            (memfdName[i] >= '0' && memfdName[i] <= '9')
            || (memfdName[i] >= 'a' && memfdName[i] <= 'z')
            || (memfdName[i] >= 'A' && memfdName[i] <= 'Z')
            || memfdName[i] == '-'
            || memfdName[i] == '_'
        );
    }
    memfdName[write_idx] = '\0';
    linkFirstPartSize = write_idx + 7; // + 7 for "/memfd:" start text
    if (write_idx == 0)
    {
        printf("empty shared memory name\n");
        return false;
    }
    else if (write_idx > memfdNameMaxSize)
    {
        printf("shared memory name needs to be %d characters or less\n", memfdNameMaxSize);
        return false;
    }
    
    return true;
}

bool findMemFile(std::string& pathString)
{
    size_t linkFirstPartSize = 0;
    char linkFirstPart[327] = "/memfd:"; // 320, plus 7 for "/memfd:" start text
    if (!getMemfdName(&linkFirstPart[7], sizeof(linkFirstPart) - 7, linkFirstPartSize)) // starting at index 7, ahead of "/memfd:" start text
    {
        return false;
    }
    char linkSecondPart[] = " (deleted)";
    
    std::string pathStringCopy = pathString;
    char symlinkBuffer[7 + 249 + 10 + 1]{}; // "/memfd:" + maximum allowed memfd name size + " (deleted)" + null terminator
    size_t pathStringOriginalSize = pathString.size();
    
    struct dirent *directoryEntry;
    DIR* fhDirectory = opendir(pathString.c_str()); // make sure this gets closed
    if (fhDirectory == nullptr)
    {
        printf("error opening proc directory: %d", errno);
        return false;
    }
    
    bool foundfhCount = 0;
    while ((directoryEntry = readdir(fhDirectory)))
    {
        pathStringCopy += directoryEntry->d_name;
        ssize_t readlinkBytesRead = readlink(pathStringCopy.c_str(), symlinkBuffer, sizeof(symlinkBuffer));
        if (readlinkBytesRead == sizeof(symlinkBuffer))
        {
            pathStringCopy.resize(pathStringOriginalSize);
            continue;
        }
        else if (readlinkBytesRead == -1)
        {
            errno = 0;
            pathStringCopy.resize(pathStringOriginalSize);
            continue;
        }
        symlinkBuffer[readlinkBytesRead] = '\0';
        
        if (
            (strncmp(linkFirstPart, symlinkBuffer, linkFirstPartSize) == 0)
            && (
                symlinkBuffer[linkFirstPartSize] == '\0'
                || strcmp(linkSecondPart, &symlinkBuffer[linkFirstPartSize]) == 0
            )
        )
        {
            pathString = pathStringCopy;
            foundfhCount += 1;
        }
        
        pathStringCopy.resize(pathStringOriginalSize);
    }
    
    closedir(fhDirectory); // directory closed here
    
    if (foundfhCount == 0)
    {
        printf("Couldn't find memfd_create file handle\n");
        return false;
    }
    else if (foundfhCount > 1)
    {
        printf("memfd_create file name already being used by the process. Choose a different name and retry\n");
        return false;
    }
    
    return true;
}

bool getResources(int& fd, void*& mmapAddress, bool& mlockSucceeded)
{
    try
    {
        const char* gameNames[4] = {
            "/Amnesia_NOSTEAM.bin.x86_64",
            "/Amnesia.bin.x86_64",
            "/Amnesia_NOSTEAM.bin.x86",
            "/Amnesia.bin.x86"
        };
        
        pid_t pid = 0;
        std::string pidString;
        if (!findPid(pid, pidString, gameNames, sizeof(gameNames) / sizeof(char*)))
        {
            return false;
        }
        
        std::string pathString = "/proc/";
        pathString += pidString;
        pathString += "/fd/";
        
        if (!findMemFile(pathString))
        {
            return false;
        }
        
        fd = open(pathString.c_str(), O_RDONLY);
        if (fd == -1)
        {
            printf("open failure: %d\n", errno);
            return false;
        }
        
        mmapAddress = mmap(nullptr, 1, PROT_READ, MAP_SHARED, fd, 0);
        if (mmapAddress == MAP_FAILED)
        {
            printf("mmap failure: %d\n", errno);
            return false;
        }
            
        if (mlock(mmapAddress, 1) == -1)
        {
            printf("mlock failure: %d\n", errno);
            return false;
        }
        mlockSucceeded = true;
    }
    catch (const std::runtime_error& e)
    {
        char const* fixC4101Warning = e.what();
        printf("unexpected error: %s\n", fixC4101Warning);
        
        return false;
    }
    
    return true;
}

void freeResources(int& fd, void*& mmapAddress, bool& mlockSucceeded)
{
    if (fd != -1)
    {
        close(fd);
        fd = -1;
    }
    if (mlockSucceeded)
    {
        munlock(mmapAddress, 1);
        mlockSucceeded = false;
    }
    if (mmapAddress != MAP_FAILED)
    {
        munmap(mmapAddress, 1);
        mmapAddress = MAP_FAILED;
    }
}

int main()
{
    int fd = -1;
    void* mmapAddress = MAP_FAILED;
    bool mlockSucceeded = false;
    bool getResourcesSucceeded = getResources(fd, mmapAddress, mlockSucceeded); // remember to release resources from this function
    if (getResourcesSucceeded)
    {
        std::atomic_ref<unsigned char> timerByteAtomicRef(*((unsigned char*)mmapAddress));
        unsigned char timerByteCurrentValue = 0;
        unsigned char timerBytePreviousValue = 0;
        
        struct timespec sleepTime = {0, 1000000}; // one millisecond
        
        printf("load detection ready.\n");
        
        while (true)
        {
            nanosleep(&sleepTime, nullptr);
            timerByteCurrentValue = timerByteAtomicRef.load();
            
            if (timerByteCurrentValue != timerBytePreviousValue)
            {
                timerBytePreviousValue = timerByteCurrentValue;
                
                if (timerByteCurrentValue == 0)
                {
                    printf("resume timer\n");
                }
                else if (timerByteCurrentValue == 1)
                {
                    printf("pause timer without splitting\n");
                }
                else if (timerByteCurrentValue == 2)
                {
                    printf("pause timer and split\n");
                }
                else if (timerByteCurrentValue == 255)
                {
                    printf("finished\n");
                    break;
                }
            }
        }
    }
    
    freeResources(fd, mmapAddress, mlockSucceeded); // resources from getResources released here
    
    return getResourcesSucceeded ? EXIT_SUCCESS : EXIT_FAILURE;
}

