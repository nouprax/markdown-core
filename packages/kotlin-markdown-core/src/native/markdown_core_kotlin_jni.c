#include "markdown_core_kotlin_bridge.h"

#include <jni.h>

JNIEXPORT jbyteArray JNICALL Java_com_nouprax_markdown_core_JvmNative_parse(JNIEnv *environment,
                                                                            jobject receiver,
                                                                            jbyteArray source,
                                                                            jint options_mask) {
    jbyte *source_bytes;
    jsize source_length;
    uint8_t *output = NULL;
    size_t output_length = 0;
    jbyteArray result;
    (void)receiver;

    source_length = (*environment)->GetArrayLength(environment, source);
    source_bytes = (*environment)->GetByteArrayElements(environment, source, NULL);
    if (source_bytes == NULL) {
        return NULL;
    }
    if (!markdown_core_kotlin_parse((const uint8_t *)source_bytes, (size_t)source_length,
                                    (uint32_t)options_mask, &output, &output_length)) {
        (*environment)->ReleaseByteArrayElements(environment, source, source_bytes, JNI_ABORT);
        jclass error_class = (*environment)->FindClass(environment, "java/lang/OutOfMemoryError");
        if (error_class != NULL) {
            (*environment)->ThrowNew(environment, error_class, "native AST copy failed");
        }
        return NULL;
    }
    (*environment)->ReleaseByteArrayElements(environment, source, source_bytes, JNI_ABORT);
    if (output_length > (size_t)INT32_MAX) {
        markdown_core_kotlin_free(output);
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
