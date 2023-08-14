
#include <cstdio>
#include <climits>
#include <mutex>
#include <thread>
#include <vector>
#include <memory>
#include <cstring>
#include <charconv>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>

static auto originalFopen = reinterpret_cast<FILE * (*)(const char* path, const char* mode)>(dlsym(RTLD_NEXT, "fopen"));
static auto originalFreopen = reinterpret_cast<FILE * (*)(const char* path, const char* mode, FILE * stream)>(dlsym(RTLD_NEXT, "freopen"));
static auto originalFopen64 = reinterpret_cast<FILE * (*)(const char* path, const char* mode)>(dlsym(RTLD_NEXT, "fopen64"));
static auto originalFreopen64 = reinterpret_cast<FILE * (*)(const char* path, const char* mode, FILE * stream)>(dlsym(RTLD_NEXT, "freopen64"));

static bool delaysActive = false;
static const char delaysFileName[] = "files_and_delays.txt";

struct MapValue
{
    // delays[0] == -1 says to reset all MapValue.position to 0
    // delays ending with -1 says to reset at the end
    // delays ending with -2 says to NOT reset at the end
    std::unique_ptr<int[]> delays;
    size_t position = 0;
    size_t fullResetCheckNumber = 0;

    explicit MapValue(std::vector<int>& delaysVector) : delays(std::make_unique<int[]>(delaysVector.size()))
    {
        memcpy(delays.get(), delaysVector.data(), delaysVector.size() * sizeof(int));
    }
};

struct KeyCmp
{
    using is_transparent = void;

    bool operator()(const std::unique_ptr<char[]>& cstr1, const std::unique_ptr<char[]>& cstr2) const
    {
        return strcmp(cstr1.get(), cstr2.get()) == 0;
    }

    bool operator()(const std::unique_ptr<char[]>& cstr, const std::string_view sv) const
    {
        return strcmp(cstr.get(), sv.data()) == 0;
    }

    bool operator()(const std::string_view sv, const std::unique_ptr<char[]>& cstr) const
    {
        return strcmp(sv.data(), cstr.get()) == 0;
    }
};

struct KeyHash
{
    using is_transparent = void;

    size_t operator()(const std::string_view sv) const
    {
        return _hashObject(sv);
    }

    size_t operator()(const std::unique_ptr<char[]>& cstr) const
    {
        // if they add an easy way to do it, change this so it doesn't need to find the c-string length
        return _hashObject(std::string_view(cstr.get()));
    }

private:
    std::hash<std::string_view> _hashObject = std::hash<std::string_view>();
};

using myMapType = std::unordered_map<std::unique_ptr<char[]>, MapValue, KeyHash, KeyCmp>;

class MapAndMutex
{
public:
    std::mutex mutexForMap;
    myMapType fileMap;
    
    MapAndMutex()
    {
        try
        {
            bool utf16WarningWritten = false;

            // intAsChars used in fillDelaysVector but made here so it doesn't need to be remade repeatedly
            std::vector<char> intAsChars;
            intAsChars.reserve(10);
            intAsChars.push_back('0'); // empty vector causes std::errc::invalid_argument
            std::vector<char> keyVector;
            std::vector<int> delaysVector;

            FileHelper fhelper(delaysFileName);
            if (fhelper.fd == -1) // error message will have been printed in the constructor
            {
                return;
            }

            while (addMapPair(fileMap, keyVector, delaysVector, fhelper, intAsChars));
        }
        catch (const std::runtime_error& e)
        {
            char const* fixC4101Warning = e.what();
            printf("%s\nfiles can't be load extended\n", fixC4101Warning);
            fileMap.clear(); // clear map so failure is more obvious
        }
    }

    bool addMapPair(myMapType& fileMap, std::vector<char>& keyVector, std::vector<int>& delaysVector, FileHelper& fhelper, std::vector<char>& intAsChars)
    {
        keyVector.clear();
        delaysVector.clear();
        char ch = '\0';
        bool stripWhitespace = false;
        bool textRemaining = fhelper.getCharacter(ch);

        if (ch == '/')
        {
            stripWhitespace = true;
            textRemaining = fhelper.getCharacter(ch);
        }
        else if (ch == ' ' || ch == '\f' || ch == '\r' || ch == '\t' || ch == '\v')
        {
            // don't include starting whitespace
            for (
                textRemaining = fhelper.getCharacter(ch);
                textRemaining && (ch == ' ' || ch == '\f' || ch == '\r' || ch == '\t' || ch == '\v');
                textRemaining = fhelper.getCharacter(ch));
        }

        while (ch != '\n' && ch != '/' && textRemaining)
        {
            keyVector.push_back(ch);
            textRemaining = fhelper.getCharacter(ch);
        }

        // don't include ending whitespace
        if (!stripWhitespace)
        {
            while (!keyVector.empty())
            {
                char endChar = keyVector.back();

                if (endChar == ' ' || endChar == '\f' || endChar == '\r' || endChar == '\t' || endChar == '\v')
                {
                    keyVector.pop_back();
                }
                else
                {
                    break;
                }
            }
        }

        if (textRemaining && ch == '/') // line didn't end abruptly
        {
            fillDelaysVector(textRemaining, delaysVector, fhelper, intAsChars);
            
            if (!keyVector.empty() && !delaysVector.empty())
            {
                if (delaysVector.back() != -1)
                {
                    delaysVector.push_back(-2);
                }
                
                keyVector.push_back('\0');
                std::unique_ptr<char[]> keyPtr = std::make_unique<char[]>(keyVector.size());
                memcpy(keyPtr.get(), keyVector.data(), keyVector.size() * sizeof(char));
                fileMap.emplace(std::move(keyPtr), MapValue(delaysVector));
            }
        }
        
        return textRemaining;
    }

