//
// config.cpp
//
#include <stdio.h>
#include <unistd.h> // access()
#include <ctype.h>  // isspace()
#include <errno.h>
#include <string.h>
#include <assert.h>
#include "config.h"

//
// Class CConfig implementation
//
bool Config::Init(const char* fileName, const char* key)
{
    assert(fileName);
    assert(key);
    
    memset(mFileName, 0, sizeof(mFileName));
    memset(mKey, 0, sizeof(mKey));
    mIsValid = false;
    
    // Check if the file exist
    if(access(fileName, 0) == 0)
    {
        // Validate key size
        if(strlen(key) < sizeof(mKey))
        {
            // Check key for existence
            if(ReadValue(fileName, key, nullptr, nullptr, 0))
            {
                // Success - the file exist and has the key
                strcpy(mFileName, fileName);
                strcpy(mKey, key);
                mIsValid = true;
            }
            else
            {
                printf("%s: The key '%s' is not found in the configuration file \"%s\"\n",
                       __func__, key, fileName);
            }
        }
        else
        {
            printf("%s: The key '%s' exeeds %lu bytes\n", __func__, key, sizeof(mKey)-1);
        }
    }
    else
    {
        printf("%s: Configuration file \"%s\" does not exist\n", __func__, fileName);
    }
    
    return mIsValid;
}

bool Config::GetBoolValue(const char* name, bool& val) const
{
    char buf[MAX_CONFIG_LINE]{};
    if(!ReadValue(mFileName, mKey, name, buf, sizeof(buf)-1))
        return false;
    
    if(strcasecmp(buf, "true") == 0)
    {
        val = true;
    }
    else if(strcasecmp(buf, "false") == 0)
    {
        val = false;
    }
    else
    {
        return false;
    }
    
    return true;
}

bool Config::GetStringValue(const char* name, char* val, int len) const
{
    return ReadValue(mFileName, mKey, name, val, len);
}

bool Config::GetIntValue(const char* name, int& val) const
{
    char buf[MAX_CONFIG_LINE]{};
    if(!ReadValue(mFileName, mKey, name, buf, sizeof(buf)-1))
        return false;
    
    int value = 0;
    if(sscanf(buf, "%d", &value) != 1)
        return false;
    
    val = value;
    return true;
}

void Config::SetBoolValue(const char* name, bool val)
{
    WriteValue(mFileName, mKey, name, val ? "true" : "false");
}

void Config::SetStringValue(const char* name, const char* buf)
{
    WriteValue(mFileName, mKey, name, buf);
}

void Config::SetIntValue(const char* name, int val)
{
    if(name == nullptr)
        return;
    
    char buf[64];
    
    if(sprintf(buf, "%d", val) < 0)
    {
        printf("%s: sprintf failed for name=%s\n", __func__, name);
        return;
    }
    
    WriteValue(mFileName, mKey, name, buf);
}

char* Config::TrimLine(char* line)
{
    // Trimming whitespace (both side)
    if(line == nullptr)
        return nullptr;

    // Trim left side
    char* ptr = line;
    while(isspace(*ptr))
        ptr++;

    line = ptr;

    // Trim right side
    char* ptrLast = nullptr;
    while(*ptr != '\0')
    {
        //if(*ptr == ' ')
        if(isspace(*ptr))
        {
            if(ptrLast == nullptr)
                ptrLast = ptr;
        }
        else
        {
            ptrLast = nullptr;
        }
        ptr++;
    }

    // Truncate at trailing spaces
    if(ptrLast != nullptr)
        *ptrLast = '\0'; 

    // Remove new-line character at the end - if any
    if(*(ptr - 1) == '\n')
        *(ptr - 1) = '\0';

    return line;
}

bool Config::IsKey(char* line, const char* key)
{
    if(key == nullptr || line == nullptr)
        return false;

    // Check if line is a settings key 

    // Trimming whitespace on both sides
    char* ptr = TrimLine(line);

    // Check for key beginning
    if(ptr[0] != '[') 
        return false;
    ptr++;

    // Check for key ending
    int len = (int)strlen(ptr);
    if(ptr[len-1] != ']') 
        return false;
    ptr[len-1] = '\0';

    // Trimming whitespace
    //ptr = TrimLine(ptr); 

    // Is key match?
    return (strcasecmp(ptr, key) == 0);
}

