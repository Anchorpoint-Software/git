#include "virtual_fs.h"
#include <stddef.h>
#include <stdint.h>
#include <WinSock2.h>
#include <windows.h>
#include <stdio.h>
#include "git-compat-util.h"
#include "abspath.h"
#include "hash.h"
#include "hex.h"

static int execute_cli_process(const char *args[], char *outputBuffer, size_t outputBufferSize, DWORD *exitCode) 
{
    HANDLE hStdoutReadPipe, hStdoutWritePipe;
    HANDLE hStderrReadPipe, hStderrWritePipe;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    PROCESS_INFORMATION pi = { 0 };
    STARTUPINFO si = { 0 };
    DWORD bytesRead = 0;
    BOOL success;
    char *commandLine;
    size_t commandLineLength = 0;

    // Calculate total length for the command line
    for (size_t i = 0; args[i] != NULL; i++) {
        commandLineLength += strlen(args[i]) + 3; // Add space for quotes and space
    }

    // Allocate memory for the command line
    commandLine = (char *)malloc(commandLineLength + 1);
    if (!commandLine) {
        error("Failed to allocate memory for ap.exe command line.");
        return -1;
    }
    commandLine[0] = '\0';

    // Construct the command line
    for (size_t i = 0; args[i] != NULL; i++) {
        snprintf(commandLine + strlen(commandLine), commandLineLength - strlen(commandLine), "\"%s\" ", args[i]);
    }

    // Create pipes for stdout and stderr
    if (!CreatePipe(&hStdoutReadPipe, &hStdoutWritePipe, &sa, 0) ||
        !CreatePipe(&hStderrReadPipe, &hStderrWritePipe, &sa, 0)) {
        error("Failed to create pipes. Error: %lu", GetLastError());
        free(commandLine);
        return -1;
    }

    // Ensure read handles are not inherited
    if (!SetHandleInformation(hStdoutReadPipe, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(hStderrReadPipe, HANDLE_FLAG_INHERIT, 0)) {
        error("Failed to set pipe handle information. Error: %lu", GetLastError());
        CloseHandle(hStdoutReadPipe);
        CloseHandle(hStdoutWritePipe);
        CloseHandle(hStderrReadPipe);
        CloseHandle(hStderrWritePipe);
        free(commandLine);
        return -1;
    }

    // Configure STARTUPINFO to redirect stdout and stderr
    si.cb = sizeof(STARTUPINFO);
    si.hStdOutput = hStdoutWritePipe;
    si.hStdError = hStderrWritePipe;
    si.dwFlags |= STARTF_USESTDHANDLES;

    // Create the process
    success = CreateProcess(
        NULL,               // Application name
        commandLine,        // Command line
        NULL,               // Process security attributes
        NULL,               // Thread security attributes
        TRUE,               // Inherit handles
        CREATE_NO_WINDOW,   // Creation flags to prevent console window
        NULL,               // Environment
        NULL,               // Current directory
        &si,                // Startup info
        &pi                 // Process information
    );

    // Close write pipes in parent process
    CloseHandle(hStdoutWritePipe);
    CloseHandle(hStderrWritePipe);
    free(commandLine);

    if (!success) {
        CloseHandle(hStdoutReadPipe);
        CloseHandle(hStderrReadPipe);
        return -1;
    }

    // Read from stderr pipe
    bytesRead = 0;
    while (TRUE) {
        DWORD bytesAvailable = 0;
        DWORD chunkSize = 0;
        DWORD readBytes = 0;

        if (!PeekNamedPipe(hStderrReadPipe, NULL, 0, NULL, &bytesAvailable, NULL)) 
            break;

        if (bytesAvailable == 0) {
            // No data, wait a bit or break
            Sleep(10);
            continue;
        }

        chunkSize = min(bytesAvailable, outputBufferSize - bytesRead - 1);
        if (!ReadFile(hStderrReadPipe, outputBuffer + bytesRead, chunkSize, &readBytes, NULL)) 
            break;

        bytesRead += readBytes;
        if (bytesRead >= outputBufferSize - 1) break;
    }

    // Null-terminate the error buffer
    outputBuffer[bytesRead] = '\0';

    // Wait for the process to complete and retrieve the exit code
    WaitForSingleObject(pi.hProcess, INFINITE);
    if (!GetExitCodeProcess(pi.hProcess, exitCode)) {
        error("Failed to get exit code for ap.exe. Error: %lu", GetLastError());
        CloseHandle(hStdoutReadPipe);
        CloseHandle(hStderrReadPipe);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return -1;
    }

    // Clean up
    CloseHandle(hStdoutReadPipe);
    CloseHandle(hStderrReadPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return 0;
}

static void normalize_directory_name(char *path) {
    size_t len = strlen(path);
    if (len > 0 && path[len - 1] == '\\') {
        path[len - 1] = '\0';
    }
}

static int compare_versions(const void *a, const void *b) {
    const char *verA = *(const char **)a;
    const char *verB = *(const char **)b;

    int numA[3] = {0}, numB[3] = {0};
    if (sscanf(verA, "%d.%d.%d", &numA[0], &numA[1], &numA[2]) < 1) return -1;
    if (sscanf(verB, "%d.%d.%d", &numB[0], &numB[1], &numB[2]) < 1) return 1;

    for (int i = 0; i < 3; i++) {
        if (numA[i] < numB[i]) return -1;
        if (numA[i] > numB[i]) return 1;
    }
    return 0;
}

static char *get_install_folder(void) {
    static char installDirectory[1024] = "";

    if (strlen(installDirectory) == 0) {
        #ifndef GIT_WINDOWS_NATIVE
            strcpy(installDirectory, "/Applications/Anchorpoint.app/Contents/Frameworks");
        #else
            char anchorpointVersionsPath[1024];
            char *anchorpointVersions[256];
            char *localAppData = getenv("LOCALAPPDATA");
            int count = 0;
            struct dirent *entry;
            DIR *dir = NULL;

            if (!localAppData) {
                return NULL;
            }
            
            snprintf(anchorpointVersionsPath, sizeof(anchorpointVersionsPath), "%s\\Anchorpoint", localAppData);

            dir = opendir(anchorpointVersionsPath);
            if (!dir) return NULL;

            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_name[0] == '.' || strlen(entry->d_name) < 4 || strncmp(entry->d_name, "app-", 4) != 0) continue;
                anchorpointVersions[count++] = strdup(entry->d_name);
            }
            closedir(dir);

            if (count == 0) return NULL;

            qsort(anchorpointVersions, count, sizeof(char *), compare_versions);

            snprintf(installDirectory, sizeof(installDirectory), "%s\\%s", anchorpointVersionsPath, anchorpointVersions[count - 1]);
            normalize_directory_name(installDirectory);

            for (int i = 0; i < count; i++) free(anchorpointVersions[i]);
        #endif
    }
    return installDirectory;
}

