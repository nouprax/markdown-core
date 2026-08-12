#include "markdown_core_kotlin_bridge.h"

#include <jni.h>

static void throw_out_of_memory(JNIEnv *environment, const char *message) {
    jclass error_class = (*environment)->FindClass(environment, "java/lang/OutOfMemoryError");
    if (error_class != NULL) {
        (*environment)->ThrowNew(environment, error_class, message);
    }
}

static jbyteArray payload_to_array(JNIEnv *environment, uint8_t *output, size_t output_length) {
    jbyteArray result;
    if (output_length > (size_t)INT32_MAX) {
        markdown_core_kotlin_free(output);
        throw_out_of_memory(environment, "native payload exceeds the JVM array limit");
        return NULL;
    }
    result = (*environment)->NewByteArray(environment, (jsize)output_length);
    if (result != NULL) {
        (*environment)
            ->SetByteArrayRegion(environment, result, 0, (jsize)output_length,
                                 (const jbyte *)output);
    }
    markdown_core_kotlin_free(output);
    return result;
}

JNIEXPORT jbyteArray JNICALL Java_com_nouprax_markdown_core_JvmNative_open(JNIEnv *environment,
                                                                           jobject receiver,
                                                                           jbyteArray source,
                                                                           jint options_mask) {
    jbyte *source_bytes;
    jsize source_length;
    uint8_t *output = NULL;
    size_t output_length = 0;
    bool encoded;
    (void)receiver;

    source_length = (*environment)->GetArrayLength(environment, source);
    source_bytes = (*environment)->GetByteArrayElements(environment, source, NULL);
    if (source_bytes == NULL) {
        return NULL;
    }
    encoded = markdown_core_kotlin_open((const uint8_t *)source_bytes, (size_t)source_length,
                                        (uint32_t)options_mask, &output, &output_length);
    (*environment)->ReleaseByteArrayElements(environment, source, source_bytes, JNI_ABORT);
    if (!encoded) {
        throw_out_of_memory(environment, "native AST copy failed");
        return NULL;
    }
    return payload_to_array(environment, output, output_length);
}

JNIEXPORT jbyteArray JNICALL Java_com_nouprax_markdown_core_JvmNative_edit(JNIEnv *environment,
                                                                           jobject receiver,
                                                                           jlong handle,
                                                                           jbyteArray source) {
    jbyte *source_bytes;
    jsize source_length;
    uint8_t *output = NULL;
    size_t output_length = 0;
    bool encoded;
    (void)receiver;

    source_length = (*environment)->GetArrayLength(environment, source);
    source_bytes = (*environment)->GetByteArrayElements(environment, source, NULL);
    if (source_bytes == NULL) {
        return NULL;
    }
    encoded = markdown_core_kotlin_edit((uint64_t)handle, (const uint8_t *)source_bytes,
                                        (size_t)source_length, &output, &output_length);
    (*environment)->ReleaseByteArrayElements(environment, source, source_bytes, JNI_ABORT);
    if (!encoded) {
        throw_out_of_memory(environment, "native document edit failed");
        return NULL;
    }
    return payload_to_array(environment, output, output_length);
}

JNIEXPORT void JNICALL Java_com_nouprax_markdown_core_JvmNative_release(JNIEnv *environment,
                                                                        jobject receiver,
                                                                        jlong handle) {
    (void)environment;
    (void)receiver;
    markdown_core_kotlin_release((uint64_t)handle);
}