bool Config::IsValue(char* line, bool* isEmptyLine, char** ppValueName, char** ppValue)
{
    if(line == nullptr || ppValueName == nullptr || ppValue == nullptr)
        return false;

    // Read parameter name and value
    char* valueName = line;
    char* value = nullptr;

    // Find name-value delimeter
    char* ptr = strchr(line, '=');
    if(ptr == nullptr)
    {
        // Value not set or invalid format.        
        // Check for an empty line
        ptr = TrimLine(line);
        if(*ptr == '\0' && isEmptyLine != nullptr)
            *isEmptyLine = true;
        return false; 
    }

    // Trimming whitespace on both name and value
    *ptr = '\0';
    valueName = TrimLine(valueName);
    value = TrimLine(ptr + 1);

    //
    // Check value name for open " and close "
    //
    int len = (int)strlen(valueName);
    if(valueName[0] == '"' && valueName[len-1] == '"')
    {
        valueName[0] = '\0';
        valueName[len-1] = '\0';
        valueName++;
    }
    else if(valueName[0] == '"' && valueName[len-1] != '"')
        return false; // Invalid parameter name
    else if(valueName[0] != '"' && valueName[len-1] == '"')
        return false; // Invalid parameter name

    //
    // Get value itself
    //
    len = (int)strlen(value);
    if(value[0] == '"' && value[len-1] == '"')
    {
        value[0] = '\0';
        value[len-1] = '\0';
        value++;
    }
    else if(value[0] == '"' && value[len-1] != '"')
        return false; // Invalid parameter name
    else if(value[0] != '"' && value[len-1] == '"')
        return false; // Invalid parameter name

    *ppValueName = valueName;
    *ppValue = value;
    return true;
}

bool Config::IsComment(const char* line)
{
    if(line == nullptr)
        return false;

    // Check if line is a comment
	// Trim whitespaces on the left side
	const char* ptr = line;
	while(isspace(*ptr))
		ptr++;
    return (*ptr == '#');
}                   

bool Config::ReadValue(const char* confFile, const char* key,
                        const char* name, char* buf, unsigned int bufSize)
{
    if(confFile == nullptr || confFile[0] == '\0' || key == nullptr || key[0] == '\0')
        return false;
    
    // Note: If name == nullptr then just check key for existence
    
    FILE* file = fopen(confFile, "r"); // Open for reading
    if(file == nullptr)
    {
        printf("%s: Failed to open configuration file \"%s\" for writing: %s\n",
                __func__, confFile, strerror(errno));
        return false;
    }
    
    // Set enumerate=false to only read the first value
    bool res = ReadNextValue(file, key, name, buf, bufSize, false /*enumerate*/, [](const char*){return false;});
    
    fclose(file);
    return res;
}

