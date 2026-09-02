#include "markdown_core_kotlin_jni_payload.h"

#include <jni.h>
#include <limits.h>

static void throw_new(JNIEnv *environment, const char *class_name, const char *message) {
    jclass error_class = (*environment)->FindClass(environment, class_name);
    if (error_class != NULL) {
        (*environment)->ThrowNew(environment, error_class, message);
        (*environment)->DeleteLocalRef(environment, error_class);
    }
}

static jbyteArray JNICALL native_parse(JNIEnv *environment, jobject receiver, jbyteArray source, jint options_mask) {
    jbyte *source_bytes;
    jsize source_length;
    uint8_t *output = NULL;
    size_t output_length = 0;
    jbyteArray result;
    (void)receiver;

    if (source == NULL) {
        throw_new(environment, "java/lang/NullPointerException", "source");
        return NULL;
    }
    source_length = (*environment)->GetArrayLength(environment, source);
    source_bytes = NULL;
    if (source_length != 0) {
        source_bytes = (*environment)->GetByteArrayElements(environment, source, NULL);
        if (source_bytes == NULL) {
            return NULL;
        }
    }
    if (!markdown_core_kotlin_jni_encode((const uint8_t *)source_bytes, (size_t)source_length, (uint32_t)options_mask,
                                         &output, &output_length)) {
        if (source_bytes != NULL) {
            (*environment)->ReleaseByteArrayElements(environment, source, source_bytes, JNI_ABORT);
        }
        throw_new(environment, "java/lang/OutOfMemoryError", "JNI AST payload allocation failed");
        return NULL;
    }
    if (source_bytes != NULL) {
        (*environment)->ReleaseByteArrayElements(environment, source, source_bytes, JNI_ABORT);
    }
    if (output_length > (size_t)INT32_MAX) {
        markdown_core_kotlin_jni_payload_free(output);
        throw_new(environment, "java/lang/OutOfMemoryError", "native AST exceeds the JVM array limit");
        return NULL;
    }
    result = (*environment)->NewByteArray(environment, (jsize)output_length);
    if (result != NULL) {
        (*environment)->SetByteArrayRegion(environment, result, 0, (jsize)output_length, (const jbyte *)output);
    }
    markdown_core_kotlin_jni_payload_free(output);
    if ((*environment)->ExceptionCheck(environment)) {
        return NULL;
    }
    return result;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *virtual_machine, void *reserved) {
    static const JNINativeMethod methods[] = {
        {"parsePayload", "([BI)[B", (void *)native_parse},
    };
    JNIEnv *environment = NULL;
    jclass parser_class;
    (void)reserved;

    if ((*virtual_machine)->GetEnv(virtual_machine, (void **)&environment, JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    parser_class = (*environment)->FindClass(environment, "com/nouprax/markdown/core/JniParser");
    if (parser_class == NULL) {
        return JNI_ERR;
    }
    if ((*environment)
            ->RegisterNatives(environment, parser_class, methods, (jint)(sizeof(methods) / sizeof(methods[0]))) !=
        JNI_OK) {
        (*environment)->DeleteLocalRef(environment, parser_class);
        return JNI_ERR;
    }
    (*environment)->DeleteLocalRef(environment, parser_class);
    return JNI_VERSION_1_6;
}