static char *get_ap_cli_path(void) {
    static char cliPath[1024] = "";
    
    if (strlen(cliPath) == 0) {
        const char *installFolder = get_install_folder();
        if (strlen(installFolder) == 0) return NULL;
    
        #ifdef GIT_WINDOWS_NATIVE
            snprintf(cliPath, sizeof(cliPath), "%s\\ap.exe", installFolder);
        #else
            snprintf(cliPath, sizeof(cliPath), "%s/ap", installFolder);
        #endif
    }

    fprintf(stderr, "CLI Path: %s\n", cliPath);

    return cliPath;
}

static int get_error_from_json(const char *jsonBuffer, char* errorBuffer, size_t errorBufferSize) 
{
    size_t value_length;
    char* key_pos = NULL;
    const char* key = "\"error\"";
    const char* value_start = NULL;

    if (jsonBuffer == NULL) { 
        return -1; 
    }

    key_pos = strstr(jsonBuffer, key); // Find the "error" key

    if (key_pos == NULL) {
        return -2; // Key not found
    }

    // Move to the value after the key
    key_pos += strlen(key);

    // Skip whitespace and the colon
    while (*key_pos && (isspace((unsigned char)*key_pos) || *key_pos == ':')) {
        key_pos++;
    }

    // Check if the value is a string
    if (*key_pos != '\"') {
        return -3; // Not a valid string value
    }

    // Extract the value between the quotes
    key_pos++; // Skip the opening quote
    value_start = key_pos;
    while (*key_pos && *key_pos != '\"') {
        key_pos++;
    }

    if (*key_pos != '\"') {
        return -4; // Closing quote not found
    }

    value_length = key_pos - value_start;
    if (value_length >= errorBufferSize) {
        return -5; // Buffer too small
    }

    snprintf(errorBuffer, errorBufferSize, "%.*s", (int)value_length, value_start);

    return 0;
}

