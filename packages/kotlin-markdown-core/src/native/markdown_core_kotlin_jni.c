#include <markdown_core_wire.h>

#include <jni.h>
#include <stdint.h>

static void throw_new(JNIEnv *environment, const char *class_name, const char *message) {
    jclass error_class = (*environment)->FindClass(environment, class_name);
    if (error_class != NULL) {
        (*environment)->ThrowNew(environment, error_class, message);
        (*environment)->DeleteLocalRef(environment, error_class);
    }
}

static uint32_t message_length(const uint8_t *message) {
    return (uint32_t)message[4] | (uint32_t)message[5] << 8 | (uint32_t)message[6] << 16 | (uint32_t)message[7] << 24;
}

/* Returns the MCB3 message (docs/architecture/wire-format.md) for `source`;
 * the Kotlin decoder owns everything after the copy. */
static jbyteArray JNICALL native_parse(JNIEnv *environment, jobject receiver, jbyteArray source) {
    jbyte *source_bytes;
    jsize source_length;
    uint8_t *message;
    uint32_t length;
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
    message = markdown_core_wire_parse((const uint8_t *)source_bytes, (size_t)source_length);
    if (source_bytes != NULL) {
        (*environment)->ReleaseByteArrayElements(environment, source, source_bytes, JNI_ABORT);
    }
    if (message == NULL) {
        throw_new(environment, "java/lang/OutOfMemoryError", "native AST message allocation failed");
        return NULL;
    }
    length = message_length(message);
    if (length > (uint32_t)INT32_MAX) {
        markdown_core_wire_free(message);
        throw_new(environment, "java/lang/OutOfMemoryError", "native AST exceeds the JVM array limit");
        return NULL;
    }
    result = (*environment)->NewByteArray(environment, (jsize)length);
    if (result != NULL) {
        (*environment)->SetByteArrayRegion(environment, result, 0, (jsize)length, (const jbyte *)message);
    }
    markdown_core_wire_free(message);
    if ((*environment)->ExceptionCheck(environment)) {
        return NULL;
    }
    return result;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *virtual_machine, void *reserved) {
    static const JNINativeMethod methods[] = {
        {"parsePayload", "([B)[B", (void *)native_parse},
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
