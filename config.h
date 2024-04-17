//
// config.h
//
#ifndef _CONFIG_H_
#define _CONFIG_H_

#include <limits.h> // for PATH_MAX
#include <errno.h>
#include <string.h>

#define MAX_CONFIG_LINE 1024

//
// Class Config
//
class Config
{
public:
    Config() : mIsValid(false) { mFileName[0] = 0; mKey[0] = 0; }
    Config(const char* fileName, const char* key) : Config() { Init(fileName, key); }
    ~Config() {}

    bool Init(const char* fileName, const char* key);
    bool IsValid() const { return mIsValid; }
    const char* GetFileName() const { return mFileName; }
    
    bool GetBoolValue(const char* name, bool& val) const;
    bool GetIntValue(const char* name, int& val) const;
    bool GetStringValue(const char* name, char* val, int len) const;
    
    void SetBoolValue(const char* name, bool val);
    void SetIntValue(const char* name, int val);
    void SetStringValue(const char* name, const char* buf);
    
    bool LookupKey(const char* key) const { return ReadValue(mFileName, key, nullptr, nullptr, 0); }
    bool DeleteValue(const char* name) { return WriteValue(mFileName, mKey, name, nullptr); }
    bool DeleteKey() { return WriteValue(mFileName, mKey, nullptr, nullptr, true); }

    template<typename Func>
    bool EnumValue(const char* name, Func fNotify) const;

protected:
    static bool ReadValue(const char* confFile,
                          const char* key, const char* name,
                          char* buf, unsigned int bufSize);
    static bool WriteValue(const char* confFile,
                           const char* key, const char* name, const char* value,
                           bool bRemoveKey = false);
    
    template<typename Func>
    static bool ReadNextValue(FILE* file, const char* key, const char* name,
                              char* buf, unsigned int bufSize, bool enumerate, Func fNotify);
    
    // Check if line is a key
    static bool IsKey(char* line, const char* key);

    // Read value and value name
    static bool IsValue(char* line, bool* isEmptyLine, char** ppValueName, char** ppValue);

    // Check if line is a comment
    static bool IsComment(const char* line);

    // Trimming whitespace (both side)
    static char* TrimLine(char* line);
    
    static bool GetTmpFileName(char* templateName);
    
private:
    char mFileName[PATH_MAX]{};
    char mKey[256]{};
    bool mIsValid{false};
};

template<typename Func>
bool Config::ReadNextValue(FILE* file, const char* key, const char* name,
                           char* buf, unsigned int bufSize, bool enumerate, Func fNotify)
{
    if(file == nullptr)
        return false;
    
    // Note: If name == nullptr then just check key for existence
    
    char* valueName = nullptr;
    char* value = nullptr;
    bool bKeyFound = false;
    bool bEndOfFile = false;
    bool bEndOfSection = false; // empty line
    bool bEnumAborted = false;
    
    try
    {
        char line[MAX_CONFIG_LINE]{};
        bool bLineFound = false;
        
        // Cycle until end of file reached
        while(fgets(line, sizeof(line), file) != nullptr)
        {
            // Skip the comment line or an empty line
            if(IsComment(line))
                continue; // skip the comment line
            
            if(!bKeyFound)
            {
                // Check line for a key
                bKeyFound = IsKey(line, key);
            }
            else
            {
                if(name == nullptr)
                    break; // We only interested in key existence
                
                // Check line for a value
                if(IsValue(line, &bEndOfSection, &valueName, &value) &&
                   strcasecmp(valueName, name) == 0)
                {
                    // If we are enumerating all values - then keep going.
                    // Otherwise, break on first value found
                    if(enumerate)
                    {
                        bEnumAborted = !fNotify(value);
                    }
                    else
                    {
                        bLineFound = true;
                        break;
                    }
                }
                
                valueName = nullptr;
                value = nullptr;
                
                if(bEndOfSection)
                    break; // Reach end of section (empty line)
                if(bEnumAborted)
                    break; // Enumeration aborted
            }
        }
        
        // Check end-of-file reached
        bEndOfFile = feof(file);
        
        if(!bEndOfFile)
        {
            // It's not the end of file.
            // We might stoped reading file because of:
            // - line was found,
            // - empty line was found
            // - enumeration was aborted
            // - we only need to find key and it was found
            if(bLineFound || bEndOfSection || bEnumAborted || (bKeyFound && name == nullptr))
                ; // Not an error, one of above condition met
            else
                printf("%s: Error in reading configuration file: %s\n", __func__, strerror(errno));
        }
    }
    catch(...)
    {
        printf("%s: Failed to read configuration file: Unknown error\n", __func__);
    }
    
    if(enumerate)
    {
        // We were enumerating
        if(bEndOfFile || bEndOfSection) // End-of-file or end-of-section reached
            return true;
        else
            return false; // Enumeration aborted
    }
    else
    {
        if(name == nullptr)
        {
            // We are only interesed in key existence
            return bKeyFound;
        }
        else if(value != nullptr)
        {
            // The value is found
            size_t valSize = strlen(value);
            
            if(buf != nullptr && valSize < bufSize)
            {
                strcpy(buf, value);
                return true;
            }
            else
            {
                printf("%s: The %d bytes buffer is not sufficient for %zu bytes value of '%s'\n",
                       __func__, bufSize, valSize, name);
                return false;
            }
        }
        else
        {
            // The value is not found
            printf("%s: Cannot find the value of '%s' under the key '%s'\n", __func__, name, key);
            return false;
        }
    }
}

template<typename Func>
bool Config::EnumValue(const char* name, Func fNotify) const
{
    if(name == nullptr || name[0] == '\0')
        return false;
    
    FILE* file = fopen(mFileName, "r"); // Open for reading
    if(file == nullptr)
    {
        printf("%s: Failed to open configuration file \"%s\" for writing: %s\n",
               __func__, mFileName, strerror(errno));
        return false;
    }
    
    char buf[MAX_CONFIG_LINE]{};
    bool res = ReadNextValue(file, mKey, name, buf, sizeof(buf)-1, true, fNotify);
    
    fclose(file);
    return res;
}

#endif // _CONFIG_H_
