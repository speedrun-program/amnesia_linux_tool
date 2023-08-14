
// I avoided using standard C/C++ functions and objects here because I was told you shouldn't use them in __attribute__((constructor)) functions

#include <sys/mman.h>
#include <cstring> // memcpy is used from here, but the compiler (g++-11) inlines it
#include <errno.h>
#include <atomic> // atomic_ref is used in the __attribute__((destructor)) function. It makes the compiler use an xchg instruction.

#include "non_std_functions.h"
#include "load_extender.h"

#if __x86_64__ || __ppc64__
using uint_t = uint64_t;
static const uint_t UINTT_MAX = UINT64_MAX;
static const uint_t loadDetectionInstructionsSize = 128;
static const uint_t flashbackSkipInstructionsSize = 128;
static const uint_t flashbackWaitInstructionsSize = 192;
#else
using uint_t = uint32_t;
static const uint_t UINTT_MAX = UINT32_MAX;
static const uint_t loadDetectionInstructionsSize = 64;
static const uint_t flashbackSkipInstructionsSize = 64;
static const uint_t flashbackWaitInstructionsSize = 128;
#endif

static int memfd = -1;
static void* mmapAddress = MAP_FAILED;
static size_t extraMemorySize = 0;

#if __x86_64__ || __ppc64__
struct SavedInstructions
{
    unsigned char loadEndBytes[14]{};
    unsigned char menuLoadBytes[16]{};
    unsigned char mapLoadBytes[7]{};
    unsigned char gettingSoundHandler[14]{};
    unsigned char beforeFadeOutBytes[18]{};
    unsigned char mapLoadEndBytes[10]{};
    uint_t gpBaseAddress = 0;
    uint_t loadEndAddress = 0;
    uint_t menuLoadAddress = 0;
    uint_t mapLoadAddress = 0;
    uint_t beforeFadeOutAddress = 0;
    uint_t mapLoadEndAddress = 0;
    uint_t isQuitMessagePostedAddress = 0;
    uint_t getApplicationTimeAddress = 0;
    uint_t stopAddress = 0;
    uint_t isPlayingAddress = 0;
    uint_t flWaitAddress = 0;
};
#else
struct SavedInstructions
{
    unsigned char loadEndBytes[6]{};
    unsigned char menuLoadBytes[6]{};
    unsigned char mapLoadBytes[5]{};
    unsigned char gettingSoundHandler[16]{};
    unsigned char beforeFadeOutBytes[24]{};
    unsigned char mapLoadEndBytes[5]{};
    uint_t loadEndAddress = 0;
    uint_t menuLoadAddress = 0;
    uint_t mapLoadAddress = 0;
    uint_t beforeFadeOutAddress = 0;
    uint_t mapLoadEndAddress = 0;
    uint_t stopAddress = 0;
    uint_t isPlayingAddress = 0;
    uint_t flWaitAddress = 0;
};
#endif

// copy line if it might say the location of the game's memory or the mmap memory
static bool getPotentialLine(FileHelper& fh, char* mapLine, const size_t maxLineSize, size_t& filenameStart)
{
    filenameStart = 0;
    mapLine[0] = '\0';
    
    char ch = '\0';
    size_t mapLineIdx = 0;
    size_t filenameMaxIdx = 73; // it shouldn't reach this index before getting to the file path part
    bool thereAreMoreCharacters = false;
    
    char readPermission = '\0';
    char writePermission = '\0';
    char executePermission = '\0';
    char sharePermission = '\0';
    
    // copying memory region
    while ((thereAreMoreCharacters = fh.getCharacter(ch)))
    {
        mapLine[mapLineIdx] = ch; mapLineIdx += (mapLineIdx <= filenameMaxIdx);
        if (ch == ' ')
        {
            break;
        }
    }
    if (!thereAreMoreCharacters)
    {
        mapLine[0] = '\0';
        return false;
    }
    
    // copying permissions
    fh.getCharacter(readPermission);    mapLine[mapLineIdx] = readPermission;    mapLineIdx += (mapLineIdx <= filenameMaxIdx);
    fh.getCharacter(writePermission);   mapLine[mapLineIdx] = writePermission;   mapLineIdx += (mapLineIdx <= filenameMaxIdx);
    fh.getCharacter(executePermission); mapLine[mapLineIdx] = executePermission; mapLineIdx += (mapLineIdx <= filenameMaxIdx);
    fh.getCharacter(sharePermission);   mapLine[mapLineIdx] = sharePermission;   mapLineIdx += (mapLineIdx <= filenameMaxIdx);
    fh.getCharacter(ch);                mapLine[mapLineIdx] = ch;                mapLineIdx += (mapLineIdx <= filenameMaxIdx);
    
    // checking if permissions match what's being searched for
    // if they aren't, skip the line
    if (!(readPermission == 'r' && writePermission == '-' && executePermission == 'x' && sharePermission == 'p'))
    {
        mapLine[0] = '\0';
        while (fh.getCharacter(ch))
        {
            if (ch == '\n')
            {
                return true;
            }
        }
        return false;
    }
    
    // copying offset
    while ((thereAreMoreCharacters = fh.getCharacter(ch)))
    {
        mapLine[mapLineIdx] = ch; mapLineIdx += (mapLineIdx <= filenameMaxIdx);
        if (ch == ' ')
        {
            break;
        }
    }
    if (!thereAreMoreCharacters)
    {
        mapLine[0] = '\0';
        return false;
    }
    
    if (mapLineIdx > filenameMaxIdx) // this shouldn't ever happen
    {
        mapLine[0] = '\0';
        return true;
    }
    
    // going to the start of the file path
    while ((thereAreMoreCharacters = fh.getCharacter(ch)))
    {
        if (ch == '/')
        {
            break;
        }
        else if (ch == '\n')
        {
            mapLine[0] = '\0';
            return true;
        }
    }
    if (!thereAreMoreCharacters)
    {
        mapLine[0] = '\0';
        return false;
    }
    mapLine[mapLineIdx] = '/'; mapLineIdx += 1; // add this for consistency in case EOF was reached with no '/' found
    
    // finding and copying filename
    filenameStart = mapLineIdx;
    size_t filenameIdx = 0;
    bool tooLong = false;
    while (fh.getCharacter(ch))
    {
        if (ch == '\n')
        {
            if (tooLong)
            {
                return false;
            }
            
            mapLine[mapLineIdx + filenameIdx] = '\0';
            filenameStart -= 1; // start on slash
            return true;
        }
        else if (ch == '/')
        {
            mapLine[filenameStart] = '\0';
            filenameIdx = 0;
        }
        else if (!tooLong)
        {
            if (filenameIdx == 255) // the maximum filename length on Linux is 255
            {
                tooLong = true;
                continue;
            }
            
            mapLine[mapLineIdx + filenameIdx] = ch;
            filenameIdx += 1;
        }
    }
    
    return false;
}

static uint_t hexStringToInt(const char* s, size_t pos, const char endChar)
{
    uint_t n = 0;
    for (char ch = s[pos]; ch != '\0' && ch != endChar; ch = s[pos])
    {
        n <<= 4;
        n += ch - '0' - (('a' - '9' - 1) * (ch >= 'a'));
        pos++;
    }
    return n;
}

static bool checkForFilenameMatch(const char* filename, const char** gameNames, const size_t howManyGameNames)
{
    for (size_t i = 0; i < howManyGameNames; i++)
    {
        if (myStrncmp(filename, gameNames[i], 256) == 0) // the maximum filename length on Linux is 255
        {
            return true;
        }
    }
    return false;
}

// check if a copied line says the location of the game's memory or the mmap memory
static bool checkPotentialLine(
    const char* mapLine,
    const char** gameNames,
    const size_t howManyGameNames,
    uint_t& gameStartAddress,
    uint_t& gameEndAddress,
    const size_t filenameStart)
{
    if (!checkForFilenameMatch(&mapLine[filenameStart], gameNames, howManyGameNames))
    {
        return false;
    }
    
    size_t endAddressStart = myStrFind(mapLine, '-', 0) + 1;
    size_t permissionStart = myStrFind(mapLine, ' ', endAddressStart) + 1;
    size_t offsetStart = myStrFind(mapLine, ' ', permissionStart) + 1;
    
    gameStartAddress = hexStringToInt(mapLine, 0, '-');
    gameStartAddress += hexStringToInt(mapLine, offsetStart, ' ');
    gameEndAddress = hexStringToInt(mapLine, endAddressStart, ' ');
    
    return true;
}

static bool findExecutableMemory(uint_t& gameStartAddress, uint_t& gameEndAddress, const char** gameNames, const size_t howManyGameNames)
{
    FileHelper fh("/proc/self/maps");
    if (fh.fd == -1) // error message will have been printed in the constructor
    {
        return false;
    }
    
    size_t filenameStart = 0;
    char mapLine[73 + 1 + 255 + 1 + 1]{}; // start of maps file line + slash + filename max size + null terminator + extra null terminator
    while (getPotentialLine(fh, mapLine, sizeof(mapLine), filenameStart))
    {
        if (mapLine[0] != '\0' && checkPotentialLine(mapLine, gameNames, howManyGameNames, gameStartAddress, gameEndAddress, filenameStart))
        {
            return true;
        }
    }
    
    printCstr("ERROR: couldn't find amnesia executable memory\n");
    return false;
}