bool Config::WriteValue(const char* confFile, const char* key,
    const char* name, const char* value, bool bRemoveKey /* false */)
{
    if(confFile == nullptr || confFile[0] == '\0' || key == nullptr || key[0] == '\0')
        return false;

    // Open confituration file for reading
    FILE* file = fopen(confFile, "r");
    if(file == nullptr)
    {
        printf("%s: Failed to open configuration file \"%s\" for writing: %s\n",
               __func__, confFile, strerror(errno));
        return false;
    }

    // Create temporary file name and file to write new configuration 
    FILE* fileTmp = nullptr;
    char tmp_fileName[PATH_MAX]{};
    strcpy(tmp_fileName, confFile);
    strcat(tmp_fileName, "XXXXXX");
    
    if(GetTmpFileName(tmp_fileName))
        fileTmp = fopen(tmp_fileName, "w"); 

    if(fileTmp == nullptr)
    {
        printf("%s: Failed to open new configuration file \"%s\" for writing: %s\n",
               __func__, confFile, strerror(errno));

        if(file != nullptr)
            fclose(file);
        return false;
    }

    //
    // Process configuration file
    //
    bool bIsError = false;

    try
    {
        char line[MAX_CONFIG_LINE]{};
        char buf[MAX_CONFIG_LINE]{};
        bool bKeyFound = false;
        bool bLineFound = false;
        bool bEndOfSection = false; // end of section
        char* valueName = nullptr;
        char* lpOldValue = nullptr;

        // Cycle until end of file reached
        while(fgets(line, sizeof(line), file) != nullptr)
        {
            // Skip the comment line
            if(IsComment(line))
                continue; // skip the comment line

            if(!bLineFound)
            {
                //
                // Replace existed value line or add new one
                //
                strcpy(buf, line); // copy line to buffer

                if(!bKeyFound) 
                {
                    // Check line for a key 
                    bKeyFound = IsKey(buf, key);
                }
                else
                {
                    valueName = nullptr;
                    lpOldValue = nullptr;

                    // Check line for a value
                    if(IsValue(buf, &bEndOfSection, &valueName, &lpOldValue))
                    {
                        // If we are not going to remove whole key, then
                        // compare line name with given one
                        if(!bRemoveKey && strcasecmp(valueName, name) == 0)
                        {
                            // Line for given value is found
                            bLineFound = true;

                            if(value != nullptr)
                            {
                                // Update line with new value
                                sprintf(line, "\"%s\"=\"%s\"\n", name, value);
                            }
                            else
                            {
                                // Remove this line - just skip it and don't write
                                continue;
                            }
                        }
                    }
                    else if(bEndOfSection)
                    {
                        // Get an empty line - end of section
                        bLineFound = true;

                        if(value != nullptr)
                        {
                            // Add new line for this value.
                            // Note: we also need to add an empty line
                            sprintf(line, "\"%s\"=\"%s\"\n\n", name, value);
                        }
                    }
                }
            }

            if(bRemoveKey && bKeyFound)
            {
                // Remove whole section under this key, including empty line
                if(bEndOfSection)
                    bRemoveKey = false; // Key is removed
                continue; 
            }

            // Write line to temporary configuration file 
            if(fputs(line, fileTmp) < 0)
            {
                printf("%s: Failed to write in new configuration file: %s\n", __func__, strerror(errno));
                bIsError = true;
                break;
            }
        }

        // Check for end-of-file
        if(feof(file) == 0)
        {
            // It's not end of file
            if(!bIsError)
            {
                printf("%s: Error in reading configuration file: %s\n", __func__, strerror(errno));
                bIsError = true;
            }
        }
        else if(!bKeyFound)
        {
            // No key found, create new key, new line and empty line
            // and write them to temporary configuration file 
            sprintf(line, "[%s]\n\"%s\"=\"%s\"\n\n", key, name, value);
            if(fputs(line, fileTmp) < 0)
            {
                printf("%s: Failed to write new key and value in new configuration file: %s\n",
                       __func__, strerror(errno));
                bIsError = true;
            }
            bKeyFound = true;
        }
    }
    catch(...)
    {
        bIsError = true;
        printf("%s: Failed to read configuration file: Unknown error\n", __func__);
    }

    // Close both files
    if(file != nullptr)
        fclose(file);
    if(fileTmp != nullptr)
        fclose(fileTmp); 

    if(bIsError)
        return false;

    //
    // Swap temporary and configuration files
    //

    // Create backup copy of the confituration file
    char conf_backup[PATH_MAX]{};
    strcpy(conf_backup, confFile);
    strcat(conf_backup, "XXXXXX");

    if(!GetTmpFileName(conf_backup) || rename(confFile, conf_backup) != 0)
    {
        printf("%s: Failed to create backup configuration file: %s\n", __func__, strerror(errno));
        remove(tmp_fileName); // Remove temporary file
        return false;
    }

    // Rename temporary configuration file to a base configuration file
    if(rename(tmp_fileName, confFile) != 0)
    {
        printf("%s: Failed to rename temporary configuration file: %s\n", __func__, strerror(errno));
        remove(tmp_fileName); // Remove temporary file
        rename(conf_backup, confFile); // Restore configuration file from backup
        return false;
    }

    // Remove backup copy
    remove(conf_backup); 

    return true; 
}

bool Config::GetTmpFileName(char* templateName)
{
    if(templateName == nullptr)
        return false;

    int i = (int)strlen(templateName);
    char* ptr = templateName + i - 1;

    for(i=0; *ptr == 'X'; i++)
        ptr--;
    ptr++; // point to first 'X'

    if(i < 6)
        return false; // Template Name should have six 'X' at the end

    i = 1;
    do 
    {
        sprintf(ptr, "%d", i);
        i++;
    }
    while(access(templateName, 0) == 0);

    return true;
}

