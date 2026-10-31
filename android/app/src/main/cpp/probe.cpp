#include <jni.h>

extern "C" JNIEXPORT jstring JNICALL
Java_com_gungnir_recomp_MainActivity_nativeProbe(JNIEnv *env, jobject) {
#if defined(__aarch64__)
    const char *abi = "arm64-v8a";
#elif defined(__x86_64__)
    const char *abi = "x86_64";
#else
    const char *abi = "unknown";
#endif
    return env->NewStringUTF(abi);
}