// this needs to be done to find how much memory to allocate for the virtual pages
static void preprocessFlashbackNames(FileHelper& fh, uint_t& howManyNames, uint_t& longestName)
{
    char ch = '\0';

    uint_t currentNameLength = 0;

    while (fh.getCharacter(ch))
    {
        if (ch == '\r') // windows puts this at the end of lines
        {
            continue;
        }
        else if (ch == '\n')
        {
            if (currentNameLength > longestName)
            {
                longestName = currentNameLength;
            }
            howManyNames += currentNameLength > 0;
            currentNameLength = 0;
        }
        else
        {
            currentNameLength++;
        }
    }

    if (currentNameLength > longestName) // last line
    {
        longestName = currentNameLength;
    }
    howManyNames += currentNameLength > 0;

    fh.resetFile();
}

static bool setupMemfdPages(const char* memfdName, const size_t extraMemorySize)
{
    memfd = memfd_create(memfdName, MFD_ALLOW_SEALING);
    if (memfd == -1)
    {
        printCstr("memfd_create error: "); printInt(errno); printCstr("\n");
        // printf("memfd_create error: %d\n", errno);
        return false;
    }
    
    if (ftruncate(memfd, extraMemorySize) == -1)
    {
        printCstr("ftruncate error: "); printInt(errno); printCstr("\n");
        // printf("ftruncate error: %d\n", errno);
        return false;
    }
    
    mmapAddress = mmap(nullptr, extraMemorySize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED, memfd, 0);
    if (mmapAddress == MAP_FAILED)
    {
        printCstr("mmap error: "); printInt(errno); printCstr("\n");
        // printf("mmap error: %d\n", errno);
        return false;
    }
    
    if (fcntl(memfd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_FUTURE_WRITE) == -1)
    {
        printCstr("fcntl error: "); printInt(errno); printCstr("\n");
        // printf("fcntl error: %d\n", errno);
        return false;
    }
    
    return true;
}

static bool setFlashbackNames(unsigned char* extraMemory, FileHelper& fh, const uint_t startOffset, const uint_t spacePerName, const uint_t extraMemorySize)
{
    const uint_t stringDataSize = sizeof(uint_t) * 3;
    const uint_t stringCapacity = spacePerName - stringDataSize - 1; // - 1 for null terminator
    char ch = '\0';
    uint_t writeOffset = startOffset;
    uint_t nameSize = 0;

    while (fh.getCharacter(ch))
    {
        if (ch == '\n')
        {
            if (nameSize > 0)
            {
                memcpy(&extraMemory[writeOffset], &nameSize, sizeof(nameSize));
                memcpy(&extraMemory[writeOffset + sizeof(nameSize)], &stringCapacity, sizeof(stringCapacity));
                writeOffset += spacePerName;
                nameSize = 0;
            }
        }
        else
        {
            if (nameSize == spacePerName - stringDataSize - 1) // this shouldn't ever happen, flashback line names shouldn't need to be long enough to cause this
            {
                printCstr("ERROR: a flashback line name was longer than expected, possibly because of integer overflow\n");
                return false;
            }
            else if (writeOffset + stringDataSize + nameSize >= extraMemorySize) // this also shouldn't ever happen, there shouldn't need to be enough to cause this
            {
                printCstr("ERROR: there were more flashback line names than expected, possibly because of integer overflow\n");
                return false;
            }

            extraMemory[writeOffset + stringDataSize + nameSize] = (unsigned char)ch;
            nameSize += 1;
        }
    }
    if (nameSize > 0) // last name
    {
        memcpy(&extraMemory[writeOffset], &nameSize, sizeof(nameSize));
        memcpy(&extraMemory[writeOffset + sizeof(nameSize)], &stringCapacity, sizeof(stringCapacity));
    }

    return true;
}

static void copyGameBytesAndLocation(unsigned char*& gamePtr, const size_t moveForwardBy, unsigned char* copyTo, const size_t copySize, uint_t& copyAddressTo)
{
    gamePtr += moveForwardBy;
    memcpy(copyTo, gamePtr, copySize);
    copyAddressTo = (uint_t)gamePtr;
}

#if __x86_64__ || __ppc64__
static uint32_t getOffset(unsigned char*& gamePtr, const size_t moveForwardBy)
{
    gamePtr += moveForwardBy;
    uint32_t offset = 0;
    memcpy(&offset, gamePtr, sizeof(offset));
    
    return offset;
}

static bool findInstructions(SavedInstructions& si, unsigned char* gamePtr, const uint_t gameSize)
{
    size_t patternsFound = 0; // if this reaches 7 before all patterns were found, there were duplicate patterns
    
    // finding where to write to and copy from in amnesia's memory based on instruction byte patterns
    for (uint_t giveUpHere = (uint_t)(gamePtr + (gameSize - 257)); (uint_t)gamePtr < giveUpHere && patternsFound < 7; gamePtr++)
    {
        if (gamePtr[0] == 0xe8 && gamePtr[7] == 0x7b && gamePtr[9] == 0xbe && gamePtr[10] == 0x06)
        {
            patternsFound += 1;
            
            copyGameBytesAndLocation(gamePtr, 19, si.loadEndBytes, sizeof(si.loadEndBytes), si.loadEndAddress);
            
            uint32_t callOffset = getOffset(gamePtr, 8);
            si.isQuitMessagePostedAddress = (uint_t)callOffset + (uint_t)gamePtr + 4;
        }
        else if (gamePtr[0] == 0xc1 && gamePtr[3] == 0xe7 && gamePtr[5] == 0x8b && gamePtr[6] == 0x80)
        {
            patternsFound += 1;
            
            copyGameBytesAndLocation(gamePtr, 20, si.menuLoadBytes, sizeof(si.menuLoadBytes), si.menuLoadAddress);
            
            // rsp offset correction
            uint32_t rspOffset1 = getOffset(gamePtr, 4) + 8;
            uint32_t rspOffset2 = getOffset(gamePtr, 8) + 8;
            memcpy(&si.menuLoadBytes[4], &rspOffset1, sizeof(rspOffset1));
            memcpy(&si.menuLoadBytes[12], &rspOffset2, sizeof(rspOffset2));
        }
        else if (gamePtr[0] == 0xff && gamePtr[3] == 0x4c && gamePtr[12] == 0x4f)
        {
            patternsFound += 1;
            
            uint32_t ripOffset = getOffset(gamePtr, 57);
            si.gpBaseAddress = (uint_t)ripOffset + (uint_t)gamePtr + 4;
            
            copyGameBytesAndLocation(gamePtr, 4, si.mapLoadBytes, sizeof(si.mapLoadBytes), si.mapLoadAddress);
            si.mapLoadAddress -= 7;
            
            // the rip offset needs to be corrected here because these instructions get moved forward 14 bytes
            copyGameBytesAndLocation(gamePtr, 35, si.beforeFadeOutBytes, sizeof(si.beforeFadeOutBytes), si.beforeFadeOutAddress);
            memcpy(&ripOffset, &si.beforeFadeOutBytes[14], sizeof(ripOffset));
            ripOffset -= 14;
            memcpy(&si.beforeFadeOutBytes[14], &ripOffset, sizeof(ripOffset));
            
            gamePtr += 18;
            memcpy(si.gettingSoundHandler, gamePtr, sizeof(si.gettingSoundHandler));
        }
        else if (gamePtr[0] == 0xed && gamePtr[9] == 0x6c && gamePtr[14] == 0x54)
        {
            patternsFound += 1;
            
            copyGameBytesAndLocation(gamePtr, 50, si.mapLoadEndBytes, sizeof(si.mapLoadEndBytes), si.mapLoadEndAddress);
            
            uint32_t callOffset = getOffset(gamePtr, 11);
            si.getApplicationTimeAddress = (uint_t)callOffset + (uint_t)gamePtr + 4;
        }
        else if (gamePtr[0] == 0x80 && gamePtr[1] == 0x7b && gamePtr[4] == 0xb8 && gamePtr[11] == 0x80)
        {
            patternsFound += 1;
            
            si.stopAddress = (uint_t)gamePtr - 98;
        }
        else if (gamePtr[0] == 0x8b && gamePtr[1] == 0x7b && gamePtr[5] == 0x07 && gamePtr[13] == 0x83)
        {
            patternsFound += 1;
            
            si.isPlayingAddress = (uint_t)gamePtr - 99;
        }
        else if (gamePtr[0] == 0x89 && gamePtr[1] == 0xf8 && gamePtr[2] == 0xba && gamePtr[9] == 0xec)
        {
            patternsFound += 1;
            
            si.flWaitAddress = (uint_t)gamePtr - 0;
        }
    }
    
    if (
        si.loadEndAddress != 0
        && si.menuLoadAddress != 0
        && si.mapLoadAddress != 0
        && si.mapLoadEndAddress != 0
        && si.stopAddress != 0
        && si.isPlayingAddress != 0
        && si.flWaitAddress != 0)
    {
        return true;
    }
    
    printCstr("ERROR: "); printCstr(patternsFound == 7 ? "duplicate injection location patterns found\n" : "couldn't find all instruction locations\n");
    // printf("ERROR: %s\n", patternsFound == 7 ? "duplicate injection location patterns found" : "couldn't find all instruction locations");
    return false;
}