    // the -2 at the end is added in addMapPair when there isn't already a -1
    void fillDelaysVector(bool& textRemaining, std::vector<int>& delaysVector, FileHelper& fhelper, std::vector<char>& intAsChars)
    {
        char ch = '\0';
        int delay = 0;

        for (
            textRemaining = fhelper.getCharacter(ch);
            ch != '\n' && textRemaining;
            textRemaining = fhelper.getCharacter(ch))
        {
            if (ch >= '0' && ch <= '9')
            {
                intAsChars.push_back((char)ch);
            }
            else if (ch == '-')
            {
                delaysVector.push_back(-1);

                break;
            }
            else if (ch == '/')
            {
                auto [ptr, ec] = std::from_chars(intAsChars.data(), intAsChars.data() + intAsChars.size(), delay);

                if (ec == std::errc::result_out_of_range)
                {
                    throw std::runtime_error("delays can't be larger than INT_MAX1");
                }

                delaysVector.push_back(delay);
                intAsChars.clear();
                intAsChars.push_back('0'); // empty vector causes std::errc::invalid_argument
            }
        }

        if (delaysVector.empty() || delaysVector.back() != -1)
        {
            if (intAsChars.size() > 1)
            {
                auto [ptr, ec] = std::from_chars(intAsChars.data(), intAsChars.data() + intAsChars.size(), delay);

                if (ec == std::errc::result_out_of_range)
                {
                    throw std::runtime_error("delays can't be larger than INT_MAX2");
                }

                delaysVector.push_back(delay);
            }
        }

        intAsChars.clear();
        intAsChars.push_back('0');

        // make sure to go to end of line
        for (; ch != '\n' && textRemaining; textRemaining = fhelper.getCharacter(ch));
    }

    void delayFile(MapValue& fileMapValue)
    {
        static size_t fullResetCount = 0;
        
        int delay = 0;

        {
            std::lock_guard<std::mutex> mutexForMapLock(mutexForMap);

            if (fileMapValue.fullResetCheckNumber < fullResetCount)
            {
                fileMapValue.position = 0;
                fileMapValue.fullResetCheckNumber = fullResetCount;
            }

            if (fileMapValue.delays[0] == -1)
            {
                if (fullResetCount == SIZE_MAX) // this probably won't ever happen
                {
                    fullResetCount = 0;

                    for (auto& [k, v] : fileMap)
                    {
                        v.fullResetCheckNumber = 0;
                    }
                }

                fullResetCount++;
            }
            else if (fileMapValue.delays[fileMapValue.position] == -1)
            {
                fileMapValue.position = 0;
            }

            if (fileMapValue.delays[fileMapValue.position] >= 0)
            {
                delay = fileMapValue.delays[fileMapValue.position];
                fileMapValue.position++;
            }
        }
        
        if (delay > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
    }
};


static void sharedPathCheckingFunction(const char* path)
{
    static MapAndMutex mapAndMutexObject;
    
    int filenameIndex = -1;
    int pathEndIndex = 0;

    for (; path[pathEndIndex] != '\0'; pathEndIndex++)
    {
        if (path[pathEndIndex] == '/')
        {
            filenameIndex = pathEndIndex;
        }
    }

    filenameIndex++; // moving past '/' character or to 0 if no '/' was found
    auto it = mapAndMutexObject.fileMap.find(
        std::string_view(path + filenameIndex,
            (size_t)pathEndIndex - filenameIndex)
    );
    
    if (it != mapAndMutexObject.fileMap.end())
    {
        mapAndMutexObject.delayFile(it->second);
    }
}

FILE* fopen(const char* path, const char* mode)
{
    if (delaysActive)
    {
        sharedPathCheckingFunction(path);
    }

    return originalFopen(path, mode);
}

FILE* freopen(const char* path, const char* mode, FILE* stream)
{
    if (delaysActive)
    {
        sharedPathCheckingFunction(path);
    }

    return originalFreopen(path, mode, stream);
}

FILE* fopen64(const char* path, const char* mode)
{
    if (delaysActive)
    {
        sharedPathCheckingFunction(path);
    }

    return originalFopen64(path, mode);
}

FILE* freopen64(const char* path, const char* mode, FILE* stream)
{
    if (delaysActive)
    {
        sharedPathCheckingFunction(path);
    }

    return originalFreopen64(path, mode, stream);
}

