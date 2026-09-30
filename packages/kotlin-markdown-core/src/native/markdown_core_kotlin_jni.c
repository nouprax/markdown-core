#include <markdown_core_wire.h>

#include <jni.h>
#include <stdint.h>

/* The MCB3 message's u32 length, which counts the whole message. */
static uint32_t message_length(const uint8_t *message) {
    return (uint32_t)message[4] | (uint32_t)message[5] << 8 | (uint32_t)message[6] << 16 | (uint32_t)message[7] << 24;
}

/* Returns the MCB3 message (docs/architecture/wire-format.md) for `source`.
 * NULL with no exception pending means ALLOCATION_FAILED: the engine could not
 * allocate the message, or it is longer than a JVM array can hold. NULL with an
 * exception pending is the JVM's own failure to allocate. The Kotlin decoder
 * owns everything after the copy. */
static jbyteArray JNICALL native_parse(JNIEnv *environment, jobject receiver, jbyteArray source) {
    jbyte *source_bytes;
    jsize source_length;
    uint8_t *message;
    uint32_t length;
    jbyteArray result;
    (void)receiver;

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
        return NULL;
    }
    length = message_length(message);
    result = NULL;
    if (length <= INT32_MAX) {
        /* NULL leaves the JVM's OutOfMemoryError pending. */
        result = (*environment)->NewByteArray(environment, (jsize)length);
        if (result != NULL) {
            (*environment)->SetByteArrayRegion(environment, result, 0, (jsize)length, (const jbyte *)message);
        }
    }
    markdown_core_wire_free(message);
    return result;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *virtual_machine, void *reserved) {
    static const JNINativeMethod methods[] = {
        {"parsePayload", "([B)[B", (void *)native_parse},
    };
    JNIEnv *environment;
    jclass parser_class;
    (void)reserved;

    (*virtual_machine)->GetEnv(virtual_machine, (void **)&environment, JNI_VERSION_1_6);
    parser_class = (*environment)->FindClass(environment, "com/nouprax/markdown/core/JniParser");
    (*environment)->RegisterNatives(environment, parser_class, methods, (jint)(sizeof(methods) / sizeof(methods[0])));
    return JNI_VERSION_1_6;
}