// for the following three functions, remember that the stack depth starts at +8 due to pushing rcx before using it to jump
static void injectLoadDetectionInstructions(SavedInstructions& si, unsigned char* extraMemory)
{
    unsigned char loadDetectionInstructions[loadDetectionInstructionsSize] = {
        // start of mmap memory
        0x00,                                                           // 0000 // pause/split timer byte
        
        // start of load finished instructions:
        // byte update instructions (plus dummy push)
        0x51,                                                           // 0001 // push rcx // dummy push so stack is aligned by 16 for function call
        0xb1, 0x00,                                                     // 0002 // mov cl, 0x00
        0x86, 0x0d, 0xf6, 0xff, 0xff, 0xff,                             // 0004 // xchg byte ptr [rip - 10], cl
        // original instructions
        0x48, 0x8b, 0xbb, 0xd8, 0x00, 0x00, 0x00,                       // 0010 // mov rdi, qword ptr [rbx + 0xd8] // COPY THIS
        0x48, 0xb9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // 0017 // mov rcx, isQuitMessagePosted address
        0xff, 0xd1,                                                     // 0027 // call rcx
        0x84, 0xc0,                                                     // 0029 // test al, al // COPY THIS
        // jump back instructions (plus pop to undo dummy push)
        0x59,                                                           // 0031 // pop rcx // undoing dummy push
        0x48, 0xb9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // 0032 // mov rcx, address of pop rcx instruction
        0xff, 0xe1,                                                     // 0042 // jmp rcx
        0x90, 0x90, 0x90, 0x90,                                         // 0044 // nops so menu load instructions start on byte 48
        
        // start of menu load instructions:
        // byte update instructions
        0xb1, 0x01,                                                     // 0048 // mov cl, 0x01
        0x86, 0x0d, 0xc8, 0xff, 0xff, 0xff,                             // 0050 // xchg byte ptr [rip - 56], cl
        // original instructions with corrected rsp offsets
        0x48, 0x8d, 0xac, 0x24, 0xb8, 0x00, 0x00, 0x00,                 // 0056 // lea rbp, [rsp + 0xb8] // COPY THIS after correcting the rsp offset
        0x48, 0x8d, 0x94, 0x24, 0xf5, 0x00, 0x00, 0x00,                 // 0064 // lea rdx, [rsp + 0xf5] // COPY THIS after correcting the rsp offset
        // jump back instructions
        0x48, 0xb9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // 0072 // mov rcx, address of pop rcx instruction
        0xff, 0xe1,                                                     // 0082 // jmp rcx
        0x90,                                                           // 0084 // nop
        
        // start of map load instructions:
        // byte update instructions
        0xb1, 0x02,                                                     // 0085 // mov cl, 0x02
        0x86, 0x0d, 0xa3, 0xff, 0xff, 0xff,                             // 0087 // xchg byte ptr [rip - 93], cl
        // original instructions
        0x48, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // 0093 // mov rax, qword ptr [gpBase address]
        0x48, 0x8b, 0xb8, 0x38, 0x01, 0x00, 0x00,                       // 0103 // mov rdi, qword ptr [rax + 0x138] // COPY THIS
        // jump back instructions
        0x48, 0xb9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // 0110 // mov rcx, address of pop rcx instruction
        0xff, 0xe1,                                                     // 0120 // jmp rcx
        0x90,                                                           // 0122 // nop
        0xcc, 0xcc, 0xcc, 0xcc, 0xcc                                    // 0123 // INT3 filler
    };
    
    unsigned char jmpToMmap[] = {
        0x51,                                                           // push rcx
        0x48, 0xb9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // mov rcx, mmapAddress
        0xff, 0xe1,                                                     // jmp rcx
        0x59                                                            // pop rcx
    };
    
    uint_t poprcxAddress = 0;
    uint_t mmapJumpAddress = 0;
    
    // writing to mmap memory
    memcpy(&loadDetectionInstructions[10], si.loadEndBytes, 7); // the instruction after this one needs to be corrected for rip offset
    memcpy(&loadDetectionInstructions[19], &si.isQuitMessagePostedAddress, sizeof(si.isQuitMessagePostedAddress));
    memcpy(&loadDetectionInstructions[29], &si.loadEndBytes[12], 2);
    poprcxAddress = si.loadEndAddress + 13;
    memcpy(&loadDetectionInstructions[34], &poprcxAddress, sizeof(poprcxAddress));
    
    memcpy(&loadDetectionInstructions[56], si.menuLoadBytes, sizeof(si.menuLoadBytes));
    poprcxAddress = si.menuLoadAddress + 15;
    memcpy(&loadDetectionInstructions[74], &poprcxAddress, sizeof(poprcxAddress));
    
    memcpy(&loadDetectionInstructions[95], &si.gpBaseAddress, sizeof(si.gpBaseAddress));
    memcpy(&loadDetectionInstructions[103], si.mapLoadBytes, sizeof(si.mapLoadBytes));
    poprcxAddress = si.mapLoadAddress + 13;
    memcpy(&loadDetectionInstructions[112], &poprcxAddress, sizeof(poprcxAddress));
    
    memcpy(extraMemory, loadDetectionInstructions, sizeof(loadDetectionInstructions));
    
    // writing to game executable memory
    mmapJumpAddress = (uint_t)extraMemory + 1;
    memcpy(&jmpToMmap[3], &mmapJumpAddress, sizeof(mmapJumpAddress));
    memcpy((unsigned char*)si.loadEndAddress, jmpToMmap, sizeof(jmpToMmap));
    
    mmapJumpAddress = (uint_t)extraMemory + 48;
    memcpy(&jmpToMmap[3], &mmapJumpAddress, sizeof(mmapJumpAddress));
    memcpy((unsigned char*)si.menuLoadAddress, jmpToMmap, sizeof(jmpToMmap) - 1);
    *((unsigned char*)(si.menuLoadAddress + 13)) = 0x90; // nop
    *((unsigned char*)(si.menuLoadAddress + 14)) = 0x90; // nop
    *((unsigned char*)(si.menuLoadAddress + 15)) = 0x59; // pop rcx
    
    mmapJumpAddress = (uint_t)extraMemory + 85;
    memcpy(&jmpToMmap[3], &mmapJumpAddress, sizeof(mmapJumpAddress));
    memcpy((unsigned char*)si.mapLoadAddress, jmpToMmap, sizeof(jmpToMmap));
}