static int _create_placeholder(const char *path, unsigned int size, const struct object_id *oid) 
{
    char oid_hex[GIT_MAX_HEXSZ + 1];
    char sizeStr[32];
    DWORD exitCode;
    int result;
    char outputBuffer[1024];
    const char *ap_cli_path = get_ap_cli_path();
    const char *args[] = { ap_cli_path, "--json", "vfs", "create", "--path", absolute_path(path), "--size", NULL, "--id", NULL, NULL };

    if (strlen(ap_cli_path) == 0) {
        return -1;
    }

    // Convert the size to a string
    _snprintf(sizeStr, sizeof(sizeStr), "%d", size);
    args[7] = sizeStr;

    // Convert the object ID to a hex string
    oid_to_hex_r(oid_hex, oid);
    args[9] = oid_hex;

    // Execute the ap.exe process
    result = execute_cli_process(args, outputBuffer, sizeof(outputBuffer), &exitCode);
    if (result != 0) {
        error("Failed to execute ap.exe to create placeholder.");
        return -1;
    }

    if (exitCode != 0) {
        char errorBuffer[1024];
        int errorResult = get_error_from_json(outputBuffer, errorBuffer, sizeof(errorBuffer));
        if (errorResult == 0) {
            error("Failed to create placeholder. Error: %s", errorBuffer);
        } else {
            error("Failed to create placeholder. ap.exe exited with code %lu.", exitCode);
        }
        return -1;
    }

    return 0;
}

static int _is_path_virtual(const char* path) {
    DWORD exitCode;
    int result;
    char outputBuffer[1024];
    const char *ap_cli_path = get_ap_cli_path();
    const char *args[] = { ap_cli_path, "--json", "vfs", "virtual", "--path", absolute_path(path),  NULL };

    if (strlen(ap_cli_path) == 0) {
        return -1;
    }

    // Execute the ap.exe process
    result = execute_cli_process(args, outputBuffer, sizeof(outputBuffer), &exitCode);
    if (result != 0) {
        error("Failed to execute ap.exe to check for virtual state.");
        return -1;
    }

    if (exitCode == 1) {
        return 1; // indicates path is virtual
    } else if (exitCode != 0) {
        char errorBuffer[1024];
        int errorResult = get_error_from_json(outputBuffer, errorBuffer, sizeof(errorBuffer));
        if (errorResult == 0) {
            error("Failed to check for virtual path. Error: %s", errorBuffer);
        } else {
            error("Failed to check for virtual path. ap.exe exited with code %lu.", exitCode);
        }
        return -1; // error
    }

    return 0; // path is not virtual
}

static int _is_sync_root(const char *path) 
{
    DWORD exitCode;
    int result;
    char outputBuffer[1024];
    const char *ap_cli_path = get_ap_cli_path();
    const char *args[] = { ap_cli_path, "--json", "vfs", "syncroot", "--path", absolute_path(path),  NULL };

    if (strlen(ap_cli_path) == 0) {
        return -1;
    }

    // Execute the ap.exe process
    result = execute_cli_process(args, outputBuffer, sizeof(outputBuffer), &exitCode);
    if (result != 0) {
        error("Failed to execute ap.exe to check for sync root state.");
        return -1;
    }

    if (exitCode == 1) {
        return 1; // indicates path is under a sync root
    } else if (exitCode != 0) {
        char errorBuffer[1024];
        int errorResult = get_error_from_json(outputBuffer, errorBuffer, sizeof(errorBuffer));
        if (errorResult == 0) {
            error("Failed to check if path is under a sync root. Error: %s", errorBuffer);
        } else {
            error("Failed to check if path is under a sync root. ap.exe exited with code %lu.", exitCode);
        }
        return -1; // error
    }

    return 0; // path is not under a sync root
}


int is_path_virtual(const char* path) 
{
    if (!path) {
        die("is_path_virtual: path is NULL");
    }

    return _is_path_virtual(path);
}

int create_placeholder(const char *path, unsigned int size, const struct object_id *oid) 
{
    if (!oid) {
        die("create_placeholder: oid is NULL");
    }

    if (!path) {
        die("create_placeholder: path is NULL");
    }

    return _create_placeholder(path, size, oid);
}

int is_sync_root(const char *path)
{
    if (!path) {
        die("is_sync_root: path is NULL");
    }

    return _is_sync_root(path);
}