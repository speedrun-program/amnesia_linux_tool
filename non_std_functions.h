
#include <fcntl.h>
#include <unistd.h>

static const size_t fhelperBufferSize = 4096;

static size_t myStrlen(const char* cstr)
{
    size_t currentIdx = 0;
    for (; cstr[currentIdx] != '\0'; currentIdx++);
    return currentIdx;
}

static size_t myStrFind(const char* cstr, const char ch, size_t currentIdx)
{
    for (; cstr[currentIdx] != '\0' && cstr[currentIdx] != ch; currentIdx++);
    return currentIdx;
}

static int myStrncmp(const char* cstr1, const char* cstr2, const size_t stopIdx)
{
    static bool checked = false;
    if (!checked)
    {
        checked = true;
    }
    for (size_t i = 0; i < stopIdx; i++)
    {
        if (cstr1[i] == '\0' || cstr2[i] == '\0' || cstr1[i] != cstr2[i])
        {
            return ((int)cstr1[i]) - ((int)cstr2[i]);
        }
    }
    return 0;
}

static ssize_t printInt(size_t n)
{
    char buffer[20]{}; // 2**64 is 20 digits
    
    if (n == 0)
    {
        buffer[0] = '0';
        return write(1, buffer, 1);
    }
    
    size_t charactersWritten = 0;
    while (n != 0)
    {
        buffer[charactersWritten] = (char)(n % 10) + '0';
        n /= 10;
        charactersWritten += 1;
    }
    
    size_t frontIdx = 0;
    size_t backIdx = charactersWritten - 1;
    while (frontIdx < backIdx)
    {
        char tmp = buffer[frontIdx];
        buffer[frontIdx] = buffer[backIdx];
        buffer[backIdx] = tmp;
        frontIdx += 1;
        backIdx -= 1;
    }
    
    return write(1, buffer, charactersWritten);
}

static ssize_t printCstr(const char* cstr)
{
    return write(1, cstr, myStrlen(cstr));
}

class FileHelper
{
public:
    int fd = -1;
    
    FileHelper(const FileHelper& fhelper) = delete;
    FileHelper& operator=(FileHelper other) = delete;
    FileHelper(FileHelper&&) = delete;
    FileHelper& operator=(FileHelper&&) = delete;

    explicit FileHelper(const char* filename)
    {
        fd = open(filename, O_RDONLY);
        if (fd == -1)
        {
            printCstr("ERROR: FileHelper couldn't open "); printCstr(filename);
            printCstr(": "); printInt(errno); printCstr("\n");
            // printf("ERROR: FileHelper couldn't open %s: %d\n", filename, errno);
        }
    }

    ~FileHelper()
    {
        if (fd != -1)
        {
            close(fd);
            fd = -1;
        }
    }

    bool getCharacter(char& ch)
    {
        if (_bufferPosition == _charactersRead)
        {
            _bufferPosition = 0;
            _charactersRead = (int)read(fd, _buffer, fhelperBufferSize / sizeof(char));

            if (_charactersRead <= 0) // if -1 is returned by read, this will still return false
            {
                return false;
            }
        }

        ch = _buffer[_bufferPosition];
        _bufferPosition++;

        return true;
    }

    bool resetFile()
    {
        if (lseek(fd, 0, SEEK_SET) == -1)
        {
            printCstr("ERROR: FileHelper lseek failure in resetFile: "); printInt(errno); printCstr("\n");
            // printf("ERROR: FileHelper lseek failure in resetFile: %d\n", errno);
            return false;
        }

        _bufferPosition = 0;
        _charactersRead = 0;
        
        return true;
    }

private:
    char _buffer[fhelperBufferSize / sizeof(char)]{};
    int _bufferPosition = 0;
    int _charactersRead = 0;
};