static void injectSkipInstructions(SavedInstructions& si, unsigned char* extraMemory, const uint_t howManyNames, const uint32_t spacePerName)
{
    // this is jumped to after a call instruction, so the caller-saved registers shouldn't need to be saved
    unsigned char flashbackSkipInstructions[flashbackSkipInstructionsSize] = {
        // finishing putting SoundHandler object into rdi. copy from si.gettingSoundHandler.
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // 0000 //
        
        0x41, 0x54,                                                     // 0014 // push r12 // stack depth +16
        0x41, 0x55,                                                     // 0016 // push r13 // stack depth +24
        0x41, 0x56,                                                     // 0018 // push r14 // stack depth +32
        0x41, 0x57,                                                     // 0020 // push r15 // stack depth +40
        0x53,                                                           // 0022 // push rbx // stack depth +48
        0x53,                                                           // 0023 // push rbx // stack depth +56
        0x53,                                                           // 0024 // push rbx // stack depth +64
        0x49, 0xbc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // 0025 // mov r12, stringObjectAddress
        0x49, 0xbd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // 0035 // mov r13, loopStopAddress
        0x49, 0xbe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // 0045 // mov r14, cSoundHandler::Stop address
        0x49, 0x89, 0xff,                                               // 0055 // mov r15, rdi // cSoundHandler object
        0x49, 0x8b, 0x1c, 0x24,                                         // 0058 // mov rbx, qword ptr [r12] // remember to initialize this during injection
        0x4c, 0x89, 0xe6,                                               // 0062 // mov rsi, r12
        // start of loop
        0x49, 0x89, 0x1c, 0x24,                                         // 0065 // mov qword ptr [r12], rbx
        0x41, 0xff, 0xd6,                                               // 0069 // call r14 // cSoundHandler::Stop
        0x4c, 0x89, 0xff,                                               // 0072 // mov rdi, r15 // cSoundHandler object
        0x4c, 0x89, 0xe6,                                               // 0075 // mov rsi, r12
        0x48, 0x81, 0xc3, 0x00, 0x00, 0x00, 0x00,                       // 0078 // add rbx, spacePerName
        0x4c, 0x39, 0xeb,                                               // 0085 // cmp rbx, r13
        0x75, 0xe7,                                                     // 0088 // jnz, -25
        // end of loop
        
        0x5b,                                                           // 0090 // pop rbx // stack depth +56
        0x5b,                                                           // 0091 // pop rbx // stack depth +48
        0x5b,                                                           // 0092 // pop rbx // stack depth +40
        0x49, 0x83, 0xc4, 0x58,                                         // 0093 // add r12, 88
        0x4d, 0x89, 0x64, 0x24, 0xa8,                                   // 0097 // mov qword ptr [r12 - 88], r12 // resetting string object
        0x41, 0x5f,                                                     // 0102 // pop r15 // stack depth +32
        0x41, 0x5e,                                                     // 0104 // pop r14 // stack depth +24
        0x41, 0x5d,                                                     // 0106 // pop r13 // stack depth +16
        0x41, 0x5c,                                                     // 0108 // pop r12 // stack depth +8
        0x48, 0xb9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // 0110 // mov rcx, address of pop rcx instruction
        0xff, 0xe1,                                                     // 0120 // jmp rcx
        0x90,                                                           // 0122 // nop
        
        0xcc, 0xcc, 0xcc, 0xcc, 0xcc,                                   // 0123 // INT3 filler
    };
    
    unsigned char jmpToMmap[] = {
        0x51,                                                           // push rcx
        0x48, 0xb9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // mov rcx, mmapAddress
        0xff, 0xe1,                                                     // jmp rcx
        0x59                                                            // pop rcx
    };
    
    uint_t poprcxAddress = 0;
    uint_t mmapJumpAddress = (uint_t)extraMemory + loadDetectionInstructionsSize;
    uint_t stringObjectAddress = mmapJumpAddress + flashbackSkipInstructionsSize;
    // + 64 + (sizeof(uint_t) * 3) because 64 bytes are used for the string plus padding, and the string points to the byte after sizeof(uint_t) * 3
    uint_t firstStringDataAddress = stringObjectAddress + 64 + (sizeof(uint_t) * 3);
    uint_t loopStopAddress = firstStringDataAddress + (howManyNames * spacePerName);
    
    // writing to mmap memory
    memcpy(&flashbackSkipInstructions[0], si.gettingSoundHandler, sizeof(si.gettingSoundHandler));
    memcpy(&flashbackSkipInstructions[27], &stringObjectAddress, sizeof(stringObjectAddress));
    memcpy(&flashbackSkipInstructions[37], &loopStopAddress, sizeof(loopStopAddress));
    memcpy(&flashbackSkipInstructions[47], &si.stopAddress, sizeof(si.stopAddress));
    memcpy(&flashbackSkipInstructions[81], &spacePerName, sizeof(spacePerName));
    poprcxAddress = si.beforeFadeOutAddress + 13;
    memcpy(&flashbackSkipInstructions[112], &poprcxAddress, sizeof(poprcxAddress));
    memcpy((unsigned char*)stringObjectAddress, &firstStringDataAddress, sizeof(firstStringDataAddress));
    
    memcpy(extraMemory + loadDetectionInstructionsSize, flashbackSkipInstructions, sizeof(flashbackSkipInstructions));
    
    // writing to game executable memory
    memcpy(&jmpToMmap[3], &mmapJumpAddress, sizeof(mmapJumpAddress));
    memcpy((unsigned char*)si.beforeFadeOutAddress, jmpToMmap, sizeof(jmpToMmap));
    memcpy((unsigned char*)(si.beforeFadeOutAddress + sizeof(jmpToMmap)), si.beforeFadeOutBytes, sizeof(si.beforeFadeOutBytes));
}

static void injectWaitInstructions(SavedInstructions& si, unsigned char* extraMemory, const uint_t howManyNames, const uint32_t spacePerName)
{
    // this is jumped to after a call instruction, so the caller-saved registers shouldn't need to be saved
    unsigned char flashbackWaitInstructions[flashbackWaitInstructionsSize] = {
        0x48, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // 0000 // mov rax, gpBaseAddress
        // finishing putting SoundHandler object into rdi. copy from si.gettingSoundHandler.
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // 0010 //
        
        0x41, 0x54,                                                     // 0024 // push r12 // stack depth +16
        0x41, 0x55,                                                     // 0026 // push r13 // stack depth +24
        0x41, 0x56,                                                     // 0028 // push r14 // stack depth +32
        0x41, 0x57,                                                     // 0030 // push r15 // stack depth +40
        0x53,                                                           // 0032 // push rbx // stack depth +48
        0x57,                                                           // 0033 // push rdi // stack depth +56 // cSoundHandler object
        0x31, 0xdb,                                                     // 0034 // xor ebx, ebx
        0x48, 0xbe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // 0036 // mov rsi, stringObjectAddress
        0x49, 0xbd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // 0046 // mov r13, loopStopAddress
        0x49, 0xbe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // 0056 // mov r14, cSoundHandler::IsPlaying address
        0x49, 0xbf, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // 0066 // mov r15, FLwait address
        0x4c, 0x8b, 0x26,                                               // 0076 // mov r12, qword ptr [rsi] // remember to initialize this during injection
        0x56,                                                           // 0079 // push rsi // stack depth +64 // stringObjectAddress
        // start of loop
        0x48, 0x8b, 0x7c, 0x24, 0x08,                                   // 0080 // mov rdi, qword ptr [rsp + 8] // cSoundHandler object
        0x48, 0x8b, 0x34, 0x24,                                         // 0085 // mov rsi, qword ptr [rsp] // stringObjectAddress
        0x4c, 0x89, 0x26,                                               // 0089 // mov qword ptr [rsi], r12
        0x41, 0xff, 0xd6,                                               // 0092 // call r14 // cSoundHandler::IsPlaying
        0x09, 0xc3,                                                     // 0095 // or ebx, eax
        0x49, 0x81, 0xc4, 0x00, 0x00, 0x00, 0x00,                       // 0097 // add r12, spacePerName
        0x4d, 0x39, 0xec,                                               // 0104 // cmp r12, r13
        0x75, 0xe3,                                                     // 0107 // jnz, -29
        0x83, 0xfb, 0x00,                                               // 0109 // cmp ebx, 0
        0x74, 0x19,                                                     // 0112 // jz 25
        0xbf, 0xe8, 0x03, 0x00, 0x00,                                   // 0114 // mov edi, 1000 // 1000 microseconds (1 millisecond)
        0x41, 0xff, 0xd7,                                               // 0119 // call r15 // FLwait
        0x4c, 0x8b, 0x24, 0x24,                                         // 0122 // mov r12, qword ptr [rsp] // stringObjectAddress
        0x49, 0x83, 0xc4, 0x58,                                         // 0126 // add r12, 88
        0x4d, 0x89, 0x64, 0x24, 0xa8,                                   // 0130 // mov qword ptr [r12 - 88], r12 // resetting string object
        0x31, 0xdb,                                                     // 0135 // xor ebx, ebx
        0xeb, 0xc5,                                                     // 0137 // jmp -59
        // end of loop
        
        0x5e,                                                           // 0139 // pop rsi // stack depth +56 // stringObjectAddress
        0x48, 0x89, 0x36,                                               // 0140 // mov qword ptr [rsi], rsi
        0x48, 0x83, 0x06, 0x58,                                         // 0143 // add qword ptr [rsi], 88 // resetting string object
        0x5f,                                                           // 0147 // pop rdi // stack depth +48
        0x5b,                                                           // 0148 // pop rbx // stack depth +40
        0x41, 0x5f,                                                     // 0149 // pop r15 // stack depth +32
        0x41, 0x5e,                                                     // 0151 // pop r14 // stack depth +24
        0x41, 0x5d,                                                     // 0153 // pop r13 // stack depth +16
        0x48, 0x8b, 0x7b, 0x28,                                         // 0155 // mov rdi, qword ptr [rbx + 0x28] // COPY THIS
        0x48, 0x8b, 0x07,                                               // 0159 // mov rax, qword ptr [rdi] // COPY THIS
        0xff, 0x50, 0x18,                                               // 0162 // call qword ptr [rax + 0x18] // COPY THIS
        0x48, 0xb9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // 0165 // mov rcx, getApplicationTimeAddress
        0xff, 0xd1,                                                     // 0175 // call rcx
        0x41, 0x5c,                                                     // 0177 // pop r12 // stack depth +8
        0x48, 0xb9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // 0179 // mov rcx, address of pop rcx instruction
        0xff, 0xe1,                                                     // 0189 // jmp rcx
        0x90                                                            // 0191 // nop
    };
    
    unsigned char jmpToMmap[] = {
        0x51,                                                           // push rcx
        0x48, 0xb9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // mov rcx, mmapAddress
        0xff, 0xe1,                                                     // jmp rcx
        0x59                                                            // pop rcx
    };
    
    uint_t poprcxAddress = 0;
    uint_t mmapJumpAddress = (uint_t)extraMemory + loadDetectionInstructionsSize;
    uint_t stringObjectAddress = mmapJumpAddress + flashbackWaitInstructionsSize;
    // + 64 + (sizeof(uint_t) * 3) because 64 bytes are used for the string plus padding, and the string points to the byte after sizeof(uint_t) * 3
    uint_t firstStringDataAddress = stringObjectAddress + 64 + (sizeof(uint_t) * 3);
    uint_t loopStopAddress = firstStringDataAddress + (howManyNames * spacePerName);
    
    // writing to mmap memory
    memcpy(&flashbackWaitInstructions[2], &si.gpBaseAddress, sizeof(si.gpBaseAddress));
    memcpy(&flashbackWaitInstructions[10], si.gettingSoundHandler, sizeof(si.gettingSoundHandler));
    memcpy(&flashbackWaitInstructions[38], &stringObjectAddress, sizeof(stringObjectAddress));
    memcpy(&flashbackWaitInstructions[48], &loopStopAddress, sizeof(loopStopAddress));
    memcpy(&flashbackWaitInstructions[58], &si.isPlayingAddress, sizeof(si.isPlayingAddress));
    memcpy(&flashbackWaitInstructions[68], &si.flWaitAddress, sizeof(si.flWaitAddress));
    memcpy(&flashbackWaitInstructions[100], &spacePerName, sizeof(spacePerName));
    memcpy(&flashbackWaitInstructions[155], si.mapLoadEndBytes, sizeof(si.mapLoadEndBytes));
    memcpy(&flashbackWaitInstructions[167], &si.getApplicationTimeAddress, sizeof(si.getApplicationTimeAddress));
    poprcxAddress = si.mapLoadEndAddress + 14;
    memcpy(&flashbackWaitInstructions[181], &poprcxAddress, sizeof(poprcxAddress));
    memcpy((unsigned char*)stringObjectAddress, &firstStringDataAddress, sizeof(firstStringDataAddress));
    
    memcpy(extraMemory + loadDetectionInstructionsSize, flashbackWaitInstructions, sizeof(flashbackWaitInstructions));
    
    // writing to game executable memory
    memcpy(&jmpToMmap[3], &mmapJumpAddress, sizeof(mmapJumpAddress));
    memcpy((unsigned char*)si.mapLoadEndAddress, jmpToMmap, sizeof(jmpToMmap) - 1);
    *((unsigned char*)(si.mapLoadEndAddress + 13)) = 0x90; // nop
    *((unsigned char*)(si.mapLoadEndAddress + 14)) = 0x59; // pop rcx
}

