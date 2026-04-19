#pragma once

#ifdef __ANDROID__

#include <jni.h>
#include <string>
#include <vector>
#include <memory>
#include <istream>
#include <cstdint>
#include <cstdio>

// Android SAF (Storage Access Framework) JNI bridge.
// When native code can't open files on external storage (FUSE denies access
// without MANAGE_EXTERNAL_STORAGE), this bridge calls back to Java which
// opens files via ContentResolver/SAF and returns file descriptors or data.

namespace AndroidSAF {

// Initialize the bridge with a Java NativeSAFBridge object.
// Must be called from a JNI-attached thread before any other calls.
void Init(JNIEnv* env, jobject bridge);

// Tear down global refs.
void Shutdown(JNIEnv* env);

// Check if the bridge is initialized and ready.
bool IsReady();

// Check if a file exists (falls back to SAF if stat() fails).
bool FileExists(const std::string& path);

// Check if a directory exists (falls back to SAF if stat() fails).
bool DirExists(const std::string& path);

// Read an entire file into memory via SAF.
// Returns empty vector if file not found or bridge not ready.
std::vector<uint8_t> ReadFile(const std::string& path);

// Open a file via SAF and return a POSIX fd.
// Caller owns the fd and must close() it.
// Returns -1 on failure.
int OpenFd(const std::string& path, const std::string& mode);

// Write data to a file via SAF.
// Returns true on success.
bool WriteFile(const std::string& path, const uint8_t* data, size_t size);

// Convenience: open a FILE* via SAF. Caller must fclose().
// Returns nullptr on failure.
FILE* FOpen(const std::string& path, const char* mode);

// Open a file as an istream. Reads the whole file into memory and
// wraps it in a stringstream. Only use for small config files.
// Returns nullptr if the file can't be read.
std::unique_ptr<std::istream> OpenFileAsStream(const std::string& path);

// List the names of entries in a directory via SAF.
// Returns empty vector if directory not found or bridge not ready.
std::vector<std::string> ListDirectory(const std::string& path);

} // namespace AndroidSAF

#endif // __ANDROID__
