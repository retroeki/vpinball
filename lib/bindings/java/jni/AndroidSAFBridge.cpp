#ifdef __ANDROID__

#include "AndroidSAFBridge.h"
#include <android/log.h>
#include <unistd.h>
#include <mutex>
#include <sstream>

#define LOG_TAG "AndroidSAFBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace AndroidSAF {

static JavaVM* g_jvm = nullptr;
static jobject g_bridge = nullptr;
static jmethodID g_openFdMethod = nullptr;
static jmethodID g_fileExistsMethod = nullptr;
static jmethodID g_dirExistsMethod = nullptr;
static jmethodID g_readFileMethod = nullptr;
static jmethodID g_writeFileMethod = nullptr;
static jmethodID g_listDirMethod = nullptr;
static std::mutex g_mutex;

// Get JNIEnv for the current thread, attaching if needed.
static JNIEnv* GetEnv() {
    if (!g_jvm) return nullptr;
    JNIEnv* env = nullptr;
    int status = g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        JavaVMAttachArgs args;
        args.version = JNI_VERSION_1_6;
        args.name = "NativeSAFBridge";
        args.group = nullptr;
        if (g_jvm->AttachCurrentThread(&env, &args) != JNI_OK) {
            LOGE("Failed to attach thread to JVM");
            return nullptr;
        }
    }
    return env;
}

void Init(JNIEnv* env, jobject bridge) {
    std::lock_guard<std::mutex> lock(g_mutex);

    env->GetJavaVM(&g_jvm);

    // Clean up old refs if re-initializing
    if (g_bridge) {
        env->DeleteGlobalRef(g_bridge);
    }

    g_bridge = env->NewGlobalRef(bridge);

    jclass cls = env->GetObjectClass(bridge);
    g_openFdMethod = env->GetMethodID(cls, "openFileDescriptor", "(Ljava/lang/String;Ljava/lang/String;)I");
    g_fileExistsMethod = env->GetMethodID(cls, "fileExists", "(Ljava/lang/String;)Z");
    g_dirExistsMethod = env->GetMethodID(cls, "dirExists", "(Ljava/lang/String;)Z");
    g_readFileMethod = env->GetMethodID(cls, "readFileBytes", "(Ljava/lang/String;)[B");
    g_writeFileMethod = env->GetMethodID(cls, "writeFileBytes", "(Ljava/lang/String;[B)Z");
    g_listDirMethod = env->GetMethodID(cls, "listDirectory", "(Ljava/lang/String;)[Ljava/lang/String;");
    env->DeleteLocalRef(cls);

    LOGI("SAF bridge initialized");
}

void Shutdown(JNIEnv* env) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_bridge) {
        env->DeleteGlobalRef(g_bridge);
        g_bridge = nullptr;
    }
    g_jvm = nullptr;
    LOGI("SAF bridge shutdown");
}

bool IsReady() {
    return g_bridge != nullptr && g_jvm != nullptr;
}

bool FileExists(const std::string& path) {
    if (!IsReady()) return false;
    JNIEnv* env = GetEnv();
    if (!env) return false;

    jstring jpath = env->NewStringUTF(path.c_str());
    jboolean result = env->CallBooleanMethod(g_bridge, g_fileExistsMethod, jpath);
    env->DeleteLocalRef(jpath);

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    return result;
}

bool DirExists(const std::string& path) {
    if (!IsReady()) return false;
    JNIEnv* env = GetEnv();
    if (!env) return false;

    jstring jpath = env->NewStringUTF(path.c_str());
    jboolean result = env->CallBooleanMethod(g_bridge, g_dirExistsMethod, jpath);
    env->DeleteLocalRef(jpath);

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    return result;
}

std::vector<uint8_t> ReadFile(const std::string& path) {
    if (!IsReady()) return {};
    JNIEnv* env = GetEnv();
    if (!env) return {};

    jstring jpath = env->NewStringUTF(path.c_str());
    jbyteArray jarr = (jbyteArray)env->CallObjectMethod(g_bridge, g_readFileMethod, jpath);
    env->DeleteLocalRef(jpath);

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return {};
    }
    if (!jarr) return {};

    jsize len = env->GetArrayLength(jarr);
    std::vector<uint8_t> data(len);
    env->GetByteArrayRegion(jarr, 0, len, (jbyte*)data.data());
    env->DeleteLocalRef(jarr);
    return data;
}

int OpenFd(const std::string& path, const std::string& mode) {
    if (!IsReady()) return -1;
    JNIEnv* env = GetEnv();
    if (!env) return -1;

    jstring jpath = env->NewStringUTF(path.c_str());
    jstring jmode = env->NewStringUTF(mode.c_str());
    jint fd = env->CallIntMethod(g_bridge, g_openFdMethod, jpath, jmode);
    env->DeleteLocalRef(jpath);
    env->DeleteLocalRef(jmode);

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return -1;
    }
    return fd;
}

bool WriteFile(const std::string& path, const uint8_t* data, size_t size) {
    if (!IsReady()) return false;
    JNIEnv* env = GetEnv();
    if (!env) return false;

    jstring jpath = env->NewStringUTF(path.c_str());
    jbyteArray jarr = env->NewByteArray(size);
    env->SetByteArrayRegion(jarr, 0, size, (const jbyte*)data);
    jboolean result = env->CallBooleanMethod(g_bridge, g_writeFileMethod, jpath, jarr);
    env->DeleteLocalRef(jpath);
    env->DeleteLocalRef(jarr);

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    return result;
}

FILE* FOpen(const std::string& path, const char* mode) {
    int fd = OpenFd(path, mode);
    if (fd < 0) return nullptr;
    FILE* f = fdopen(fd, mode);
    if (!f) {
        close(fd);
        return nullptr;
    }
    return f;
}

std::unique_ptr<std::istream> OpenFileAsStream(const std::string& path) {
    std::vector<uint8_t> data = ReadFile(path);
    if (data.empty()) return nullptr;
    std::string content(data.begin(), data.end());
    return std::make_unique<std::istringstream>(std::move(content));
}

std::vector<std::string> ListDirectory(const std::string& path) {
    if (!IsReady()) return {};
    JNIEnv* env = GetEnv();
    if (!env) return {};

    jstring jpath = env->NewStringUTF(path.c_str());
    jobjectArray jarr = (jobjectArray)env->CallObjectMethod(g_bridge, g_listDirMethod, jpath);
    env->DeleteLocalRef(jpath);

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return {};
    }
    if (!jarr) return {};

    jsize len = env->GetArrayLength(jarr);
    std::vector<std::string> result;
    result.reserve(len);
    for (jsize i = 0; i < len; i++) {
        jstring jstr = (jstring)env->GetObjectArrayElement(jarr, i);
        if (jstr) {
            const char* chars = env->GetStringUTFChars(jstr, nullptr);
            if (chars) {
                result.emplace_back(chars);
                env->ReleaseStringUTFChars(jstr, chars);
            }
            env->DeleteLocalRef(jstr);
        }
    }
    env->DeleteLocalRef(jarr);
    return result;
}

} // namespace AndroidSAF

#endif // __ANDROID__