#else
static bool findInstructions(SavedInstructions& si, unsigned char* gamePtr, const uint_t gameSize)
{
    size_t patternsFound = 0; // if this reaches 7 before all patterns were found, there were duplicate patterns
    
    // finding where to write to and copy from in amnesia's memory based on instruction byte patterns
    for (uint_t giveUpHere = (uint_t)(gamePtr + (gameSize - 257)); (uint_t)gamePtr < giveUpHere && patternsFound < 7; gamePtr++)
    {
        if (gamePtr[0] == 0xe8 && gamePtr[9] == 0x06 && gamePtr[10] == 0x00 && gamePtr[13] == 0xd9)
        {
            patternsFound += 1;
            
            copyGameBytesAndLocation(gamePtr, 28, si.loadEndBytes, sizeof(si.loadEndBytes), si.loadEndAddress);
        }
        else if (gamePtr[0] == 0x7d && gamePtr[3] == 0x40 && gamePtr[5] == 0x8b && gamePtr[8] == 0xc7)
        {
            patternsFound += 1;
            
            copyGameBytesAndLocation(gamePtr, 38, si.menuLoadBytes, sizeof(si.menuLoadBytes), si.menuLoadAddress);
        }
        else if (gamePtr[0] == 0x52 && gamePtr[2] == 0x8d && gamePtr[5] == 0x8d && gamePtr[8] == 0x89)
        {
            patternsFound += 1;
            
            copyGameBytesAndLocation(gamePtr, 59, si.mapLoadBytes, sizeof(si.mapLoadBytes), si.mapLoadAddress);
            
            // the si.beforeFadeOutBytes instructions are between the si.gettingSoundHandler instructions
            gamePtr += 37;
            memcpy(si.gettingSoundHandler, gamePtr, 5);
            gamePtr += 5;
            memcpy(si.beforeFadeOutBytes, gamePtr, sizeof(si.beforeFadeOutBytes));
            gamePtr += sizeof(si.beforeFadeOutBytes);
            memcpy(&si.gettingSoundHandler[5], gamePtr, sizeof(si.gettingSoundHandler) - 5);
            si.beforeFadeOutAddress = (uint_t)gamePtr - sizeof(si.beforeFadeOutBytes) - 5;
        }
        else if (gamePtr[0] == 0xf2 && gamePtr[1] == 0x84 && gamePtr[13] == 0x75)
        {
            patternsFound += 1;
            
            copyGameBytesAndLocation(gamePtr, 58, si.mapLoadEndBytes, sizeof(si.mapLoadEndBytes), si.mapLoadEndAddress);
        }
        else if (gamePtr[0] == 0x80 && gamePtr[1] == 0x7b && gamePtr[4] == 0xb8 && gamePtr[11] == 0x80)
        {
            patternsFound += 1;
            
            si.stopAddress = (uint_t)gamePtr - 126;
        }
        else if (gamePtr[0] == 0x43 && gamePtr[2] == 0x8b && gamePtr[11] == 0x65)
        {
            patternsFound += 1;
            
            si.isPlayingAddress = (uint_t)gamePtr - 119;
        }
        else if (gamePtr[0] == 0x53 && gamePtr[3] == 0x18 && gamePtr[8] == 0xba && gamePtr[13] == 0x89)
        {
            patternsFound += 1;
            
            si.flWaitAddress = (uint_t)gamePtr - 0;
        }
    }
    
    if (
        si.loadEndAddress != 0
        && si.menuLoadAddress != 0
        && si.mapLoadAddress != 0
        && si.mapLoadEndAddress != 0
        && si.stopAddress != 0
        && si.isPlayingAddress != 0
        && si.flWaitAddress != 0)
    {
        return true;
    }
    
    printCstr("ERROR: "); printCstr(patternsFound == 7 ? "duplicate injection location patterns found\n" : "couldn't find all instruction locations\n");
    // printf("ERROR: %s\n", patternsFound == 7 ? "duplicate injection location patterns found" : "couldn't find all instruction locations");
    return false;
}

static void injectLoadDetectionInstructions(SavedInstructions& si, unsigned char* extraMemory)
{
    unsigned char loadDetectionInstructions[loadDetectionInstructionsSize] = {
        // start of mmap memory
        0x00,                                                           // 0000 // pause/split timer byte
        
        // start of load finished instructions:
        // byte update instructions
        0xb0, 0x00,                                                     // 0001 // mov al, 0x00
        0x86, 0x05, 0x00, 0x00, 0x00, 0x00,                             // 0003 // xchg byte ptr [timer_byte_address], al
        // original instructions
        0x8b, 0x43, 0x74,                                               // 0009 // mov eax, dword ptr [ebx + 0x74] // COPY THIS
        0x89, 0x04, 0x24,                                               // 0012 // mov dword ptr [esp], eax // COPY THIS
        // jump back to game executable memory
        0xe9, 0x00, 0x00, 0x00, 0x00,                                   // 0015 // jmp address of next instruction
        0x90,                                                           // 0020 // nop
        
        // start of menu load instructions:
        // byte update instructions
        0xb0, 0x01,                                                     // 0021 // mov al, 0x01
        0x86, 0x05, 0x00, 0x00, 0x00, 0x00,                             // 0023 // xchg byte ptr [timer_byte_address], al
        // original instructions
        0x8d, 0x45, 0xe5,                                               // 0029 // lea eax, [ebp + -0x1b] // COPY THIS
        0x8d, 0x75, 0xd0,                                               // 0032 // lea esi, [ebp + -0x30] // COPY THIS
        // jump back to game executable memory
        0xe9, 0x00, 0x00, 0x00, 0x00,                                   // 0035 // jmp address of next instruction
        0x90,                                                           // 0040 // nop
        
        // start of map load instructions:
        // byte update instructions
        0xb0, 0x02,                                                     // 0041 // mov al, 0x02
        0x86, 0x05, 0x00, 0x00, 0x00, 0x00,                             // 0043 // xchg byte ptr [timer_byte_address], al
        // original instructions
        0xa1, 0xf8, 0x87, 0xf1, 0x08,                                   // 0049 // mov eax, [0x08f187f8] // COPY THIS
        // jump back to game executable memory
        0xe9, 0x00, 0x00, 0x00, 0x00,                                   // 0054 // jmp address of next instruction
        0x90,                                                           // 0059 // nop
        
        0xcc, 0xcc, 0xcc, 0xcc,                                         // 0060 // INT3 filler
    };
    
    unsigned char jmpToMmap[] = {0xe9, 0x00, 0x00, 0x00, 0x00, 0x90}; // nop because si.loadEndBytes and si.menuLoadBytes are six bytes
    
    uint_t timerByteAddress = (uint_t)extraMemory;
    uint_t jumpOffset = 0;
    
    // writing to mmap memory
    memcpy(&loadDetectionInstructions[5], &timerByteAddress, sizeof(timerByteAddress));
    memcpy(&loadDetectionInstructions[9], si.loadEndBytes, sizeof(si.loadEndBytes));
    jumpOffset = (si.loadEndAddress + sizeof(si.loadEndBytes)) - (timerByteAddress + 20);
    memcpy(&loadDetectionInstructions[16], &jumpOffset, sizeof(jumpOffset));
    
    memcpy(&loadDetectionInstructions[25], &timerByteAddress, sizeof(timerByteAddress));
    memcpy(&loadDetectionInstructions[29], si.menuLoadBytes, sizeof(si.menuLoadBytes));
    jumpOffset = (si.menuLoadAddress + sizeof(si.menuLoadBytes)) - (timerByteAddress + 40);
    memcpy(&loadDetectionInstructions[36], &jumpOffset, sizeof(jumpOffset));
    
    memcpy(&loadDetectionInstructions[45], &timerByteAddress, sizeof(timerByteAddress));
    memcpy(&loadDetectionInstructions[49], si.mapLoadBytes, sizeof(si.mapLoadBytes));
    jumpOffset = (si.mapLoadAddress + sizeof(si.mapLoadBytes)) - (timerByteAddress + 59);
    memcpy(&loadDetectionInstructions[55], &jumpOffset, sizeof(jumpOffset));
    
    memcpy(extraMemory, loadDetectionInstructions, sizeof(loadDetectionInstructions));
    
    // writing to game executable memory
    jumpOffset = (timerByteAddress + 1) - (si.loadEndAddress + 5);
    memcpy(&jmpToMmap[1], &jumpOffset, sizeof(jumpOffset));
    memcpy((unsigned char*)si.loadEndAddress, jmpToMmap, sizeof(si.loadEndBytes));
    
    jumpOffset = (timerByteAddress + 21) - (si.menuLoadAddress + 5);
    memcpy(&jmpToMmap[1], &jumpOffset, sizeof(jumpOffset));
    memcpy((unsigned char*)si.menuLoadAddress, jmpToMmap, sizeof(si.menuLoadBytes));
    
    jumpOffset = (timerByteAddress + 41) - (si.mapLoadAddress + 5);
    memcpy(&jmpToMmap[1], &jumpOffset, sizeof(jumpOffset));
    memcpy((unsigned char*)si.mapLoadAddress, jmpToMmap, sizeof(si.mapLoadBytes));
}

static void injectSkipInstructions(SavedInstructions& si, unsigned char* extraMemory, const uint_t howManyNames, const uint32_t spacePerName)
{
    // this is jumped to after a call instruction, so the caller-saved registers shouldn't need to be saved
    unsigned char flashbackSkipInstructions[flashbackSkipInstructionsSize] = {
        0x53,                                               // 0000 // push ebx // stack depth +4
        0x53,                                               // 0001 // push ebx // stack depth +8
        0x68, 0x00, 0x00, 0x00, 0x00,                       // 0002 // push stringObjectAddress // stack depth +12
        0x50,                                               // 0007 // push eax // stack depth +16 // cSoundHandler object
        0xbb, 0x00, 0x00, 0x00, 0x00,                       // 0008 // mov ebx, firstStringDataAddress
        0x90, 0x90, 0x90,                                   // 0013 // nops so loop starts on byte 16
        // start of loop
        0x89, 0x1d, 0x00, 0x00, 0x00, 0x00,                 // 0016 // mov dword ptr [stringObjectAddress], ebx
        0xe8, 0x00, 0x00, 0x00, 0x00,                       // 0022 // call cSoundHandler::Stop
        0x81, 0xc3, 0x00, 0x00, 0x00, 0x00,                 // 0027 // add ebx, spacePerName
        0x81, 0xfb, 0x00, 0x00, 0x00, 0x00,                 // 0033 // cmp ebx, loopStopAddress
        0x75, 0xe7,                                         // 0039 // jnz -25
        // end of loop
        
        0x58,                                               // 0041 // pop eax // stack depth +12
        0x5b,                                               // 0042 // pop ebx // stack depth +8
        0x5b,                                               // 0043 // pop ebx // stack depth +4
        0x5b,                                               // 0044 // pop ebx // stack depth +0
        0xc7, 0x44, 0x24, 0x0c, 0x01, 0x00, 0x00, 0x00,     // 0045 // mov dword ptr [esp + 12], 1 // COPY THIS
        0xe9, 0x00, 0x00, 0x00, 0x00,                       // 0053 // jmp address of next instruction
        0x90,                                               // 0058 // nop
        0xcc, 0xcc, 0xcc, 0xcc, 0xcc                        // 0059 // INT3 filler
    };
    
    unsigned char jmpToMmap[] = {0xe9, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90}; // nops because the first instruction in si.beforeFadeOutBytes is 8 bytes
    
    uint_t jumpOffset = 0;
    uint_t mmapJumpAddress = (uint_t)extraMemory + loadDetectionInstructionsSize;
    uint_t stringObjectAddress = mmapJumpAddress + flashbackSkipInstructionsSize;
    // + 64 + (sizeof(uint_t) * 3) because 64 bytes are used for the string plus padding, and the string points to the byte after sizeof(uint_t) * 3
    uint_t firstStringDataAddress = stringObjectAddress + 64 + (sizeof(uint_t) * 3);
    uint_t loopStopAddress = firstStringDataAddress + (howManyNames * spacePerName);
    
    // writing to mmap memory
    memcpy(&flashbackSkipInstructions[3], &stringObjectAddress, sizeof(stringObjectAddress));
    memcpy(&flashbackSkipInstructions[9], &firstStringDataAddress, sizeof(firstStringDataAddress));
    memcpy(&flashbackSkipInstructions[18], &stringObjectAddress, sizeof(stringObjectAddress));
    jumpOffset = si.stopAddress - (mmapJumpAddress + 27);
    memcpy(&flashbackSkipInstructions[23], &jumpOffset, sizeof(si.stopAddress));
    memcpy(&flashbackSkipInstructions[29], &spacePerName, sizeof(spacePerName));
    memcpy(&flashbackSkipInstructions[35], &loopStopAddress, sizeof(loopStopAddress));
    memcpy(&flashbackSkipInstructions[45], si.beforeFadeOutBytes, sizeof(jmpToMmap));
    jumpOffset = (si.beforeFadeOutAddress + sizeof(si.gettingSoundHandler) + sizeof(jmpToMmap)) - (mmapJumpAddress + 58);
    memcpy(&flashbackSkipInstructions[54], &jumpOffset, sizeof(jumpOffset));
    
    memcpy(extraMemory + loadDetectionInstructionsSize, flashbackSkipInstructions, sizeof(flashbackSkipInstructions));
    
    // writing to game executable memory
    memcpy((unsigned char*)si.beforeFadeOutAddress, si.gettingSoundHandler, sizeof(si.gettingSoundHandler));
    memcpy((unsigned char*)(si.beforeFadeOutAddress + sizeof(si.gettingSoundHandler)), si.beforeFadeOutBytes, sizeof(si.beforeFadeOutBytes));
    jumpOffset = mmapJumpAddress - (si.beforeFadeOutAddress + sizeof(si.gettingSoundHandler) + 5);
    memcpy(&jmpToMmap[1], &jumpOffset, sizeof(jumpOffset));
    memcpy((unsigned char*)(si.beforeFadeOutAddress + sizeof(si.gettingSoundHandler)), jmpToMmap, sizeof(jmpToMmap));
}

static void injectWaitInstructions(SavedInstructions& si, unsigned char* extraMemory, const uint_t howManyNames, const uint32_t spacePerName)
{
    // this is jumped to after a call instruction, so the caller-saved registers shouldn't need to be saved
    unsigned char flashbackWaitInstructions[flashbackWaitInstructionsSize] = {
        0x53,                                               // 0000 // push ebx // stack depth +4
        0x56,                                               // 0001 // push esi // stack depth +8
        0x68, 0x00, 0x00, 0x00, 0x00,                       // 0002 // push stringObjectAddress // stack depth +12
        // putting SoundHandler object into eax. copy from si.gettingSoundHandler.
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     // 0007 //
        
        0x50,                                               // 0023 // push eax // stack depth +16 // cSoundHandler object
        0xbb, 0x00, 0x00, 0x00, 0x00,                       // 0024 // mov ebx, firstStringDataAddress
        0x31, 0xf6,                                         // 0029 // xor esi, esi
        0x90,                                               // 0031 // nop so loop starts on byte 32
        // start of loop
        0x89, 0x1d, 0x00, 0x00, 0x00, 0x00,                 // 0032 // mov dword ptr [stringObjectAddress], ebx
        0xe8, 0x00, 0x00, 0x00, 0x00,                       // 0038 // call cSoundHandler::IsPlaying
        0x09, 0xc6,                                         // 0043 // or esi, eax
        0x81, 0xc3, 0x00, 0x00, 0x00, 0x00,                 // 0045 // add ebx, spacePerName
        0x81, 0xfb, 0x00, 0x00, 0x00, 0x00,                 // 0051 // cmp ebx, loopStopAddress
        0x75, 0xe5,                                         // 0057 // jnz -27
        0x83, 0xfe, 0x00,                                   // 0059 // cmp esi, 0
        0x74, 0x1a,                                         // 0062 // jz 26
        0x8b, 0x1c, 0x24,                                   // 0064 // mov ebx, dword ptr [esp] // cSoundHandler object
        0xc1, 0xe6, 0x0a,                                   // 0067 // shl esi, 10 // changing esi from 1 to 1024 for approximately one millisecond
        0x89, 0x34, 0x24,                                   // 0070 // mov dword ptr [esp], esi
        0xe8, 0x00, 0x00, 0x00, 0x00,                       // 0073 // call FLwait
        0x89, 0x1c, 0x24,                                   // 0078 // mov dword ptr [esp], ebx // cSoundHandler object
        0xbb, 0x00, 0x00, 0x00, 0x00,                       // 0081 // mov ebx, firstStringDataAddress
        0x31, 0xf6,                                         // 0086 // xor esi, esi
        0xeb, 0xc6,                                         // 0088 // jmp -58
        // end of loop
        
        0x5e,                                               // 0090 // pop esi // stack depth +12
        0x5e,                                               // 0091 // pop esi // stack depth +8
        0x5e,                                               // 0092 // pop esi // stack depth +4
        0x5b,                                               // 0093 // pop ebx // stack depth +0
        0x8b, 0x43, 0x14,                                   // 0094 // mov eax, dword ptr [ebx + 0x14] // COPY THIS
        0x8b, 0x10,                                         // 0097 // edx, dword ptr [eax] // COPY THIS
        0xe9, 0x00, 0x00, 0x00, 0x00,                       // 0099 // jmp address of next instruction
        0x90,                                               // 0104 // nop
        0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,     // 0105 // INT3 filler
        0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,     // 0113 // INT3 filler
        0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc            // 0121 // INT3 filler
    };
    
    unsigned char jmpToMmap[] = {0xe9, 0x00, 0x00, 0x00, 0x00};
    
    uint_t jumpOffset = 0;
    uint_t mmapJumpAddress = (uint_t)extraMemory + loadDetectionInstructionsSize;
    uint_t stringObjectAddress = mmapJumpAddress + flashbackWaitInstructionsSize;
    // + 64 + (sizeof(uint_t) * 3) because 64 bytes are used for the string plus padding, and the string points to the byte after sizeof(uint_t) * 3
    uint_t firstStringDataAddress = stringObjectAddress + 64 + (sizeof(uint_t) * 3);
    uint_t loopStopAddress = firstStringDataAddress + (howManyNames * spacePerName);
    
    // writing to mmap memory
    memcpy(&flashbackWaitInstructions[3], &stringObjectAddress, sizeof(stringObjectAddress));
    memcpy(&flashbackWaitInstructions[7], si.gettingSoundHandler, sizeof(si.gettingSoundHandler));
    memcpy(&flashbackWaitInstructions[25], &firstStringDataAddress, sizeof(firstStringDataAddress));
    memcpy(&flashbackWaitInstructions[34], &stringObjectAddress, sizeof(stringObjectAddress));
    jumpOffset = si.isPlayingAddress - (mmapJumpAddress + 43);
    memcpy(&flashbackWaitInstructions[39], &jumpOffset, sizeof(jumpOffset));
    memcpy(&flashbackWaitInstructions[47], &spacePerName, sizeof(spacePerName));
    memcpy(&flashbackWaitInstructions[53], &loopStopAddress, sizeof(loopStopAddress));
    jumpOffset = si.flWaitAddress - (mmapJumpAddress + 78);
    memcpy(&flashbackWaitInstructions[74], &jumpOffset, sizeof(jumpOffset));
    memcpy(&flashbackWaitInstructions[82], &firstStringDataAddress, sizeof(firstStringDataAddress));
    memcpy(&flashbackWaitInstructions[94], si.mapLoadEndBytes, sizeof(si.mapLoadEndBytes));
    jumpOffset = (si.mapLoadEndAddress + sizeof(si.mapLoadEndBytes)) - (mmapJumpAddress + 104);
    memcpy(&flashbackWaitInstructions[100], &jumpOffset, sizeof(jumpOffset));
    
    memcpy(extraMemory + loadDetectionInstructionsSize, flashbackWaitInstructions, sizeof(flashbackWaitInstructions));
    
    // writing to game executable memory
    jumpOffset = mmapJumpAddress - (si.mapLoadEndAddress + 5);
    memcpy(&jmpToMmap[1], &jumpOffset, sizeof(jumpOffset));
    memcpy((unsigned char*)si.mapLoadEndAddress, jmpToMmap, sizeof(jmpToMmap));
}
#endif

static bool getMemfdName(char* memfdName, const size_t memfdNameBufferSize)
{
    const size_t memfdNameMaxSize = 249; // "The limit is 249 bytes, excluding the terminating null byte."
    const char fileWithMemfdName[] = "shared_memory_name.txt";
    
    int fd = open(fileWithMemfdName, O_RDONLY); // make sure this gets closed
    if (fd == -1)
    {
        printCstr("open syscall error when opening "); printCstr(fileWithMemfdName);
        printCstr(": "); printInt(errno); printCstr("\n"); 
        // printf("open syscall error when opening %s: %d\n", fileWithMemfdName, errno);
        return false;
    }
    
    size_t charactersRead = read(fd, &memfdName[0], memfdNameBufferSize);
    close(fd); // file closed here
    fd = -1;
    if (charactersRead == memfdNameBufferSize)
    {
        printCstr("ERROR: ");
        printCstr(fileWithMemfdName); printCstr(" should be shorter than "); printInt(memfdNameBufferSize); printCstr("bytes\n");
        // printf("ERROR: %s should be shorter than %zu bytes\n", fileWithMemfdName, memfdNameBufferSize);
        return false;
    }
    
    // removing non-alphanumeric characters and characters which aren't dashes or underscores
    size_t write_idx = 0;
    for (size_t i = 0; memfdName[i] != '\0'; i++)
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
    if (write_idx == 0)
    {
        printCstr("ERROR: empty shared memory name\n");
        return false;
    }
    else if (write_idx > memfdNameMaxSize)
    {
        printCstr("ERROR: shared memory name needs to be "); printInt(memfdNameMaxSize); printCstr(" characters or less\n");
        // printf("ERROR: shared memory name needs to be %zu characters or less\n", memfdNameMaxSize);
        return false;
    }
    
    return true;
}

static bool setupMemory(const bool skipFlashbacks, const bool delayFlashbacks)
{
    char memfdName[320]{};
    
    if (!getMemfdName(memfdName, sizeof(memfdName)))
    {
        return false;
    }
    
    uint_t gameStartAddress = 0;
    uint_t gameEndAddress = 0;
    
    const char* gameNames[4] = {
        "/Amnesia_NOSTEAM.bin.x86_64",
        "/Amnesia.bin.x86_64",
        "/Amnesia_NOSTEAM.bin.x86",
        "/Amnesia.bin.x86"
    };
    
    if (!findExecutableMemory(gameStartAddress, gameEndAddress, gameNames, sizeof(gameNames) / sizeof(char*)))
    {
        return false;
    }
    
    size_t gameSize = gameEndAddress - gameStartAddress;
    
    uint_t howManyNames = 0;
    uint_t longestName = 0;
    uint_t spacePerName = 0;
    uint_t nameAreaOffset = 0;
    // also extraMemorySize, which is global so the __attribute__((destructor)) function can access it
    
    SavedInstructions si;
    
    if (skipFlashbacks || delayFlashbacks)
    {
        const uint_t stringDataSize = sizeof(uint_t) * 3;
        const char flashbackNameFile[] = "flashback_names.txt";
        bool flashbackInjectionReady = true;
        
        // FileHelper object is only used in this area, so this scope is used so it doesn't stay allocated longer than it's needed
        {
            FileHelper fh(flashbackNameFile);
            if (fh.fd == -1) // error message will have been printed in the constructor
            {
                return false;
            }
            
            preprocessFlashbackNames(fh, howManyNames, longestName);
            
             // + 1 for null terminator
            spacePerName = (((longestName + stringDataSize + 1) / 64) + (((longestName + stringDataSize + 1) % 64) != 0)) * 64;
            // 64 bytes to store string object plus padding
            nameAreaOffset = loadDetectionInstructionsSize + (skipFlashbacks ? flashbackSkipInstructionsSize : flashbackWaitInstructionsSize) + 64;
            extraMemorySize = nameAreaOffset + (spacePerName * howManyNames);
            
            if (howManyNames == 0)
            {
                printCstr("ERROR: no flashback line names found in "); printCstr(flashbackNameFile); printCstr("\n");
                // printf("ERROR: no flashback line names found in %s\n", flashbackNameFile);
                flashbackInjectionReady = false;
            }
            
            if (spacePerName > 0x7fffffff) // rbx can't have an immediate value larger than this added to it
            {
                printCstr("ERROR: flashback line names can't be longer than 2147483647\n");
                flashbackInjectionReady = false;
            }
            
            if (!setupMemfdPages(memfdName, extraMemorySize))
            {
                return false;
            }
            
            if (!setFlashbackNames((unsigned char*)mmapAddress, fh, nameAreaOffset, spacePerName, extraMemorySize))
            {
                flashbackInjectionReady = false;
            }
        }
        
        if (!flashbackInjectionReady)
        {
            printCstr("can't inject flashback skip/wait instructions\n");
        }
        
        if (!findInstructions(si, (unsigned char*)gameStartAddress, gameSize))
        {
            return false;
        }
        
        if (mprotect((void*)gameStartAddress, gameSize, PROT_READ | PROT_WRITE) != 0)
        {
            printCstr("ERROR: mprotect failure when setting game memory access protection to PROT_READ | PROT_WRITE: "); printInt(errno); printCstr("\n");
            // printf("ERROR: mprotect failure when setting game memory access protection to PROT_READ | PROT_WRITE: %d\n", errno);
            return false;
        }
        
        injectLoadDetectionInstructions(si, (unsigned char*)mmapAddress);
        
        if (flashbackInjectionReady)
        {
            if (skipFlashbacks)
            {
                injectSkipInstructions(si, (unsigned char*)mmapAddress, howManyNames, (uint32_t)spacePerName);
            }
            else
            {
                injectWaitInstructions(si, (unsigned char*)mmapAddress, howManyNames, (uint32_t)spacePerName);
            }
        }
        
        if (mprotect((void*)gameStartAddress, gameSize, PROT_READ | PROT_EXEC) != 0)
        {
            printCstr("WARNING: mprotect failure when setting game memory back to PROT_READ | PROT_EXEC: "); printInt(errno); printCstr("\n");
            // printf("WARNING: mprotect failure when setting game memory back to PROT_READ | PROT_EXEC: %d\n", errno);
            return false;
        }
    }
    else
    {
        if (!setupMemfdPages(memfdName, loadDetectionInstructionsSize))
        {
            return false;
        }
        
        if (!findInstructions(si, (unsigned char*)(gameStartAddress), gameSize))
        {
            return false;
        }
        
        if (mprotect((void*)gameStartAddress, gameSize, PROT_READ | PROT_WRITE) != 0)
        {
            printCstr("ERROR: mprotect failure when setting game memory access protection to PROT_READ | PROT_WRITE: "); printInt(errno); printCstr("\n");
            // printf("ERROR: mprotect failure when setting game memory access protection to PROT_READ | PROT_WRITE: %d\n", errno);
            return false;
        }
        
        injectLoadDetectionInstructions(si, (unsigned char*)mmapAddress);
        
        if (mprotect((void*)gameStartAddress, gameSize, PROT_READ | PROT_EXEC) != 0)
        {
            printCstr("WARNING: mprotect failure when setting game memory back to PROT_READ | PROT_EXEC: "); printInt(errno); printCstr("\n");
            // printf("WARNING: mprotect failure when setting game memory back to PROT_READ | PROT_EXEC: %d\n", errno);
            return false;
        }
    }
    
    return true;
}

static bool readSettingsFile(bool& skipFlashbacks, bool& delayFlashbacks, bool& delayFiles)
{
    // make sure this is small enough to fit in the buffer
    char defaultText[] = "skip flashbacks: n\ndelay flashbacks: y\ndelay files: n\n";
    
    char buffer[256]{};
    const char settingsFileName[] = "amnesia_settings.txt";
    
    char skipFlashbacksSettingName[] = "skip flashbacks";
    char delayFlashbacksSettingName[] = "delay flashbacks";
    char delayFilesSettingName[] = "delay files";
    
    bool skipFlashbacksFound = false;
    bool delayFlashbacksFound = false;
    bool delayFilesFound = false;
    
    int fd = open(settingsFileName, O_RDONLY); // make sure this gets closed (1)
    
    if (fd == -1)
    {
        printCstr("open syscall error when opening "); printCstr(settingsFileName);
        printCstr(": "); printInt(errno); printCstr("\n");
        // printf("open syscall error when opening %s: %d\n", settingsFileName, errno);
        return false;
    }
    
    size_t charactersRead = read(fd, &buffer[0], sizeof(buffer));
    close(fd); // file closed here (1)
    fd = -1;
    if (charactersRead == sizeof(buffer))
    {
        printCstr("ERROR: "); printCstr(settingsFileName); printCstr(" should be shorter than ");
        printInt(sizeof(buffer)); printCstr(" bytes\nresetting "); printCstr(settingsFileName); printCstr("\n");
        // printf("ERROR: %s should be shorter than %zu bytes\nresetting %s\n", settingsFileName, sizeof(buffer), settingsFileName);
        fd = open(settingsFileName, O_WRONLY | O_TRUNC); // make sure this gets closed (2)
        if (write(fd, defaultText, sizeof(defaultText) - 1) == -1) // - 1 because the null character isn't needed
        {
            printCstr("ERROR: couldn't reset "); printCstr(settingsFileName);
            printCstr(": "); printInt(errno); printCstr("\n");
            // printf("ERROR: couldn't reset %s: %d\n", settingsFileName, errno);
        }
        close(fd); // file closed here (2)
        fd = -1;
        return false;
    }
    
    size_t bufferIdx = 0;
    
    // bufferIdx += (bufferIdx < charactersRead) makes it go to the start of the next line
    for (; bufferIdx < charactersRead; bufferIdx += (bufferIdx < charactersRead))
    {
        size_t lineStartIdx = bufferIdx;
        
        bool settingOnOrOff = false;
        size_t settingNameLength = 0;
        
        while (bufferIdx < charactersRead && buffer[bufferIdx] != ':' && buffer[bufferIdx] != '\n') // find length of setting name
        {
            bufferIdx += 1;
            settingNameLength += 1;
        }
        if (bufferIdx == charactersRead || buffer[bufferIdx] == '\n') // no colon found on line
        {
            continue;
        }
        
        while (bufferIdx < charactersRead && buffer[bufferIdx] != '\n') // find y or n option
        {
            if (buffer[bufferIdx] == 'y' || buffer[bufferIdx] == 'Y' || buffer[bufferIdx] == 'n' || buffer[bufferIdx] == 'N')
            {
                settingOnOrOff = (buffer[bufferIdx] == 'y' || buffer[bufferIdx] == 'Y');
                break;
            }
            
            bufferIdx += 1;
        }
        if (bufferIdx == charactersRead || buffer[bufferIdx] == '\n') // y or n option not found on line
        {
            continue;
        }
        
        while (bufferIdx < charactersRead && buffer[bufferIdx] != '\n') // read to end of line
        {
            bufferIdx += 1;
        }
        
        if (myStrncmp(&buffer[lineStartIdx], skipFlashbacksSettingName, settingNameLength) == 0)
        {
            skipFlashbacks = settingOnOrOff;
            skipFlashbacksFound = true;
        }
        else if (myStrncmp(&buffer[lineStartIdx], delayFlashbacksSettingName, settingNameLength) == 0)
        {
            delayFlashbacks = settingOnOrOff;
            delayFlashbacksFound = true;
        }
        else if (myStrncmp(&buffer[lineStartIdx], delayFilesSettingName, settingNameLength) == 0)
        {
            delayFiles = settingOnOrOff;
            delayFilesFound = true;
        }
    }
    
    if (!(skipFlashbacksFound && delayFlashbacksFound && delayFilesFound))
    {
        printCstr("ERROR: couldn't find all settings in "); printCstr(settingsFileName);
        printCstr("\nresetting "); printCstr(settingsFileName); printCstr("\n");
        // printf("ERROR: couldn't find all settings in %s\nresetting %s\n", settingsFileName, settingsFileName);
        fd = open(settingsFileName, O_WRONLY | O_TRUNC); // make sure this gets closed (3)
        if (write(fd, defaultText, sizeof(defaultText) - 1) == -1) // - 1 because the null character isn't needed
        {
            printCstr("ERROR: couldn't reset "); printCstr(settingsFileName);
            printCstr(": "); printInt(errno); printCstr("\n");
            // printf("ERROR: couldn't reset %s: %d\n", settingsFileName, errno);
        }
        close(fd); // file closed here (3)
        fd = -1;
        return false;
    }
    
    return true;
}

static void freeResources()
{
    if (memfd != -1)
    {
        close(memfd);
        memfd = -1;
    }
    if (mmapAddress != MAP_FAILED)
    {
        std::atomic_ref<unsigned char> timerByteAtomicRef(*((unsigned char*)mmapAddress));
        timerByteAtomicRef.store(255);
        munmap(mmapAddress, extraMemorySize);
        mmapAddress = MAP_FAILED;
    }
}

__attribute__((constructor)) void readSettingsAndGetResources()
{
#if __x86_64__ || __ppc64__
    
#else
    printCstr("ATTENTION: Speedrunning on the 64-bit version of Amnesia TDD saves time due to permanent slippery physics.\n\
If you have a 64-bit computer, you should speedrun on the 64-bit version of Amnesia TDD instead.\n");
#endif
    bool skipFlashbacks = false;
    bool delayFlashbacks = false;
    bool delayFiles = false;
    
    if (!readSettingsFile(skipFlashbacks, delayFlashbacks, delayFiles))
    {
        return;
    }
    
    if (!setupMemory(skipFlashbacks, delayFlashbacks))
    {
        freeResources();
        
        return;
    }
    
    delaysActive = delayFiles; // this is in load_extender.h and determines if the file delay function gets called
    
    printCstr("amnesia injected successfully.\n");
}

__attribute__((destructor)) void freeResourcesEnd()
{
    freeResources();
}

